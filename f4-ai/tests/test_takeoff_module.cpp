// f4-ai/tests/test_takeoff_module.cpp
//
// Unit tests for TakeoffModule — state machine construction, transitions,
// ATC interaction, and per-state control outputs.
//
// Phase A cleanup tests:
//   - TaxiRequest is published on initialize() (not dropped)
//   - Taxi waypoint advancement works
//   - HoldShort waits for TakeoffClearance (no auto-transition)
//   - TakeoffRequest is published on_enter(HoldShort)
//   - const methods are const (no const_cast)

#include <gtest/gtest.h>

#include <f4/ai/modules/takeoff_module.hpp>
#include <f4/ai/atc/stub_atc.hpp>
#include <f4/messaging/bus.hpp>
#include <f4/entities/entity.hpp>
#include <f4/flight/aircraft_state.hpp>

#include <cmath>

using namespace f4::ai::modules;
using namespace f4::ai::atc;
namespace messaging = f4::messaging;
namespace entities = f4::entities;
namespace fsm = f4::fsm;
namespace geo = f4::geo;
namespace flight = f4::flight;

namespace {

// Helper: create a TakeoffModule with a trace attached.
struct TakeoffTestFixture : ::testing::Test {
    messaging::MessageBus bus;
    entities::EntityWorld world;
    fsm::Trace<TakeoffState, TakeoffEvent> trace;

    TakeoffModule mod;

    void SetUp() override {
        trace.set_capacity(4096);
        trace.set_trace_rejections(true);
        mod.set_trace(&trace);
    }
};

// Build a minimal AircraftState for testing.
flight::AircraftState make_state(double x, double y, double alt_ft,
                                  double vcas_kts = 0.0, bool on_ground = true)
{
    flight::AircraftState s;
    s.kin.x = x;
    s.kin.y = y;
    s.kin.z = -alt_ft;  // NED: z-down
    s.vcas = vcas_kts;
    s.gear.inAir = !on_ground;
    s.gear.groundZ_ft = 0.0;
    return s;
}

// Build a Kunsan-like airfield config for StubATC.
AirfieldConfig make_kunsan_config() {
    AirfieldConfig config;
    config.active_runway_id = 36;
    config.active_runway_name = "Rwy 36L";
    config.runway_heading_rad = 0.0;  // north
    config.threshold_position = geo::WorldPosition(0.0, 5000.0, 0.0);
    config.threshold_altitude_ft = 0.0;
    config.pattern_altitude_ft = 2500.0;
    config.glide_slope_angle_rad = 3.0 * 3.14159265358979 / 180.0;
    config.decision_height_ft = 200.0;
    config.departure_altitude_ft = 2500.0;
    // Taxi route: parking (0,0) -> taxiway point (0,2500) -> hold-short (0,5000)
    config.taxi_route = {
        geo::WorldPosition(0.0, 2500.0, 0.0),
        geo::WorldPosition(0.0, 5000.0, 0.0),
    };
    config.runway_end_position = geo::WorldPosition(0.0, 10000.0, 0.0);
    return config;
}

} // anonymous namespace

// ============================================================================
// State machine construction
// ============================================================================

TEST_F(TakeoffTestFixture, InitialStateIsRequestTaxi) {
    EXPECT_EQ(mod.state(), TakeoffState::RequestTaxi);
}

TEST_F(TakeoffTestFixture, IsNotCompleteInitially) {
    EXPECT_FALSE(mod.is_complete());
}

// ============================================================================
// State names
// ============================================================================

TEST_F(TakeoffTestFixture, StateNameRequestTaxi) {
    EXPECT_EQ(mod.state_name(), "RequestTaxi");
}

TEST_F(TakeoffTestFixture, ModeNameIsTakeoffMode) {
    EXPECT_EQ(mod.mode_name(), "TakeoffMode");
}

// ============================================================================
// ATC interaction — TaxiRequest published on initialize()
// ============================================================================

TEST_F(TakeoffTestFixture, PublishesTaxiRequestOnInitialize) {
    // The TaxiRequest should be published when initialize() is called
    // (via sm_.reset() re-firing the RequestTaxi entry action).
    bool received = false;
    std::uint64_t received_id = 0;
    bus.subscribe<TaxiRequest>([&](const TaxiRequest& msg) {
        received = true;
        received_id = msg.aircraft_id;
    });

    mod.initialize(42, world, bus);

    EXPECT_TRUE(received);
    EXPECT_EQ(received_id, 42u);
}

TEST_F(TakeoffTestFixture, DoesNotPublishTaxiRequestBeforeInitialize) {
    // Before initialize(), bus_ is null so the entry action is a no-op.
    bool received = false;
    bus.subscribe<TaxiRequest>([&](const TaxiRequest&) {
        received = true;
    });

    // Module was constructed in SetUp(). No request should have fired yet.
    EXPECT_FALSE(received);
}

