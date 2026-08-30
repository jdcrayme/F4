// f4-ai/tests/test_navigation_module.cpp
//
// Unit tests for NavigationModule — route handling, waypoint capture,
// per-tick control outputs through the module contract (mock IAircraftState,
// same pattern as test_takeoff_module.cpp).

#include <gtest/gtest.h>

#include <f4/ai/modules/navigation_module.hpp>

#include <cmath>
#include <memory>

using namespace f4::ai::modules;
namespace geo = f4::geo;
namespace flight = f4::flight;
namespace fsm = f4::fsm;

namespace {

class TestAircraftState : public flight::IAircraftState {
public:
    double east_ft{0.0};
    double north_ft{0.0};
    double alt_msl_ft{0.0};
    double alt_agl_ft_{0.0};
    double vcas_kts_{300.0};
    double heading_rad_{0.0};
    double pitch_rad_{0.0};
    double roll_rad_{0.0};
    double roll_rate_radps_{0.0};
    double pitch_rate_radps_{0.0};
    double yaw_rate_radps_{0.0};
    double vs_fpm_{0.0};
    bool on_ground_{false};

    double position_east_ft()  const override { return east_ft; }
    double position_north_ft() const override { return north_ft; }
    double altitude_msl_ft()   const override { return alt_msl_ft; }
    double altitude_agl_ft()   const override { return alt_agl_ft_; }
    double vcas_kts()          const override { return vcas_kts_; }
    double heading_rad()       const override { return heading_rad_; }
    double pitch_angle_rad()   const override { return pitch_rad_; }
    double roll_angle_rad()    const override { return roll_rad_; }
    double roll_rate_radps()   const override { return roll_rate_radps_; }
    double pitch_rate_radps()  const override { return pitch_rate_radps_; }
    double yaw_rate_radps()    const override { return yaw_rate_radps_; }
    double vertical_speed_fpm() const override { return vs_fpm_; }
    bool   on_ground()         const override { return on_ground_; }
};

std::unique_ptr<TestAircraftState> make_state(double east, double north, double alt,
                                              double hdg = 0.0, double vcas = 300.0) {
    auto s = std::make_unique<TestAircraftState>();
    s->east_ft = east;
    s->north_ft = north;
    s->alt_msl_ft = alt;
    s->alt_agl_ft_ = alt;
    s->heading_rad_ = hdg;
    s->vcas_kts_ = vcas;
    return s;
}

NavigationModule::Waypoint make_wp(const std::string& name, double east, double north,
                                   double alt, double speed_kts = 350.0) {
    return NavigationModule::Waypoint{name, geo::WorldPosition(east, north, alt), speed_kts};
}

} // anonymous namespace

// ============================================================================
// Route handling
// ============================================================================

TEST(NavigationModule, EmptyRouteCompletesImmediately) {
    NavigationModule mod;
    mod.set_route({});
    EXPECT_EQ(mod.state(), NavigationState::Done);
    EXPECT_TRUE(mod.is_complete());
}

TEST(NavigationModule, WithRouteStartsToWaypoint) {
    NavigationModule mod;
    mod.set_route({make_wp("WP1", 0, 50000, 10000)});
    EXPECT_EQ(mod.state(), NavigationState::ToWaypoint);
    EXPECT_FALSE(mod.is_complete());
    ASSERT_NE(mod.current_waypoint(), nullptr);
    EXPECT_EQ(mod.current_waypoint()->name, "WP1");
}

TEST(NavigationModule, StateNames) {
    NavigationModule mod;
    EXPECT_EQ(mod.mode_name(), "NavigationMode");
    EXPECT_EQ(mod.state_name(), "ToWaypoint");
}

// ============================================================================
// Waypoint capture
// ============================================================================

