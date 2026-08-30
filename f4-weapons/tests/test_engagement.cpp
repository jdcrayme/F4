// test_engagement.cpp — the M1 end-to-end combat-chain test.
//
// Shooter entity + target entity in a bare EntityWorld (no flight model —
// the target is scripted constant-velocity). launch_missile() fires an
// AIM-120C from the store; world.update_all() ticks the MissileSimComponent;
// the fuze detonates near the target; damage is applied; the target's
// DamageStateComponent flips to killed; launch/detonate/damage/killed
// messages all flow; sweep_spent_missiles() removes the missile entity.
//
// This is the exact sequence the M3 combat AI and the M4 BVR scenario will
// drive — proved here once, at the lowest layer.

#include <f4/weapons/f4_weapons.hpp>

#include <gtest/gtest.h>

#include <cmath>
#include <vector>

using namespace f4::weapons;

namespace entities = f4::entities;
namespace messaging = f4::messaging;

namespace {

constexpr double kDt = 1.0 / 120.0;

/// Aggregate of every combat event observed on the bus.
struct EventLog {
    std::vector<MissileLaunchedMessage> launched;
    std::vector<MissileDetonatedMessage> detonated;
    std::vector<DamageAppliedMessage> damage;
    std::vector<EntityKilledMessage> killed;

    void attach(messaging::MessageBus& bus) {
        bus.subscribe<MissileLaunchedMessage>(
            [this](const MissileLaunchedMessage& m) { launched.push_back(m); });
        bus.subscribe<MissileDetonatedMessage>(
            [this](const MissileDetonatedMessage& m) { detonated.push_back(m); });
        bus.subscribe<DamageAppliedMessage>(
            [this](const DamageAppliedMessage& m) { damage.push_back(m); });
        bus.subscribe<EntityKilledMessage>(
            [this](const EntityKilledMessage& m) { killed.push_back(m); });
    }
};

/// A shooter: transform + stores + team tag. Position (0,0,20000), flying
/// +x at 1200 fps.
entities::EntityHandle spawn_shooter(entities::EntityWorld& world,
                                     WeaponStoreComponent store) {
    auto h = world.create();
    auto& tc = h.add<entities::TransformComponent>();
    tc.position = f4::geo::WorldPosition{0.0, 0.0, 20000.0};
    tc.vx = 1200.0;
    h.add<WeaponStoreComponent>(std::move(store));
    h.set_tag(entities::tags::TEAM, entities::TagValue::from(std::string("blue")));
    h.add<entities::CampaignIdentityComponent>();
    return h;
}

/// A target 10 NM ahead, 20,000 ft, crossing at 500 fps with 25 HP
/// (light-fighter strength — an AIM-120C fuze-range detonation carries
/// ~30 lb-scale damage onto it; heavier targets survive with damage,
/// which is exactly the marginal-shot behavior the model intends).
entities::EntityHandle spawn_target(entities::EntityWorld& world) {
    auto h = world.create();
    auto& tc = h.add<entities::TransformComponent>();
    tc.position = f4::geo::WorldPosition{10.0 * 6076.11548, 0.0, 20000.0};
    tc.vy = 500.0;
    auto& dmg = h.add<entities::DamageStateComponent>();
    dmg.hit_points = 25.0;
    dmg.max_hit_points = 25.0;
    h.set_tag(entities::tags::TEAM, entities::TagValue::from(std::string("red")));
    return h;
}

} // namespace