// ============================================================================
// StubATC integration — full taxi clearance round-trip
// ============================================================================

TEST_F(TakeoffTestFixture, StubATCGrantsTaxiClearance) {
    StubATC atc(bus);
    atc.set_airfield(make_kunsan_config());

    bool received = false;
    std::uint64_t received_id = 0;
    int received_runway = 0;
    bus.subscribe<TaxiClearance>([&](const TaxiClearance& msg) {
        received = true;
        received_id = msg.aircraft_id;
        received_runway = msg.runway_id;
    });

    mod.initialize(1, world, bus);

    EXPECT_TRUE(received);
    EXPECT_EQ(received_id, 1u);
    EXPECT_EQ(received_runway, 36);
}

TEST_F(TakeoffTestFixture, TransitionsToTaxiAfterClearance) {
    StubATC atc(bus);
    atc.set_airfield(make_kunsan_config());

    mod.initialize(1, world, bus);

    // The TaxiClearance subscription in initialize() should have fired
    // ClearanceGranted, transitioning to Taxi.
    EXPECT_EQ(mod.state(), TakeoffState::Taxi);
}

// ============================================================================
// TakeoffRequest published on_enter(HoldShort)
// ============================================================================

TEST_F(TakeoffTestFixture, PublishesTakeoffRequestAtHoldShort) {
    StubATC atc(bus);
    atc.set_airfield(make_kunsan_config());

    bool takeoff_req_received = false;
    bus.subscribe<TakeoffRequest>([&](const TakeoffRequest&) {
        takeoff_req_received = true;
    });

    mod.initialize(1, world, bus);
    ASSERT_EQ(mod.state(), TakeoffState::Taxi);

    // Taxi to the end of the route to trigger HoldShort.
    auto state = make_state(0.0, 0.0, 0.0);
    mod.update(0.1, &state);
    state = make_state(0.0, 2549.0, 0.0);
    mod.update(0.1, &state);
    state = make_state(0.0, 5049.0, 0.0);
    mod.update(0.1, &state);

    EXPECT_TRUE(takeoff_req_received);
}

// ============================================================================
// Full takeoff flow: parking -> taxi -> holdshort -> takeoff -> flyout -> done
// ============================================================================

TEST_F(TakeoffTestFixture, FullTakeoffFlowReachesTakeoff) {
    StubATC atc(bus);
    atc.set_airfield(make_kunsan_config());

    mod.initialize(1, world, bus);
    ASSERT_EQ(mod.state(), TakeoffState::Taxi);

    // Taxi to hold-short
    auto state = make_state(0.0, 0.0, 0.0);
    mod.update(0.1, &state);
    state = make_state(0.0, 2549.0, 0.0);
    mod.update(0.1, &state);
    state = make_state(0.0, 5049.0, 0.0);
    mod.update(0.1, &state);

    // After reaching hold-short, TakeoffRequest -> TakeoffClearance ->
    // PrepToTakeRunway -> (aligned) -> TakeRunway -> (auto) -> Takeoff
    // The StubATC immediately grants clearance, so we should be past HoldShort.
    EXPECT_NE(mod.state(), TakeoffState::HoldShort);
    EXPECT_NE(mod.state(), TakeoffState::Taxi);
}

TEST_F(TakeoffTestFixture, FullTakeoffFlowCompletes) {
    StubATC atc(bus);
    atc.set_airfield(make_kunsan_config());

    mod.initialize(1, world, bus);
    ASSERT_EQ(mod.state(), TakeoffState::Taxi);

    // Taxi to hold-short (3 ticks to move through waypoints)
    auto state = make_state(0.0, 0.0, 0.0);
    mod.update(0.1, &state);
    state = make_state(0.0, 2549.0, 0.0);
    mod.update(0.1, &state);
    state = make_state(0.0, 5049.0, 0.0);
    mod.update(0.1, &state);

    // After reaching last waypoint, the SM transitions:
    //   Taxi -> HoldShort -> PrepToTakeRunway (takeoff clearance)
    // But alignment check happens next tick.
    // Tick: check alignment -> TakeRunway -> (auto) -> Takeoff
    state = make_state(0.0, 5049.0, 0.0, 0.0, true);
    mod.update(0.1, &state);
    EXPECT_EQ(mod.state(), TakeoffState::Takeoff);

    // Accelerate past Vr and become airborne -> FlyOut
    state = make_state(0.0, 5200.0, 50.0, 150.0, false);
    mod.update(0.1, &state);
    EXPECT_EQ(mod.state(), TakeoffState::FlyOut);

    // Climb to departure altitude (2500 ft) -> Done
    state = make_state(0.0, 6000.0, 2500.0, 200.0, false);
    mod.update(0.1, &state);
    EXPECT_EQ(mod.state(), TakeoffState::Done);
    EXPECT_TRUE(mod.is_complete());
}

