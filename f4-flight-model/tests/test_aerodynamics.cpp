// f4-flight-model/tests/test_aerodynamics.cpp
//
// Unit tests for the Aerodynamics force model.
//
// Uses a SYNTHETIC 2x2 aero table (mach × alpha) so each branch is testable
// in isolation without the F-16 fixture. Covers:
//   - CL/CD/CY bilinear interpolation at grid points and midpoints
//   - Ground effect (cushion zone and transition zone)
//   - TEF/LEF factor scaling
//   - Stall model (stallSpeed computation, FlatSpin → lift=0)
//   - Force axis transformations (body, stability, wind)
//   - Drag additions (speed brake, gear, stores, drag chute)

#include "f4/flight/aerodynamics.hpp"
#include "f4/flight/constants.hpp"

#include <gtest/gtest.h>

#include <cmath>

namespace f4::flight {
namespace {

// ---------------------------------------------------------------------------
// Synthetic 2×2 aero table:
//   mach  = { 0.0, 1.0 }
//   alpha = { 0.0, 10.0 }  (degrees)
//
// CL values chosen to be distinguishable per grid point:
//   cl[0,0]=0.0   cl[0,1]=1.0   (mach=0)
//   cl[1,0]=0.0   cl[1,1]=1.2   (mach=1, slight compressibility boost)
//
// CD values: cd = cl * 0.1 + 0.02 (induced + parasitic)
// CY values: all zero (side force tested separately)
// ---------------------------------------------------------------------------
struct SyntheticAero {
    data::AeroTable        table;
    data::AircraftGeometry geom;
    data::AuxAero          aux;

