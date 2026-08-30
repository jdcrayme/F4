// f4-ai/tests/test_air_steering.cpp
//
// Unit tests for AirSteering — the shared air-phase control laws
// (bank-to-turn heading cascade, vertical-speed altitude cascade, speed
// hold). These pin the cascade SIGNS and CLAMPS so a refactor fails
// loudly instead of silently flying away.

#include <gtest/gtest.h>

#include <f4/ai/air_steering.hpp>

#include <algorithm>
#include <cmath>

using namespace f4::ai;
namespace geo = f4::geo;

namespace {

constexpr double PI = 3.14159265358979323846;
constexpr double D2R = PI / 180.0;

AirSteering::Input make_input(double hdg_rad, double alt_ft, double vs_fpm,
                              double vcas_kts, double roll_rad = 0.0,
                              double pitch_rad = 0.0) {
    AirSteering::Input in;
    in.position = geo::WorldPosition(0.0, 0.0, alt_ft);
    in.heading_rad = hdg_rad;
    in.pitch_rad = pitch_rad;
    in.roll_rad = roll_rad;
    in.vs_fpm = vs_fpm;
    in.vcas_kts = vcas_kts;
    in.alt_msl_ft = alt_ft;
    return in;
}

} // anonymous namespace

// ============================================================================
// Geometry helpers
// ============================================================================

TEST(AirSteeringGeometry, BearingDueEast) {
    EXPECT_NEAR(AirSteering::bearing_to(geo::WorldPosition(0, 0, 0),
                                        geo::WorldPosition(1000, 0, 0)),
                PI / 2, 1e-9);
}

TEST(AirSteeringGeometry, HeadingErrorWraps) {
    EXPECT_NEAR(AirSteering::heading_error(175 * D2R, -175 * D2R), -10 * D2R, 1e-9);
    EXPECT_NEAR(AirSteering::heading_error(-175 * D2R, 175 * D2R), 10 * D2R, 1e-9);
}

// ============================================================================
// Heading channel — bank-to-turn signs
// ============================================================================

TEST(AirSteeringHeading, RightTurnCommandsRightRoll) {
    // Need to turn right (heading error +90 deg) -> target bank right ->
    // positive roll command. Wings level initially.
    AirSteering as;
    const auto out = as.steer(90 * D2R, 5000, 300, make_input(0, 5000, 0, 300));
    EXPECT_GT(out.roll_cmd, 0.0) << "right turn requires positive roll command";
}

TEST(AirSteeringHeading, LeftTurnCommandsLeftRoll) {
    AirSteering as;
    const auto out = as.steer(-90 * D2R, 5000, 300, make_input(0, 5000, 0, 300));
    EXPECT_LT(out.roll_cmd, 0.0) << "left turn requires negative roll command";
}

TEST(AirSteeringHeading, BankTargetClamped) {
    // 90-deg error with bank_gain 2.0 -> target 3.14 rad, clamped to
    // max_bank_rad (~30 deg); roll command = 6.0 * 0.52 = 3.1 -> clamped 1.0.
    AirSteering as;
    const auto out = as.steer(90 * D2R, 5000, 300, make_input(0, 5000, 0, 300));
    EXPECT_NEAR(out.roll_cmd, 1.0, 1e-6);
}

TEST(AirSteeringHeading, OnHeadingZeroRoll) {
    // Aligned, wings level: no roll command.
    AirSteering as;
    const auto out = as.steer(1.2, 5000, 300, make_input(1.2, 5000, 0, 300, 0.0));
    EXPECT_NEAR(out.roll_cmd, 0.0, 1e-9);
}

TEST(AirSteeringHeading, RollsOutOfExistingBank) {
    // Banked left (negative roll) while heading is on-target: the cascade
    // commands positive roll to level the wings.
    AirSteering as;
    const auto out = as.steer(0.0, 5000, 300, make_input(0, 5000, 0, 300, -0.3));
    EXPECT_GT(out.roll_cmd, 0.0);
}

// ============================================================================
// Altitude channel — vertical-speed cascade
// ============================================================================

TEST(AirSteeringAltitude, BelowAltitudeCommandsPullUp) {
    // 1000 ft below target -> positive VS target -> VS error positive ->
    // positive pitch command.
    AirSteering as;
    const auto out = as.steer(0, 6000, 300, make_input(0, 5000, 0, 300));
    EXPECT_GT(out.pitch_cmd, 0.0);
}