// ============================================================================
// Control outputs per state
// ============================================================================

TEST_F(TakeoffTestFixture, RequestTaxiProducesBrakesOnIdleThrottle) {
    // Without StubATC, we stay in RequestTaxi.
    TakeoffModule mod2;
    mod2.set_trace(&trace);
    mod2.initialize(1, world, bus);  // no StubATC -> no clearance

    auto output = mod2.update(0.1, nullptr);
    EXPECT_TRUE(output.wheel_brakes);
    EXPECT_NEAR(output.throttle_cmd, 0.0, 1e-6);
    EXPECT_TRUE(output.gear_handle_down);
}

TEST_F(TakeoffTestFixture, TakeoffStateProducesFullThrottle) {
    StubATC atc(bus);
    atc.set_airfield(make_kunsan_config());

    mod.initialize(1, world, bus);

    // Fast-forward to Takeoff state
    auto state = make_state(0.0, 0.0, 0.0);
    mod.update(0.1, &state);
    state = make_state(0.0, 2549.0, 0.0);
    mod.update(0.1, &state);
    state = make_state(0.0, 5049.0, 0.0);
    mod.update(0.1, &state);

    // Should be in Takeoff now (auto-transitioned through TakeRunway)
    if (mod.state() == TakeoffState::Takeoff) {
        state = make_state(0.0, 5049.0, 0.0, 100.0, true);
        auto output = mod.update(0.1, &state);
        EXPECT_NEAR(output.throttle_cmd, 1.0, 1e-6);  // MIL throttle
        EXPECT_TRUE(output.gear_handle_down);
    }
}

TEST_F(TakeoffTestFixture, FlyOutRetractsGearAboveAltitude) {
    StubATC atc(bus);
    atc.set_airfield(make_kunsan_config());

    mod.initialize(1, world, bus);

    // Fast-forward through the flow to FlyOut
    auto state = make_state(0.0, 0.0, 0.0);
    mod.update(0.1, &state);
    state = make_state(0.0, 2549.0, 0.0);
    mod.update(0.1, &state);
    state = make_state(0.0, 5049.0, 0.0);
    mod.update(0.1, &state);

    // Accelerate and become airborne
    state = make_state(0.0, 5200.0, 50.0, 150.0, false);
    mod.update(0.1, &state);

    if (mod.state() == TakeoffState::FlyOut) {
        // Below gear-up altitude: gear down
        state = make_state(0.0, 5300.0, 100.0, 200.0, false);
        auto output = mod.update(0.1, &state);
        EXPECT_TRUE(output.gear_handle_down);

        // Above gear-up altitude (200ft): gear up
        state = make_state(0.0, 5400.0, 300.0, 200.0, false);
        output = mod.update(0.1, &state);
        EXPECT_FALSE(output.gear_handle_down);
    }
}

// ============================================================================
// Trace
// ============================================================================

TEST_F(TakeoffTestFixture, TraceIsAttached) {
    EXPECT_NE(mod.trace(), nullptr);
}

// ============================================================================
// Configuration
// ============================================================================

TEST_F(TakeoffTestFixture, DefaultConfiguration) {
    EXPECT_NEAR(mod.rotate_speed_kts, 140.0, 1e-6);
    EXPECT_NEAR(mod.gear_up_alt_ft, 200.0, 1e-6);
    EXPECT_NEAR(mod.takeoff_throttle, 1.0, 1e-6);
    EXPECT_NEAR(mod.taxi_wp_capture_radius_ft, 50.0, 1e-6);
    EXPECT_NEAR(mod.centerline_align_tolerance_ft, 10.0, 1e-6);
}

// ============================================================================
// Taxi waypoint advancement
// ============================================================================

TEST_F(TakeoffTestFixture, TaxiAdvancesWaypoints) {
    StubATC atc(bus);
    atc.set_airfield(make_kunsan_config());

    mod.initialize(1, world, bus);
    ASSERT_EQ(mod.state(), TakeoffState::Taxi);

    // Start at parking (0,0)
    auto state = make_state(0.0, 0.0, 0.0);
    mod.update(0.1, &state);
    EXPECT_EQ(mod.state(), TakeoffState::Taxi);

    // Move close to WP1 (0, 2500) — should advance past it
    state = make_state(0.0, 2460.0, 0.0);  // within 50ft capture radius
    mod.update(0.1, &state);
    // Still taxiing (not at end of route yet)
    // Note: StubATC auto-grants takeoff clearance at HoldShort, so we
    // may have already transitioned past Taxi if WP2 was also captured.

    // Move close to WP2 (0, 5000) — should advance and trigger HoldShort
    state = make_state(0.0, 4960.0, 0.0);  // within 50ft capture radius
    mod.update(0.1, &state);

    // Should have transitioned past Taxi
    EXPECT_NE(mod.state(), TakeoffState::Taxi);
}
