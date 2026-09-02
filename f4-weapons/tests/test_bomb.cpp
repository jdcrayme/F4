// test_bomb.cpp — the A-G employment slice's weapon-layer tests.
//
// Two layers, mirroring test_missile.cpp / test_engagement.cpp:
//   1. The pure flyout (Bomb): vacuum ballistics, drag, the impact plane,
//      the interpolation of the impact point, the tof limit.
//   2. The ECS chain (release_bomb → BombSimComponent → objective feature
//      damage): store debit, entity creation + tags, the fall, the impact,
//      the per-feature damage ledger, the fstatus bitmap sync (FreeFalcon
//      VIS semantics), the value-weighted destroyed %, the messages, the
//      sweep.

#include <f4/weapons/bomb.hpp>
#include <f4/weapons/bomb_battery.hpp>
#include <f4/weapons/messages.hpp>
#include <f4/weapons/weapon_class_table.hpp>
#include <f4/weapons/weapon_store.hpp>

#include <gtest/gtest.h>

#include <cmath>
#include <vector>

using namespace f4::weapons;

namespace entities = f4::entities;
namespace messaging = f4::messaging;

namespace {

constexpr double kDt = 1.0 / 120.0;

BombConfig slick_mk82() {
    const auto table = WeaponClassTable::with_builtins();
    return BombConfig::from_record(
        *table.get(table.find_by_name("MK-82")));
}

/// Aggregate of every bomb event observed on the bus.
struct BombEventLog {
    std::vector<BombReleasedMessage> released;
    std::vector<BombImpactMessage> impacts;

    void attach(messaging::MessageBus& bus) {
        bus.subscribe<BombReleasedMessage>(
            [this](const BombReleasedMessage& m) { released.push_back(m); });
        bus.subscribe<BombImpactMessage>(
            [this](const BombImpactMessage& m) { impacts.push_back(m); });
    }
};

/// A shooter: transform + store + team tag, at 10,000 ft flying +x at
/// 800 fps (a representative delivery speed).
entities::EntityHandle spawn_bomber(entities::EntityWorld& world,
                                    std::uint32_t bomb_handle, int rounds) {
    auto h = world.create();
    auto& tc = h.add<entities::TransformComponent>();
    tc.position = f4::geo::WorldPosition{0.0, 0.0, 10000.0};
    tc.vx = 800.0;
    auto& store = h.add<WeaponStoreComponent>();
    store.add_station(bomb_handle, rounds, "station 3");
    store.add_station(bomb_handle, rounds, "station 7");
    h.set_tag(entities::tags::TEAM, entities::TagValue::from(std::string("blue")));
    h.add<entities::CampaignIdentityComponent>();
    return h;
}

/// An objective: transform + feature set (placements + class hp) + an
/// fstatus bitmap. Features spread along +x at 500 ft spacing from the
/// center; the target sits at (10000, 0) so a bomb dropped from the
/// bomber's release solution lands among them.
entities::EntityHandle spawn_objective(entities::EntityWorld& world,
                                        double center_x, int n_features,
                                        int hp) {
    auto h = world.create();
    auto& tc = h.add<entities::TransformComponent>();
    tc.position = f4::geo::WorldPosition{center_x, 0.0, 0.0};
    auto& fs = h.add<entities::FeatureSetComponent>();
    fs.features_count = static_cast<std::uint8_t>(n_features);
    for (int i = 0; i < n_features; ++i) {
        entities::FeatureEntryState f;
        f.hit_points = static_cast<std::int16_t>(hp);
        f.value = 10;      // equal weights
        f.offset_x = static_cast<float>(i) * 500.0f - 1000.0f;
        f.offset_y = 0.0f;
        fs.features.push_back(f);
    }
    auto& bitmap = h.add<entities::DamageBitmapComponent>();
    bitmap.fstatus.assign((n_features + 3) / 4, 0);
    return h;
}

} // namespace

// ============================================================================
// 1. The pure flyout
// ============================================================================

TEST(Bomb, VacuumFallTimeAndRange) {
    // 5,000 ft to give up: t = sqrt(2*5000/32.174) = 17.63 s.
    const double t = bomb_fall_time_s(5000.0);
    EXPECT_NEAR(t, 17.632, 0.01);
    // 800 fps level release: vacuum range = 800 * 17.632 = 14,105 ft.
    EXPECT_NEAR(bomb_vacuum_range_ft(5000.0, 800.0), 14105.0, 5.0);
    EXPECT_DOUBLE_EQ(bomb_fall_time_s(0.0), 0.0);
    EXPECT_DOUBLE_EQ(bomb_fall_time_s(-100.0), 0.0);
}