    SyntheticAero() {
        table.mach      = { 0.0, 1.0 };
        table.alpha_deg = { 0.0, 10.0 };
        // Row-major: data[mach_idx * numAlpha + alpha_idx]
        table.clift = { 0.0, 1.0,  0.0, 1.2 };
        table.cdrag = { 0.02, 0.12, 0.02, 0.14 };
        table.cy    = { 0.0, 0.0,  0.0, 0.0  };
        table.clFactor = 1.0;
        table.cdFactor = 1.0;
        table.cyFactor = 1.0;

        geom.area_ft2 = 300.0;    // F-16-ish wing area
        geom.span_ft = 30.0;      // 30 ft span → ground effect zone at <6 ft
        geom.emptyWeight_lbs = 10000.0;
        geom.aoaMax_deg = 25.0;

        aux.CLtefFactor = 0.05;
        aux.CDtefFactor = 0.05;
        aux.CDlefFactor = 0.05;
        aux.CDSPDBFactor = 0.08;
        aux.CDLDGFactor = 0.06;
        aux.dragChuteCd = 0.0;
        aux.criticalAOA = 0.0;    // disable stall model by default
    }
};

AeroState makeAero() {
    AeroState a;
    a.tefPos = 0.0;
    a.lefPos = 0.0;
    a.dbrake = 0.0;
    a.gearPos = 0.0;  // gear up
    a.dragChutePos = 0.0;
    a.cdStores = 0.0;
    a.stallState = 0;  // None
    return a;
}

}  // namespace

// ============================================================================
// Bilinear interpolation — CL/CD at grid points and midpoints
// ============================================================================

TEST(Aerodynamics, ClAtGridPointMach0Alpha0) {
    SyntheticAero sa;
    Aerodynamics a(&sa.table, &sa.geom, &sa.aux);
    AeroState s = makeAero();
    a.update(angle_from_degrees(0.0), angle_from_degrees(0.0), /*mach=*/0.0, /*vt=*/100.0,
             /*qbar=*/1.0, /*qsom=*/1.0, /*alt=*/1000.0, /*groundZ=*/0.0,
             /*z=*/-1000.0, /*vcas=*/100.0, /*pstick=*/0.0, s);
    EXPECT_NEAR(s.cl, 0.0, 1e-9);
}

TEST(Aerodynamics, ClAtGridPointMach0Alpha10) {
    SyntheticAero sa;
    Aerodynamics a(&sa.table, &sa.geom, &sa.aux);
    AeroState s = makeAero();
    a.update(angle_from_degrees(10.0), angle_from_degrees(0.0), 0.0, 100.0, 1.0, 1.0, 1000.0, 0.0, -1000.0, 100.0, 0.0, s);
    EXPECT_NEAR(s.cl, 1.0, 1e-9);
}

TEST(Aerodynamics, ClAtMidpointMachHalfAlphaHalf) {
    // At mach=0.5, alpha=5: bilinear interpolation of the 2x2 grid.
    // cl[0,0]=0, cl[0,1]=1, cl[1,0]=0, cl[1,1]=1.2
    // At mach=0.5, alpha=5: average of the four corners weighted = (0+1+0+1.2)/4 = 0.55
    SyntheticAero sa;
    Aerodynamics a(&sa.table, &sa.geom, &sa.aux);
    AeroState s = makeAero();
    a.update(angle_from_degrees(5.0), angle_from_degrees(0.0), 0.5, 100.0, 1.0, 1.0, 1000.0, 0.0, -1000.0, 100.0, 0.0, s);
    EXPECT_NEAR(s.cl, 0.55, 1e-9);
}

TEST(Aerodynamics, ClScalesWithClFactor) {
    SyntheticAero sa;
    sa.table.clFactor = 2.0;
    Aerodynamics a(&sa.table, &sa.geom, &sa.aux);
    AeroState s = makeAero();
    a.update(angle_from_degrees(10.0), angle_from_degrees(0.0), 0.0, 100.0, 1.0, 1.0, 1000.0, 0.0, -1000.0, 100.0, 0.0, s);
    EXPECT_NEAR(s.cl, 2.0, 1e-9);  // 1.0 * 2.0
}

// ============================================================================
// Ground effect
// ============================================================================

TEST(Aerodynamics, GroundEffectBoostsClNearGround) {
    // Within 0.2*span of the ground, CL *= 1.13.
    // span = 30 ft, so within 6 ft AGL.
    SyntheticAero sa;
    Aerodynamics a(&sa.table, &sa.geom, &sa.aux);

    // Away from ground (AGL = 1000 ft)
    AeroState s_away = makeAero();
    a.update(angle_from_degrees(10.0), angle_from_degrees(0.0), 0.0, 100.0, 1.0, 1.0, 1000.0, 0.0, -1100.0, 100.0, 0.0, s_away);
    const double cl_away = s_away.cl;

    // Near ground (AGL = 3 ft, within 0.2*span = 6 ft)
    AeroState s_near = makeAero();
    a.update(angle_from_degrees(10.0), angle_from_degrees(0.0), 0.0, 100.0, 1.0, 1.0, 100.0, 0.0, -3.0, 100.0, 0.0, s_near);
    const double cl_near = s_near.cl;

    EXPECT_NEAR(cl_near / cl_away, 1.13, 1e-3)
        << "ground effect should boost CL by 13%";
}

TEST(Aerodynamics, GroundEffectFadesInTransitionZone) {
    // Between 0.2*span (6 ft) and 1.0*span (30 ft), CL fades from 1.13 to 1.0.
    SyntheticAero sa;
    Aerodynamics a(&sa.table, &sa.geom, &sa.aux);

    AeroState s_away = makeAero();
    a.update(angle_from_degrees(10.0), angle_from_degrees(0.0), 0.0, 100.0, 1.0, 1.0, 1000.0, 0.0, -1100.0, 100.0, 0.0, s_away);
    const double cl_baseline = s_away.cl;

    // AGL = 18 ft (midpoint of transition zone: 6..30)
    // Expected factor = 1.13 - ((18 - 6) / (30 - 6)) * 0.13 = 1.13 - 0.5*0.13 = 1.065
    AeroState s_mid = makeAero();
    a.update(angle_from_degrees(10.0), angle_from_degrees(0.0), 0.0, 100.0, 1.0, 1.0, 100.0, 0.0, -18.0, 100.0, 0.0, s_mid);
    EXPECT_NEAR(s_mid.cl / cl_baseline, 1.065, 1e-3);
}

TEST(Aerodynamics, GroundEffectOffBeyondOneSpan) {
    SyntheticAero sa;
    Aerodynamics a(&sa.table, &sa.geom, &sa.aux);

    AeroState s_away = makeAero();
    a.update(angle_from_degrees(10.0), angle_from_degrees(0.0), 0.0, 100.0, 1.0, 1.0, 1000.0, 0.0, -1100.0, 100.0, 0.0, s_away);

    // AGL = 35 ft > span (30 ft) → no ground effect.
    AeroState s_beyond = makeAero();
    a.update(angle_from_degrees(10.0), angle_from_degrees(0.0), 0.0, 100.0, 1.0, 1.0, 100.0, 0.0, -35.0, 100.0, 0.0, s_beyond);
    EXPECT_NEAR(s_beyond.cl, s_away.cl, 1e-9);
}

// ============================================================================
// TEF / LEF factor scaling
// ============================================================================

TEST(Aerodynamics, TefBoostsCl) {
    // cl *= (1 + tefPos * CLtefFactor)
    // With tefPos=1.0, CLtefFactor=0.05: cl *= 1.05
    SyntheticAero sa;
    Aerodynamics a(&sa.table, &sa.geom, &sa.aux);

    AeroState s_no_tef = makeAero();
    s_no_tef.tefPos = 0.0;
    a.update(angle_from_degrees(10.0), angle_from_degrees(0.0), 0.0, 100.0, 1.0, 1.0, 1000.0, 0.0, -1100.0, 100.0, 0.0, s_no_tef);

    AeroState s_tef = makeAero();
    s_tef.tefPos = 1.0;
    a.update(angle_from_degrees(10.0), angle_from_degrees(0.0), 0.0, 100.0, 1.0, 1.0, 1000.0, 0.0, -1100.0, 100.0, 0.0, s_tef);

    EXPECT_NEAR(s_tef.cl / s_no_tef.cl, 1.05, 1e-3);
}

TEST(Aerodynamics, LefChangesCd) {
    // LEF affects CD in two ways:
    //   1. tempAlpha = alpha + tef - lef (LEF reduces effective alpha)
    //   2. cd *= (1 + tefPos*CDtefFactor + lefPos*CDlefFactor)
    // The net effect depends on the CD table's slope. We just verify cd
    // CHANGES when LEF is deployed (not necessarily increases).
    SyntheticAero sa;
    Aerodynamics a(&sa.table, &sa.geom, &sa.aux);

    AeroState s_no_lef = makeAero();
    a.update(angle_from_degrees(10.0), angle_from_degrees(0.0), 0.0, 100.0, 1.0, 1.0, 1000.0, 0.0, -1100.0, 100.0, 0.0, s_no_lef);

    AeroState s_lef = makeAero();
    s_lef.lefPos = 1.0;
    a.update(angle_from_degrees(10.0), angle_from_degrees(0.0), 0.0, 100.0, 1.0, 1.0, 1000.0, 0.0, -1100.0, 100.0, 0.0, s_lef);

    EXPECT_NE(s_lef.cd, s_no_lef.cd)
        << "LEF deployment should change CD (via alpha shift and/or CDlefFactor)";
}

// ============================================================================
// Drag additions — speed brake, gear, stores, drag chute
// ============================================================================

TEST(Aerodynamics, SpeedBrakeAddsDrag) {
    // cd += CDSPDBFactor * dbrake
    SyntheticAero sa;
    sa.aux.CDSPDBFactor = 0.08;
    Aerodynamics a(&sa.table, &sa.geom, &sa.aux);

    AeroState s_no_brake = makeAero();
    a.update(angle_from_degrees(10.0), angle_from_degrees(0.0), 0.0, 100.0, 1.0, 1.0, 1000.0, 0.0, -1100.0, 100.0, 0.0, s_no_brake);

    AeroState s_brake = makeAero();
    s_brake.dbrake = 1.0;
    a.update(angle_from_degrees(10.0), angle_from_degrees(0.0), 0.0, 100.0, 1.0, 1.0, 1000.0, 0.0, -1100.0, 100.0, 0.0, s_brake);

    EXPECT_NEAR(s_brake.cd - s_no_brake.cd, 0.08, 1e-9);
}

TEST(Aerodynamics, GearAddsDrag) {
    // cd += CDLDGFactor * gearPos
    SyntheticAero sa;
    sa.aux.CDLDGFactor = 0.06;
    Aerodynamics a(&sa.table, &sa.geom, &sa.aux);

    AeroState s_gear_up = makeAero();
    s_gear_up.gearPos = 0.0;
    a.update(angle_from_degrees(10.0), angle_from_degrees(0.0), 0.0, 100.0, 1.0, 1.0, 1000.0, 0.0, -1100.0, 100.0, 0.0, s_gear_up);

    AeroState s_gear_down = makeAero();
    s_gear_down.gearPos = 1.0;
    a.update(angle_from_degrees(10.0), angle_from_degrees(0.0), 0.0, 100.0, 1.0, 1.0, 1000.0, 0.0, -1100.0, 100.0, 0.0, s_gear_down);

    EXPECT_NEAR(s_gear_down.cd - s_gear_up.cd, 0.06, 1e-9);
}

TEST(Aerodynamics, StoresDragAddedToCd) {
    SyntheticAero sa;
    Aerodynamics a(&sa.table, &sa.geom, &sa.aux);

    AeroState s = makeAero();
    s.cdStores = 0.15;
    a.update(angle_from_degrees(10.0), angle_from_degrees(0.0), 0.0, 100.0, 1.0, 1.0, 1000.0, 0.0, -1100.0, 100.0, 0.0, s);

    AeroState s_no_stores = makeAero();
    s_no_stores.cdStores = 0.0;
    a.update(angle_from_degrees(10.0), angle_from_degrees(0.0), 0.0, 100.0, 1.0, 1.0, 1000.0, 0.0, -1100.0, 100.0, 0.0, s_no_stores);

    EXPECT_NEAR(s.cd - s_no_stores.cd, 0.15, 1e-9);
}

TEST(Aerodynamics, DragChuteAddsDragWhenDeployed) {
    // cd += dragChuteCd * dragChutePos  (only if dragChutePos > 0.5)
    SyntheticAero sa;
    sa.aux.dragChuteCd = 0.3;
    Aerodynamics a(&sa.table, &sa.geom, &sa.aux);

    AeroState s_no_chute = makeAero();
    s_no_chute.dragChutePos = 0.0;
    a.update(angle_from_degrees(10.0), angle_from_degrees(0.0), 0.0, 100.0, 1.0, 1.0, 1000.0, 0.0, -1100.0, 100.0, 0.0, s_no_chute);

    AeroState s_chute = makeAero();
    s_chute.dragChutePos = 1.0;
    a.update(angle_from_degrees(10.0), angle_from_degrees(0.0), 0.0, 100.0, 1.0, 1.0, 1000.0, 0.0, -1100.0, 100.0, 0.0, s_chute);

    EXPECT_NEAR(s_chute.cd - s_no_chute.cd, 0.3, 1e-9);
}

TEST(Aerodynamics, DragChuteDoesNotDeployBelowHalfThreshold) {
    SyntheticAero sa;
    sa.aux.dragChuteCd = 0.3;
    Aerodynamics a(&sa.table, &sa.geom, &sa.aux);

    AeroState s = makeAero();
    s.dragChutePos = 0.4;  // below 0.5 threshold
    a.update(angle_from_degrees(10.0), angle_from_degrees(0.0), 0.0, 100.0, 1.0, 1.0, 1000.0, 0.0, -1100.0, 100.0, 0.0, s);

    AeroState s_zero = makeAero();
    s_zero.dragChutePos = 0.0;
    a.update(angle_from_degrees(10.0), angle_from_degrees(0.0), 0.0, 100.0, 1.0, 1.0, 1000.0, 0.0, -1100.0, 100.0, 0.0, s_zero);

    EXPECT_NEAR(s.cd, s_zero.cd, 1e-9)
        << "drag chute below 0.5 should not add drag";
}

// ============================================================================
// Stall model
// ============================================================================

TEST(Aerodynamics, FlatSpinZeroesLift) {
    // When stallState == 4 (FlatSpin), lift = 0 regardless of CL.
    SyntheticAero sa;
    Aerodynamics a(&sa.table, &sa.geom, &sa.aux);

    AeroState s = makeAero();
    s.stallState = 4;  // FlatSpin
    a.update(angle_from_degrees(10.0), angle_from_degrees(0.0), 0.0, 100.0, 1.0, 10.0, 1000.0, 0.0, -1100.0, 100.0, 0.0, s);
    EXPECT_DOUBLE_EQ(s.lift, 0.0);
}

TEST(Aerodynamics, ZeroAirspeedZeroesLift) {
    // When vt < 1e-3, lift = 0 to avoid NaN.
    SyntheticAero sa;
    Aerodynamics a(&sa.table, &sa.geom, &sa.aux);

    AeroState s = makeAero();
    a.update(angle_from_degrees(10.0), angle_from_degrees(0.0), 0.0, /*vt=*/0.0, 1.0, 1.0, 1000.0, 0.0, -1100.0, 100.0, 0.0, s);
    EXPECT_DOUBLE_EQ(s.lift, 0.0);
}

TEST(Aerodynamics, NormalFlightLiftIsClTimesQsom) {
    SyntheticAero sa;
    Aerodynamics a(&sa.table, &sa.geom, &sa.aux);

    AeroState s = makeAero();
    a.update(angle_from_degrees(10.0), angle_from_degrees(0.0), 0.0, 100.0, 1.0, /*qsom=*/5.0, 1000.0, 0.0, -1100.0, 100.0, 0.0, s);
    // lift = cl * qsom = 1.0 * 5.0 = 5.0
    EXPECT_NEAR(s.lift, s.cl * 5.0, 1e-9);
}

TEST(Aerodynamics, StallDetectionSetsStalledFlag) {
    // With criticalAOA > 0 and alpha > criticalAOA, the stalled flag is set.
    SyntheticAero sa;
    sa.aux.criticalAOA = 15.0;  // 15 deg critical AOA
    Aerodynamics a(&sa.table, &sa.geom, &sa.aux);

    AeroState s = makeAero();
    // alpha = 20 > criticalAOA = 15 → stalled
    a.update(angle_from_degrees(20.0), angle_from_degrees(0.0), 0.0, 100.0, 1.0, 5.0, 1000.0, 0.0, -1100.0, 100.0, 0.0, s);
    EXPECT_TRUE(s.stalled);
}

TEST(Aerodynamics, NoStallWhenCriticalAoaIsZero) {
    // criticalAOA = 0 disables the stall model entirely.
    SyntheticAero sa;
    sa.aux.criticalAOA = 0.0;
    Aerodynamics a(&sa.table, &sa.geom, &sa.aux);

    AeroState s = makeAero();
    a.update(angle_from_degrees(30.0), angle_from_degrees(0.0), 0.0, 100.0, 1.0, 5.0, 1000.0, 0.0, -1100.0, 100.0, 0.0, s);
    EXPECT_FALSE(s.stalled);
    EXPECT_DOUBLE_EQ(s.stallSpeed, 0.0);
}

// ============================================================================
// Force axis transformations
// ============================================================================

TEST(Aerodynamics, StabilityAxesLiftIsNegativeZ) {
    // zsaero = -lift
    SyntheticAero sa;
    Aerodynamics a(&sa.table, &sa.geom, &sa.aux);
    AeroState s = makeAero();
    a.update(angle_from_degrees(10.0), angle_from_degrees(0.0), 0.0, 100.0, 1.0, 5.0, 1000.0, 0.0, -1100.0, 100.0, 0.0, s);
    EXPECT_NEAR(s.zsaero, -s.lift, 1e-9);
}

TEST(Aerodynamics, StabilityAxesDragIsNegativeX) {
    // xsaero = -drag
    SyntheticAero sa;
    Aerodynamics a(&sa.table, &sa.geom, &sa.aux);
    AeroState s = makeAero();
    a.update(angle_from_degrees(10.0), angle_from_degrees(0.0), 0.0, 100.0, 1.0, 5.0, 1000.0, 0.0, -1100.0, 100.0, 0.0, s);
    EXPECT_NEAR(s.xsaero, -s.drag, 1e-9);
}

TEST(Aerodynamics, BodyAxesCombineLiftAndDragAtZeroAlpha) {
    // At alpha=0: xaero = -drag, zaero = -lift
    SyntheticAero sa;
    Aerodynamics a(&sa.table, &sa.geom, &sa.aux);
    AeroState s = makeAero();
    a.update(angle_from_degrees(0.0), angle_from_degrees(0.0), 0.0, 100.0, 1.0, 5.0, 1000.0, 0.0, -1100.0, 100.0, 0.0, s);
    EXPECT_NEAR(s.xaero, -s.drag, 1e-9);
    EXPECT_NEAR(s.zaero, -s.lift, 1e-9);
}

TEST(Aerodynamics, BetaRotatesWindAxes) {
    // With nonzero beta, xwaero/ywaero are rotated from xsaero/ysaero.
    SyntheticAero sa;
    // Give CY a nonzero value so yaero is nonzero.
    sa.table.cy = { 0.1, 0.1, 0.1, 0.1 };
    Aerodynamics a(&sa.table, &sa.geom, &sa.aux);
    AeroState s = makeAero();
    a.update(angle_from_degrees(5.0), angle_from_degrees(10.0), 0.0, 100.0, 1.0, 5.0, 1000.0, 0.0, -1100.0, 100.0, 0.0, s);

    // With nonzero beta and nonzero yaero, the wind-axis rotation should
    // produce different values from the stability-axis values.
    EXPECT_NE(s.xwaero, s.xsaero);
}

}  // namespace f4::flight
