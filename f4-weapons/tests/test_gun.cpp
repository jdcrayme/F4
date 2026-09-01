// test_gun.cpp — GunStream: rate-based emission, dispersion bounds,
// ballistic integration, proximity hits, damage application, lifetime
// cleanup, burst announcements on the bus.

#include <f4/weapons/gun.hpp>
#include <f4/weapons/missile.hpp>   // kGravityFps2

#include <f4/entities/entity.hpp>
#include <f4/weapons/messages.hpp>

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>

using namespace f4::weapons;

namespace entities = f4::entities;
namespace messaging = f4::messaging;

namespace {

// A no-dispersion gun for deterministic geometry tests.
GunConfig make_gun() {
    GunConfig g;
    g.muzzle_velocity_fps = 2000.0;
    g.rounds_per_minute = 600.0;      // 10 rounds/s — easy bookkeeping
    g.dispersion_rad = 0.0;
    g.round_power_lb = 0.22;
    g.lethal_radius_ft = 40.0;        // matches kGunHitRadiusFt
    g.max_flight_s = 2.0;
    return g;
}

/// Spawn a target entity with transform + damage state. Returns its id.
entities::EntityId spawn_target(entities::EntityWorld& world, double x, double y,
                                double z, double hit_points = 10.0) {
    auto h = world.create();
    auto& tc = h.add<entities::TransformComponent>();
    tc.position = f4::geo::WorldPosition{x, y, z};
    auto& dmg = h.add<entities::DamageStateComponent>();
    dmg.hit_points = hit_points;
    dmg.max_hit_points = hit_points;
    return h.id();
}

} // namespace

TEST(GunBurst, EmitsAtConfiguredRate) {
    entities::EntityWorld world;
    GunStream gun(make_gun(), /*seed=*/1);
    gun.start_burst(5);

    const auto muzzle = f4::geo::WorldPosition{0.0, 0.0, 10000.0};
    const auto dir = f4::math::Vec3<double>{1.0, 0.0, 0.0};

    // 10 rounds/s: after 0.25 s exactly ~2.5 rounds owed -> 2 emitted.
    gun.tick(0.25, world, /*shooter=*/0, muzzle, dir);
    EXPECT_EQ(gun.tracer_count(), 2u);
    gun.tick(0.25, world, 0, muzzle, dir);
    EXPECT_EQ(gun.tracer_count(), 5u);   // burst is 5 rounds; the 0.5 carry waits
    EXPECT_EQ(gun.rounds_remaining(), 0);

    // Nothing further emits: burst exhausted.
    gun.tick(1.0, world, 0, muzzle, dir);
    EXPECT_EQ(gun.tracer_count(), 5u);
}

TEST(GunBurst, TracersFlyAndExpireByLifetime) {
    entities::EntityWorld world;
    GunConfig cfg = make_gun();
    cfg.max_flight_s = 0.5;
    GunStream gun(cfg, 1);
    gun.start_burst(1);

    const auto muzzle = f4::geo::WorldPosition{0.0, 0.0, 10000.0};
    const auto dir = f4::math::Vec3<double>{1.0, 0.0, 0.0};

    gun.tick(0.1, world, 0, muzzle, dir);
    ASSERT_EQ(gun.tracer_count(), 1u);

    // Tracer moves at 2000 fps: after 0.1 s it is ~200 ft downrange
    // (gravity has bent it slightly; check x only).
    gun.tick(0.1, world, 0, muzzle, dir);
    ASSERT_EQ(gun.tracer_count(), 1u);
    EXPECT_GT(gun.tracers()[0].position.x, 150.0);

    // Push past max_flight_s (age starts at 0.2 after two ticks): the
    // tracer self-cleans once age >= 0.5.
    for (int i = 0; i < 5; ++i) {
        gun.tick(0.1, world, 0, muzzle, dir);
    }
    EXPECT_EQ(gun.tracer_count(), 0u);
}

