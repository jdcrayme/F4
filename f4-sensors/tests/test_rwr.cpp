// test_rwr.cpp — RWR: pure model classification + sorting, then the
// world-level sweep: lock/launch transitions publish, repeats don't,
// search strobes stay component-state.

#include <f4/sensors/rwr.hpp>

#include <f4/sensors/radar_component.hpp>

#include <gtest/gtest.h>

#include <cmath>

using namespace f4::sensors;

namespace entities = f4::entities;
namespace messaging = f4::messaging;

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr double kFeetPerNm = 6076.11548;

EmitterReading radar_reading(std::uint64_t id, double x, double y, double z) {
    EmitterReading r;
    r.emitter_id = id;
    r.position = f4::geo::WorldPosition{x, y, z};
    return r;
}

} // namespace

// ============================================================================
// Pure model
// ============================================================================

TEST(RwrModel, MissileBeatsLockBeatsSearch) {
    const RwrModel model;
    const auto own = f4::geo::WorldPosition{0.0, 0.0, 0.0};

    // One emitter that is somehow all three: classified as Launch.
    std::vector<EmitterReading> readings;
    auto r = radar_reading(1, 0.0, 10000.0, 0.0);
    r.is_missile = true;
    r.is_locked_on_self = true;
    r.is_illuminating_self = true;
    readings.push_back(r);

    const auto warnings = model.evaluate(readings, own, 0.0);
    ASSERT_EQ(warnings.size(), 1u);
    EXPECT_EQ(warnings[0].type, RwrWarningType::Launch);
}

TEST(RwrModel, IgnoresUnrelatedEmittersAndFarEmitters) {
    const RwrModel model;
    const auto own = f4::geo::WorldPosition{0.0, 0.0, 0.0};

    std::vector<EmitterReading> readings;
    auto silent = radar_reading(1, 0.0, 10000.0, 0.0);   // active, not on us
    auto far = radar_reading(2, 0.0, 150.0 * kFeetPerNm, 0.0);  // 150 NM: out of range
    far.is_locked_on_self = true;
    readings.push_back(silent);
    readings.push_back(far);

    EXPECT_TRUE(model.evaluate(readings, own, 0.0).empty());
}

TEST(RwrModel, SortingLaunchFirstThenById) {
    const RwrModel model;
    const auto own = f4::geo::WorldPosition{0.0, 0.0, 0.0};

    std::vector<EmitterReading> readings;
    auto search = radar_reading(5, 0.0, 10000.0, 0.0);
    search.is_illuminating_self = true;
    auto lock1 = radar_reading(9, 10000.0, 0.0, 0.0);
    lock1.is_locked_on_self = true;
    auto lock2 = radar_reading(7, -10000.0, 0.0, 0.0);
    lock2.is_locked_on_self = true;
    auto missile = radar_reading(3, 0.0, 5000.0, 0.0);
    missile.is_missile = true;
    readings = {search, lock1, lock2, missile};

    const auto w = model.evaluate(readings, own, 1.0);
    ASSERT_EQ(w.size(), 4u);
    EXPECT_EQ(w[0].type, RwrWarningType::Launch);  // missile (3) first
    EXPECT_EQ(w[1].type, RwrWarningType::Lock);
    EXPECT_EQ(w[1].emitter_id, 7u);                // locks by id
    EXPECT_EQ(w[2].emitter_id, 9u);
    EXPECT_EQ(w[3].type, RwrWarningType::Search);
    EXPECT_NEAR(w[0].bearing_rad, 0.0, 1e-9);      // missile due north
}

TEST(RwrModel, BearingPointsAtEmitter) {
    const RwrModel model;
    const auto own = f4::geo::WorldPosition{0.0, 0.0, 0.0};

    std::vector<EmitterReading> readings;
    auto lock = radar_reading(1, 10000.0, 10000.0, 0.0);  // NE = 45 deg
    lock.is_locked_on_self = true;
    readings.push_back(lock);

    const auto w = model.evaluate(readings, own, 0.0);
    ASSERT_EQ(w.size(), 1u);
    EXPECT_NEAR(w[0].bearing_rad, kPi / 4.0, 1e-9);
    EXPECT_NEAR(w[0].range_ft, std::sqrt(2.0) * 10000.0, 1e-6);
}

// ============================================================================
// World-level sweep
// ============================================================================

/// Build a two-aircraft world: `radar_id` has a RadarSimComponent locked on
/// `victim_id` (components created directly — no scan needed).
struct RwrWorld {
    entities::EntityWorld world;
    messaging::MessageBus bus;
    entities::EntityId radar_id{};
    entities::EntityId victim_id{};