TEST(Engagement, FullAirToAirChainFromLaunchToKill) {
    entities::EntityWorld world;
    messaging::MessageBus bus;
    EventLog log;
    log.attach(bus);

    WeaponClassTable table = WeaponClassTable::with_builtins();
    const auto amraam = table.find_by_name("AIM-120C");
    ASSERT_NE(amraam, kInvalidWeapon);

    auto shooter = spawn_shooter(world, WeaponStoreComponent::standard_fighter(table));
    auto target = spawn_target(world);

    // --- Launch ------------------------------------------------------------------
    MissileSimComponent::set_sim_time(0.0);
    const auto missile =
        launch_missile(world, bus, shooter, target.id(), table, amraam, 0.0);
    ASSERT_TRUE(missile.valid()) << "launch refused";

    // Store debited: 8 -> 7 AMRAAMs.
    auto* store = shooter.get<WeaponStoreComponent>();
    ASSERT_NE(store, nullptr);
    EXPECT_EQ(store->count_for(amraam), 7);

    // One launch message with correct ids.
    ASSERT_EQ(log.launched.size(), 1u);
    EXPECT_EQ(log.launched[0].missile_id, missile.value);
    EXPECT_EQ(log.launched[0].shooter_id, shooter.id().value);
    EXPECT_EQ(log.launched[0].target_id, target.id().value);
    EXPECT_EQ(log.launched[0].weapon_handle, amraam);

    // --- Tick until terminal -----------------------------------------------------
    const int max_ticks = 120 * 90;   // 90 s of sim
    int ticks = 0;
    for (; ticks < max_ticks; ++ticks) {
        // Scripted constant-velocity target motion (the ECS way: move the
        // transform; in the real sim the flight model does this).
        auto* ttc = entities::EntityHandle(target.id(), &world)
                        .get<entities::TransformComponent>();
        ASSERT_NE(ttc, nullptr);
        ttc->position.x += ttc->vx * kDt;
        ttc->position.y += ttc->vy * kDt;
        ttc->position.z += ttc->vz * kDt;

        world.update_all(kDt, bus);
        bus.flush_pending();
        MissileSimComponent::set_sim_time(static_cast<double>(ticks + 1) * kDt);
        if (log.detonated.size() == 1) {
            break;
        }
    }
    MissileSimComponent::set_sim_time(0.0);

    ASSERT_EQ(log.detonated.size(), 1u)
        << "missile never reached a terminal state in " << max_ticks * kDt << " s";

    // --- Detonation quality -------------------------------------------------------
    const auto& det = log.detonated[0];
    EXPECT_EQ(det.missile_id, missile.value);
    EXPECT_EQ(det.shooter_id, shooter.id().value);
    EXPECT_EQ(det.target_id, target.id().value);
    EXPECT_EQ(det.cause, MissileEndCause::TargetHit);
    EXPECT_LT(det.miss_distance_ft, 200.0);   // converged on a 10 NM stern/beam shot
    EXPECT_GT(det.flight_time_s, 1.0);
    EXPECT_LT(det.flight_time_s, 90.0);

    // --- Damage + kill -------------------------------------------------------------
    ASSERT_FALSE(log.damage.empty());
    EXPECT_EQ(log.damage.back().target_id, target.id().value);
    EXPECT_EQ(log.damage.back().shooter_id, shooter.id().value);
    EXPECT_TRUE(log.damage.back().killed);

    ASSERT_EQ(log.killed.size(), 1u);         // exactly one kill, ever
    EXPECT_EQ(log.killed[0].target_id, target.id().value);
    EXPECT_EQ(log.killed[0].shooter_id, shooter.id().value);

    auto* dmg = entities::EntityHandle(target.id(), &world)
                    .get<entities::DamageStateComponent>();
    ASSERT_NE(dmg, nullptr);
    EXPECT_TRUE(dmg->killed);
    EXPECT_DOUBLE_EQ(dmg->hit_points, 0.0);
    EXPECT_EQ(dmg->killed_by, shooter.id().value);

    // --- Cleanup ---------------------------------------------------------------------
    // The target entity survives (death handling belongs to higher layers).
    EXPECT_TRUE(world.alive(target.id()));
    // The spent missile sweeps away.
    EXPECT_EQ(count_live_missiles(world), 0u);
    const std::size_t removed = sweep_spent_missiles(world);
    EXPECT_EQ(removed, 1u);
    EXPECT_FALSE(world.alive(missile));
    EXPECT_EQ(sweep_spent_missiles(world), 0u);  // idempotent
}

TEST(Engagement, DryStoreRefusesLaunch) {
    entities::EntityWorld world;
    messaging::MessageBus bus;

    WeaponClassTable table = WeaponClassTable::with_builtins();
    const auto amraam = table.find_by_name("AIM-120C");

    auto shooter = spawn_shooter(world, WeaponStoreComponent::standard_fighter(table));
    auto target = spawn_target(world);

    // Empty every AMRAAM station.
    auto* store = shooter.get<WeaponStoreComponent>();
    ASSERT_NE(store, nullptr);
    for (std::size_t i = 0; i < store->station_count(); ++i) {
        store->expend(i, 100);
    }

    EventLog log;
    log.attach(bus);
    const auto missile =
        launch_missile(world, bus, shooter, target.id(), table, amraam, 0.0);
    EXPECT_FALSE(missile.valid());
    EXPECT_TRUE(log.launched.empty());
    EXPECT_EQ(count_live_missiles(world), 0u);
}

