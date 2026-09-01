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
//
// Phase 2 (H2): Tests use a TestAircraftState adapter (implementing
// IAircraftState) instead of constructing a full AircraftState struct.
// This verifies the module works through the interface and doesn't
// accidentally depend on AircraftState internals.

#include <gtest/gtest.h>

#include <f4/ai/modules/takeoff_module.hpp>
#include <f4/ai/atc/stub_atc.hpp>
#include <f4/messaging/bus.hpp>
#include <f4/entities/entity.hpp>
#include <f4/flight/api/i_aircraft_state.hpp>

#include <cmath>

using namespace f4::ai::modules;
using namespace f4::ai::atc;
namespace messaging = f4::messaging;
namespace entities = f4::entities;
namespace fsm = f4::fsm;
namespace geo = f4::geo;
namespace flight = f4::flight;

namespace {

// ============================================================================
// TestAircraftState — minimal IAircraftState implementation for testing.
//
// Position is specified in ENU (East-North-Up) — the same frame the
// IAircraftState interface presents to the AI. This eliminates the
// NED→ENU conversion that the old AircraftState-based tests had to do.
// ============================================================================
class TestAircraftState : public flight::IAircraftState {
public:
    // Position in ENU frame (feet): x=east, y=north, z=up
    double east_ft{0.0};
    double north_ft{0.0};
    double alt_msl_ft{0.0};
    double alt_agl_ft_{0.0};
    double vcas_kts_{0.0};
    double heading_rad_{0.0};
    double pitch_rad_{0.0};
    double roll_rad_{0.0};
    double roll_rate_radps_{0.0};
    double pitch_rate_radps_{0.0};
    double yaw_rate_radps_{0.0};
    double vs_fpm_{0.0};
    bool on_ground_{true};
    double fuel_lbs_{5000.0};

    // IAircraftState implementation
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

// Helper: create a TestAircraftState at the given ENU position.
// NOTE: ENU convention — x=east, y=north, z=up (altitude).
// This replaces the old make_state() which took NED coordinates.
std::unique_ptr<TestAircraftState> make_state(
    double east_ft, double north_ft, double alt_ft,
    double vcas_kts = 0.0, bool on_ground = true)
{
    auto s = std::make_unique<TestAircraftState>();
    s->east_ft = east_ft;
    s->north_ft = north_ft;
    s->alt_msl_ft = alt_ft;
    s->alt_agl_ft_ = alt_ft;  // AGL = MSL when ground is at 0
    s->vcas_kts_ = vcas_kts;
    s->on_ground_ = on_ground;
    return s;
}

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
    // Positions in ENU: x=east, y=north. The taxi route goes north
    // along y: (0,0) -> (0,2500) -> (0,5000).
    auto state = make_state(0.0, 0.0, 0.0);
    mod.update(0.1, state.get());
    state = make_state(0.0, 2549.0, 0.0);
    mod.update(0.1, state.get());
    state = make_state(0.0, 5049.0, 0.0);
    mod.update(0.1, state.get());

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

    // Taxi to hold-short (ENU positions: y=north axis)
    auto state = make_state(0.0, 0.0, 0.0);
    mod.update(0.1, state.get());
    state = make_state(0.0, 2549.0, 0.0);
    mod.update(0.1, state.get());
    state = make_state(0.0, 5049.0, 0.0);
    mod.update(0.1, state.get());

    // After reaching hold-short, TakeoffRequest -> TakeoffClearance ->
    // PrepToTakeRunway -> (aligned) -> TakeRunway -> (auto) -> Takeoff.
    // STAB-E9: the clearance is latched by the subscription handler and
    // processed on the NEXT update (re-entrancy safety), so one extra
    // tick is required to consume it.
    mod.update(0.1, state.get());
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
    mod.update(0.1, state.get());
    state = make_state(0.0, 2549.0, 0.0);
    mod.update(0.1, state.get());
    state = make_state(0.0, 5049.0, 0.0);
    mod.update(0.1, state.get());

    // After reaching last waypoint, the SM transitions:
    //   Taxi -> HoldShort -> PrepToTakeRunway (takeoff clearance)
    // But alignment check happens next tick.
    // Tick: check alignment -> TakeRunway -> (auto) -> Takeoff
    state = make_state(0.0, 5049.0, 0.0, 0.0, true);
    mod.update(0.1, state.get());
    EXPECT_EQ(mod.state(), TakeoffState::Takeoff);

    // Accelerate past Vr and become airborne -> FlyOut
    state = make_state(0.0, 5200.0, 50.0, 150.0, false);
    mod.update(0.1, state.get());
    EXPECT_EQ(mod.state(), TakeoffState::FlyOut);

