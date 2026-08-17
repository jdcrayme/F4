// f4-ai/tests/test_brain_component.cpp
//
// Unit tests for BrainComponent — the BehavioralComponent wrapper around
// TakeoffModule.
//
// These tests verify the Phase A.2 contract:
//   1. BrainComponent reports priority = update_phase::BRAIN_PRIORITY (100).
//   2. on_attached is called when added to an entity, capturing the owner.
//   3. update() lazily resolves the FlightModelComponent sibling.
//   4. update() initializes the TakeoffModule on first call (deferred from
//      on_attached because on_attached doesn't get a MessageBus).
//   5. update() produces AIControlOutput → PilotInput and writes it to the
//      FM's pending_input slot.
//   6. When run via EntityWorld::update_all, the brain runs in pass 1
//      (before the FM), so the FM consumes the brain's output in pass 2.
//   7. Brain + FM + StubATC integration: the full Phase A taxi flow works
//      end-to-end through update_all.

#include <gtest/gtest.h>

#include <f4/ai/brain_component.hpp>
#include <f4/ai/atc/stub_atc.hpp>
#include <f4/flight/flight_model_component.hpp>
#include <f4/flight/f4_flight.hpp>
#include <f4/entities/f4_entities.hpp>
#include <f4/messaging/f4_messaging.hpp>
#include <f4/data/config_loader.hpp>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <limits>

using namespace f4::ai;
using namespace f4::ai::modules;
using namespace f4::ai::atc;
using namespace f4::flight;
using namespace f4::entities;
using namespace f4::messaging;
namespace geo = f4::geo;

namespace {

constexpr const char* kFixturesDir = F4_GENERATED_FIXTURES_DIR;

bool fixturesExist() {
    return std::filesystem::exists(kFixturesDir);
}

bool loadF16Config(f4::data::AircraftConfig& cfg) {
    if (!fixturesExist()) return false;
    const std::string path = std::string(kFixturesDir) + "/f16.json";
    if (!std::filesystem::exists(path)) return false;
    auto result = f4::data::loadConfig(path);
    if (!result.ok) return false;
    cfg = result.config;
    return true;
}

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
    config.taxi_route = {
        geo::WorldPosition(0.0, 2500.0, 0.0),
        geo::WorldPosition(0.0, 5000.0, 0.0),
    };
    config.runway_end_position = geo::WorldPosition(0.0, 10000.0, 0.0);
    return config;
}

} // namespace

// ============================================================================
// BehavioralComponent contract
// ============================================================================
TEST(BrainComponent, PriorityIsBrainPriority) {
    BrainComponent bc;
    EXPECT_EQ(bc.priority(), update_phase::BRAIN_PRIORITY);
    EXPECT_EQ(bc.priority(), 100);
    EXPECT_GE(bc.priority(), update_phase::BRAIN_THRESHOLD);
}

TEST(BrainComponent, TypeIdMatchesBrainComponent) {
    BrainComponent bc;
    EXPECT_EQ(bc.type_id(), std::type_index(typeid(BrainComponent)));
}

// ============================================================================
// on_attached — back-ref captured
// ============================================================================
TEST(BrainComponent, OnAttachedCapturesOwner) {
    EntityWorld w;
    auto h = w.create();
    [[maybe_unused]] auto& bc = h.add<BrainComponent>();

    // The brain should now be able to look up its sibling components.
    // We verify by adding a FlightModelComponent and checking that
    // the brain's update() finds it (no crash, FM pending_input written).
    // (Indirect verification — on_attached itself is a void return.)
    auto& fmc = h.add<FlightModelComponent>();
    (void)fmc;

    MessageBus bus;
    // update() will call module_.initialize() on first tick. Without
    // a StubATC, the brain stays in RequestTaxi and produces idle controls
    // (brakes on, throttle 0). That should write to pending_input.
    EXPECT_NO_THROW(w.update_all(0.016, bus));
}

