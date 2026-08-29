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
    // to turn_speed_kts; at 300 kts current the throttle sits at the floor.
    mod.set_route({make_wp("EAST", 100000, 0, 10000, 400)});
    s = make_state(0, 0, 10000, /*hdg=*/0.0, /*vcas=*/300.0);
    const auto turn_out = mod.update(0.1, s.get());
    EXPECT_GT(turn_out.roll_cmd, 0.0);
    EXPECT_NEAR(turn_out.throttle_cmd, mod.air_steering.throttle_min, 1e-9);
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
