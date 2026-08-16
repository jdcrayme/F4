// f4-ai/tests/test_ground_steering.cpp
//
// Unit tests for GroundSteering — the shared ground-movement control law
// used by TakeoffModule (taxi/lineup/roll) and later LandingModule
// (rollout/taxi-in).
//
// The critical contract under test is the PEDAL SIGN: the EOM integrates
// psi_delta = -ypedal * rate * dt with psi a compass angle, so positive
// pedal turns LEFT. A positive heading error (turn right needed) must
// produce a negative yaw_cmd. These tests pin that convention so a future
// refactor of either side fails loudly instead of silently steering the
// aircraft off the taxiway.

#include <gtest/gtest.h>

#include <f4/ai/ground_steering.hpp>

#include <cmath>

using namespace f4::ai;
namespace geo = f4::geo;

namespace {

constexpr double PI = 3.14159265358979323846;

GroundSteering::Input make_input(double east_ft, double north_ft,
                                 double heading_rad, double speed_kts) {
    GroundSteering::Input in;
    in.position = geo::WorldPosition(east_ft, north_ft, 0.0);
    in.heading_rad = heading_rad;
    in.speed_kts = speed_kts;
    return in;
}

} // anonymous namespace

// ============================================================================
// Geometry helpers
// ============================================================================

TEST(GroundSteeringGeometry, BearingDueNorth) {
    EXPECT_NEAR(GroundSteering::bearing_to({0, 0, 0}, {0, 1000, 0}), 0.0, 1e-9);
}

TEST(GroundSteeringGeometry, BearingDueEast) {
    EXPECT_NEAR(GroundSteering::bearing_to({0, 0, 0}, {1000, 0, 0}), PI / 2, 1e-9);
}

TEST(GroundSteeringGeometry, BearingSouthWest) {
    // Southwest = -135 deg = -3pi/4
    EXPECT_NEAR(GroundSteering::bearing_to({0, 0, 0}, {-1000, -1000, 0}),
                -3.0 * PI / 4.0, 1e-9);
}

TEST(GroundSteeringGeometry, HeadingErrorWrapsAroundPi) {
    // Desired 170 deg, current -170 deg: shortest way is +20 deg (right).
    EXPECT_NEAR(GroundSteering::heading_error(170.0 * PI / 180, -170.0 * PI / 180),
                20.0 * PI / 180, 1e-9);
    // Desired -170 deg, current 170 deg: shortest way is -20 deg (left).
    EXPECT_NEAR(GroundSteering::heading_error(-170.0 * PI / 180, 170.0 * PI / 180),
                -20.0 * PI / 180, 1e-9);
}

// ============================================================================
// Pedal sign convention
// ============================================================================

TEST(GroundSteeringPedal, RightTurnNeedsNegativePedal) {
    // Aircraft heading north, target due east: must turn right (+90 deg
    // error). The EOM's nose-wheel law (psi_delta = -ypedal * rate * dt,
    // psi compass) means positive pedal turns left, so yaw_cmd < 0.
    GroundSteering gs;
    const auto out = gs.steer_toward({1000, 0, 0}, make_input(0, 0, 0.0, 5.0),
                                     15.0, /*stop_at_target=*/false);
    EXPECT_NEAR(out.yaw_cmd, -1.0, 1e-6)  // 90 deg error saturates the clamp
        << "turning right (compass +) requires negative pedal";
}

TEST(GroundSteeringPedal, LeftTurnNeedsPositivePedal) {
    // Aircraft heading north, target due west: must turn left (-90 deg).
    GroundSteering gs;
    const auto out = gs.steer_toward({-1000, 0, 0}, make_input(0, 0, 0.0, 5.0),
                                     15.0, /*stop_at_target=*/false);
    EXPECT_NEAR(out.yaw_cmd, 1.0, 1e-6)
        << "turning left (compass -) requires positive pedal";
}

TEST(GroundSteeringPedal, SmallErrorProportionalNoClamp) {
    GroundSteering gs;
    gs.heading_gain = 3.0;
    // Target slightly right of heading: +0.1 rad error -> -0.3 pedal.
    const auto out = gs.align_heading(0.1, make_input(0, 0, 0.0, 5.0), 15.0, false);
    EXPECT_NEAR(out.yaw_cmd, -0.3, 1e-9);
}