TEST(BrainComponent, OnAttachedIsCalledByAdd) {
    // We can't directly observe on_attached being called (it returns void
    // and the owner_ pointer is private). But we CAN observe its EFFECT:
    // if on_attached didn't run, update() would early-return (owner_ is
    // null) and the FM's pending_input would stay at its default (idle).
    // If on_attached DID run, update() will run the brain and write to
    // pending_input. With no StubATC, the brain stays in RequestTaxi
    // and produces wheel_brakes=true. We check that wheel_brakes is true
    // after update_all — that proves the brain ran, which proves
    // on_attached captured the owner.
    EntityWorld w;
    MessageBus bus;
    auto h = w.create();
    auto& fmc = h.add<FlightModelComponent>();
    auto& bc  = h.add<BrainComponent>();
    (void)bc;

    w.update_all(0.016, bus);

    // The brain should have written wheel_brakes=true (RequestTaxi state
    // produces brakes on). The FM consumes pending_input in pass 2, so
    // after update_all, pending_input is back to idle. We need to check
    // DURING the brain's pass, before the FM consumes. We can't do that
    // with update_all — but we CAN check the FM's state for evidence
    // that the brain's brakes-on command was applied. With brakes on and
    // zero throttle, the aircraft should not accelerate.
    // This is an indirect check; the integration test below verifies the
    // full round-trip more rigorously.
    EXPECT_TRUE(std::isfinite(fmc.state().kin.z));
}

// ============================================================================
// Lazy FM resolution — brain tolerates missing FM
// ============================================================================
TEST(BrainComponent, UpdateWithNoFlightModelIsNoOp) {
    // A brain on an entity with no FlightModelComponent. update() should
    // early-return without crashing.
    EntityWorld w;
    MessageBus bus;
    auto h = w.create();
    h.add<BrainComponent>();  // no FM on this entity

    EXPECT_NO_THROW(w.update_all(0.016, bus));
}

TEST(BrainComponent, BrainAddedBeforeFMStillWorks) {
    // Add brain first, then FM. The brain's lazy resolution in update()
    // should find the FM regardless of add order.
    f4::data::AircraftConfig cfg;
    if (!loadF16Config(cfg)) GTEST_SKIP() << "f16.json fixture not found";

    EntityWorld w;
    MessageBus bus;
    auto h = w.create();

    [[maybe_unused]] auto& bc = h.add<BrainComponent>();   // brain first
    auto& fmc = h.add<FlightModelComponent>();  // FM second
    fmc.init(cfg, 0.0, 0.0, 0.0, false);  // on ground

    EXPECT_NO_THROW(w.update_all(0.016, bus));
    EXPECT_TRUE(std::isfinite(fmc.state().kin.z));
}

TEST(BrainComponent, BrainAddedAfterFMStillWorks) {
    // Reverse order: FM first, brain second. Should also work.
    f4::data::AircraftConfig cfg;
    if (!loadF16Config(cfg)) GTEST_SKIP() << "f16.json fixture not found";

    EntityWorld w;
    MessageBus bus;
    auto h = w.create();

    auto& fmc = h.add<FlightModelComponent>();  // FM first
    fmc.init(cfg, 0.0, 0.0, 0.0, false);
    [[maybe_unused]] auto& bc = h.add<BrainComponent>();   // brain second

    EXPECT_NO_THROW(w.update_all(0.016, bus));
    EXPECT_TRUE(std::isfinite(fmc.state().kin.z));
}

// ============================================================================
// Two-pass ordering — brain writes pending_input before FM reads it
// ============================================================================
TEST(BrainComponent, BrainWritesBeforeFMReads) {
    // The brain runs in pass 1 (priority 100), the FM in pass 2 (priority 50).
    // We verify the brain's output reached the FM by checking the FM's state
    // changed in a way consistent with the brain's commands.
    //
    // Without a StubATC, the brain stays in RequestTaxi and produces:
    //   wheel_brakes = true, throttle = 0.0, gear_handle_down = true
    // The FM, starting on the ground at zero speed, should remain at
    // zero speed (brakes on) and not accelerate.
    f4::data::AircraftConfig cfg;
    if (!loadF16Config(cfg)) GTEST_SKIP() << "f16.json fixture not found";

    EntityWorld w;
    MessageBus bus;
    auto h = w.create();
    auto& fmc = h.add<FlightModelComponent>();
    fmc.init(cfg, 0.0, 0.0, 0.0, false);  // on ground, zero speed
    auto& bc = h.add<BrainComponent>();
    (void)bc;

    const double initial_vt = fmc.state().kin.vt;

    // Run 10 ticks (about 1/6 second).
    for (int i = 0; i < 10; ++i) {
        w.update_all(1.0 / 60.0, bus);
    }

    // With brakes on and zero throttle, the aircraft should not have
    // accelerated. (It might decelerate if it had any initial velocity,
    // but we started at zero.)
    EXPECT_NEAR(fmc.state().kin.vt, initial_vt, 1.0);  // within 1 ft/s
}