TEST(AirSteeringAltitude, AboveAltitudeCommandsPushOver) {
    AirSteering as;
    const auto out = as.steer(0, 4000, 300, make_input(0, 5000, 0, 300));
    EXPECT_LT(out.pitch_cmd, 0.0);
}

TEST(AirSteeringAltitude, OnGlidePathZeroVSZeroStick) {
    // On altitude, VS zero, pitch zero: alpha_est 0, gamma command 0 ->
    // zero stick (the gamma-hold law is in equilibrium by construction).
    AirSteering as;
    const auto out = as.steer(0, 5000, 300, make_input(0, 5000, 0, 300, 0.0, 0.0));
    EXPECT_NEAR(out.pitch_cmd, 0.0, 1e-9);
}

TEST(AirSteeringAltitude, GammaTrackingPullsOutOfSink) {
    // On altitude but sinking at -2000 fpm: the law commands pitch up to
    // rotate gamma back to zero (alpha_est + positive correction).
    AirSteering as;
    const auto out = as.steer(0, 5000, 300, make_input(0, 5000, -2000, 300, 0.0, 0.0));
    EXPECT_GT(out.pitch_cmd, 0.0);
    // Sustained descent command holds the CURRENT alpha plus the
    // commanded gamma: descending at the commanded rate = equilibrium.
    const auto desc = as.steer(0, 5000, 300, make_input(0, 5000, -1200, 300));
    EXPECT_GT(desc.pitch_cmd, 0.0) << "descending attitude still has positive alpha";
}

TEST(AirSteeringAltitude, ClimbRateClamped) {
    // 10000 ft below target commands a huge VS; the clamp caps it at
    // max_vs — but STAB-E29 slew-rate limits the VS command at
    // vs_slew_fpm_per_s (default 400), so the FIRST tick only commands
    // 400 fpm of climb, building to the clamp over ~max_vs/400 seconds.
    // The pitch command must therefore START strong (the gamma_ff of the
    // ramping target + alpha_est) and SUSTAIN as the target ramps.
    AirSteering as;
    const auto out = as.steer(0, 15000, 300, make_input(0, 5000, 0, 300));
    // First tick: vs_target = 400 fpm (slewed) — a modest climb demand,
    // not the old instant full-authority pull.
    EXPECT_GT(out.pitch_cmd, 0.0) << "expected a climb pitch command";
    // After 10 s of ticks (600 calls at the 60 Hz design point) the slew
    // has reached the 2,500 fpm default cap: the pull is strong.
    AirSteering::Input in = make_input(0, 5000, 0, 300);
    double sustained = 0.0;
    for (int i = 0; i < 600; ++i) sustained = as.steer(0, 15000, 300, in).pitch_cmd;
    EXPECT_GT(sustained, 0.25) << "expected a strong sustained pull-up command";
    EXPECT_LE(sustained, as.pitch_max);
}

TEST(AirSteeringAltitude, HighAttitudeAtLevelFlightIsEquilibrium) {
    // A high pitch attitude with ZERO VS means high alpha (slow flight):
    // under the gamma-hold law this is equilibrium — the commanded pitch
    // equals current alpha_est, so the stick is near zero.
    AirSteering as;
    const auto out = as.steer(0, 5000, 300, make_input(0, 5000, 0, 300, 0.0, 0.2));
    EXPECT_NEAR(out.pitch_cmd, 0.0, 0.05);
}

// ============================================================================
// Speed channel
// ============================================================================

TEST(AirSteeringSpeed, SlowAddsThrottle) {
    AirSteering as;
    const auto slow = as.steer(0, 5000, 300, make_input(0, 5000, 0, 280));
    const auto on_speed = as.steer(0, 5000, 300, make_input(0, 5000, 0, 300));
    EXPECT_GT(slow.throttle_cmd, on_speed.throttle_cmd);
    // On-speed throttle is near throttle_mid. The exact equality no longer
    // holds because the speed channel has an integral term: the slow call
    // accumulates a small positive integral that leaks into the on_speed
    // call (one tick at 60 Hz, leak rate 1/600). The on_speed throttle
    // is therefore within ~0.01 of throttle_mid, not exactly equal.
    EXPECT_NEAR(on_speed.throttle_cmd, as.throttle_mid, 0.02);
}