    RwrWorld() {
        auto radar = world.create();
        radar.add<entities::TransformComponent>()
             .position = f4::geo::WorldPosition{0.0, 100000.0, 20000.0};
        radar.add<RadarSimComponent>();
        radar_id = radar.id();

        auto victim = world.create();
        victim.add<entities::TransformComponent>()
              .position = f4::geo::WorldPosition{0.0, 0.0, 20000.0};
        victim.add<RwrComponent>();
        victim_id = victim.id();
    }
};

TEST(RwrSweep, LockTransitionPublishesExactlyOnce) {
    RwrWorld w;
    auto* radar = entities::EntityHandle{w.radar_id, &w.world}.get<RadarSimComponent>();

    // The victim sits due SOUTH of the radar; rotate the bar onto it and
    // run two scans so a live track exists (lock requires a track).
    radar->scan.azimuth_center_rad = kPi;
    for (int i = 0; i < 3; ++i) {
        w.world.update_all(1.0, w.bus);
    }

    // No lock yet: sweep is quiet.
    std::vector<RwrWarningMessage> messages;
    w.bus.subscribe<RwrWarningMessage>([&](const RwrWarningMessage& m) {
        messages.push_back(m);
    });
    update_rwr(w.world, w.bus, 0.0);
    EXPECT_TRUE(messages.empty());  // search strobes never publish

    // Park the antenna on the victim: next sweep publishes one Lock.
    ASSERT_TRUE(radar->command_track(w.victim_id.value));
    update_rwr(w.world, w.bus, 1.0);
    ASSERT_EQ(messages.size(), 1u);
    EXPECT_EQ(messages[0].victim_id, w.victim_id.value);
    EXPECT_EQ(messages[0].type, RwrWarningType::Lock);
    EXPECT_EQ(messages[0].emitter_id, w.radar_id.value);

    auto* rwr = entities::EntityHandle{w.victim_id, &w.world}.get<RwrComponent>();
    EXPECT_TRUE(rwr->lock_active);
    EXPECT_TRUE(rwr->new_lock);
    EXPECT_FALSE(rwr->new_launch);

    // Repeated sweep: still locked, NO new message, no new transition.
    messages.clear();
    rwr->clear_transitions();
    update_rwr(w.world, w.bus, 2.0);
    EXPECT_TRUE(messages.empty());
    EXPECT_TRUE(rwr->lock_active);
    EXPECT_FALSE(rwr->new_lock);
}

TEST(RwrSweep, SearchStrobeIsStateOnly) {
    RwrWorld w;
    auto* radar = entities::EntityHandle{w.radar_id, &w.world}.get<RadarSimComponent>();
    // Antenna remains in Search mode, default volume points north (+Y) —
    // the victim is due south... rotate the bar onto the victim instead.
    radar->scan.azimuth_center_rad = kPi;  // south

    std::vector<RwrWarningMessage> messages;
    w.bus.subscribe<RwrWarningMessage>([&](const RwrWarningMessage& m) {
        messages.push_back(m);
    });
    update_rwr(w.world, w.bus, 0.0);
    EXPECT_TRUE(messages.empty());  // search strobes never publish

    auto* rwr = entities::EntityHandle{w.victim_id, &w.world}.get<RwrComponent>();
    ASSERT_EQ(rwr->warnings.size(), 1u);
    EXPECT_EQ(rwr->warnings[0].type, RwrWarningType::Search);
    EXPECT_FALSE(rwr->lock_active);
}

TEST(RwrSweep, MissileEmitsLaunchWarning) {
    RwrWorld w;
    auto missile = w.world.create();
    missile.add<entities::TransformComponent>()
           .position = f4::geo::WorldPosition{0.0, 30000.0, 20000.0};
    missile.set_tag(entities::tags::ROLE, f4::entities::TagValue::from(std::string("missile")));

    std::vector<RwrWarningMessage> messages;
    w.bus.subscribe<RwrWarningMessage>([&](const RwrWarningMessage& m) {
        messages.push_back(m);
    });
    update_rwr(w.world, w.bus, 0.0);
    ASSERT_EQ(messages.size(), 1u);
    EXPECT_EQ(messages[0].type, RwrWarningType::Launch);
    EXPECT_EQ(messages[0].emitter_id, missile.id().value);

    auto* rwr = entities::EntityHandle{w.victim_id, &w.world}.get<RwrComponent>();
    EXPECT_TRUE(rwr->launch_active);
    EXPECT_TRUE(rwr->new_launch);

    // Missile leaves RWR range (100 NM): launch_active clears.
    missile.get<entities::TransformComponent>()->position =
        f4::geo::WorldPosition{0.0, 1.0e6, 20000.0};  // ~164 NM
    update_rwr(w.world, w.bus, 1.0);
    EXPECT_FALSE(rwr->launch_active);
}
