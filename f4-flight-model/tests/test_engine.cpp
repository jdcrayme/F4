// f4-flight-model/tests/test_engine.cpp
//
// Unit tests for EngineModel, focusing on the per-branch behavior that the
// integration tests (test_flight_model.cpp) don't exercise in isolation:
//   - The 4-branch RPM state machine (lightup / MIL / AB / no-AB)
//   - Spool rate formula and floor
//   - Fuel flow (table-based and legacy factor)
//   - The multi-aircraft firstCall_ regression (T1.1 bug fix)
//
// These tests use SYNTHETIC configs (tiny 2x2 thrust tables) so they don't
// depend on the F-16 fixture. Each branch is exercised with a minimal input
// that targets exactly one code path.

#include "f4/flight/engine.hpp"
#include "f4/flight/constants.hpp"

#include <gtest/gtest.h>

#include <cmath>

namespace f4::flight {
namespace {

// ---------------------------------------------------------------------------
// Test fixture: a tiny synthetic EngineTable + AuxAero.
//
// The table is 2 altitudes × 2 Mach numbers:
//   alt_ft  = { 0.0, 40000.0 }
//   mach    = { 0.0, 1.0  }
//
// Thrust values are chosen so each branch is distinguishable:
//   idle[0,0]=1000  idle[0,1]=2000  idle[1,0]=500   idle[1,1]=800
//   mil[0,0]=10000  mil[0,1]=15000  mil[1,0]=5000   mil[1,1]=8000
//   ab[0,0]=20000   ab[0,1]=30000   ab[1,0]=10000   ab[1,1]=18000
//
// Fuel flow tables mirror the thrust tables (scaled by 0.1).
// ---------------------------------------------------------------------------
struct SyntheticEngine {
    data::EngineTable table;
    data::AuxAero     aux;

