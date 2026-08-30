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
// NAV-A: the AI never commands steady-state rudder. The Phase A2
// bank-proportional pedal feedforward was dimensionally inverted (~250x
// too big) and pinned |beta| at the aero clamp in every turn — the
// course_intercept/standard_rate_turn baselines measured a constant
// 15 deg of sideslip. Coordination is the FCS yaw damper's job with the
// pedals centered; these tests now pin that contract.

TEST(AirSteeringCoordTurn, RightBankCommandsZeroPedal) {
    // A sustained right turn needs no steady rudder: the yaw damper holds
    // beta ~ 0 with pedals centered. Any bank-proportional pedal here is
    // the (removed) inverted feedforward creeping back.
    AirSteering as;
    const auto out = as.steer(0.5, 5000, 300, make_input(0, 5000, 0, 300));
    EXPECT_NEAR(out.yaw_cmd, 0.0, 1e-9)
        << "sustained right bank must not command steady rudder";
}

TEST(AirSteeringCoordTurn, LeftBankCommandsZeroPedal) {
    AirSteering as;
    const auto out = as.steer(-0.5, 5000, 300, make_input(0, 5000, 0, 300));
    EXPECT_NEAR(out.yaw_cmd, 0.0, 1e-9)
        << "sustained left bank must not command steady rudder";
}

TEST(AirSteeringCoordTurn, WingsLevelZeroPedal) {
    // On-heading, wings level: pedals centered.
    AirSteering as;
    const auto out = as.steer(1.2, 5000, 300, make_input(1.2, 5000, 0, 300, 0.0));
    EXPECT_NEAR(out.yaw_cmd, 0.0, 1e-9);
}

TEST(AirSteeringCoordTurn, PedalStaysCenteredAcrossBanksAndSpeeds) {
    // Sample the whole envelope: no bank/speed combination may produce a
    // steady pedal command.
    AirSteering as;
    for (double hdg_err : {-1.5, -0.5, -0.1, 0.1, 0.5, 1.5}) {
        for (double vcas : {150.0, 250.0, 350.0, 450.0}) {
            const auto out = as.steer(hdg_err, 5000, 300,
                                      make_input(0, 5000, 0, vcas));
            EXPECT_NEAR(out.yaw_cmd, 0.0, 1e-9)
                << "hdg_err=" << hdg_err << " vcas=" << vcas;
        }
    }
}

TEST(AirSteeringCoordTurn, RollStillCommandsDuringTurns) {
    // The heading channel still banks: with a heading error the roll
    // command is nonzero (right error -> right roll), even though the
    // rudder stays centered. Guards against someone "fixing" the slip by
    // zeroing the whole lateral channel.
    AirSteering as;
    const auto out = as.steer(0.5, 5000, 300, make_input(0, 5000, 0, 300));
    EXPECT_GT(out.roll_cmd, 0.0) << "right heading error must still roll right";
}

// ============================================================================
// NAV-D: bank-compensated alpha feedforward
// ============================================================================

TEST(AirSteeringAltitude, BankedTurnCommandsMorePitchThanWingsLevel) {
    // Same state except 30 deg of bank: holding altitude needs
    // 1/cos(30) = 1.155x the alpha, so the pitch command must rise. This
    // pins the in-turn altitude-hold feedforward (the "no pitch authority
    // while banking" symptom from the course_intercept trace).
    AirSteering as;
    const auto level = as.steer(0.0, 5000, 300, make_input(0, 5000, 0, 300, 0.0, 0.05));
    const auto banked = as.steer(0.0, 5000, 300,
                                 make_input(0, 5000, 0, 300, 30.0 * D2R, 0.05));
    EXPECT_GT(banked.pitch_cmd, level.pitch_cmd)
        << "banked flight must command more pitch for the same altitude error";
}

TEST(AirSteeringAltitude, WingsLevelLiftCompensationIsUnity) {
    // At zero bank the compensation is exactly 1.0: the legacy equilibrium
    // tests (zero VS, zero error -> zero-ish stick) must still hold.
    AirSteering as;
    const auto out = as.steer(0.0, 5000, 300, make_input(0, 5000, 0, 300, 0.0, 0.05));
    EXPECT_NEAR(out.pitch_cmd, 0.0, 1e-6);
}