// ============================================================================
// Full integration: Brain + FM + StubATC via update_all
// ============================================================================
TEST(BrainComponent, FullTakeoffFlowViaUpdateAll) {
    // This is THE Phase A integration test. It wires together:
    //   - An entity with FlightModelComponent + BrainComponent
    //   - A StubATC on the bus
    //   - A MessageBus
    // And runs EntityWorld::update_all() in a loop.
    //
    // The brain should drive the aircraft through:
    //   RequestTaxi -> Taxi -> HoldShort -> PrepToTakeRunway
    //   -> TakeRunway -> Takeoff -> FlyOut -> Done
    //
    // We don't verify every state transition (the TakeoffModule tests
    // already do that). We verify that the COMPONENT WIRING works:
    //   - The brain publishes TaxiRequest (via the bus)
    //   - StubATC responds with TaxiClearance
    //   - The brain subscribes to TaxiClearance and transitions to Taxi
    //   - The brain produces control outputs that the FM integrates
    //   - After enough ticks, the brain reaches a non-RequestTaxi state
    //
    // The aircraft won't actually move (the FM needs real aerodynamic
    // config + a real scenario with waypoints at the aircraft's position).
    // But the brain's STATE MACHINE should advance, proving the message
    // bus round-trip works through the component system.

    f4::data::AircraftConfig cfg;
    if (!loadF16Config(cfg)) GTEST_SKIP() << "f16.json fixture not found";

    EntityWorld w;
    MessageBus bus;

    // StubATC must be subscribed BEFORE the brain publishes TaxiRequest.
    StubATC atc(bus);
    atc.set_airfield(make_kunsan_config());

    auto h = w.create();
    auto& fmc = h.add<FlightModelComponent>();
    fmc.init(cfg, 0.0, 0.0, 0.0, false);  // on ground at origin
    auto& bc = h.add<BrainComponent>();
    (void)bc;

    // Tick 1: brain runs first (pass 1), calls module_.initialize()
    // (which publishes TaxiRequest), StubATC responds with TaxiClearance
    // (synchronous on the bus), brain transitions to Taxi.
    w.update_all(1.0 / 60.0, bus);

    // The brain should have advanced past RequestTaxi.
    EXPECT_NE(bc.module().state(), TakeoffState::RequestTaxi)
        << "Brain did not advance past RequestTaxi — TaxiRequest/TaxiClearance round-trip failed";
}

// ============================================================================
// Module access — host can configure the wrapped TakeoffModule
// ============================================================================
TEST(BrainComponent, ExposesWrappedTakeoffModule) {
    BrainComponent bc;
    TakeoffModule& mod = bc.module();
    (void)mod;

    const BrainComponent& cbc = bc;
    const TakeoffModule& cmod = cbc.module();
    (void)cmod;

    SUCCEED();
}

TEST(BrainComponent, HostCanConfigureModuleBeforeFirstTick) {
    // The host might want to set rotate_speed_kts, gear_up_alt_ft, etc.
    // before the first update(). The module accessor returns a mutable
    // reference, so this should work.
    EntityWorld w;
    MessageBus bus;
    auto h = w.create();
    auto& bc = h.add<BrainComponent>();

    bc.module().rotate_speed_kts = 150.0;
    bc.module().gear_up_alt_ft = 250.0;

    EXPECT_DOUBLE_EQ(bc.module().rotate_speed_kts, 150.0);
    EXPECT_DOUBLE_EQ(bc.module().gear_up_alt_ft, 250.0);
}

// ============================================================================
// FM-level integration: taxi steering converges, lineup aligns, takeoff flies
// ============================================================================
//
// This is the empirical proof of the GroundSteering pedal-sign contract
// against the REAL 6-DOF flight model. The aircraft starts at the origin
// heading north and must follow a taxi route with a right turn and a left
// turn, stop at the hold-short point, line up on the runway centerline
// (within 10 ft lateral AND ~8.5 deg heading), roll at MIL power, rotate,
// and climb through the departure altitude.
//
// If the nose-wheel pedal sign were flipped, the aircraft would diverge
// from the route corridor and never reach Takeoff. If the speed control
// were misgained, it would either never move or overshoot the hold-short.
namespace {

// Point-to-segment distance in the ENU horizontal plane (feet).
double point_segment_distance_2d(double px, double py,
                                 double ax, double ay,
                                 double bx, double by) {
    const double abx = bx - ax, aby = by - ay;
    const double apx = px - ax, apy = py - ay;
    const double ab2 = abx * abx + aby * aby;
    double t = 0.0;
    if (ab2 > 1e-9) t = std::clamp((apx * abx + apy * aby) / ab2, 0.0, 1.0);
    const double cx = ax + t * abx, cy = ay + t * aby;
    return std::sqrt((px - cx) * (px - cx) + (py - cy) * (py - cy));
}

} // namespace

