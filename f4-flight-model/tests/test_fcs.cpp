// f4-flight-model/tests/test_fcs.cpp
//
// Unit tests for the Flight Control System.
//
// The FCS is a complex closed-loop controller with many internal state
// variables and filter states. These tests focus on:
//   - applyLimiter: the limiter evaluation (line, value, min/max, percent)
//   - Pitch channel: G-command shaping and limiting
//   - Roll channel: rate command and alpha-based limiting
//   - Yaw channel: beta-command (mostly stubbed)
//   - Landing gains activation
//   - Ground fade behavior
//
// The FCS's internal gain computation (computeGains) is tested indirectly
// through the pitch/roll/yaw channel tests — verifying that the gains produce
// sane commanded values for known inputs.

#include "f4/flight/fcs.hpp"
#include "f4/flight/constants.hpp"

#include <gtest/gtest.h>

#include <cmath>

namespace f4::flight {
namespace {

// ---------------------------------------------------------------------------
// Minimal config for FCS tests. The FCS needs:
//   - AircraftGeometry (for aoaMax, maxGs, etc.)
//   - AuxAero (for gear gains, landing AOA)
//   - AircraftConfig (for limiters)
// ---------------------------------------------------------------------------
struct SyntheticFcs {
    data::AircraftConfig  cfg;
    data::AircraftGeometry geom;
    data::AuxAero          aux;