TEST(GroundSteeringPedal, AlignHeadingHoldsHeading) {
    // Already aligned: no pedal.
    GroundSteering gs;
    const auto out = gs.align_heading(1.0, make_input(0, 0, 1.0, 5.0), 15.0, false);
    EXPECT_NEAR(out.yaw_cmd, 0.0, 1e-9);
}

// ============================================================================
// Speed control
// ============================================================================

TEST(GroundSteeringSpeed, BelowTargetAppliesThrottle) {
    GroundSteering gs;
    const auto out = gs.steer_toward({0, 1000, 0}, make_input(0, 0, 0.0, 3.0),
                                     15.0, /*stop_at_target=*/false);
    EXPECT_GT(out.throttle_cmd, 0.0);
    EXPECT_LE(out.throttle_cmd, gs.max_throttle);
    EXPECT_FALSE(out.wheel_brakes);
}

TEST(GroundSteeringSpeed, AboveTargetPlusMarginBrakes) {
    GroundSteering gs;
    gs.brake_margin_kts = 4.0;
    const auto out = gs.steer_toward({0, 1000, 0}, make_input(0, 0, 0.0, 25.0),
                                     15.0, /*stop_at_target=*/false);
    EXPECT_TRUE(out.wheel_brakes);
    EXPECT_NEAR(out.throttle_cmd, 0.0, 1e-9);
}

TEST(GroundSteeringSpeed, SharpTurnReducesTargetSpeed) {
    // 90-degree turn: target speed capped at sharp_turn_speed_kts (8 kts).
    // At 10 kts current, the 8-kt target is exceeded by 2 (< 4 margin), so
    // no brakes, but throttle should be at the floor (no creep surplus).
    GroundSteering gs;
    gs.sharp_turn_speed_kts = 8.0;
    const auto out = gs.steer_toward({1000, 0, 0}, make_input(0, 0, 0.0, 10.0),
                                     15.0, /*stop_at_target=*/false);
    EXPECT_FALSE(out.wheel_brakes);
    EXPECT_NEAR(out.throttle_cmd, 0.0, 1e-6);
}

TEST(GroundSteeringSpeed, StopAtTargetDeceleratesNearTarget) {
    // 20 ft from the stop point with 3 ft/s^2 decel: v_limit = sqrt(2*3*5)
    // ~= 5.5 fps ~= 3.2 kts. Current 10 kts is above target + margin -> brakes.
    GroundSteering gs;
    const auto out = gs.steer_toward({0, 20, 0}, make_input(0, 0, 0.0, 10.0),
                                     15.0, /*stop_at_target=*/true);
    EXPECT_TRUE(out.wheel_brakes);
}

TEST(GroundSteeringSpeed, StopAtTargetFullSpeedFarAway) {
    GroundSteering gs;
    const auto out = gs.steer_toward({0, 5000, 0}, make_input(0, 0, 0.0, 0.0),
                                     15.0, /*stop_at_target=*/true);
    EXPECT_GT(out.throttle_cmd, 0.0);
    EXPECT_FALSE(out.wheel_brakes);
}

TEST(GroundSteeringSpeed, StoppedTargetHoldsBrakes) {
    // Inside the stop radius the target speed collapses to 0.
    GroundSteering gs;
    const auto out = gs.steer_toward({0, 5, 0}, make_input(0, 0, 0.0, 0.0),
                                     15.0, /*stop_at_target=*/true);
    EXPECT_TRUE(out.wheel_brakes);
    EXPECT_NEAR(out.throttle_cmd, 0.0, 1e-9);
}

// ============================================================================
// Hold
// ============================================================================

TEST(GroundSteeringHold, HoldBrakesIdleGearDown) {
    GroundSteering gs;
    const auto out = gs.hold();
    EXPECT_TRUE(out.wheel_brakes);
    EXPECT_NEAR(out.throttle_cmd, 0.0, 1e-9);
    EXPECT_TRUE(out.gear_handle_down);
    EXPECT_NEAR(out.yaw_cmd, 0.0, 1e-9);
}

// ============================================================================
// Gear always down on ground
// ============================================================================

TEST(GroundSteeringOutput, GearDownInAllGroundModes) {
    GroundSteering gs;
    EXPECT_TRUE(gs.steer_toward({0, 100, 0}, make_input(0, 0, 0, 5), 15, false)
                    .gear_handle_down);
    EXPECT_TRUE(gs.align_heading(0.0, make_input(0, 0, 0, 5), 15, false)
                    .gear_handle_down);
    EXPECT_TRUE(gs.hold().gear_handle_down);
}