TEST(GunBurst, GravityBendsTracerDown) {
    entities::EntityWorld world;
    GunStream gun(make_gun(), 1);
    gun.start_burst(1);

    const auto muzzle = f4::geo::WorldPosition(0.0, 0.0, 10000.0);
    const auto dir = f4::math::Vec3<double>{1.0, 0.0, 0.0};
    gun.tick(0.1, world, 0, muzzle, dir);   // emits the round, then integrates
    ASSERT_EQ(gun.tracer_count(), 1u);

    // Semi-implicit Euler: the same tick that emits applies gravity and
    // moves with the UPDATED velocity. After tick 1:
    //   vz = -g*0.1, z = 10000 + vz*0.1
    const double vz_after_one = -kGravityFps2 * 0.1;
    EXPECT_NEAR(gun.tracers()[0].velocity.z, vz_after_one, 1e-9);
    EXPECT_NEAR(gun.tracers()[0].position.z, 10000.0 + vz_after_one * 0.1, 1e-9);
    EXPECT_LT(gun.tracers()[0].position.z, 10000.0);

    // Tick 2 adds another -g*0.1 to vz and moves with it.
    const double vz_after_two = vz_after_one - kGravityFps2 * 0.1;
    const double z_after_one = gun.tracers()[0].position.z;
    gun.tick(0.1, world, 0, muzzle, dir);
    ASSERT_EQ(gun.tracer_count(), 1u);
    EXPECT_NEAR(gun.tracers()[0].velocity.z, vz_after_two, 1e-9);
    EXPECT_NEAR(gun.tracers()[0].position.z, z_after_one + vz_after_two * 0.1, 1e-9);
}

TEST(GunHits, AppliesDamageToTargetInRange) {
    entities::EntityWorld world;
    const auto target = spawn_target(world, 500.0, 0.0, 10000.0, 10.0);

    GunConfig cfg = make_gun();
    cfg.muzzle_velocity_fps = 5000.0;
    GunStream gun(cfg, 1);
    gun.start_burst(100);

    // Fire straight at the target from 500 ft away; tracer reaches it in
    // ~0.1 s, then proximity hits apply damage.
    const auto muzzle = f4::geo::WorldPosition{0.0, 0.0, 10000.0};
    const auto dir = f4::math::Vec3<double>{1.0, 0.0, 0.0};

    std::vector<GunHit> all_hits;
    for (int i = 0; i < 30; ++i) {
        auto hits = gun.tick(0.02, world, /*shooter=*/0, muzzle, dir);
        all_hits.insert(all_hits.end(), hits.begin(), hits.end());
    }
    EXPECT_FALSE(all_hits.empty());
    bool hit_target = false;
    for (const auto& h : all_hits) {
        if (h.target_id == target.value) {
            hit_target = true;
        }
    }
    EXPECT_TRUE(hit_target);

    auto* dmg = entities::EntityHandle(target, &world)
                    .get<entities::DamageStateComponent>();
    ASSERT_NE(dmg, nullptr);
    EXPECT_LT(dmg->hit_points, 10.0);
}

TEST(GunHits, ShooterIsImmuneToOwnStream) {
    entities::EntityWorld world;
    auto shooter = world.create();
    auto& stc = shooter.add<entities::TransformComponent>();
    stc.position = f4::geo::WorldPosition{0.0, 0.0, 10000.0};
    auto& sdmg = shooter.add<entities::DamageStateComponent>();
    sdmg.hit_points = 100.0;
    sdmg.max_hit_points = 100.0;

    GunStream gun(make_gun(), 1);
    gun.start_burst(50);
    const auto muzzle = f4::geo::WorldPosition{0.0, 0.0, 10000.0};
    const auto dir = f4::math::Vec3<double>{1.0, 0.0, 0.0};
    for (int i = 0; i < 5; ++i) {
        // Muzzle sits ON the shooter: every tracer is within hit radius at
        // birth — but the shooter id is excluded.
        const auto hits = gun.tick(0.02, world, shooter.id().value, muzzle, dir);
        for (const auto& h : hits) {
            EXPECT_NE(h.target_id, shooter.id().value);
        }
    }
    auto* dmg = shooter.get<entities::DamageStateComponent>();
    ASSERT_NE(dmg, nullptr);
    EXPECT_DOUBLE_EQ(dmg->hit_points, 100.0);
}

TEST(GunHits, TargetWithoutDamageStateConsumesRoundSilently) {
    entities::EntityWorld world;
    auto bare = world.create();
    bare.add<entities::TransformComponent>()
        .position = f4::geo::WorldPosition{500.0, 0.0, 10000.0};

    GunConfig cfg = make_gun();
    cfg.muzzle_velocity_fps = 5000.0;
    GunStream gun(cfg, 1);
    gun.start_burst(100);

    const auto muzzle = f4::geo::WorldPosition{0.0, 0.0, 10000.0};
    const auto dir = f4::math::Vec3<double>{1.0, 0.0, 0.0};
    std::size_t total_hits = 0;
    for (int i = 0; i < 30; ++i) {
        total_hits += gun.tick(0.02, world, 0, muzzle, dir).size();
    }
    EXPECT_GE(total_hits, 1u);  // rounds consumed by the bare entity...
    // ...and no DamageStateComponent was created on it.
    EXPECT_EQ(bare.get<entities::DamageStateComponent>(), nullptr);
}