TEST(Bomb, LevelReleaseFallsToPlaneInVacuumTime) {
    Bomb b;
    BombConfig cfg = slick_mk82();
    cfg.cd = 0.0;  // vacuum: no drag
    cfg.ref_area_ft2 = 0.0;
    b.release(cfg, f4::geo::WorldPosition{0.0, 0.0, 5000.0},
              f4::math::Vec3<double>{800.0, 0.0, 0.0}, 0.0);

    const double t_vacuum = bomb_fall_time_s(5000.0);
    double t = 0.0;
    while (!b.terminal() && t < 2.0 * t_vacuum) {
        b.tick(kDt);
        t += kDt;
    }
    ASSERT_EQ(b.status(), BombStatus::Impact);
    // Fall time within one tick of the closed form.
    EXPECT_NEAR(b.flight_time_s(), t_vacuum, kDt * 1.5);
    // Horizontal distance within ~1% of the closed form.
    EXPECT_NEAR(b.ground_range_ft(), 800.0 * t_vacuum, 0.01 * 800.0 * t_vacuum);
    // The impact point sits exactly on the plane.
    EXPECT_NEAR(b.position().z, 0.0, 1.0e-9);
}

TEST(Bomb, DragShortensTheRange) {
    Bomb vacuum, dragged;
    BombConfig cfg_v = slick_mk82();
    cfg_v.cd = 0.0;
    cfg_v.ref_area_ft2 = 0.0;
    vacuum.release(cfg_v, f4::geo::WorldPosition{0.0, 0.0, 5000.0},
                   f4::math::Vec3<double>{800.0, 0.0, 0.0}, 0.0);
    dragged.release(slick_mk82(), f4::geo::WorldPosition{0.0, 0.0, 5000.0},
                    f4::math::Vec3<double>{800.0, 0.0, 0.0}, 0.0);

    auto fly = [](Bomb& b) {
        double t = 0.0;
        while (!b.terminal() && t < 60.0) { b.tick(kDt); t += kDt; }
    };
    fly(vacuum);
    fly(dragged);
    ASSERT_EQ(vacuum.status(), BombStatus::Impact);
    ASSERT_EQ(dragged.status(), BombStatus::Impact);
    EXPECT_LT(dragged.ground_range_ft(), vacuum.ground_range_ft());
    // A slick Mk-82's drag is real but not catastrophic: still > 60% of
    // vacuum range at 800 fps from 5,000 ft.
    EXPECT_GT(dragged.ground_range_ft(), 0.6 * vacuum.ground_range_ft());
}

TEST(Bomb, ReleaseAtOrBelowPlaneIsImmediateImpact) {
    Bomb b;
    b.release(slick_mk82(), f4::geo::WorldPosition{100.0, 50.0, -10.0},
              f4::math::Vec3<double>{0.0, 0.0, 0.0}, 0.0);
    EXPECT_EQ(b.status(), BombStatus::Impact);
    EXPECT_DOUBLE_EQ(b.flight_time_s(), 0.0);
    // Terminal ticks are no-ops.
    b.tick(kDt);
    EXPECT_EQ(b.status(), BombStatus::Impact);
}

TEST(Bomb, TofLimitExpiresBeforePlane) {
    // A plane far below (a bomb that can never reach it in 90 s from a
    // slow release): the tof limit ends the flight.
    Bomb b;
    BombConfig cfg = slick_mk82();
    b.release(cfg, f4::geo::WorldPosition{0.0, 0.0, 100000.0},
              f4::math::Vec3<double>{0.0, 0.0, 0.0}, 0.0);
    // Released with zero velocity from very high: it accelerates under
    // gravity but 90 s of fall covers ~0.5*32.174*90^2 = 130,304 ft of
    // vacuum distance; drag caps it lower, so from 100,000 ft... it may
    // actually impact. Use a tof limit of 5 s instead to force expiry.
    BombConfig cfg2 = slick_mk82();
    cfg2.tof_limit_s = 5.0;
    Bomb b2;
    b2.release(cfg2, f4::geo::WorldPosition{0.0, 0.0, 100000.0},
               f4::math::Vec3<double>{0.0, 0.0, 0.0}, 0.0);
    double t = 0.0;
    while (!b2.terminal() && t < 20.0) { b2.tick(kDt); t += kDt; }
    EXPECT_EQ(b2.status(), BombStatus::Expired);
    (void)b;
}

// ============================================================================
// 2. The ECS chain — release → fall → impact → objective damage
// ============================================================================

TEST(BombBattery, ReleaseRefusesNonBombCategory) {
    const auto table = WeaponClassTable::with_builtins();
    entities::EntityWorld world;
    messaging::MessageBus bus;
    auto shooter = spawn_bomber(world, table.find_by_name("AIM-120C"), 4);
    // AIM-120C is not a Bomb-category card — refused, nothing happens.
    const auto id = release_bomb(world, bus, shooter, entities::EntityId{},
                                 table, table.find_by_name("AIM-120C"), 1.0);
    EXPECT_FALSE(id.valid());
    EXPECT_EQ(world.with_component<WeaponStoreComponent>().size(), 1u);
    EXPECT_EQ(count_live_bombs(world), 0u);
}