TEST(AirSteeringSpeed, FastReducesThrottle) {
    AirSteering as;
    // 60 kts fast pulls the throttle toward the floor (the exact value
    // depends on the default throttle_gain, which STAB-E1 lowered from
    // 0.008 to 0.005 — assert direction and floor-clamping behavior
    // instead of one magic number).
    const auto fast = as.steer(0, 5000, 300, make_input(0, 5000, 0, 360));
    EXPECT_LT(fast.throttle_cmd, as.throttle_mid);
    EXPECT_NEAR(fast.throttle_cmd, as.throttle_min, 0.31);  // at/near floor
}

TEST(AirSteeringSpeed, ThrottleNeverExceedsMIL) {
    AirSteering as;
    const auto very_slow = as.steer(0, 5000, 300, make_input(0, 5000, 0, 100));
    EXPECT_LE(very_slow.throttle_cmd, 1.0) << "nav never selects afterburner";
}

// ============================================================================
// Coordinated-turn feedforward (Phase A2)
// ============================================================================
//
// rudder-for-bank: pedal_ff = tan(bank_target) * v / g, mapped to the
// normalized [-1, +1] command space via coord_turn_scale. Eliminates
// steady-state sideslip in turns. See FLIGHT_CONTROL_NEXT_STEPS.md Phase A2.

TEST(AirSteeringCoordTurn, RightBankCommandsRightRudder) {
    // Right turn (positive heading error) -> positive target bank -> the
    // feedforward commands POSITIVE pedal (right rudder), which the EOM
    // interprets as a turn-toward-bank (positive ypedal = right turn per
    // the sign convention documented in air_steering.cpp).
    AirSteering as;
    const auto out = as.steer(0.5, 5000, 300, make_input(0, 5000, 0, 300));
    EXPECT_GT(out.yaw_cmd, 0.0)
        << "right bank should command right rudder (positive pedal)";
}

TEST(AirSteeringCoordTurn, LeftBankCommandsLeftRudder) {
    AirSteering as;
    const auto out = as.steer(-0.5, 5000, 300, make_input(0, 5000, 0, 300));
    EXPECT_LT(out.yaw_cmd, 0.0)
        << "left bank should command left rudder (negative pedal)";
}

TEST(AirSteeringCoordTurn, WingsLevelZeroPedal) {
    // On-heading, wings level: bank_target is zero, feedforward is zero.
    AirSteering as;
    const auto out = as.steer(1.2, 5000, 300, make_input(1.2, 5000, 0, 300, 0.0));
    EXPECT_NEAR(out.yaw_cmd, 0.0, 1e-9);
}

TEST(AirSteeringCoordTurn, PedalMagnitudeScalesWithBank) {
    // Larger bank -> larger pedal command.
    AirSteering as;
    const auto small = as.steer(0.1, 5000, 300, make_input(0, 5000, 0, 300));
    const auto large = as.steer(0.5, 5000, 300, make_input(0, 5000, 0, 300));
    EXPECT_GT(std::fabs(large.yaw_cmd), std::fabs(small.yaw_cmd));
}

TEST(AirSteeringCoordTurn, PedalClampedAtHighBank) {
    // A 90-deg heading error commands max_bank (30 deg). The feedforward
    // uses the clamped bank (not 90 deg), so the pedal stays finite.
    AirSteering as;
    const auto out = as.steer(90 * D2R, 5000, 300, make_input(0, 5000, 0, 300));
    EXPECT_LE(std::fabs(out.yaw_cmd), 1.0)
        << "pedal must stay within [-1, +1] even at max bank";
}

TEST(AirSteeringCoordTurn, ZeroScaleDisablesFeedforward) {
    // When coord_turn_scale is 0, the feedforward is disabled (the FCS yaw
    // damper alone handles coordination). Allows easy A/B comparison in
    // diagnostic runs.
    AirSteering as;
    as.coord_turn_scale = 0.0;
    const auto out = as.steer(0.5, 5000, 300, make_input(0, 5000, 0, 300));
    EXPECT_NEAR(out.yaw_cmd, 0.0, 1e-9);
}
