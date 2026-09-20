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
    double fuel_lbs_{5000.0};

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
    double fuel_lbs()          const override { return fuel_lbs_; }
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

// ============================================================================
// P7 — the station hold (the racetrack anchor's loop-until-timer)
// ============================================================================

// The strategy tranche's racetrack shape, in feet: PRE → ANCHOR (the
// station contract) → C1 → C2 → C3 (the loop span = 4, anchor first) →
// POST. The hold loops ANCHOR..C3 until station_time_s elapses, then
// releases out of the span toward POST. Post-capture index notes: after
// waypoint N is captured the ACTIVE waypoint is N+1 (the leg being
// flown TO it).
static std::vector<NavigationModule::Waypoint> make_racetrack_route(
        double station_time_s, std::uint8_t loop) {
    NavigationModule::Waypoint anchor =
        make_wp("ANCHOR", 0, 50000, 10000);
    anchor.station_time_s = station_time_s;
    anchor.loop_waypoints = loop;
    return {make_wp("PRE", 0, 0, 10000),
            anchor,
            make_wp("C1", 0, 100000, 10000),
            make_wp("C2", 50000, 100000, 10000),
            make_wp("C3", 50000, 50000, 10000),
            make_wp("POST", 0, 150000, 10000)};
}

TEST(NavigationStationHold, LoopsUntilTheTimerExpiresThenReleases) {
    NavigationModule mod;
    mod.set_route(make_racetrack_route(120.0, 4));

    // Capture the anchor (index 1): the hold arms, the clock starts at
    // zero, the active waypoint is the first corner (index 2).
    auto s = make_state(0, 49000, 10000);
    mod.update(0.1, s.get());
    ASSERT_EQ(mod.current_waypoint_index(), 2u);
    EXPECT_TRUE(mod.holding_station());
    EXPECT_NEAR(mod.station_elapsed_s(), 0.0, 1e-9);

    // Fly the circuit (teleport to each corner; each update accrues
    // 10 s of station time). Capturing C3 (index 4, the span's last
    // corner) with the timer still running WRAPS back to the anchor.
    int wraps = 0;
    for (int i = 0; i < 60; ++i) {
        const std::size_t idx = mod.current_waypoint_index();
        if (idx == 1u) {
            s = make_state(0, 49000, 10000);        // anchor
        } else if (idx == 2u) {
            s = make_state(0, 99000, 10000);        // C1
        } else if (idx == 3u) {
            s = make_state(49000, 100000, 10000);   // C2
        } else if (idx == 4u) {
            s = make_state(50000, 51000, 10000);    // C3 (loop end)
        } else {
            break;                                   // released (POST)
        }
        mod.update(10.0, s.get());
        if (idx == 4u && mod.current_waypoint_index() == 1u) ++wraps;
    }

    // The hold ran to its contract (120 s at 10 s a tick), wrapped the
    // circuit at least twice, then released out of the span — the
    // active waypoint left the loop and the module flies on to POST.
    EXPECT_GE(wraps, 2);
    EXPECT_FALSE(mod.holding_station());
    EXPECT_GE(mod.station_elapsed_s(), 120.0);
    EXPECT_EQ(mod.current_waypoint_index(), 5u);

    // The route still completes after the hold (POST capture).
    s = make_state(0, 149000, 10000);
    mod.update(0.1, s.get());
    EXPECT_TRUE(mod.is_complete());
}

TEST(NavigationStationHold, RouteWithoutTheContractBehavesIdentically) {
    // The same geometry with the station fields zero: no hold, no
    // wrap — the pre-P7 shape (every saved route and every
    // pre-strategy synthetic route).
    NavigationModule mod;
    mod.set_route(make_racetrack_route(0.0, 0));

    auto s = make_state(0, 49000, 10000);
    mod.update(0.1, s.get());
    EXPECT_EQ(mod.current_waypoint_index(), 2u);
    EXPECT_FALSE(mod.holding_station());

    s = make_state(0, 99000, 10000);
    mod.update(0.1, s.get());
    EXPECT_EQ(mod.current_waypoint_index(), 3u);
    EXPECT_FALSE(mod.holding_station());

    s = make_state(49000, 100000, 10000);
    mod.update(0.1, s.get());
    EXPECT_EQ(mod.current_waypoint_index(), 4u);

    s = make_state(50000, 51000, 10000);
    mod.update(0.1, s.get());
    // C3 captured with no contract: NO wrap — straight to POST.
    EXPECT_EQ(mod.current_waypoint_index(), 5u);
    EXPECT_FALSE(mod.holding_station());
}

