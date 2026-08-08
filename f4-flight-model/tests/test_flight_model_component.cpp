// f4-flight-model/tests/test_flight_model_component.cpp
//
// Unit tests for FlightModelComponent — the BehavioralComponent wrapper
// around FlightModel.
//
// These tests verify the Phase A.2 contract:
//   1. The component reports priority = update_phase::PHYSICS_PRIORITY (50).
//   2. pending_input is the channel the brain writes to; it is consumed
//      and reset to idle by update().
//   3. ground_z / ground_normal slots are settable and forwarded to the
//      FlightModel.
//   4. The component is registered with EntityWorld::update_all() and
//      runs in pass 2 (after brains).
//   5. When attached to an entity via EntityHandle::add<FlightModelComponent>(),
//      on_attached is NOT called (it's not a behavioral brain; the
//      EntityHandle back-ref isn't needed by the FM). Actually — the
//      if constexpr in add<T>() calls on_attached for ALL BehavioralComponents,
//      including FlightModelComponent. The default on_attached is a no-op,
//      so this is fine. The test verifies add() works without throwing.
//
// The tests that exercise update() use the F16 config from generated
// fixtures; tests that only check wrapper mechanics skip the FM init().

#include <gtest/gtest.h>

#include <f4/flight/f4_flight.hpp>
#include <f4/entities/f4_entities.hpp>
#include <f4/messaging/f4_messaging.hpp>
#include <f4/data/config_loader.hpp>

#include <cmath>
#include <filesystem>

using namespace f4::flight;
using namespace f4::entities;
using namespace f4::messaging;
using f4::math::Vec3d;

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

} // namespace

// ============================================================================
// BehavioralComponent contract
// ============================================================================
TEST(FlightModelComponent, PriorityIsPhysicsPriority) {
    FlightModelComponent fmc;
    EXPECT_EQ(fmc.priority(), update_phase::PHYSICS_PRIORITY);
    EXPECT_EQ(fmc.priority(), 50);
    EXPECT_LT(fmc.priority(), update_phase::BRAIN_THRESHOLD);
    EXPECT_GT(fmc.priority(), 0);
}

TEST(FlightModelComponent, TypeIdMatchesFlightModelComponent) {
    FlightModelComponent fmc;
    EXPECT_EQ(fmc.type_id(), std::type_index(typeid(FlightModelComponent)));
}

TEST(FlightModelComponent, DefaultsToIdlePilotInput) {
    FlightModelComponent fmc;
    // Default-constructed PilotInput has throttle=0, brakes off, gear down.
    const auto& pi = fmc.pending_input();
    EXPECT_DOUBLE_EQ(pi.throttle, 0.0);
    EXPECT_FALSE(pi.wheelBrakes);
    EXPECT_DOUBLE_EQ(pi.gearHandle, 1.0);  // default down
}

TEST(FlightModelComponent, DefaultsToFlatGroundAtZeroAltitude) {
    FlightModelComponent fmc;
    EXPECT_DOUBLE_EQ(fmc.ground_z_ft(), 0.0);
    // NED frame: up = (0, 0, -1)
    EXPECT_DOUBLE_EQ(fmc.ground_normal().x, 0.0);
    EXPECT_DOUBLE_EQ(fmc.ground_normal().y, 0.0);
    EXPECT_DOUBLE_EQ(fmc.ground_normal().z, -1.0);
}

// ============================================================================
// pending_input slot — brain writes, FM consumes
// ============================================================================
TEST(FlightModelComponent, PendingInputIsWritable) {
    FlightModelComponent fmc;
    auto& pi = fmc.pending_input();
    pi.throttle = 0.8;
    pi.pstick = 0.2;
    pi.wheelBrakes = true;

    EXPECT_DOUBLE_EQ(fmc.pending_input().throttle, 0.8);
    EXPECT_DOUBLE_EQ(fmc.pending_input().pstick, 0.2);
    EXPECT_TRUE(fmc.pending_input().wheelBrakes);
}

TEST(FlightModelComponent, GroundStateIsSettable) {
    FlightModelComponent fmc;
    fmc.set_ground(5000.0, Vec3d{0.1, 0.2, -0.97});

    EXPECT_DOUBLE_EQ(fmc.ground_z_ft(), 5000.0);
    EXPECT_DOUBLE_EQ(fmc.ground_normal().x, 0.1);
    EXPECT_DOUBLE_EQ(fmc.ground_normal().y, 0.2);
    EXPECT_DOUBLE_EQ(fmc.ground_normal().z, -0.97);
}

// ============================================================================
// FlightModel access — the wrapper exposes the underlying FM
// ============================================================================
TEST(FlightModelComponent, ExposesUnderlyingFlightModel) {
    FlightModelComponent fmc;
    FlightModel& fm = fmc.model();
    (void)fm;  // just verify it compiles & returns a reference

    const FlightModelComponent& cfmc = fmc;
    const FlightModel& cfm = cfmc.model();
    (void)cfm;
    SUCCEED();
}

// ============================================================================
// update() — drives the FM one step and resets pending_input
// ============================================================================
TEST(FlightModelComponent, UpdateResetsPendingInputToIdle) {
    f4::data::AircraftConfig cfg;
    if (!loadF16Config(cfg)) GTEST_SKIP() << "f16.json fixture not found";

    FlightModelComponent fmc;
    fmc.init(cfg, 10000.0, 500.0, 0.0, true);
    ASSERT_TRUE(fmc.model().trim());

    // Write a non-idle input.
    auto& pi = fmc.pending_input();
    pi.throttle = 0.8;
    pi.pstick = 0.1;

    MessageBus bus;
    fmc.update(1.0 / 60.0, bus);

    // After update, pending_input must be back to idle defaults.
    EXPECT_DOUBLE_EQ(fmc.pending_input().throttle, 0.0);
    EXPECT_DOUBLE_EQ(fmc.pending_input().pstick, 0.0);
    EXPECT_FALSE(fmc.pending_input().wheelBrakes);
}