    SyntheticEngine() {
        table.alt_ft = { 0.0, 40000.0 };
        table.mach   = { 0.0, 1.0 };
        // Row-major flat: data[alt_idx * num_mach + mach_idx]
        table.thrust_idle = { 1000.0, 2000.0,  500.0,  800.0 };
        table.thrust_mil  = { 10000.0, 15000.0, 5000.0, 8000.0 };
        table.thrust_ab   = { 20000.0, 30000.0, 10000.0, 18000.0 };
        table.fuelflow_idle = { 100.0, 200.0,  50.0,  80.0 };
        table.fuelflow_mil  = { 1000.0, 1500.0, 500.0, 800.0 };
        table.fuelflow_ab   = { 2000.0, 3000.0, 1000.0, 1800.0 };
        table.thrustFactor = 1.0;
        table.fuelFlowFactor = 1.0;

        // AuxAero defaults are already reasonable; just set the engine type
        // and spool rates explicitly so the tests don't depend on defaults.
        aux.typeEngine = 2;        // PW-220 family
        aux.nEngines = 1;
        aux.normSpoolRate = 1.0;   // 1 rad/s — fast but not instant
        aux.lightupSpoolRate = 10.0;
        aux.flameoutSpoolRate = 5.0;
        aux.minFuelFlow = 0.0;     // disable floor for easier assertion
        aux.fuelFlowFactorNormal = 0.25;
        aux.fuelFlowFactorAb = 0.65;
    }
};

// Helper: build an EngineState seeded with a given RPM.
EngineState makeState(double rpm) {
    EngineState s;
    s.rpm = rpm;
    s.engLit = true;
    return s;
}

// Helper: a typical mass for the synthetic tests (10000 lbf aircraft).
constexpr double kMassSlugs = 10000.0 / GRAVITY;  // ≈ 310.8 slugs

}  // namespace

// ============================================================================
// Construction
// ============================================================================

TEST(EngineModel, DefaultConstructedHasNoTables) {
    EngineModel e;
    EngineState s = makeState(1.0);
    e.update(0.01, 0.0, 0.0, 100.0, kMassSlugs, 1.0, 1.0, false, s);
    EXPECT_DOUBLE_EQ(s.thrust, 0.0);
    EXPECT_DOUBLE_EQ(s.fuelFlow, 0.0);
}

TEST(EngineModel, ConstructedWithTablesProducesNonzeroThrust) {
    SyntheticEngine se;
    EngineModel e(&se.table, &se.aux);
    EngineState s = makeState(1.0);
    e.update(0.01, 0.0, 0.0, 100.0, kMassSlugs, 1.0, 1.0, false, s);
    EXPECT_GT(s.thrust, 0.0);
}

TEST(EngineModel, ZeroMassProducesZeroThrust) {
    SyntheticEngine se;
    EngineModel e(&se.table, &se.aux);
    EngineState s = makeState(1.0);
    e.update(0.01, 0.0, 0.0, 100.0, 0.0, 1.0, 1.0, false, s);
    EXPECT_DOUBLE_EQ(s.thrust, 0.0);
    EXPECT_DOUBLE_EQ(s.fuelFlow, 0.0);
}

// ============================================================================
// First-call sync — the T1.1 multi-aircraft bug
// ============================================================================

TEST(EngineModel, FirstCallSeedsRpmLagFromCurrentRpm) {
    // When the first update() is called with state.rpm = 0.85, the lag filter
    // should be seeded to 0.85. Without the seed, the filter starts at 0 and
    // produces a spurious spool-up transient.
    SyntheticEngine se;
    EngineModel e(&se.table, &se.aux);
    EngineState s = makeState(0.85);

    // First call: should seed the lag filter so rpmCmd doesn't get lagged
    // from 0.
    e.update(0.01, 0.0, 0.0, 100.0, kMassSlugs, 0.85, 1.0, false, s);

    // After one step, rpm should be close to 0.85 (not close to 0).
    EXPECT_GT(s.rpm, 0.7)
        << "rpm should be seeded near 0.85, not start from 0";
    EXPECT_TRUE(s.rpmLagSeeded)
        << "rpmLagSeeded flag should be set after the first call";
}

TEST(EngineModel, MultiAircraftFirstCallSeedsEachEngineIndependently) {
    // REGRESSION GATE for T1.1: the original code had `static bool firstCall`
    // which was shared process-wide. Aircraft #1's first call flipped the
    // flag; aircraft #2's first call saw the flag already false and skipped
    // the seed, producing a spurious transient.
    //
    // Even after the per-EngineModel fix, the seed flag must live on
    // EngineState (not EngineModel) so that reusing an EngineModel across
    // states, or resetting a state, also re-seeds correctly.
    SyntheticEngine se;
    EngineModel e(&se.table, &se.aux);

    // Two independent EngineStates, SAME EngineModel.
    EngineState s1 = makeState(0.90);
    EngineState s2 = makeState(0.90);

    e.update(0.01, 0.0, 0.0, 100.0, kMassSlugs, 0.90, 1.0, false, s1);
    EXPECT_GT(s1.rpm, 0.75) << "state #1 rpm should be seeded near 0.90";

    // The bug would cause s2.rpm to start near 0 (the unseeded filter).
    e.update(0.01, 0.0, 0.0, 100.0, kMassSlugs, 0.90, 1.0, false, s2);
    EXPECT_GT(s2.rpm, 0.75)
        << "state #2 rpm should ALSO be seeded near 0.90 — "
        << "if this fails, the seed flag is on EngineModel instead of EngineState";
}

TEST(EngineModel, ResettingStateReSeedsLagFilter) {
    // When the host resets the EngineState (retrim, scenario reset), the
    // rpmLagSeeded flag should also reset, so the next update() re-seeds
    // the lag filter from the new state.rpm.
    SyntheticEngine se;
    EngineModel e(&se.table, &se.aux);

    EngineState s = makeState(0.90);
    e.update(0.01, 0.0, 0.0, 100.0, kMassSlugs, 0.90, 1.0, false, s);
    ASSERT_TRUE(s.rpmLagSeeded);

    // Reset the state (as if for a retrim).
    s = EngineState{};
    s.rpm = 0.85;
    s.engLit = true;
    ASSERT_FALSE(s.rpmLagSeeded) << "freshly constructed state should be unseeded";

    e.update(0.01, 0.0, 0.0, 100.0, kMassSlugs, 0.85, 1.0, false, s);
    EXPECT_GT(s.rpm, 0.7) << "rpm should be re-seeded near 0.85 after reset";
    EXPECT_TRUE(s.rpmLagSeeded);
}

TEST(EngineModel, FirstCallWithZeroRpmDoesNotSeed) {
    // If state.rpm is 0 on the first call, the seed is skipped (no point
    // seeding the filter to 0 — it's already at 0). The flag still flips
    // so the next call with nonzero rpm doesn't trigger a seed either.
    SyntheticEngine se;
    EngineModel e(&se.table, &se.aux);
    EngineState s = makeState(0.0);

    e.update(0.01, 0.0, 0.0, 100.0, kMassSlugs, 0.0, 1.0, false, s);
    EXPECT_LT(s.rpm, 0.1);
    // Note: rpmLagSeeded stays false here because state.rpm was 0. The next
    // call with nonzero rpm WILL seed. This is intentional — a 0 rpm means
    // "engine off", and seeding to 0 is a no-op.
}

// ============================================================================
// RPM state machine — the 4 branches in update()
// ============================================================================

TEST(EngineModel, LightupBranchSpoolsUpToIdle) {
    // When state.rpm < 0.68 AND engLit is true, the lightup branch fires:
    // rpmCmd = 0.7, spoolrate = lightupSpoolRate (fast).
    SyntheticEngine se;
    se.aux.lightupSpoolRate = 10.0;  // very fast spool
    EngineModel e(&se.table, &se.aux);
    EngineState s = makeState(0.50);  // below 0.68 threshold

    // Throttle = 0 (no MIL command) — lightup branch ignores throttle.
    e.update(0.1, 0.0, 0.0, 100.0, kMassSlugs, 0.0, 1.0, false, s);

    // After 0.1s with spoolrate=10, rpm should have moved toward 0.7.
    EXPECT_GT(s.rpm, 0.50) << "rpm should increase toward 0.7 in lightup branch";
    EXPECT_LT(s.rpm, 0.71) << "rpm should not overshoot the 0.7 command";

    // Thrust in lightup branch is 0 (engine spooling, not producing thrust yet).
    EXPECT_DOUBLE_EQ(s.thrust, 0.0);
}

TEST(EngineModel, MilBranchInterpolatesIdleToMil) {
    // When 0.68 <= rpm <= 1.0, thrust interpolates between idle and MIL
    // based on throttle position.
    SyntheticEngine se;
    EngineModel e(&se.table, &se.aux);

    // Throttle = 0.0 (idle end of the MIL branch)
    {
        EngineState s = makeState(0.90);  // well above 0.68, below 1.0
        e.update(0.001, 0.0, 0.0, 100.0, kMassSlugs, 0.0, 1.0, false, s);
        // pwrlev=0, so thrust = idle[0,0] / mass = 1000 / 310.8 ≈ 3.22
        EXPECT_NEAR(s.thrust, 1000.0 / kMassSlugs, 0.5);
    }

    // Throttle = 1.0 (MIL end of the MIL branch)
    {
        EngineState s = makeState(0.90);
        e.update(0.001, 0.0, 0.0, 100.0, kMassSlugs, 1.0, 1.0, false, s);
        // pwrlev=1, so thrust = mil[0,0] / mass = 10000 / 310.8 ≈ 32.17
        EXPECT_NEAR(s.thrust, 10000.0 / kMassSlugs, 0.5);
    }

    // Throttle = 0.5 (midpoint)
    {
        EngineState s = makeState(0.90);
        e.update(0.001, 0.0, 0.0, 100.0, kMassSlugs, 0.5, 1.0, false, s);
        // pwrlev=0.5, thrust = (mil-idle)*0.5 + idle = (10000-1000)*0.5 + 1000 = 5500
        EXPECT_NEAR(s.thrust, 5500.0 / kMassSlugs, 0.5);
    }
}

TEST(EngineModel, AfterburnerBranchInterpolatesMilToAb) {
    // When rpm > 1.0 AND hasAB, thrust interpolates between MIL and AB.
    SyntheticEngine se;
    EngineModel e(&se.table, &se.aux);

    // Throttle = 1.5 (full AB), rpm already above 1.0
    EngineState s = makeState(1.05);
    e.update(0.001, 0.0, 0.0, 100.0, kMassSlugs, 1.5, 1.0, false, s);
    // pwrlev=1.5, thrtb1 = 2*(ab-mil)*(0.5) + mil = (ab-mil) + mil = ab
    // = 20000 / mass
    EXPECT_NEAR(s.thrust, 20000.0 / kMassSlugs, 0.5);
    EXPECT_TRUE(s.aburnLit) << "AB should be lit when pwrlev > 1.0 and rpm > 0.95";
}

TEST(EngineModel, NoAbBranchUsesMilThrust) {
    // When rpm > 1.0 but no AB is fitted, thrust = MIL (no interpolation).
    SyntheticEngine se;
    se.table.thrust_ab.clear();  // remove AB table → hasAB() returns false
    EngineModel e(&se.table, &se.aux);

    EngineState s = makeState(1.05);
    e.update(0.001, 0.0, 0.0, 100.0, kMassSlugs, 1.5, 1.0, false, s);
    EXPECT_NEAR(s.thrust, 10000.0 / kMassSlugs, 0.5);
    EXPECT_FALSE(s.aburnLit);
}

// ============================================================================
// Spool rate
// ============================================================================

TEST(EngineModel, SpoolRateFormulaScalesWithAltitude) {
    // The spoolrate variable is computed as:
    //   spoolrate = normSpoolRate + alt/25000 - mach/2
    // and passed to LagFilter::step(u, tau, dt) as the `tau` parameter.
    // Higher tau → slower filter response → rpm stays closer to its previous
    // value.
    //
    // NOTE: engine.cpp's comment says "increases with altitude (thinner air
    // = faster spool)", but the code passes spoolrate as tau (time constant),
    // so the actual effect is the opposite: higher altitude → larger tau →
    // SLOWER rpm change. This discrepancy between comment and code is a
    // pre-existing issue outside the scope of T1.4d. These tests assert the
    // ACTUAL behavior (not the commented behavior) as a regression gate.
    SyntheticEngine se;
    se.aux.normSpoolRate = 1.0;
    EngineModel e(&se.table, &se.aux);

    EngineState s_low = makeState(0.80);  // above 0.68 → MIL branch
    e.update(0.1, 0.0, 0.5, 100.0, kMassSlugs, 1.0, 1.0, false, s_low);

    EngineState s_high = makeState(0.80);
    e.update(0.1, 40000.0, 0.5, 100.0, kMassSlugs, 1.0, 1.0, false, s_high);

    // Higher altitude → larger tau → slower response → rpm farther from command.
    EXPECT_LT(s_high.rpm, s_low.rpm)
        << "actual behavior: higher altitude → slower rpm change (tau increases)";
}

TEST(EngineModel, SpoolRateFormulaScalesWithMach) {
    // spoolrate = normSpoolRate + alt/25000 - mach/2
    // Higher mach → lower tau → faster response → rpm closer to command.
    SyntheticEngine se;
    se.aux.normSpoolRate = 1.0;
    EngineModel e(&se.table, &se.aux);

    EngineState s_low_mach = makeState(0.80);
    e.update(0.1, 0.0, 0.3, 100.0, kMassSlugs, 1.0, 1.0, false, s_low_mach);

    EngineState s_high_mach = makeState(0.80);
    e.update(0.1, 0.0, 0.9, 100.0, kMassSlugs, 1.0, 1.0, false, s_high_mach);

    // Higher mach → smaller tau → faster response → rpm closer to command.
    EXPECT_GT(s_high_mach.rpm, s_low_mach.rpm)
        << "actual behavior: higher mach → faster rpm change (tau decreases)";
}

TEST(EngineModel, SpoolRateFlooredAt01) {
    // spoolrate = max(0.1, normSpoolRate + alt/25000 - mach/2)
    // With normSpoolRate=0.1, alt=0, mach=1.5: 0.1 + 0 - 0.75 = -0.65 → floored to 0.1
    // NOTE: starting rpm must be >= 0.68 to avoid the lightup branch.
    SyntheticEngine se;
    se.aux.normSpoolRate = 0.1;
    EngineModel e(&se.table, &se.aux);

    EngineState s = makeState(0.80);
    e.update(0.1, 0.0, 1.5, 100.0, kMassSlugs, 1.0, 1.0, false, s);
    EXPECT_GT(s.rpm, 0.80)
        << "rpm should still move toward command even with floored spoolrate";
}

// ============================================================================
// Fuel flow
// ============================================================================

TEST(EngineModel, FuelFlowUsesTablesWhenAvailable) {
    SyntheticEngine se;
    EngineModel e(&se.table, &se.aux);

    // Idle throttle → fuel flow should be near fuelflow_idle[0,0] = 100
    EngineState s = makeState(0.90);
    e.update(0.5, 0.0, 0.0, 100.0, kMassSlugs, 0.0, 1.0, false, s);
    // The fuel flow is smoothed (first-order, tau=0.1s), so after 0.5s it
    // should be close to the commanded value.
    EXPECT_NEAR(s.fuelFlow, 100.0, 30.0);
}

TEST(EngineModel, FuelFlowAbUsesAbTable) {
    SyntheticEngine se;
    EngineModel e(&se.table, &se.aux);

    // AB throttle, rpm > 1.0 → fuel flow interpolates mil→ab.
    // The fuel flow is smoothed (tau=0.1s), so we need enough time for it
    // to converge. With dt=2.0, alpha = 2.0/2.1 ≈ 0.952, so fuelFlow
    // reaches ~95% of the commanded value.
    EngineState s = makeState(1.05);
    e.update(2.0, 0.0, 0.0, 100.0, kMassSlugs, 1.5, 1.0, false, s);
    // pwrlev=1.5 → ff = 2*(ffAb-ffMil)*0.5 + ffMil = ffAb = 2000
    // After smoothing: 2000 * 0.952 ≈ 1905
    EXPECT_NEAR(s.fuelFlow, 2000.0, 150.0);
}

TEST(EngineModel, SimplifiedFlagReducesFuelFlowBy25Percent) {
    // simplified=true scales fuel flow by 0.75 (for AI aircraft).
    SyntheticEngine se;
    EngineModel e(&se.table, &se.aux);

    EngineState s_normal = makeState(0.90);
    e.update(0.5, 0.0, 0.0, 100.0, kMassSlugs, 0.0, 1.0, false, s_normal);

    EngineState s_simplified = makeState(0.90);
    e.update(0.5, 0.0, 0.0, 100.0, kMassSlugs, 0.0, 1.0, true, s_simplified);

    EXPECT_NEAR(s_simplified.fuelFlow, s_normal.fuelFlow * 0.75, 5.0);
}

TEST(EngineModel, FuelFlowFlooredAtMinFuelFlow) {
    SyntheticEngine se;
    se.aux.minFuelFlow = 500.0;  // high floor
    EngineModel e(&se.table, &se.aux);

    EngineState s = makeState(0.90);
    e.update(0.5, 0.0, 0.0, 100.0, kMassSlugs, 0.0, 1.0, false, s);
    EXPECT_GE(s.fuelFlow, 500.0);
}

// ============================================================================
// Flameout
// ============================================================================

TEST(EngineModel, FlameoutSpoolsDownToZero) {
    SyntheticEngine se;
    se.aux.flameoutSpoolRate = 5.0;
    EngineModel e(&se.table, &se.aux);

    EngineState s = makeState(0.95);
    s.engLit = false;  // engine flamed out

    e.update(1.0, 0.0, 0.0, 100.0, kMassSlugs, 1.0, 1.0, false, s);

    EXPECT_LT(s.rpm, 0.95) << "rpm should spool down after flameout";
    EXPECT_FALSE(s.aburnLit) << "AB should be off after flameout";
}

// ============================================================================
// FTIT (turbine temperature)
// ============================================================================

TEST(EngineModel, FtitScalesWithRpm) {
    // FTIT should be higher at high RPM than at low RPM.
    SyntheticEngine se;
    EngineModel e(&se.table, &se.aux);

    EngineState s_low = makeState(0.50);
    e.update(0.5, 0.0, 0.0, 100.0, kMassSlugs, 0.0, 1.0, false, s_low);

    EngineState s_high = makeState(1.00);
    e.update(0.5, 0.0, 0.0, 100.0, kMassSlugs, 1.0, 1.0, false, s_high);

    EXPECT_GT(s_high.ftit, s_low.ftit)
        << "FTIT should be higher at MIL than at idle";
}

TEST(EngineModel, FtitClampedToZeroToTen) {
    SyntheticEngine se;
    EngineModel e(&se.table, &se.aux);

    // Extreme RPM values shouldn't push FTIT outside [0, 10].
    EngineState s = makeState(2.0);  // unrealistic high RPM
    e.update(0.5, 0.0, 0.0, 100.0, kMassSlugs, 1.5, 1.0, false, s);
    EXPECT_GE(s.ftit, 0.0);
    EXPECT_LE(s.ftit, 10.0);
}

// ============================================================================
// Multi-engine thrust multiplication
// ============================================================================

TEST(EngineModel, MultiEngineMultipliesThrust) {
    // aux_->nEngines scales thrust multiplicatively. A 2-engine aircraft
    // should produce 2x the thrust of a 1-engine aircraft (all else equal).
    SyntheticEngine se1, se2;
    se1.aux.nEngines = 1;
    se2.aux.nEngines = 2;
    EngineModel e1(&se1.table, &se1.aux);
    EngineModel e2(&se2.table, &se2.aux);

    EngineState s1 = makeState(0.90);
    EngineState s2 = makeState(0.90);
    e1.update(0.001, 0.0, 0.0, 100.0, kMassSlugs, 1.0, 1.0, false, s1);
    e2.update(0.001, 0.0, 0.0, 100.0, kMassSlugs, 1.0, 1.0, false, s2);

    EXPECT_NEAR(s2.thrust, s1.thrust * 2.0, 0.5);
}

// ============================================================================
// bodyForces (static helper)
// ============================================================================

TEST(EngineModelBodyForces, NonVectoredThrustIsAlongX) {
    double xprop, yprop, zprop, xsprop, zsprop;
    EngineModel::bodyForces(/*thrust_accel=*/100.0,
                            /*sinAlpha=*/0.0, /*cosAlpha=*/1.0,
                            /*nozzlePos=*/0.0,
                            xprop, yprop, zprop, xsprop, zsprop);
    EXPECT_DOUBLE_EQ(xprop, 100.0);
    EXPECT_DOUBLE_EQ(yprop, 0.0);
    EXPECT_DOUBLE_EQ(zprop, 0.0);
    EXPECT_DOUBLE_EQ(xsprop, 100.0);  // x * cosAlpha
    EXPECT_DOUBLE_EQ(zsprop, 0.0);    // -x * sinAlpha + z * cosAlpha
}

TEST(EngineModelBodyForces, VectoredThrustRedirectsToZ) {
    // Nozzle at 90 degrees → all thrust along body -Z (upward).
    double xprop, yprop, zprop, xsprop, zsprop;
    EngineModel::bodyForces(/*thrust_accel=*/100.0,
                            /*sinAlpha=*/0.0, /*cosAlpha=*/1.0,
                            /*nozzlePos=*/90.0,
                            xprop, yprop, zprop, xsprop, zsprop);
    EXPECT_NEAR(xprop, 0.0, 1e-9);
    EXPECT_NEAR(zprop, -100.0, 1e-9);  // negative = upward
}

TEST(EngineModelBodyForces, AlphaRotatesStabilityAxisComponents) {
    // At alpha=30°, the stability-axis X component should be cos(30°) of the
    // body-axis X component.
    constexpr double alpha_rad = 30.0 * 0.017453292519943295;
    const double sa = std::sin(alpha_rad);
    const double ca = std::cos(alpha_rad);
    double xprop, yprop, zprop, xsprop, zsprop;
    EngineModel::bodyForces(100.0, sa, ca, 0.0,
                            xprop, yprop, zprop, xsprop, zsprop);
    EXPECT_NEAR(xsprop, 100.0 * ca, 1e-9);
    EXPECT_NEAR(zsprop, -100.0 * sa, 1e-9);
}

}  // namespace f4::flight