TEST(NavigationModule, CapturesWaypointWithinRadius) {
    NavigationModule mod;
    mod.capture_radius_ft = 3000.0;
    mod.set_route({make_wp("WP1", 0, 50000, 10000), make_wp("WP2", 0, 90000, 12000)});

    // 20,000 ft from WP1: no capture.
    auto s = make_state(0, 30000, 10000);
    mod.update(0.1, s.get());
    EXPECT_EQ(mod.state(), NavigationState::ToWaypoint);
    EXPECT_EQ(mod.current_waypoint_index(), 0u);

    // Inside 3000 ft of WP1: advance to WP2 (route not complete).
    s = make_state(0, 48000, 10000);
    mod.update(0.1, s.get());
    EXPECT_EQ(mod.current_waypoint_index(), 1u);
    EXPECT_EQ(mod.state(), NavigationState::ToWaypoint);

    // Inside WP2: route complete.
    s = make_state(0, 89000, 12000);
    mod.update(0.1, s.get());
    EXPECT_EQ(mod.state(), NavigationState::Done);
    EXPECT_TRUE(mod.is_complete());
}

TEST(NavigationModule, DoneProducesNoControl) {
    NavigationModule mod;
    mod.set_route({make_wp("WP1", 0, 5000, 5000)});
    auto s = make_state(0, 4000, 5000);  // inside capture radius
    const auto out = mod.update(0.1, s.get());
    EXPECT_TRUE(mod.is_complete());
    EXPECT_NEAR(out.pitch_cmd, 0.0, 1e-9);
    EXPECT_NEAR(out.throttle_cmd, 0.0, 1e-9);
}

// ============================================================================
// Control outputs
// ============================================================================

TEST(NavigationModule, SteersTowardCurrentWaypoint) {
    NavigationModule mod;
    // Aligned case: waypoint due north, below its speed -> right roll-free
    // output with throttle above mid.
    mod.set_route({make_wp("NORTH", 0, 100000, 10000, 400)});
    auto s = make_state(0, 0, 10000, /*hdg=*/0.0, /*vcas=*/300.0);
    const auto out = mod.update(0.1, s.get());
    EXPECT_NEAR(out.roll_cmd, 0.0, 1e-9);
    EXPECT_GT(out.throttle_cmd, mod.air_steering.throttle_mid);

    // Turning case: waypoint due east (90 deg off-nose) slows the target
    // to turn_speed_kts; at 300 kts current the throttle pulls below mid
    // toward the floor (exact value depends on the default throttle_gain,
    // lowered 0.008 -> 0.005 by STAB-E1).
    mod.set_route({make_wp("EAST", 100000, 0, 10000, 400)});
    s = make_state(0, 0, 10000, /*hdg=*/0.0, /*vcas=*/300.0);
    const auto turn_out = mod.update(0.1, s.get());
    EXPECT_GT(turn_out.roll_cmd, 0.0);
    EXPECT_LT(turn_out.throttle_cmd, mod.air_steering.throttle_mid);
    EXPECT_GE(turn_out.throttle_cmd, mod.air_steering.throttle_min);
}

TEST(NavigationModule, NullStateIsSafeNoOp) {
    NavigationModule mod;
    mod.set_route({make_wp("WP1", 0, 50000, 10000)});
    const auto out = mod.update(0.1, nullptr);
    // Cached state stays at defaults (heading 0, alt 0): waypoint far north
    // of the default (0,0) position -> no capture, outputs finite.
    EXPECT_TRUE(std::isfinite(out.roll_cmd));
    EXPECT_TRUE(std::isfinite(out.pitch_cmd));
    EXPECT_EQ(mod.state(), NavigationState::ToWaypoint);
}

// ============================================================================
// Trace support
// ============================================================================

TEST(NavigationModule, AcceptsTrace) {
    NavigationModule mod;
    fsm::Trace<NavigationState, NavigationEvent> trace;
    trace.set_capacity(64);
    mod.set_trace(&trace);
    EXPECT_NE(mod.trace(), nullptr);
}

// ============================================================================
// NAV-B: leg tracking (cross-track LNAV) + turn anticipation
// ============================================================================

TEST(NavigationLnav, EstablishedOnCourseCommandsCourse) {
    // Mid-leg, on the centerline: the commanded heading IS the leg course
    // (0 = due north). The old pursuit law also gave 0 here only because
    // the aircraft happened to be exactly on the bearing line — this pins
    // the LNAV math (right-vector xte must be exactly 0 on the line).
    NavigationModule mod;
    mod.set_route({make_wp("WP1", 0, 100000, 10000)});
    auto s = make_state(0, 50000, 10000, /*hdg=*/0.0);
    mod.update(0.1, s.get());
    EXPECT_NEAR(mod.nav_heading_rad(), 0.0, 1e-9);
}