TEST(BombBattery, ReleaseDebitsStoreCreatesEntityPublishes) {
    const auto table = WeaponClassTable::with_builtins();
    const auto bomb_handle = table.find_by_name("MK-82");
    entities::EntityWorld world;
    messaging::MessageBus bus;
    BombEventLog log;
    log.attach(bus);
    auto shooter = spawn_bomber(world, bomb_handle, 4);

    const auto id = release_bomb(world, bus, shooter, entities::EntityId{},
                                 table, bomb_handle, 10.0);
    ASSERT_TRUE(id.valid());

    // Store debited by one round (4 -> 3 on station 3).
    const auto* store = shooter.get<WeaponStoreComponent>();
    ASSERT_NE(store, nullptr);
    EXPECT_EQ(store->station(0)->rounds, 3);

    // Entity carries the bomb contract: transform, BombComponent, tags.
    entities::EntityHandle bomb(id, &world);
    ASSERT_NE(bomb.get<BombComponent>(), nullptr);
    const auto role = bomb.get_tag(entities::tags::ROLE);
    ASSERT_TRUE(role.has_value());
    ASSERT_NE(role->as_string(), nullptr);
    EXPECT_EQ(*role->as_string(), "bomb");
    EXPECT_EQ(count_live_bombs(world), 1u);

    // The release message flowed.
    ASSERT_EQ(log.released.size(), 1u);
    EXPECT_EQ(log.released[0].weapon_handle, bomb_handle);
    EXPECT_EQ(log.released[0].shooter_id, shooter.id().value);
}

TEST(BombBattery, ImpactDamagesFeaturesAndSyncsFstatus) {
    const auto table = WeaponClassTable::with_builtins();
    const auto bomb_handle = table.find_by_name("MK-82");
    entities::EntityWorld world;
    messaging::MessageBus bus;
    BombEventLog log;
    log.attach(bus);
    BombSimComponent::set_sim_time(0.0);

    // Objective at (10000, 0): five features at x = 9000..11000, hp 100.
    auto objective = spawn_objective(world, 10000.0, 5, 100);

    // Bomber at (0,0,5000) flying +x at 800 fps. Vacuum release range is
    // 800 * sqrt(2*5000/32.174) = 14,105 ft — beyond the target; use the
    // drag solution: release at ~10,300 ft gives a close-in impact with
    // drag. Instead of hand-solving, release from a position that makes
    // the aim point exact: drop from 5,000 ft at the drag-corrected
    // pull-up point. For the test, simply release 10,000 ft short: the
    // miss distance will be nonzero but the features at 9,000..11,000 ft
    // are all within the 300 ft lethal radius of SOME impact point only
    // if the impact lands among them — so aim AT the objective and
    // verify damage rather than the exact release geometry.
    auto shooter = world.create();
    {
        auto& tc = shooter.add<entities::TransformComponent>();
        // The drag-corrected release point for 5,000 ft / 800 fps lands
        // within ~1,000 ft of the aim: release from x = 12,000 (short
        // of 10,000... no: x = 12,000 is PAST the target). Release from
        // x = 12,000 flying -x? Keep it simple: release overhead.
        tc.position = f4::geo::WorldPosition{10000.0, 0.0, 5000.0};
        tc.vx = 0.0;
        auto& store = shooter.add<WeaponStoreComponent>();
        store.add_station(bomb_handle, 4, "station 3");
        shooter.set_tag(entities::tags::TEAM,
                        entities::TagValue::from(std::string("blue")));
    }

    const auto id = release_bomb(world, bus, shooter, objective.id(),
                                 table, bomb_handle, 10.0);
    ASSERT_TRUE(id.valid());

    // Fly to impact.
    double t = 0.0;
    while (count_live_bombs(world) > 0 && t < 60.0) {
        BombSimComponent::set_sim_time(t);
        world.update_all(kDt, bus);
        sweep_spent_bombs(world);
        t += kDt;
    }
    EXPECT_EQ(count_live_bombs(world), 0u);

    // The impact message carries the damage summary.
    ASSERT_EQ(log.impacts.size(), 1u);
    const auto& impact = log.impacts[0];
    EXPECT_EQ(impact.cause, BombEndCause::Impact);
    EXPECT_EQ(impact.target_id, objective.id().value);
    // Released directly overhead with no forward velocity: the impact
    // lands at the objective center (miss ~ 0).
    EXPECT_NEAR(impact.miss_distance_ft, 0.0, 200.0);
    // The middle feature (offset 0) took the full warhead; neighbors
    // catch the falloff. The 500 ft spacing exceeds the 300 ft lethal
    // radius, so only the center feature is in radius.
    EXPECT_EQ(impact.features_destroyed, 1);
    EXPECT_NEAR(impact.destroyed_pct, 20.0, 0.5);  // 1 of 5, equal weights

    // The ledger + the wire state agree.
    const auto* fs = entities::EntityHandle(objective.id(), &world)
                         .get<entities::FeatureSetComponent>();
    ASSERT_NE(fs, nullptr);
    ASSERT_EQ(fs->features.size(), 5u);
    EXPECT_EQ(fs->features[2].damage_state, 3);       // center: VIS_DESTROYED
    EXPECT_EQ(fs->features[0].damage_state, 0);       // out of radius: normal
    ASSERT_EQ(fs->feature_hp.size(), 5u);
    EXPECT_NEAR(fs->feature_hp[2], 0.0, 1.0e-9);
    EXPECT_NEAR(fs->feature_hp[0], 100.0, 1.0e-9);

    // fstatus bitmap: feature 2 (byte 0, shift 4) = 3.
    const auto* bitmap = entities::EntityHandle(objective.id(), &world)
                             .get<entities::DamageBitmapComponent>();
    ASSERT_NE(bitmap, nullptr);
    ASSERT_GE(bitmap->fstatus.size(), 1u);
    EXPECT_EQ((bitmap->fstatus[0] >> 4) & 0x03, 3);

    // Summary reads the same state back out.
    const auto summary = objective_damage_summary(world, objective.id().value);
    EXPECT_TRUE(summary.objective_found);
    EXPECT_EQ(summary.features_total, 5);
    EXPECT_EQ(summary.features_destroyed_total, 1);
    EXPECT_NEAR(summary.destroyed_pct, 20.0, 0.5);
}