TEST(Engagement, GunCannotLaunchThroughMissilePath) {
    entities::EntityWorld world;
    messaging::MessageBus bus;
    WeaponClassTable table = WeaponClassTable::with_builtins();
    const auto gun = table.find_by_name("M61A1");

    auto shooter = spawn_shooter(world, WeaponStoreComponent::standard_fighter(table));
    auto target = spawn_target(world);

    const auto missile = launch_missile(world, bus, shooter, target.id(), table, gun, 0.0);
    EXPECT_FALSE(missile.valid());  // guns have their own employment path
}

TEST(Engagement, SeekerSourceOverrideControlsGuidance) {
    // The M2 integration point: a MissileComponent::seeker_source that
    // reports "lost lock" must drive the flyout Ballistic even though the
    // target entity is still alive and sitting right there.
    entities::EntityWorld world;
    messaging::MessageBus bus;
    WeaponClassTable table = WeaponClassTable::with_builtins();
    const auto amraam = table.find_by_name("AIM-120C");
    ASSERT_NE(amraam, kInvalidWeapon);

    auto shooter = spawn_shooter(world, WeaponStoreComponent::standard_fighter(table));
    auto target = spawn_target(world);

    MissileSimComponent::set_sim_time(0.0);
    const auto missile =
        launch_missile(world, bus, shooter, target.id(), table, amraam, 0.0);
    ASSERT_TRUE(missile.valid());

    // Blind the seeker: every tick's picture reads invalid.
    auto* mc = entities::EntityHandle(missile, &world).get<MissileComponent>();
    ASSERT_NE(mc, nullptr);
    mc->seeker_source = [](const entities::EntityWorld&, std::uint64_t) {
        return TargetSnapshot{};  // valid=false
    };

    bool went_ballistic = false;
    for (int i = 0; i < 120 * 10; ++i) {  // 10 s is plenty to notice
        world.update_all(kDt, bus);
        bus.flush_pending();
        if (mc->missile.status() == MissileStatus::Ballistic) {
            went_ballistic = true;
            break;
        }
    }
    EXPECT_TRUE(went_ballistic) << "overridden seeker had no effect";

    // Restore a valid picture (the direct read): guidance re-acquires.
    mc->seeker_source = nullptr;
    bool reacquired = false;
    for (int i = 0; i < 120 * 5 && !reacquired; ++i) {
        world.update_all(kDt, bus);
        bus.flush_pending();
        reacquired = mc->missile.status() == MissileStatus::Guided;
    }
    EXPECT_TRUE(reacquired) << "seeker did not re-acquire after override cleared";
    sweep_spent_missiles(world);
}

TEST(Engagement, KilledTargetCannotBeKilledAgainBySecondShot) {
    entities::EntityWorld world;
    messaging::MessageBus bus;
    EventLog log;
    log.attach(bus);

    WeaponClassTable table = WeaponClassTable::with_builtins();
    const auto amraam = table.find_by_name("AIM-120C");

    auto shooter = spawn_shooter(world, WeaponStoreComponent::standard_fighter(table));
    auto target = spawn_target(world);

    // Shot 1: kill it.
    const auto m1 =
        launch_missile(world, bus, shooter, target.id(), table, amraam, 0.0);
    ASSERT_TRUE(m1.valid());
    for (int i = 0; i < 120 * 90 && log.detonated.empty(); ++i) {
        auto* ttc = entities::EntityHandle(target.id(), &world)
                        .get<entities::TransformComponent>();
        ttc->position.x += ttc->vx * kDt;
        ttc->position.y += ttc->vy * kDt;
        ttc->position.z += ttc->vz * kDt;
        world.update_all(kDt, bus);
        bus.flush_pending();
    }
    sweep_spent_missiles(world);
    ASSERT_EQ(log.killed.size(), 1u);

    // Shot 2 at the corpse: flies, detonates (or expires), but must NOT
    // publish a second EntityKilledMessage.
    const std::size_t kills_before = log.killed.size();
    const auto m2 =
        launch_missile(world, bus, shooter, target.id(), table, amraam, 10.0);
    ASSERT_TRUE(m2.valid());
    for (int i = 0; i < 120 * 90 && log.detonated.size() < 2; ++i) {
        world.update_all(kDt, bus);
        bus.flush_pending();
    }
    EXPECT_EQ(log.killed.size(), kills_before);
    EXPECT_DOUBLE_EQ(
        entities::EntityHandle(target.id(), &world)
            .get<entities::DamageStateComponent>()
            ->hit_points,
        0.0);
}