TEST(NavigationStationHold, ContractWithoutALoopIsInert) {
    // station_time_s > 0 with loop_waypoints < 2: no circuit to fly —
    // the contract is inert (the builder only arms holds on ≥4-WP
    // spans; the module defends against malformed routes).
    NavigationModule mod;
    mod.set_route(make_racetrack_route(120.0, 1));

    auto s = make_state(0, 49000, 10000);
    mod.update(0.1, s.get());
    EXPECT_EQ(mod.current_waypoint_index(), 2u);
    EXPECT_FALSE(mod.holding_station());
}

TEST(NavigationStationHold, SetRouteResetsTheHold) {
    NavigationModule mod;
    mod.set_route(make_racetrack_route(120.0, 4));
    auto s = make_state(0, 49000, 10000);
    mod.update(0.1, s.get());
    ASSERT_TRUE(mod.holding_station());

    // A re-tasked module must not inherit the previous hold's state.
    mod.set_route(make_racetrack_route(0.0, 0));
    EXPECT_FALSE(mod.holding_station());
    EXPECT_NEAR(mod.station_elapsed_s(), 0.0, 1e-9);
    EXPECT_EQ(mod.current_waypoint_index(), 0u);
}

// ============================================================================
// EMPL-1a — the attack run (delivery-leg steering + delivery altitude)
// ============================================================================

namespace {

NavigationModule::Waypoint make_strike_wp(const std::string& name, double east,
                                          double north, double alt) {
    NavigationModule::Waypoint wp =
        make_wp(name, east, north, alt);
    wp.action = 17;  // WP_STRIKE
    wp.target_id = 42;
    return wp;
}

} // anonymous namespace

TEST(NavigationAttackRun, VirtualLegConvergesLateralOffset) {
    // The EMPL-1a defect, kinematically: pure pursuit of a stationary
    // point CONSERVES its entry lateral offset almost to the target (the
    // TestCamp stick released ~6 deg off bearing: 675 of its 682-889 ft
    // miss was lateral). The virtual leg's cross-track law must DRIVE
    // the offset to zero well before the release boundary.
    NavigationModule mod;
    const double aim_e = 0.0, aim_n = 60000.0;
    mod.set_route({make_strike_wp("STRIKE", aim_e, aim_n, 10000.0)});

    // Engage 3,000 ft east of the direct north line to the aim, pointed
    // north (the offset the corner handed over).
    TestAircraftState s;
    s.east_ft = 3000.0; s.north_ft = 0.0;
    s.alt_msl_ft = 10000.0; s.alt_agl_ft_ = 10000.0;
    s.heading_rad_ = 0.0; s.vcas_kts_ = 400.0;

    // Kinematic march with an IDEALIZED heading response: the state's
    // heading tracks the module's command instantly (the steering law's
    // geometry is what's under test, not the FCS's lag).
    const double speed = 400.0 * 1.68781;
    const double dt = 0.5;
    const int steps = static_cast<int>(58000.0 / (speed * dt));
    for (int i = 0; i < steps; ++i) {
        (void)mod.update(dt, &s);              // caches + engages the anchor
        s.heading_rad_ = mod.nav_heading_rad(); // idealized heading response
        s.east_ft += std::sin(s.heading_rad_) * speed * dt;
        s.north_ft += std::cos(s.heading_rad_) * speed * dt;
    }
    // Lateral offset from the engagement->aim line (engagement was at
    // (3000, 0); the line runs to (0, 60000)).
    const double lex = aim_e - 3000.0, ley = aim_n - 0.0;
    const double llen = std::sqrt(lex * lex + ley * ley);
    const double rx = ley / llen, ry = -lex / llen;
    const double xte = (s.east_ft - 3000.0) * rx + (s.north_ft - 0.0) * ry;

    // The aircraft stopped ~2,000 ft short of the aim (the release
    // boundary's neighborhood). Pursuit-style conservation would carry
    // ~2,900 ft of offset here; the virtual leg must be inside 200 ft.
    EXPECT_LT(std::abs(xte), 200.0)
        << "attack run must converge the lateral offset (pursuit conserves it)";
    // And it must still be short of the aim (flying TOWARD it, not past).
    EXPECT_LT(s.north_ft, aim_n);
}