TEST(GunBus, PublishesBurstStartAndKillMessages) {
    entities::EntityWorld world;
    messaging::MessageBus bus;

    int fired_count = 0;
    int killed_count = 0;
    bus.subscribe<GunFiredMessage>([&fired_count](const GunFiredMessage& msg) {
        if (msg.rounds > 0) {
            ++fired_count;
        }
    });
    bus.subscribe<EntityKilledMessage>(
        [&killed_count](const EntityKilledMessage&) { ++killed_count; });

    const auto target = spawn_target(world, 300.0, 0.0, 10000.0, 0.05);  // dies on one hit

    GunConfig cfg = make_gun();
    cfg.muzzle_velocity_fps = 5000.0;
    GunStream gun(cfg, 1);
    gun.set_message_bus(&bus);
    gun.start_burst(100);

    const auto muzzle = f4::geo::WorldPosition{0.0, 0.0, 10000.0};
    const auto dir = f4::math::Vec3<double>{1.0, 0.0, 0.0};
    for (int i = 0; i < 30; ++i) {
        gun.tick(0.02, world, 0, muzzle, dir);
    }
    EXPECT_EQ(fired_count, 1);  // one announcement per burst
    EXPECT_EQ(killed_count, 1);

    auto* dmg = entities::EntityHandle(target, &world)
                    .get<entities::DamageStateComponent>();
    ASSERT_NE(dmg, nullptr);
    EXPECT_TRUE(dmg->killed);
}

TEST(GunDispersion, StaysInsideConfiguredCone) {
    // Statistical bound: every tracer's direction must stay inside the
    // configured cone around the boresight. Fire straight DOWN so gravity
    // acts along the flight direction and adds no lateral component — the
    // measured angle is then pure dispersion.
    entities::EntityWorld world;
    GunConfig cfg = make_gun();
    cfg.dispersion_rad = 0.004;
    GunStream gun(cfg, 7);
    gun.start_burst(500);

    const auto muzzle = f4::geo::WorldPosition(0.0, 0.0, 40000.0);
    const auto down = f4::math::Vec3<double>{0.0, 0.0, -1.0};

    double max_angle = 0.0;
    for (int i = 0; i < 60; ++i) {
        gun.tick(0.02, world, 0, muzzle, down);
        for (const auto& t : gun.tracers()) {
            const double speed = t.velocity.length();
            ASSERT_GT(speed, 0.0);
            const double cos_angle = t.velocity.dot(down) / speed;
            const double angle = std::acos(std::clamp(cos_angle, -1.0, 1.0));
            max_angle = std::max(max_angle, angle);
        }
    }
    // 60 ticks x 0.02 s x 10 rounds/s = 12 emitted; tracers live 2 s so most
    // are still in flight. Check we saw a real crowd and every one is in cone.
    EXPECT_EQ(gun.tracer_count(), 12u);
    EXPECT_LE(max_angle, cfg.dispersion_rad + 1e-9);
    // And the practical footprint stays small: ~24 ft lateral at 1 NM.
    EXPECT_LE(std::tan(cfg.dispersion_rad) * 6076.0, 30.0);
}

// ============================================================================
// Segment hit detection (the tunneling fix)
// ============================================================================
//
// At 3,400 ft/s a tracer covers ~57 ft per 1/60 s tick — MORE than the
// 40 ft hit radius. Point-in-sphere checks jump straight through a
// target between ticks; the segment sweep must catch it.

TEST(GunHits, FastTracerDoesNotTunnelThroughTarget) {
    entities::EntityWorld world;
    // A coarse host step (dt = 0.05 s): the tracer advances 170 ft per
    // tick — MORE than the 80 ft hit sphere. With the target at 1995 ft
    // the consecutive point positions (1870, 2040) are BOTH outside the
    // 40 ft radius: a point check jumps straight over. The segment
    // sweep must catch the crossing (closest approach = 0).
    const auto target = spawn_target(world, 1995.0, 0.0, 10000.0, 10.0);

    GunConfig cfg = make_gun();
    cfg.muzzle_velocity_fps = 3400.0;    // the real M61A1 speed
    GunStream gun(cfg, 1);
    gun.start_burst(20);

    const auto muzzle = f4::geo::WorldPosition{0.0, 0.0, 10000.0};
    const auto dir = f4::math::Vec3<double>{1.0, 0.0, 0.0};

    std::size_t hits = 0;
    for (int i = 0; i < 30; ++i) {
        hits += gun.tick(0.05, world, 0, muzzle, dir).size();
    }
    EXPECT_GE(hits, 1u) << "tracer tunneled through the target (segment "
                          "hit detection broken)";
    auto* dmg = entities::EntityHandle(target, &world)
                    .get<entities::DamageStateComponent>();
    ASSERT_NE(dmg, nullptr);
    EXPECT_LT(dmg->hit_points, 10.0);
}