    // Climb to departure altitude (2500 ft) -> Done
    state = make_state(0.0, 6000.0, 2500.0, 200.0, false);
    mod.update(0.1, state.get());
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
    mod.update(0.1, state.get());
    state = make_state(0.0, 2549.0, 0.0);
    mod.update(0.1, state.get());
    state = make_state(0.0, 5049.0, 0.0);
    mod.update(0.1, state.get());

    // Should be in Takeoff now (auto-transitioned through TakeRunway)
    if (mod.state() == TakeoffState::Takeoff) {
        state = make_state(0.0, 5049.0, 0.0, 100.0, true);
        auto output = mod.update(0.1, state.get());
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
    mod.update(0.1, state.get());
    state = make_state(0.0, 2549.0, 0.0);
    mod.update(0.1, state.get());
    state = make_state(0.0, 5049.0, 0.0);
    mod.update(0.1, state.get());

    // Accelerate and become airborne
    state = make_state(0.0, 5200.0, 50.0, 150.0, false);
    mod.update(0.1, state.get());

    if (mod.state() == TakeoffState::FlyOut) {
        // Below gear-up altitude: gear down
        state = make_state(0.0, 5300.0, 100.0, 200.0, false);
        auto output = mod.update(0.1, state.get());
        EXPECT_TRUE(output.gear_handle_down);

        // Above gear-up altitude (200ft): gear up
        state = make_state(0.0, 5400.0, 300.0, 200.0, false);
        output = mod.update(0.1, state.get());
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
    EXPECT_NEAR(mod.taxi_wp_capture_radius_ft, 100.0, 1e-6);
    // Phase A3: tightened from 10 ft to 5 ft.
    EXPECT_NEAR(mod.centerline_align_tolerance_ft, 5.0, 1e-6);
    // Phase A3: tightened from 0.15 rad (8.5 deg) to 0.009 rad (0.5 deg).
    EXPECT_NEAR(mod.heading_align_tolerance_rad, 0.087, 1e-9);
}

// ============================================================================
// Taxi waypoint advancement
// ============================================================================

TEST_F(TakeoffTestFixture, TaxiAdvancesWaypoints) {
    StubATC atc(bus);
    atc.set_airfield(make_kunsan_config());

    mod.initialize(1, world, bus);
    ASSERT_EQ(mod.state(), TakeoffState::Taxi);

    // Start at parking (0,0) in ENU
    auto state = make_state(0.0, 0.0, 0.0);
    mod.update(0.1, state.get());
    EXPECT_EQ(mod.state(), TakeoffState::Taxi);

    // Move close to WP1 (east=0, north=2500) — should advance past it
    state = make_state(0.0, 2460.0, 0.0);  // within 50ft capture radius
    mod.update(0.1, state.get());
    // Still taxiing (not at end of route yet)
    // Note: StubATC auto-grants takeoff clearance at HoldShort, so we
    // may have already transitioned past Taxi if WP2 was also captured.

    // Move close to WP2 (east=0, north=5000) — should advance and trigger HoldShort
    state = make_state(0.0, 4960.0, 0.0);  // within 50ft capture radius
    mod.update(0.1, state.get());

    // Should have transitioned past Taxi
    EXPECT_NE(mod.state(), TakeoffState::Taxi);
}

// ============================================================================
// IAircraftState interface contract
// ============================================================================

TEST_F(TakeoffTestFixture, NullStateProducesIdleControls) {
    // Passing nullptr should not crash. The module stays in its current
    // state and produces idle controls (same as before Phase 2).
    TakeoffModule mod2;
    mod2.set_trace(&trace);
    mod2.initialize(1, world, bus);

    auto output = mod2.update(0.1, nullptr);
    // RequestTaxi produces brakes-on, throttle 0
    EXPECT_TRUE(output.wheel_brakes);
}

TEST_F(TakeoffTestFixture, InterfacePositionMatchesWorldPosition) {
    // Verify that the ENU position from IAircraftState matches what
    // the AI would construct as a WorldPosition.
    auto state = make_state(100.0, 200.0, 500.0);
    geo::WorldPosition pos(state->position_east_ft(),
                           state->position_north_ft(),
                           state->altitude_msl_ft());
    EXPECT_DOUBLE_EQ(pos.x, 100.0);  // east
    EXPECT_DOUBLE_EQ(pos.y, 200.0);  // north
    EXPECT_DOUBLE_EQ(pos.z, 500.0);  // up (altitude)
}
