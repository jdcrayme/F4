// f4-flight-model/tests/test_eom.cpp
//
// Unit tests for the Equations of Motion (EOM) integrator.
//
// The EOM integrates the 6-DOF rigid-body state forward in time from the
// aerodynamic and thrust forces. These tests focus on:
//   - Position integration (world frame)
//   - Velocity integration (with gravity)
//   - Quaternion orientation integration
//   - Body rate computation (from commanded G and roll rate)
//   - Ground clamp behavior
//   - Trigonometry cache consistency
//
// The EOM is the hardest subsystem to unit-test in isolation because it
// reads from many state sub-structs (kin, aero, fcs, gear, loads). These
// tests set up minimal but internally-consistent state and verify that
// integration produces physically plausible results.

#include "f4/flight/eom.hpp"
#include "f4/flight/constants.hpp"

#include <gtest/gtest.h>

#include <cmath>

namespace f4::flight {
namespace {

data::AircraftGeometry makeGeom() {
    data::AircraftGeometry g;
    g.emptyWeight_lbs = 10000.0;
    g.area_ft2 = 300.0;
    g.span_ft = 30.0;
    g.aoaMax_deg = 25.0;
    g.maxGs = 9.0;
    g.thetaMax_rad = 1.4;
    return g;
}

data::AuxAero makeAux() {
    data::AuxAero a;
    a.pitchMomentum = 1.0;
    a.pitchElasticity = 1.0;
    a.rollGearGain = 0.6;
    a.yawGearGain = 0.6;
    return a;
}

// Set up a level-flight state: 500 ft/s northbound at 1000 ft AGL.
AircraftState makeLevelFlight() {
    AircraftState s;
    s.kin.x = 0.0; s.kin.y = 0.0; s.kin.z = -1000.0;  // 1000 ft AGL
    s.kin.xdot = 500.0; s.kin.ydot = 0.0; s.kin.zdot = 0.0;
    s.kin.vt = 500.0;
    s.kin.quat = math::Quatd::identity();  // wings level, nose north
    s.kin.psi = 0.0; s.kin.theta = 0.0; s.kin.phi = 0.0;
    s.kin.sigma = 0.0; s.kin.gmma = 0.0; s.kin.mu = 0.0;
    s.kin.cosalp = 1.0; s.kin.sinalp = 0.0;
    s.kin.cosbet = 1.0; s.kin.sinbet = 0.0;
    s.kin.cosgam = 1.0; s.kin.singam = 0.0;
    s.kin.cossig = 1.0; s.kin.sinsig = 0.0;
    s.kin.cosmu  = 1.0; s.kin.sinmu  = 0.0;
    s.kin.costhe = 1.0; s.kin.sinthe = 0.0;
    s.kin.cosphi = 1.0; s.kin.sinphi = 0.0;
    s.kin.cospsi = 1.0; s.kin.sinpsi = 0.0;

    s.aero.xaero = 0.0;  // no axial accel
    s.aero.zaero = GRAVITY;  // gravity pulls down (z+ = down in NED)
    s.aero.xwaero = 0.0;
    s.aero.gearPos = 0.0;  // gear up

    s.gear.inAir = true;
    s.gear.minHeight_ft = 0.0;
    s.gear.groundZ_ft = 0.0;

    s.fcs.pscmd = 0.0;
    s.fcs.pstab = 0.0;
    s.fcs.kp01 = 1.0; s.fcs.kp02 = 1.0; s.fcs.kp03 = 2.0; s.fcs.kp05 = 1.0;
    s.fcs.tp01 = 0.2; s.fcs.tp02 = 0.2; s.fcs.tp03 = 0.2;
    s.fcs.zp01 = 0.9;

    s.loads.nzcgs = 1.0;
    s.loads.nycgw = 0.0;

    s.fuel.mass_slugs = 10000.0 / GRAVITY;

    return s;
}

}  // namespace

// ============================================================================
// Position integration — level flight moves north
// ============================================================================

TEST(EomPosition, LevelFlightMovesNorth) {
    data::AircraftGeometry g = makeGeom();
    data::AuxAero a = makeAux();
    EquationsOfMotion eom(&g, &a);

    AircraftState s = makeLevelFlight();
    PilotInput input{};

    const double x0 = s.kin.x;
    eom.update(0.1, input, s);
    // After 0.1s at 500 ft/s, x should increase by ~50 ft (minus drag/gravity effects).
    EXPECT_GT(s.kin.x, x0 + 40.0) << "aircraft should move north";
    EXPECT_LT(s.kin.x, x0 + 60.0);
}

TEST(EomPosition, GroundOperationKeepsZNearGround) {
    // On the ground, the EOM should keep z near the ground clamp target
    // (groundZ - minHeight). This test spawns the aircraft on the ground
    // and verifies z stays in a reasonable range after one step.
    data::AircraftGeometry g = makeGeom();
    data::AuxAero a = makeAux();
    EquationsOfMotion eom(&g, &a);

    AircraftState s = makeLevelFlight();
    s.gear.inAir = false;
    s.gear.groundZ_ft = 0.0;
    s.gear.minHeight_ft = 3.0;
    s.kin.z = -3.0;  // at the clamp target
    s.kin.vt = 10.0;  // slow taxi

    PilotInput input{};
    eom.update(0.01, input, s);
    // z should stay near -3 (the clamp target). Allow some drift from the
    // integration step.
    EXPECT_NEAR(s.kin.z, -3.0, 1.0);
}

// ============================================================================
// Velocity integration — gravity accelerates downward
// ============================================================================

// ============================================================================
// Velocity integration
//
// NOTE: The EOM derives zdot from vt and singam (zdot = -vt * singam), not
// from direct z-force integration. Gravity acts through singam via calculateVt's
// vtDot = xwaero - g*singam formula. Testing gravity in isolation requires
// setting up singam (the flight-path angle) consistently with the velocity
// vector, which is really an integration-test concern. The integration tests
// in test_flight_model.cpp cover this end-to-end.
// ============================================================================

TEST(EomVelocity, LevelFlightMaintainsAltitude) {
    // In level flight (singam=0), zdot = -vt * 0 = 0. The aircraft should
    // not gain or lose altitude.
    data::AircraftGeometry g = makeGeom();
    data::AuxAero a = makeAux();
    EquationsOfMotion eom(&g, &a);

    AircraftState s = makeLevelFlight();
    PilotInput input{};

    const double z0 = s.kin.z;
    eom.update(0.1, input, s);
    // z should stay near -1000 (no significant altitude change in level flight).
    EXPECT_NEAR(s.kin.z, z0, 5.0);
}

// ============================================================================
// Quaternion orientation integration
// ============================================================================

// ============================================================================
// Quaternion orientation integration
//
// NOTE: calcBodyRates() overwrites p/q/r from the commanded G and roll rate.
// Setting body rates directly in the state does NOT survive the EOM update —
// the rates are recomputed from nzcgs, pstab, and the flight condition.
// These tests verify that the quaternion integration preserves identity
// when the commanded rates are zero (level flight, zero G command).
// ============================================================================

TEST(EomQuaternion, ZeroGCommandPreservesIdentityQuaternion) {
    // With nzcgs=1 (level flight, 1g) and zero roll command, the body rates
    // should be near zero and the quaternion should stay near identity.
    data::AircraftGeometry g = makeGeom();
    data::AuxAero a = makeAux();
    EquationsOfMotion eom(&g, &a);

    AircraftState s = makeLevelFlight();
    PilotInput input{};

    eom.update(0.1, input, s);
    // Identity quaternion = {1, 0, 0, 0}. In level flight with no command,
    // the quaternion should stay near identity.
    EXPECT_NEAR(s.kin.quat.w, 1.0, 0.01);
    EXPECT_NEAR(std::fabs(s.kin.quat.x), 0.0, 0.01);
    EXPECT_NEAR(std::fabs(s.kin.quat.y), 0.0, 0.01);
    EXPECT_NEAR(std::fabs(s.kin.quat.z), 0.0, 0.01);
}

// ============================================================================
// Trigonometry cache — consistency between euler angles and sin/cos cache
// ============================================================================

TEST(EomTrigonometry, ZeroStateHasUnityCosines) {
    data::AircraftGeometry g = makeGeom();
    data::AuxAero a = makeAux();
    EquationsOfMotion eom(&g, &a);

    AircraftState s = makeLevelFlight();
    PilotInput input{};
    eom.update(0.01, input, s);

    // At zero attitude, all cosines should be ~1.0
    EXPECT_NEAR(s.kin.cosphi, 1.0, 1e-3);
    EXPECT_NEAR(s.kin.costhe, 1.0, 1e-3);
    EXPECT_NEAR(s.kin.cospsi, 1.0, 1e-3);
}

TEST(EomTrigonometry, SinCosConsistentWithEulerAngles) {
    // After an EOM step, the sin/cos cache should be consistent with the
    // euler angles (sin(x) == sin_cache, cos(x) == cos_cache).
    data::AircraftGeometry g = makeGeom();
    data::AuxAero a = makeAux();
    EquationsOfMotion eom(&g, &a);

    AircraftState s = makeLevelFlight();
    PilotInput input{};
    eom.update(0.01, input, s);

    EXPECT_NEAR(s.kin.sinthe, std::sin(s.kin.theta), 1e-3);
    EXPECT_NEAR(s.kin.costhe, std::cos(s.kin.theta), 1e-3);
    EXPECT_NEAR(s.kin.sinphi, std::sin(s.kin.phi), 1e-3);
    EXPECT_NEAR(s.kin.cosphi, std::cos(s.kin.phi), 1e-3);
}

// ============================================================================
// Ground operation — nose-wheel steering
// ============================================================================

TEST(EomGround, NoseWheelSteeringTurnsHeading) {
    // On ground with ypedal input, psi (heading) should change.
    data::AircraftGeometry g = makeGeom();
    data::AuxAero a = makeAux();
    EquationsOfMotion eom(&g, &a);

    AircraftState s = makeLevelFlight();
    s.gear.inAir = false;
    s.gear.groundZ_ft = 0.0;
    s.gear.minHeight_ft = 3.0;
    s.kin.z = -3.0;  // on ground
    s.kin.vt = 30.0;  // taxi speed (< 50 ft/s → steer rate = 30 deg/s)

    PilotInput input{};
    input.ypedal = 1.0;  // full right pedal

    const double psi0 = s.kin.psi;
    eom.update(0.1, input, s);
    // After 0.1s at 30 deg/s with full right pedal: psi should decrease
    // (positive ypedal = right turn = psi decreases in NED CCW frame).
    EXPECT_LT(s.kin.psi, psi0)
        << "right pedal on ground should turn heading right (psi decreases)";
}

TEST(EomGround, GroundClampsRollToZero) {
    // On ground, roll (phi) should be clamped to 0 (wings level).
    data::AircraftGeometry g = makeGeom();
    data::AuxAero a = makeAux();
    EquationsOfMotion eom(&g, &a);

    AircraftState s = makeLevelFlight();
    s.gear.inAir = false;
    s.gear.groundZ_ft = 0.0;
    s.gear.minHeight_ft = 3.0;
    s.kin.z = -3.0;
    s.kin.phi = 0.5;  // start with some roll
    s.kin.p = 0.0;

    PilotInput input{};
    eom.update(0.01, input, s);
    EXPECT_NEAR(s.kin.phi, 0.0, 1e-6) << "ground should clamp roll to 0";
}

TEST(EomGround, GroundClampsPitchToRange) {
    // On ground, pitch is clamped to [-2°, 15°].
    data::AircraftGeometry g = makeGeom();
    data::AuxAero a = makeAux();
    EquationsOfMotion eom(&g, &a);

    AircraftState s = makeLevelFlight();
    s.gear.inAir = false;
    s.gear.groundZ_ft = 0.0;
    s.gear.minHeight_ft = 3.0;
    s.kin.z = -3.0;
    s.kin.theta = 0.5;  // 0.5 rad ≈ 28.6°, above the 15° clamp

    PilotInput input{};
    eom.update(0.01, input, s);
    EXPECT_LE(s.kin.theta, 15.0 * DTR + 1e-6) << "ground should clamp pitch to 15°";
}

// ============================================================================
// Body rate computation
// ============================================================================

TEST(EomBodyRates, PositivePitchCommandProducesPositiveQ) {
    // A positive G command (nzcgs > 1) should produce a positive pitch rate q.
    data::AircraftGeometry g = makeGeom();
    data::AuxAero a = makeAux();
    EquationsOfMotion eom(&g, &a);

    AircraftState s = makeLevelFlight();
    s.loads.nzcgs = 3.0;  // command 3g
    s.fcs.pstab = 0.0;
    s.fcs.kp01 = 1.0; s.fcs.kp02 = 1.0; s.fcs.kp03 = 2.0;
    s.fcs.tp01 = 0.2; s.fcs.tp02 = 0.2; s.fcs.tp03 = 0.2;
    s.fcs.zp01 = 0.9;
    s.aero.cnalpha = 0.5;  // normal-force slope

    PilotInput input{};
    eom.update(0.01, input, s);
    EXPECT_GT(s.kin.q, 0.0) << "positive G command should produce positive pitch rate";
}

}  // namespace f4::flight