TEST(NavigationLnav, RightOfCourseSteersLeftOfCourse) {
    // 5,000 ft right of a due-north LEG (anchored at BACK 20,000 ft behind
    // the aircraft — route activation skips BACK and flies the BACK->WP1
    // leg): the raw correction atan2(-5000, 5000) = -45 deg clamps to the
    // 20-deg max intercept (kept below the bank cap so the convergence is
    // damped, not rate-limited). The old pursuit law commanded only -3.6
    // deg (bearing to the wp) — this is the homing-vs-intercept
    // distinction.
    NavigationModule mod;
    mod.set_route({make_wp("BACK", 0, 0, 10000),
                   make_wp("WP1", 0, 100000, 10000)});
    auto s = make_state(5000, 20000, 10000, /*hdg=*/0.0);
    mod.update(0.1, s.get());
    EXPECT_EQ(mod.current_waypoint_index(), 1u);
    EXPECT_NEAR(mod.nav_heading_rad(), -mod.max_intercept_rad, 0.02);
}

TEST(NavigationLnav, LeftOfCourseSteersRight) {
    // Mirror case: 2,000 ft left -> raw +21.8 deg clamps to +20 (with
    // wings-level heading the track-rate damping term is zero).
    NavigationModule mod;
    mod.set_route({make_wp("BACK", 0, 0, 10000),
                   make_wp("WP1", 0, 100000, 10000)});
    auto s = make_state(-2000, 20000, 10000, /*hdg=*/0.0);
    mod.update(0.1, s.get());
    EXPECT_NEAR(mod.nav_heading_rad(), mod.max_intercept_rad, 0.02);
}

TEST(NavigationLnav, InterceptAngleSaturates) {
    // 14,000 ft right (inside the abeam window so leg 1 is active):
    // raw correction atan2(-14k, 8k) = -60 deg clamps at
    // max_intercept_rad, never more — an aircraft bank-limited to 30 deg
    // cannot fly a steeper stable intercept anyway. (50k ft off would NOT
    // reach leg 1 — the abeam-window rule sends it to wp0 first, which is
    // the correct consolidation behavior.)
    NavigationModule mod;
    mod.set_route({make_wp("BACK", 0, 0, 10000),
                   make_wp("WP1", 0, 100000, 10000)});
    auto s = make_state(14000, 20000, 10000, /*hdg=*/0.0);
    mod.update(0.1, s.get());
    EXPECT_EQ(mod.current_waypoint_index(), 1u);
    EXPECT_NEAR(mod.nav_heading_rad(), -mod.max_intercept_rad, 1e-9);
}

TEST(NavigationLnav, PastAbeamStillFlysTheCourse) {
    // 2,000 ft right and 4,000 ft BEYOND the active waypoint (4,472 ft
    // total — outside the 3,000 ft capture radius): pursuit guidance
    // reverses (bearing to wp = -117 deg, the "nose slews to point at the
    // waypoint as it passes" symptom). LNAV keeps flying the course with
    // a bounded correction (-14 deg) — capture sequencing handles the
    // waypoint, not the steering law.
    NavigationModule mod;
    mod.set_route({make_wp("PREV", 0, -100000, 10000),
                   make_wp("WP_LAST", 0, 100000, 10000)});
    auto s = make_state(2000, 104000, 10000, /*hdg=*/0.0);
    mod.update(0.1, s.get());
    EXPECT_EQ(mod.current_waypoint_index(), 1u);  // on the last leg, not captured
    const double hdg = mod.nav_heading_rad();
    EXPECT_NEAR(hdg, -mod.max_intercept_rad, 0.02)
        << "past-abeam must not reverse toward the waypoint";
    EXPECT_GT(hdg, -1.0) << "must stay within 57 deg of course (no pursuit reversal)";
}

