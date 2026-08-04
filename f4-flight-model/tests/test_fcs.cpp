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
        geom.aoaMax_deg = 25.0;
        geom.aoaMin_deg = -5.0;
        geom.maxGs = 9.0;
        geom.maxRoll_deg = 80.0;
        geom.cornerVcas_kts = 330.0;
        geom.minVcas_kts = 140.0;
        geom.maxVcas_kts = 800.0;

        aux.pitchGearGain = 0.8;
        aux.rollGearGain = 0.6;
        aux.yawGearGain = 0.6;
        aux.landingAOA = 12.5;

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

    fcs.update(/*dt=*/0.01, /*qbar=*/100.0, /*qsom=*/10.0, /*mach=*/0.5,
               /*vt=*/500.0, /*vcas=*/300.0, /*alpha=*/5.0, /*beta=*/0.0,
               /*cosmu=*/1.0, /*cosgam=*/1.0, /*singam=*/0.0,
               /*costhe=*/1.0, /*cosphi=*/1.0, /*phi=*/0.0,
               /*loading=*/1.0, /*inAir=*/true,
               /*nzcgs=*/1.0, /*nycgw=*/0.0,
               /*gearDown=*/false, /*refueling=*/false, /*landing=*/false,
               input, fcs_state, aero);

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

    fcs.update(0.01, 100.0, 10.0, 0.5, 500.0, 300.0, 5.0, 0.0,
               1.0, 1.0, 0.0, 1.0, 1.0, 0.0, 1.0, true, 1.0, 0.0,
               false, false, false, input, fcs_state, aero);

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

    fcs.update(0.01, 100.0, 10.0, 0.5, 500.0, 300.0, 5.0, 0.0,
               1.0, 1.0, 0.0, 1.0, 1.0, 0.0, 1.0, true, 1.0, 0.0,
               false, false, false, input, fcs_state, aero);

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

    fcs.update(0.01, 200.0, 30.0, 0.5, 500.0, 300.0, 5.0, 0.0,
               1.0, 1.0, 0.0, 1.0, 1.0, 0.0, 1.0, true, 1.0, 0.0,
               false, false, false, input, fcs_state, aero);

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

    fcs.update(0.01, 100.0, 10.0, 0.5, 500.0, 300.0, 5.0, 0.0,
               1.0, 1.0, 0.0, 1.0, 1.0, 0.0, 1.0, true, 1.0, 0.0,
               false, false, false, input, fcs_state, aero);

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

    fcs.update(0.01, 100.0, 10.0, 0.5, 500.0, 300.0, 5.0, 0.0,
               1.0, 1.0, 0.0, 1.0, 1.0, 0.0, 1.0, true, 1.0, 0.0,
               false, false, false, input, fcs_state, aero);

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

    fcs.update(0.01, 100.0, 10.0, 0.5, 500.0, 300.0, 5.0, 0.0,
               1.0, 1.0, 0.0, 1.0, 1.0, 0.0, 1.0, true, 1.0, 0.0,
               false, false, false, input, fcs_state, aero);

    EXPECT_LT(fcs_state.pscmd, 0.0)
        << "left stick should produce a negative roll rate command";
}

// ============================================================================
// Landing gains — gear down / refueling / explicit
// ============================================================================

TEST(FcsLandingGains, GearDownActivatesLandingGains) {
    // When gear is down, the FCS should use landing gains (lower kp05,
    // higher alpha command authority for approach).
    SyntheticFcs sf;
    FlightControlSystem fcs(&sf.cfg, &sf.geom, &sf.aux);

    FcsState fcs_airborne;
    FcsState fcs_landing;
    AeroState aero;
    PilotInput input = makeInput();

    // Airborne, gear up
    fcs.update(0.01, 100.0, 10.0, 0.5, 500.0, 300.0, 5.0, 0.0,
               1.0, 1.0, 0.0, 1.0, 1.0, 0.0, 1.0, true, 1.0, 0.0,
               /*gearDown=*/false, /*refueling=*/false, /*landing=*/false,
               input, fcs_airborne, aero);

    // Landing, gear down
    fcs.update(0.01, 100.0, 10.0, 0.5, 500.0, 300.0, 5.0, 0.0,
               1.0, 1.0, 0.0, 1.0, 1.0, 0.0, 1.0, /*inAir=*/false, 1.0, 0.0,
               /*gearDown=*/true, /*refueling=*/false, /*landing=*/false,
               input, fcs_landing, aero);

    // The landing configuration should produce different gains than airborne.
    // We don't assert exact values (the gain computation is complex), just
    // that the two configurations produce DIFFERENT gains.
    EXPECT_NE(fcs_airborne.kp05, fcs_landing.kp05)
        << "landing gains should differ from airborne gains";
}

TEST(FcsLandingGains, RefuelingActivatesLandingGains) {
    SyntheticFcs sf;
    FlightControlSystem fcs(&sf.cfg, &sf.geom, &sf.aux);

    FcsState fcs_normal;
    FcsState fcs_refuel;
    AeroState aero;
    PilotInput input = makeInput();

    fcs.update(0.01, 100.0, 10.0, 0.5, 500.0, 300.0, 5.0, 0.0,
               1.0, 1.0, 0.0, 1.0, 1.0, 0.0, 1.0, true, 1.0, 0.0,
               false, /*refueling=*/false, false, input, fcs_normal, aero);

    fcs.update(0.01, 100.0, 10.0, 0.5, 500.0, 300.0, 5.0, 0.0,
               1.0, 1.0, 0.0, 1.0, 1.0, 0.0, 1.0, true, 1.0, 0.0,
               false, /*refueling=*/true, false, input, fcs_refuel, aero);

    EXPECT_NE(fcs_normal.kp05, fcs_refuel.kp05);
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
    fcs.update(0.01, /*qbar=*/10.0, 10.0, 0.1, 100.0, 100.0, 2.0, 0.0,
               1.0, 1.0, 0.0, 1.0, 1.0, 0.0, 1.0, /*inAir=*/false, 1.0, 0.0,
               true, false, false, input, fcs_low_q, aero);

    // Higher qbar on ground
    fcs.update(0.01, /*qbar=*/70.0, 10.0, 0.5, 500.0, 300.0, 5.0, 0.0,
               1.0, 1.0, 0.0, 1.0, 1.0, 0.0, 1.0, /*inAir=*/false, 1.0, 0.0,
               true, false, false, input, fcs_high_q, aero);

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

    fcs.update(0.01, 100.0, 10.0, 0.5, 500.0, 300.0, 5.0, 0.0,
               1.0, 1.0, 0.0, 1.0, 1.0, 0.0, 1.0, true, 1.0, 0.0,
               false, false, false, input, fcs_state, aero);

    // The yaw channel forces beta to 0 (stubbed). Just verify it ran.
    EXPECT_NO_FATAL_FAILURE();
}

}  // namespace f4::flight