TEST(GunBus, SimTimeAndAimHintStampMessages) {
    entities::EntityWorld world;
    messaging::MessageBus bus;

    double fired_sim_time = -1.0;
    std::uint64_t aim_id_seen = 999;
    std::uint32_t handle_seen = 0;
    bus.subscribe<GunFiredMessage>([&](const GunFiredMessage& msg) {
        fired_sim_time = msg.sim_time_s;
        aim_id_seen = msg.target_id;
        handle_seen = msg.weapon_handle;
    });
    double damage_time = -1.0;
    bus.subscribe<DamageAppliedMessage>(
        [&](const DamageAppliedMessage& msg) {
            damage_time = msg.sim_time_s;
        });

    spawn_target(world, 300.0, 0.0, 10000.0, 0.05);

    GunConfig cfg = make_gun();
    cfg.muzzle_velocity_fps = 5000.0;
    GunStream gun(cfg, 1);
    gun.set_message_bus(&bus);
    gun.set_sim_time(41.5);          // the host sweep's stamp
    gun.set_weapon_handle(0u);
    gun.start_burst(100, /*aim_target_id=*/77);

    const auto muzzle = f4::geo::WorldPosition{0.0, 0.0, 10000.0};
    const auto dir = f4::math::Vec3<double>{1.0, 0.0, 0.0};
    for (int i = 0; i < 30; ++i) {
        gun.tick(0.02, world, 0, muzzle, dir);
    }

    EXPECT_NEAR(fired_sim_time, 41.5, 1e-9);
    EXPECT_EQ(aim_id_seen, 77u);
    EXPECT_EQ(handle_seen, 0u);
    EXPECT_NEAR(damage_time, 41.5, 1e-9);   // damage carries the same clock
}

// ============================================================================
// update_guns — the world sweep (GunComponent-driven)
// ============================================================================

#include <f4/weapons/gun_component.hpp>

TEST(GunSweep, UpdateGunsFliesStreamsFromFreshMuzzlePose) {
    entities::EntityWorld world;
    messaging::MessageBus bus;

    // A shooter entity: transform at the origin moving EAST at 500 ft/s
    // with a GunComponent.
    auto shooter = world.create();
    auto& tf = shooter.add<entities::TransformComponent>();
    tf.position = f4::geo::WorldPosition{0.0, 0.0, 10000.0};
    tf.vx = 500.0;
    tf.vy = 0.0;
    tf.vz = 0.0;
    auto& gun = shooter.add<GunComponent>();
    GunConfig cfg = make_gun();
    cfg.muzzle_velocity_fps = 5000.0;
    gun.stream = GunStream(cfg, 3);
    gun.weapon_handle = 0;
    gun.muzzle_offset_ft = 15.0;

    // Target 1000 ft east.
    const auto target = spawn_target(world, 1000.0, 0.0, 10000.0, 0.05);

    // Start a burst (as the combat driver would), then sweep.
    gun.stream.start_burst(100, target.value);

    int fired_events = 0;
    bus.subscribe<GunFiredMessage>(
        [&fired_events](const GunFiredMessage&) { ++fired_events; });

    std::size_t total_hits = 0;
    for (int i = 0; i < 40; ++i) {
        // The shooter "moves" between sweeps: the sweep must read the
        // FRESH pose each call (muzzle + boresight from velocity).
        tf.position.x += 500.0 * 0.02;
        total_hits += update_guns(world, bus, 0.02, 1.0 + i * 0.02).size();
    }

    EXPECT_GE(total_hits, 1u);
    EXPECT_EQ(fired_events, 1);   // one announcement per burst
    auto* dmg = entities::EntityHandle(target, &world)
                    .get<entities::DamageStateComponent>();
    ASSERT_NE(dmg, nullptr);
    EXPECT_TRUE(dmg->killed);
    // The shooter never shot itself despite the muzzle sweeping from its
    // own origin.
    auto* sdmg = shooter.get<entities::DamageStateComponent>();
    EXPECT_EQ(sdmg, nullptr);
}