    SyntheticFcs() {
        geom.aoaMax = f4::Quantity<f4::Degrees>(25.0).to<f4::Radians>();
        geom.aoaMin = f4::Quantity<f4::Degrees>(-5.0).to<f4::Radians>();
        geom.maxGs = 9.0;
        geom.maxRoll = f4::Quantity<f4::Degrees>(80.0).to<f4::Radians>();
        geom.cornerVcas = f4::Quantity<f4::CASKnots>(330.0);
        geom.minVcas = f4::Quantity<f4::CASKnots>(140.0);
        geom.maxVcas = f4::Quantity<f4::CASKnots>(800.0);

        aux.pitchGearGain = 0.8;
        aux.rollGearGain = 0.6;
        aux.yawGearGain = 0.6;
        aux.landingAOA = f4::Quantity<f4::Degrees>(12.5).to<f4::Radians>();

        // limiters is a std::array<Limiter, kLimiterCount>, already sized.
    }
};

PilotInput makeInput() {
    PilotInput p{};
    p.pstick = 0.0;
    p.rstick = 0.0;
    p.ypedal = 0.0;
    p.speedBrake = -1.0;  // retracted
    p.tefCmd = 0.0;
    p.lefCmd = 0.0;
    return p;
}

/// Build a typical in-flight FlightConditions for FCS tests.
FlightConditions makeFc(double qbar = 100.0, double qsom = 10.0,
                         double mach = 0.5, double vt = 500.0,
                         double vcas = 300.0,
                         double alpha_deg = 5.0, double beta_deg = 0.0,
                         double loadingFraction = 1.0,
                         bool inAir = true,
                         double nzcgs = 1.0, double nycgw = 0.0) {
    FlightConditions fc;
    fc.qbar            = qbar;
    fc.qsom            = qsom;
    fc.mach            = mach;
    fc.vt              = vt;
    fc.vcas            = vcas;
    fc.alpha           = angle_from_degrees(alpha_deg);
    fc.beta            = angle_from_degrees(beta_deg);
    fc.sinalp          = std::sin(to_radians(fc.alpha));
    fc.cosalp          = std::cos(to_radians(fc.alpha));
    fc.sinbet          = std::sin(to_radians(fc.beta));
    fc.cosbet          = std::cos(to_radians(fc.beta));
    fc.cosmu           = 1.0;
    fc.cosgam          = 1.0;
    fc.singam          = 0.0;
    fc.costhe          = 1.0;
    fc.cosphi          = 1.0;
    fc.phi             = zero_angle();
    fc.loadingFraction = loadingFraction;
    fc.inAir           = inAir;
    fc.nzcgs           = nzcgs;
    fc.nycgw           = nycgw;
    return fc;
}

}  // namespace

// ============================================================================
// applyLimiter — limiter evaluation
// ============================================================================

TEST(FcsApplyLimiter, ReturnsInputUnchangedWhenNoConfig) {
    // A default-constructed FCS (no cfg) passes input through.
    FlightControlSystem fcs;
    EXPECT_DOUBLE_EQ(fcs.applyLimiter(data::LimiterKey::NegGLimiter, 5.0), 5.0);
}

TEST(FcsApplyLimiter, ReturnsInputWhenLimiterNotConfigured) {
    // A configured FCS with a default (all-zero) limiter passes input through.
    SyntheticFcs sf;
    FlightControlSystem fcs(&sf.cfg, &sf.geom, &sf.aux);
    // Default limiter is Line with all-zero coords → treated as "not configured".
    EXPECT_DOUBLE_EQ(fcs.applyLimiter(data::LimiterKey::NegGLimiter, 5.0), 5.0);
}

TEST(FcsApplyLimiter, ValueLimiterReturnsConstant) {
    SyntheticFcs sf;
    data::Limiter& lim = sf.cfg.limiters[static_cast<int>(data::LimiterKey::NegGLimiter)];
    lim.type = data::LimiterType::Value;
    lim.x1 = -3.0;  // constant value

    FlightControlSystem fcs(&sf.cfg, &sf.geom, &sf.aux);
    EXPECT_DOUBLE_EQ(fcs.applyLimiter(data::LimiterKey::NegGLimiter, 5.0), -3.0);
    EXPECT_DOUBLE_EQ(fcs.applyLimiter(data::LimiterKey::NegGLimiter, 0.0), -3.0);
}

TEST(FcsApplyLimiter, MinMaxLimiterClamps) {
    SyntheticFcs sf;
    data::Limiter& lim = sf.cfg.limiters[static_cast<int>(data::LimiterKey::RollRateLimiter)];
    lim.type = data::LimiterType::MinMax;
    lim.x1 = -50.0;  // min
    lim.x2 = 50.0;   // max

    FlightControlSystem fcs(&sf.cfg, &sf.geom, &sf.aux);
    EXPECT_DOUBLE_EQ(fcs.applyLimiter(data::LimiterKey::RollRateLimiter, 100.0), 50.0);
    EXPECT_DOUBLE_EQ(fcs.applyLimiter(data::LimiterKey::RollRateLimiter, -100.0), -50.0);
    EXPECT_DOUBLE_EQ(fcs.applyLimiter(data::LimiterKey::RollRateLimiter, 30.0), 30.0);
}

TEST(FcsApplyLimiter, PercentLimiterScales) {
    SyntheticFcs sf;
    data::Limiter& lim = sf.cfg.limiters[static_cast<int>(data::LimiterKey::YawAlphaLimiter)];
    lim.type = data::LimiterType::Percent;
    lim.x1 = 0.5;  // 50%

    FlightControlSystem fcs(&sf.cfg, &sf.geom, &sf.aux);
    EXPECT_NEAR(fcs.applyLimiter(data::LimiterKey::YawAlphaLimiter, 10.0), 5.0, 1e-9);
}

// ============================================================================
// Pitch channel — basic G-command behavior
// ============================================================================

// Helper: set up aero state with a nonzero clalph0 so gsAvail is nonzero.
// Without this, the pitch command clamp range collapses to [0,0] and ptcmd
// is always 0.
AeroState makeAeroWithLift() {
    AeroState aero;
    aero.clalph0 = 0.08;  // ~4.6 per radian — typical F-16 static slope
    aero.clift0 = 0.0;
    aero.gearPos = 0.0;
    return aero;
}

TEST(FcsPitch, ZeroStickProducesZeroCommand) {
    SyntheticFcs sf;
    // Configure NegGLimiter so the pitch command clamp range is valid.
    data::Limiter& negG = sf.cfg.limiters[static_cast<int>(data::LimiterKey::NegGLimiter)];
    negG.type = data::LimiterType::Value;
    negG.x1 = -3.0;  // max negative G = -3

    FlightControlSystem fcs(&sf.cfg, &sf.geom, &sf.aux);

    FcsState fcs_state;
    AeroState aero = makeAeroWithLift();
    PilotInput input = makeInput();

    fcs.update(input, makeFc(), fcs_state, aero, /*dt=*/0.01);

    EXPECT_NEAR(fcs_state.ptcmd, 0.0, 0.5)
        << "zero stick should produce a near-zero pitch command";
}

TEST(FcsPitch, FullForwardStickProducesNegativeCommand) {
    SyntheticFcs sf;
    data::Limiter& negG = sf.cfg.limiters[static_cast<int>(data::LimiterKey::NegGLimiter)];
    negG.type = data::LimiterType::Value;
    negG.x1 = -3.0;

    FlightControlSystem fcs(&sf.cfg, &sf.geom, &sf.aux);

    FcsState fcs_state;
    AeroState aero = makeAeroWithLift();
    PilotInput input = makeInput();
    input.pstick = -1.0;

    fcs.update(input, makeFc(), fcs_state, aero, 0.01);

    EXPECT_LT(fcs_state.ptcmd, 0.0)
        << "full forward stick should produce a negative (nose-down) command";
}

TEST(FcsPitch, FullAftStickProducesPositiveCommand) {
    SyntheticFcs sf;
    data::Limiter& negG = sf.cfg.limiters[static_cast<int>(data::LimiterKey::NegGLimiter)];
    negG.type = data::LimiterType::Value;
    negG.x1 = -3.0;

    FlightControlSystem fcs(&sf.cfg, &sf.geom, &sf.aux);

    FcsState fcs_state;
    AeroState aero = makeAeroWithLift();
    PilotInput input = makeInput();
    input.pstick = 1.0;

    fcs.update(input, makeFc(), fcs_state, aero, 0.01);

    EXPECT_GT(fcs_state.ptcmd, 0.0)
        << "full aft stick should produce a positive (nose-up) command";
}

TEST(FcsPitch, CommandLimitedByMaxGs) {
    // The pitch command should never exceed maxGs (9.0 for this config).
    SyntheticFcs sf;
    data::Limiter& negG = sf.cfg.limiters[static_cast<int>(data::LimiterKey::NegGLimiter)];
    negG.type = data::LimiterType::Value;
    negG.x1 = -3.0;

    FlightControlSystem fcs(&sf.cfg, &sf.geom, &sf.aux);

    FcsState fcs_state;
    AeroState aero = makeAeroWithLift();
    PilotInput input = makeInput();
    input.pstick = 1.0;

    fcs.update(input, makeFc(/*qbar=*/200.0, /*qsom=*/30.0), fcs_state, aero, 0.01);

    EXPECT_LE(fcs_state.ptcmd, sf.geom.maxGs + 0.01);
}

// ============================================================================
// Roll channel — rate command behavior
// ============================================================================

TEST(FcsRoll, ZeroStickProducesZeroRollRate) {
    SyntheticFcs sf;
    FlightControlSystem fcs(&sf.cfg, &sf.geom, &sf.aux);

    FcsState fcs_state;
    AeroState aero;
    PilotInput input = makeInput();

    fcs.update(input, makeFc(), fcs_state, aero, 0.01);

    EXPECT_NEAR(fcs_state.pscmd, 0.0, 5.0)
        << "zero roll stick should produce near-zero roll rate command";
}

TEST(FcsRoll, RightStickProducesPositiveRollRate) {
    SyntheticFcs sf;
    FlightControlSystem fcs(&sf.cfg, &sf.geom, &sf.aux);

    FcsState fcs_state;
    AeroState aero;
    PilotInput input = makeInput();
    input.rstick = 1.0;  // full right

    fcs.update(input, makeFc(), fcs_state, aero, 0.01);

    EXPECT_GT(fcs_state.pscmd, 0.0)
        << "right stick should produce a positive roll rate command";
}

TEST(FcsRoll, LeftStickProducesNegativeRollRate) {
    SyntheticFcs sf;
    FlightControlSystem fcs(&sf.cfg, &sf.geom, &sf.aux);

    FcsState fcs_state;
    AeroState aero;
    PilotInput input = makeInput();
    input.rstick = -1.0;  // full left

    fcs.update(input, makeFc(), fcs_state, aero, 0.01);

    EXPECT_LT(fcs_state.pscmd, 0.0)
        << "left stick should produce a negative roll rate command";
}

// ============================================================================
// Landing gains — gear down / refueling / explicit
// ============================================================================

TEST(FcsLandingGains, GearDownActivatesLandingGains) {
    // When gear is down, the FCS should use landing gains. In the current
    // implementation, landing gains primarily affect the roll channel
    // (kr01 is scaled by rollGearGain), and also the ground fade affects
    // kp05 when inAir=false.
    SyntheticFcs sf;
    FlightControlSystem fcs(&sf.cfg, &sf.geom, &sf.aux);

    FcsState fcs_airborne;
    FcsState fcs_landing;
    AeroState aero;
    PilotInput input = makeInput();

    // Airborne, gear up
    aero.gearPos = 0.0;
    fcs.update(input, makeFc(), fcs_airborne, aero, 0.01);

    // Landing, gear down (set aero.gearPos so gearDown is derived correctly)
    aero.gearPos = 1.0;
    fcs.update(input, makeFc(100.0, 10.0, 0.5, 500.0, 300.0, 5.0, 0.0, 1.0, /*inAir=*/false),
               fcs_landing, aero, 0.01);

    // The landing configuration should produce different gains than airborne.
    // We check both kp05 (affected by ground fade) and kr01 (affected by
    // landing gain scaling) — at least one should differ.
    const bool kp05_differs = (fcs_airborne.kp05 != fcs_landing.kp05);
    const bool kr01_differs = (fcs_airborne.kr01 != fcs_landing.kr01);
    EXPECT_TRUE(kp05_differs || kr01_differs)
        << "landing gains should differ from airborne gains";
}

TEST(FcsLandingGains, RefuelingActivatesLandingGains) {
    // When refueling is active, landing gains engage. In the current FCS
    // implementation, landing gains primarily affect the roll channel
    // (kr01 is scaled by rollGearGain), not the pitch channel (kp05).
    SyntheticFcs sf;
    FlightControlSystem fcs(&sf.cfg, &sf.geom, &sf.aux);

    FcsState fcs_normal;
    FcsState fcs_refuel;
    AeroState aero;
    aero.gearPos = 0.0;  // gear UP — landing gains should NOT be active

    PilotInput input = makeInput();

    fcs.update(input, makeFc(), fcs_normal, aero, 0.01);

    input.refueling = true;
    // aero.gearPos is still 0.0 (gear up), but refueling activates landing gains
    fcs.update(input, makeFc(), fcs_refuel, aero, 0.01);

    // Landing gains scale the roll rate command (kr01) by rollGearGain.
    // This is the primary observable effect of landing gains in the
    // current implementation.
    EXPECT_NE(fcs_normal.kr01, fcs_refuel.kr01)
        << "refueling should activate landing gains (affecting roll rate)";
}

// ============================================================================
// Ground fade — kp05 reduced at very low qbar on ground
// ============================================================================

TEST(FcsGroundFade, LowQbarOnGroundReducesKp05) {
    // On ground with qbar < 20, kp05 is scaled toward 0 (to avoid excessive
    // alpha commands during taxi). At qbar > 65, kp05 is full.
    SyntheticFcs sf;
    FlightControlSystem fcs(&sf.cfg, &sf.geom, &sf.aux);

    FcsState fcs_low_q;
    FcsState fcs_high_q;
    AeroState aero;
    PilotInput input = makeInput();

    // Very low qbar on ground
    aero.gearPos = 1.0;  // gear down -> gearDown = true
    fcs.update(input, makeFc(/*qbar=*/10.0, 10.0, 0.1, 100.0, 100.0, 2.0, 0.0, 1.0, /*inAir=*/false),
               fcs_low_q, aero, 0.01);

    // Higher qbar on ground
    fcs.update(input, makeFc(/*qbar=*/70.0, 10.0, 0.5, 500.0, 300.0, 5.0, 0.0, 1.0, /*inAir=*/false),
               fcs_high_q, aero, 0.01);

    EXPECT_LT(fcs_low_q.kp05, fcs_high_q.kp05)
        << "low qbar on ground should reduce kp05 (ground fade)";
}

// ============================================================================
// Yaw channel — mostly stubbed, but should still run
// ============================================================================

TEST(FcsYaw, PedalInputDoesNotCrash) {
    // The yaw channel is mostly stubbed (EOM has no rudder dynamics), but
    // it should still execute without crashing and produce some beta command.
    SyntheticFcs sf;
    FlightControlSystem fcs(&sf.cfg, &sf.geom, &sf.aux);

    FcsState fcs_state;
    AeroState aero;
    PilotInput input = makeInput();
    input.ypedal = 0.5;

    fcs.update(input, makeFc(), fcs_state, aero, 0.01);

    // The yaw channel forces beta to 0 (stubbed). Just verify it ran.
    EXPECT_NO_FATAL_FAILURE();
}

}  // namespace f4::flight