TEST(NavigationAttackRun, OnLineCommandsHoldTheAnchoredCourse) {
    // The anchor freezes at engagement: an aircraft ON the virtual leg
    // keeps commanding the SAME course across ticks (no pursuit slew —
    // the nose does not chase the aim as the distance closes).
    NavigationModule mod;
    mod.set_route({make_strike_wp("STRIKE", 0.0, 60000.0, 10000.0)});

    // Start 60,000 ft south, exactly on the line, heading north.
    TestAircraftState s;
    s.east_ft = 0.0; s.north_ft = 0.0;
    s.alt_msl_ft = 10000.0; s.alt_agl_ft_ = 10000.0;
    s.heading_rad_ = 0.0; s.vcas_kts_ = 400.0;

    (void)mod.update(0.5, &s);
    const double h1 = mod.nav_heading_rad();
    // Fly 20,000 ft straight up the line and ask again.
    s.north_ft += 20000.0;
    (void)mod.update(0.5, &s);
    const double h2 = mod.nav_heading_rad();
    EXPECT_NEAR(h1, h2, 1e-9)
        << "anchored course must not slew (pursuit would re-point at the aim)";
    EXPECT_NEAR(h1, 0.0, 1e-6);  // due north — the engagement->aim course
}

TEST(NavigationAttackRun, ShortEngagementFallsBackToPursuit) {
    // Engaged inside attack_min_virtual_leg_ft of the aim, the anchored
    // course is bearing-noise: the law falls back to pursuit, whose
    // commanded heading CHANGES as the aircraft closes (the opposite of
    // the anchored-course constancy above).
    NavigationModule mod;
    mod.set_route({make_strike_wp("STRIKE", 0.0, 60000.0, 10000.0)});
    mod.attack_min_virtual_leg_ft = 2000.0;
    // Stay ToWaypoint: the capture floor is max(radius, 10*vcas), so a
    // 400-kt state captures the aim from 4,000 ft — before the fallback
    // is observable. 150 kts floors the capture at 1,500 ft.
    mod.capture_radius_ft = 500.0;

    TestAircraftState s;
    s.east_ft = 1000.0; s.north_ft = 58100.0;   // ~1,900 ft from the aim
    s.alt_msl_ft = 10000.0; s.alt_agl_ft_ = 10000.0;
    s.heading_rad_ = 0.0; s.vcas_kts_ = 150.0;

    (void)mod.update(0.5, &s);
    const double h1 = mod.nav_heading_rad();
    // Fly 200 ft straight north (NOT along the bearing — pursuit along
    // its own bearing to a stationary aim never changes the bearing).
    s.north_ft += 200.0;                        // ~1,720 ft out
    (void)mod.update(0.5, &s);
    const double h2 = mod.nav_heading_rad();
    EXPECT_NE(h1, h2)
        << "pursuit fallback must re-point as the aircraft closes";
}

TEST(NavigationAttackRun, DeliveryAltitudeFliesTheWaypointNotTheFloor) {
    // The bridge floors delivery waypoints at 1,500 ft MSL for the
    // release envelope; the 3,000 ft terrain floor used to override it
    // and double the throw. A delivery waypoint must command its OWN
    // altitude (level at 1,500), while a plain waypoint at the same
    // altitude still climbs to the floor (the floor's regression guard).
    double delivery_pitch = 0.0;
    {
        NavigationModule mod;
        NavigationModule::Waypoint egress = make_wp("EGRESS", 0, 120000, 10000);
        mod.set_route({make_strike_wp("STRIKE", 0.0, 60000.0, 1500.0),
                       egress});
        // Level AT the delivery altitude AND at the commanded speed — the
        // speed channel rides pitch too (energy exchange), so a speed
        // error would mask the altitude assertion.
        auto s = make_state(0, 40000, 1500, 0.0, 350.0);
        const auto out = mod.update(0.1, s.get());
        delivery_pitch = out.pitch_cmd;
        EXPECT_LT(std::abs(delivery_pitch), 0.005)
            << "delivery waypoint must fly its own 1,500 ft (no climb to the floor)";
    }
    {
        NavigationModule mod;
        mod.set_route({make_wp("ENROUTE", 0.0, 60000.0, 1500.0),
                       make_wp("EGRESS", 0.0, 120000.0, 10000.0)});
        auto s = make_state(0, 40000, 1500, 0.0, 350.0);
        const auto out = mod.update(0.1, s.get());
        // The floor's climb is gentle in the calm enroute tune (a 1,500-ft
        // error rides ~0.003 pitch) — assert the ORDERING, not an
        // absolute: the floored leg climbs, the delivery leg does not.
        EXPECT_GT(out.pitch_cmd, delivery_pitch + 0.001)
            << "non-delivery legs keep the terrain floor (climb toward 3,000)";
    }
}