TEST(NavigationLnav, TurnAnticipationSequencesEarly) {
    // 10,000 ft from WP1 with a 90-deg turn onto leg 2 at 300 kts:
    // R = v^2/(g*tan(30deg)) ~ 13,900 ft, lead = 1.15*R*tan(45) ~ 16,000
    // ft. The module must sequence NOW (dist < lead) so the turn rolls
    // out established on the next leg — the pursuit code stayed on WP1
    // until the 3,000 ft capture radius (by then: guaranteed overshoot).
    NavigationModule mod;
    mod.set_route({make_wp("WP1", 0, 100000, 10000),
                   make_wp("WP2", 100000, 100000, 10000)});
    auto s = make_state(0, 90000, 10000, /*hdg=*/0.0, /*vcas=*/300.0);
    mod.update(0.1, s.get());
    EXPECT_EQ(mod.current_waypoint_index(), 1u)
        << "must sequence to WP2 early (turn anticipation)";
}

TEST(NavigationLnav, LastWaypointStillCapturesByRadius) {
    // No next leg -> no turn anticipation: the last waypoint uses the
    // plain capture radius (nothing to establish on afterwards).
    NavigationModule mod;
    mod.set_route({make_wp("WP1", 0, 100000, 10000)});
    auto s = make_state(0, 96000, 10000, /*hdg=*/0.0);  // 4,000 ft out
    mod.update(0.1, s.get());
    EXPECT_EQ(mod.current_waypoint_index(), 0u)
        << "4,000 ft from the LAST waypoint: no early sequencing";
    s = make_state(0, 98000, 10000, /*hdg=*/0.0);       // 2,000 ft out
    mod.update(0.1, s.get());
    EXPECT_TRUE(mod.is_complete()) << "inside capture radius: Done";
}

TEST(NavigationLnav, StraightThroughRouteUsesNoLead) {
    // Collinear legs (dtheta = 0): lead = 0, sequencing behaves like the
    // old capture-radius rule.
    NavigationModule mod;
    mod.set_route({make_wp("WP1", 0, 100000, 10000),
                   make_wp("WP2", 0, 200000, 10000)});
    auto s = make_state(0, 90000, 10000, /*hdg=*/0.0, /*vcas=*/300.0);
    mod.update(0.1, s.get());
    EXPECT_EQ(mod.current_waypoint_index(), 0u)
        << "straight-through course change: no early sequencing";
    s = make_state(0, 99000, 10000, /*hdg=*/0.0);
    mod.update(0.1, s.get());
    EXPECT_EQ(mod.current_waypoint_index(), 1u);
}

TEST(NavigationLnav, SpawnEstablishedOnFirstLegSkipsToSecond) {
    // Aircraft 40,000 ft past wp0, 5,000 ft right of the wp0->wp1 course
    // (route activation over an existing leg): the module must anchor on
    // wp0 and fly leg 1 with a cross-track correction — NOT fly a course
    // through itself at wp1 (homing).
    NavigationModule mod;
    mod.set_route({make_wp("WP0", 0, 0, 10000),
                   make_wp("WP1", 0, 100000, 10000)});
    auto s = make_state(5000, 40000, 10000, /*hdg=*/0.0);
    mod.update(0.1, s.get());
    EXPECT_EQ(mod.current_waypoint_index(), 1u)
        << "spawn past WP0 within the abeam window: fly leg 1";
    EXPECT_NEAR(mod.nav_heading_rad(), -mod.max_intercept_rad, 0.02)
        << "5,000 ft right of the leg: clamped left intercept";
}

TEST(NavigationLnav, SpawnFarFromFirstLegFliesToWaypointZero) {
    // 50,000 ft off the wp0->wp1 line (outside the abeam window): wp0 is
    // a real first waypoint — anchor at the aircraft and fly to wp0.
    NavigationModule mod;
    mod.set_route({make_wp("WP0", 0, 0, 10000),
                   make_wp("WP1", 0, 100000, 10000)});
    auto s = make_state(50000, 40000, 10000, /*hdg=*/0.0);
    mod.update(0.1, s.get());
    EXPECT_EQ(mod.current_waypoint_index(), 0u);
}