TEST(BombBattery, DamagedFeatureBelowHalfStrengthIsVisDamaged) {
    entities::EntityWorld world;
    messaging::MessageBus bus;
    BombSimComponent::set_sim_time(0.0);

    // One feature with 400 hp (spawn_objective places the single feature
    // at offset x = -1000 from the center): the 192-lb warhead at ~120 ft
    // burst range deals ~115 damage — survived, but marked VIS_DAMAGED
    // (FreeFalcon marks a feature damaged on the first blast it survives).
    auto objective = spawn_objective(world, 0.0, 1, 400);
    const auto result = apply_objective_feature_damage(
        world, objective.id().value,
        f4::geo::WorldPosition{-880.0, 0.0, 0.0},
        192.0, 300.0, 0.5);
    EXPECT_TRUE(result.objective_found);
    EXPECT_EQ(result.features_damaged, 1);
    EXPECT_EQ(result.features_destroyed, 0);
    EXPECT_NEAR(result.destroyed_pct, 0.0, 1.0e-9);
    EXPECT_GT(result.damage_applied, 100.0);   // ~115 lb of blast
    const auto* fs = entities::EntityHandle(objective.id(), &world)
                         .get<entities::FeatureSetComponent>();
    EXPECT_EQ(fs->features[0].damage_state, 2);  // VIS_DAMAGED
    EXPECT_NEAR(fs->feature_hp[0], 400.0 - result.damage_applied, 1.0e-9);
}

TEST(BombBattery, DestroyedFeatureTakesNoFurtherDamage) {
    entities::EntityWorld world;
    auto objective = spawn_objective(world, 0.0, 1, 100);
    // First blast kills it (full warhead, zero range — the feature sits at
    // offset x = -1000 from the objective center).
    auto r1 = apply_objective_feature_damage(
        world, objective.id().value,
        f4::geo::WorldPosition{-1000.0, 0.0, 0.0},
        192.0, 300.0, 0.5);
    EXPECT_EQ(r1.features_destroyed, 1);
    EXPECT_GT(r1.damage_applied, 0.0);
    // Second blast on the rubble: nothing.
    auto r2 = apply_objective_feature_damage(
        world, objective.id().value,
        f4::geo::WorldPosition{-1000.0, 0.0, 0.0},
        192.0, 300.0, 0.5);
    EXPECT_EQ(r2.features_destroyed, 0);
    EXPECT_EQ(r2.damage_applied, 0.0);
    EXPECT_NEAR(r2.destroyed_pct, 100.0, 1.0e-9);
}

TEST(BombBattery, MissingObjectiveReportsNotFound) {
    entities::EntityWorld world;
    const auto result = apply_objective_feature_damage(
        world, 12345u, f4::geo::WorldPosition{0.0, 0.0, 0.0},
        192.0, 300.0, 0.5);
    EXPECT_FALSE(result.objective_found);
}