TEST(FlightModelComponent, UpdateAdvancesFlightModelState) {
    f4::data::AircraftConfig cfg;
    if (!loadF16Config(cfg)) GTEST_SKIP() << "f16.json fixture not found";

    FlightModelComponent fmc;
    fmc.init(cfg, 10000.0, 500.0, 0.0, true);
    ASSERT_TRUE(fmc.model().trim());

    // Record the initial sim time (we'll detect advancement via state changes).
    // We use kin.vt as a proxy — with throttle 0.5 the aircraft should
    // accelerate or decelerate (not stay exactly at the trim speed).
    const double initial_vt = fmc.state().kin.vt;

    auto& pi = fmc.pending_input();
    pi.throttle = 0.5;

    MessageBus bus;
    constexpr double dt = 1.0 / 60.0;
    for (int i = 0; i < 60; ++i) {  // 1 second
        // Must re-write pending_input every tick — update() clears it.
        pi.throttle = 0.5;
        fmc.update(dt, bus);
    }

    // After 1 second of throttle 0.5 (not the trim throttle), the speed
    // should have changed. We don't care about direction — just that the
    // FM actually integrated something.
    EXPECT_NE(fmc.state().kin.vt, initial_vt);
}

TEST(FlightModelComponent, UpdateWithIdleInputIsSafeOnGround) {
    // If no brain writes to pending_input, update() runs the FM with
    // idle controls. For an aircraft on the ground, this is safe
    // (zero throttle, brakes off, gear down).
    f4::data::AircraftConfig cfg;
    if (!loadF16Config(cfg)) GTEST_SKIP() << "f16.json fixture not found";

    FlightModelComponent fmc;
    fmc.init(cfg, 0.0, 0.0, 0.0, false);  // on ground, zero speed

    MessageBus bus;
    constexpr double dt = 1.0 / 60.0;
    for (int i = 0; i < 10; ++i) {
        fmc.update(dt, bus);  // no pending_input write — idle controls
    }

    // Should not crash, should not produce NaNs.
    EXPECT_TRUE(std::isfinite(fmc.state().kin.z));
    EXPECT_TRUE(std::isfinite(fmc.state().kin.vt));
}

TEST(FlightModelComponent, UpdateSkipsWhenNotInitialized) {
    // A FlightModelComponent that hasn't been init()ed should silently
    // skip update() — no crash, no state change. This lets the host add
    // the component to an entity and tick the world before wiring up
    // the config (important for brain-first init ordering).
    FlightModelComponent fmc;
    EXPECT_FALSE(fmc.is_initialized());

    MessageBus bus;
    EXPECT_NO_THROW(fmc.update(1.0 / 60.0, bus));

    // Still not initialized, no state produced.
    EXPECT_FALSE(fmc.is_initialized());
}

// ============================================================================
// EntityWorld integration — update_all calls FlightModelComponent
// ============================================================================
TEST(FlightModelComponent, UpdateAllInvokesFlightModelComponent) {
    f4::data::AircraftConfig cfg;
    if (!loadF16Config(cfg)) GTEST_SKIP() << "f16.json fixture not found";

    EntityWorld w;
    MessageBus bus;
    auto h = w.create();
    auto& fmc = h.add<FlightModelComponent>();
    fmc.init(cfg, 10000.0, 500.0, 0.0, true);
    ASSERT_TRUE(fmc.model().trim());

    const double initial_vt = fmc.state().kin.vt;

    // Write pending_input, run update_all, verify FM advanced.
    auto& pi = fmc.pending_input();
    pi.throttle = 0.5;

    w.update_all(1.0 / 60.0, bus);

    // After update_all, pending_input must be reset (the FM consumed it).
    EXPECT_DOUBLE_EQ(fmc.pending_input().throttle, 0.0);
    // The FM should have integrated one step.
    // (vt might or might not change in one tick at 60Hz; just verify
    // it didn't crash and produced finite output.)
    EXPECT_TRUE(std::isfinite(fmc.state().kin.vt));
    (void)initial_vt;
}

TEST(FlightModelComponent, UpdateAllWithNoPendingInputIsSafe) {
    f4::data::AircraftConfig cfg;
    if (!loadF16Config(cfg)) GTEST_SKIP() << "f16.json fixture not found";

    EntityWorld w;
    MessageBus bus;
    auto h = w.create();
    auto& fmc = h.add<FlightModelComponent>();
    fmc.init(cfg, 0.0, 0.0, 0.0, false);

    // Don't write pending_input — update_all should still work (idle controls).
    w.update_all(1.0 / 60.0, bus);
    EXPECT_TRUE(std::isfinite(fmc.state().kin.z));
}

TEST(FlightModelComponent, UpdateAllSkipsUninitializedFM) {
    // An entity with an uninitialized FlightModelComponent. update_all
    // should not crash — the component's update() early-returns.
    EntityWorld w;
    MessageBus bus;
    auto h = w.create();
    h.add<FlightModelComponent>();  // not initialized

    EXPECT_NO_THROW(w.update_all(1.0 / 60.0, bus));
}