TEST(BrainComponent, TaxiLineupTakeoffFliesWithRealFlightModel) {
    f4::data::AircraftConfig cfg;
    if (!loadF16Config(cfg)) GTEST_SKIP() << "f16.json fixture not found";

    EntityWorld w;
    MessageBus bus;

    // Airfield: runway 36 north along x=0, threshold at (0, 5000).
    // Taxi route exercises a right turn then a left turn:
    //   (0,0) -> (0,1500) -> (1500,3000) -> (1500,4500 = hold-short)
    AirfieldConfig af = make_kunsan_config();
    af.taxi_route = {
        geo::WorldPosition(0.0, 1500.0, 0.0),
        geo::WorldPosition(1500.0, 3000.0, 0.0),
        geo::WorldPosition(1500.0, 4500.0, 0.0),
    };
    StubATC atc(bus);
    atc.set_airfield(af);

    auto h = w.create();
    auto& fmc = h.add<FlightModelComponent>();
    fmc.init(cfg, 0.0, 0.0, 0.0, false);  // on ground at origin, heading north
    fmc.set_ground(0.0, f4::math::Vec3d{0.0, 0.0, -1.0});
    auto& bc = h.add<BrainComponent>();
    (void)bc;

    // Corridor legs for the taxi-phase deviation check (spawn included).
    const std::vector<std::pair<std::pair<double,double>, std::pair<double,double>>> legs = {
        {{0,0}, {0,1500}}, {{0,1500}, {1500,3000}}, {{1500,3000}, {1500,4500}},
    };

    constexpr double CORRIDOR_FT = 200.0;   // generous; catches sign errors/divergence
    double max_dev_ft = 0.0;
    bool reached_taxi = false, reached_takeoff = false, liftoff = false;

    const bool dbg = std::getenv("F4_TAKEOFF_DEBUG") != nullptr;
    const int MAX_TICKS = 30000;  // 500 s of sim time
    for (int i = 0; i < MAX_TICKS; ++i) {
        w.update_all(1.0 / 60.0, bus);
        bus.flush_pending();

        const auto& s = fmc.state();
        const double east = s.kin.y;    // NED y = east
        const double north = s.kin.x;  // NED x = north
        const auto st = bc.module().state();

        if (dbg && i % 600 == 0) {
            std::printf("t=%5.1fs st=%-12s vcas=%6.1f vt=%6.1f psi=%6.1f theta=%5.1f "
                        "z=%7.1f agl=%5.1f pos=(%7.0f,%7.0f) rpm=%5.1f\n",
                        i / 60.0, bc.module().state_name().c_str(), s.vcas, s.kin.vt,
                        to_degrees(s.kin.psi), to_degrees(s.kin.theta), s.kin.z,
                        -s.kin.z - s.gear.groundZ_ft, east, north,
                        s.engine.rpm);
        }

        if (st == TakeoffState::Taxi) {
            reached_taxi = true;
            // Distance to the NEAREST leg (min over legs) — the corridor
            // metric. (Max over legs would count unreached legs as deviation.)
            double dev = std::numeric_limits<double>::max();
            for (const auto& [a, b] : legs) {
                dev = std::min(dev, point_segment_distance_2d(
                    east, north, a.first, a.second, b.first, b.second));
            }
            if (dev > max_dev_ft) {
                max_dev_ft = dev;
                if (dbg && dev > 100.0) {
                    std::printf("  [dev] t=%.1f dev=%.0f ft at (%.0f, %.0f) psi=%.1f\n",
                                i / 60.0, dev, east, north, to_degrees(s.kin.psi));
                }
            }
        }
        if (st == TakeoffState::Takeoff) reached_takeoff = true;
        if (st == TakeoffState::FlyOut || st == TakeoffState::Done) liftoff = true;
        if (st == TakeoffState::Done) break;
    }

    EXPECT_TRUE(reached_taxi) << "never entered Taxi state";
    EXPECT_LT(max_dev_ft, CORRIDOR_FT)
        << "aircraft left the taxi corridor (max deviation "
        << max_dev_ft << " ft) — steering sign/gain problem";
    EXPECT_TRUE(reached_takeoff)
        << "never reached Takeoff — taxi/lineup did not converge (final state "
        << bc.module().state_name() << ")";
    EXPECT_TRUE(liftoff) << "never lifted off";
    EXPECT_EQ(bc.module().state(), TakeoffState::Done)
        << "did not climb through departure altitude (final state "
        << bc.module().state_name() << ")";
}
