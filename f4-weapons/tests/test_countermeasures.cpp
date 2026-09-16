// test_countermeasures.cpp — dispensers, decoy entities, and the
// seeker-seduction model. Unit coverage for deploy_countermeasure (debits,
// salvo pacing, clip, IFF tags), the decoy motion + ttl sweep, and the
// decoy-aware seeker source: cone gating, one honest roll per decoy,
// sticky seduction through burnout, guidance-kind matching, team IFF,
// identity when no decoys fly — plus the full missile-vs-flare
// integration that motivated the tranche: a seduced missile detonates
// at the flare and the jet walks away (the miss-distance fix).

#include <f4/weapons/countermeasures.hpp>
#include <f4/weapons/messages.hpp>
#include <f4/weapons/weapon_class_table.hpp>

#include <f4/entities/entity.hpp>

#include <gtest/gtest.h>

#include <cmath>
#include <vector>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif // !M_PI

using namespace f4::weapons;

namespace entities = f4::entities;
namespace messaging = f4::messaging;
namespace weapons = f4::weapons;

namespace {

constexpr double kFeetPerNm = 6076.11548;

struct World {
    entities::EntityWorld world;
    messaging::MessageBus bus;

    template <typename Msg>
    std::vector<Msg> collect() {
        std::vector<Msg> out;
        bus.subscribe<Msg>([&](const Msg& m) { out.push_back(m); });
        return out;
    }
};

/// An aircraft at `pos` flying `vel`, with a full dispenser.
struct Jet {
    entities::EntityHandle h;
    Jet(World& w, f4::geo::WorldPosition pos,
        f4::math::Vec3<double> vel, const std::string& team = "blue")
        : h(w.world.create()) {
        auto& tf = h.add<entities::TransformComponent>();
        tf.position = pos;
        tf.vx = vel.x;
        tf.vy = vel.y;
        tf.vz = vel.z;
        h.add<CountermeasureComponent>();
        h.set_tag(entities::tags::TEAM,
                  entities::TagValue::from(team));
    }
};

/// A bare decoy entity (no sim component — motion tests add their own).
entities::EntityHandle make_decoy(World& w, f4::geo::WorldPosition pos,
                                  f4::math::Vec3<double> vel,
                                  const std::string& team,
                                  DecoyKind kind, double born_s,
                                  double ttl_s) {
    auto d = w.world.create();
    auto& tf = d.add<entities::TransformComponent>();
    tf.position = pos;
    tf.vx = vel.x;
    tf.vy = vel.y;
    tf.vz = vel.z;
    auto& dc = d.add<DecoyComponent>();
    dc.kind = kind;
    dc.born_s = born_s;
    dc.ttl_s = ttl_s;
    d.set_tag(entities::tags::TEAM, entities::TagValue::from(team));
    return d;
}

/// A seeker-source rig: a missile entity flying north, a target it was
/// assigned, and the factory-built closure under test.
struct SeekerRig {
    World w;
    entities::EntityHandle missile;
    entities::EntityHandle target;
    SeekerCountermeasureConfig cfg{};

    SeekerRig(f4::math::Vec3<double> missile_vel, std::uint32_t seed,
              GuidanceKind guidance = GuidanceKind::Ir) {
        missile = w.world.create();
        auto& tf = missile.add<entities::TransformComponent>();
        tf.position = f4::geo::WorldPosition{0.0, 0.0, 20000.0};
        tf.vx = missile_vel.x;
        tf.vy = missile_vel.y;
        tf.vz = missile_vel.z;
        missile.add<MissileComponent>();

        target = w.world.create();
        auto& ttf = target.add<entities::TransformComponent>();
        ttf.position = f4::geo::WorldPosition{0.0, 5000.0, 20000.0};

        cfg.guidance = guidance;
        cfg.flare_chance = 0.4;
        cfg.chaff_transfer_p = 0.5;
        cfg.seeker_half_angle_rad = 20.0 * (M_PI / 180.0);
        cfg.seeker_max_range_ft = 12.0 * kFeetPerNm;
        cfg.missile_id = missile.id().value;
        cfg.shooter_team = "red";
        cfg.rng_seed = seed;
    }

    TargetSnapshot snap() {
        auto fn = make_decoy_aware_seeker_source(cfg);
        return fn(w.world, target.id().value);
    }
};

} // namespace

// ============================================================================
// deploy_countermeasure
// ============================================================================
TEST(CountermeasureDeploy, DebitsStoreSpawnsSalvoPublishesOnce) {
    World w;
    Jet jet(w, f4::geo::WorldPosition{0.0, 0.0, 20000.0},
            f4::math::Vec3<double>{0.0, 500.0, 0.0});
    const auto deployed = w.collect<CountermeasureDeployedMessage>();

    const int n = deploy_countermeasure(w.world, w.bus, jet.h,
                                        DecoyKind::Chaff, 10.0, 0x1234);
    EXPECT_EQ(n, 2);   // the default chaff salvo
    EXPECT_EQ(deployed.size(), 1u);
    EXPECT_EQ(deployed[0].count, 2);
    EXPECT_EQ(deployed[0].owner_id, jet.h.id().value);

    auto* cm = jet.h.get<CountermeasureComponent>();
    EXPECT_EQ(cm->chaff_rounds, CountermeasureComponent::kDefaultChaffRounds - 2);
    EXPECT_EQ(count_live_decoys(w.world, DecoyKind::Chaff, 10.0), 2u);
    EXPECT_EQ(count_live_decoys(w.world, DecoyKind::Flare, 10.0), 0u);

    // The decoys fly the OWNER's colors and the "decoy" role.
    for (const auto id : w.world.with_component<DecoyComponent>()) {
        const entities::EntityHandle d(id, &w.world);
        const auto team = d.get_tag(entities::tags::TEAM);
        ASSERT_TRUE(team.has_value());
        const auto* s = team->as_string();
        ASSERT_NE(s, nullptr);
        EXPECT_EQ(*s, "blue");
        const auto role = d.get_tag(entities::tags::ROLE);
        ASSERT_TRUE(role.has_value());
        const auto* rs = role->as_string();
        ASSERT_NE(rs, nullptr);
        EXPECT_EQ(*rs, "decoy");
    }
}

TEST(CountermeasureDeploy, IntervalPacesRepeatPulls) {
    World w;
    Jet jet(w, f4::geo::WorldPosition{0.0, 0.0, 20000.0},
            f4::math::Vec3<double>{0.0, 500.0, 0.0});

    EXPECT_EQ(deploy_countermeasure(w.world, w.bus, jet.h,
                                    DecoyKind::Chaff, 10.0, 1u), 2);
    // Same tick, second pull: the dispenser refuses.
    EXPECT_EQ(deploy_countermeasure(w.world, w.bus, jet.h,
                                    DecoyKind::Chaff, 10.0, 1u), 0);
    // Half the interval: still refused.
    EXPECT_EQ(deploy_countermeasure(w.world, w.bus, jet.h,
                                    DecoyKind::Chaff, 10.25, 1u), 0);
    // Past the interval: releases again.
    EXPECT_EQ(deploy_countermeasure(w.world, w.bus, jet.h,
                                    DecoyKind::Chaff, 10.5, 1u), 2);
    auto* cm = jet.h.get<CountermeasureComponent>();
    EXPECT_EQ(cm->chaff_rounds,
              CountermeasureComponent::kDefaultChaffRounds - 4);
}

TEST(CountermeasureDeploy, DryOrClippedStoreChangesNothing) {
    World w;
    Jet jet(w, f4::geo::WorldPosition{0.0, 0.0, 20000.0},
            f4::math::Vec3<double>{0.0, 500.0, 0.0});
    auto* cm = jet.h.get<CountermeasureComponent>();
    cm->flare_rounds = 0;
    EXPECT_EQ(deploy_countermeasure(w.world, w.bus, jet.h,
                                    DecoyKind::Flare, 1.0, 1u), 0);
    EXPECT_EQ(count_live_decoys(w.world, DecoyKind::Flare, 1.0), 0u);

    // One round left: the salvo clips to it.
    cm->flare_rounds = 1;
    EXPECT_EQ(deploy_countermeasure(w.world, w.bus, jet.h,
                                    DecoyKind::Flare, 1.0, 1u), 1);
    EXPECT_EQ(cm->flare_rounds, 0);
}

TEST(CountermeasureDeploy, MissingComponentsRefuse) {
    World w;
    auto bare = w.world.create();
    bare.add<entities::TransformComponent>();
    EXPECT_EQ(deploy_countermeasure(w.world, w.bus, bare,
                                    DecoyKind::Chaff, 1.0, 1u), 0);
}

// ============================================================================
// decoy motion + ttl sweep
// ============================================================================
TEST(DecoyMotion, ChaffStallsAndFlareFalls) {
    World w;
    auto chaff = w.world.create();
    auto& ctf = chaff.add<entities::TransformComponent>();
    ctf.position = f4::geo::WorldPosition{0.0, 0.0, 20000.0};
    ctf.vy = 800.0;
    auto& cc = chaff.add<DecoyComponent>();
    cc.kind = DecoyKind::Chaff;
    chaff.add<DecoySimComponent>();

    auto flare = w.world.create();
    auto& ftf = flare.add<entities::TransformComponent>();
    ftf.position = f4::geo::WorldPosition{0.0, 0.0, 20000.0};
    ftf.vy = 800.0;
    auto& fc = flare.add<DecoyComponent>();
    fc.kind = DecoyKind::Flare;
    flare.add<DecoySimComponent>();

    for (int i = 0; i < 10; ++i) {
        w.world.update_all(0.1, w.bus);
    }

    // The chaff cloud bled most of its airframe velocity (hard drag).
    EXPECT_LT(ctf.vy, 800.0 * 0.9);
    // The flare kept more of it (light drag) and FELL.
    EXPECT_GT(ftf.vy, ctf.vy);
    EXPECT_LT(ftf.position.z, ctf.position.z);
}

TEST(DecoySweep, DestroysOnlyExpiredDecoys) {
    World w;
    const double now = 100.0;
    auto old_chaff = make_decoy(w, f4::geo::WorldPosition{0, 0, 0},
                   f4::math::Vec3<double>{0, 0, 0}, "blue",
                   DecoyKind::Chaff, 90.0, 5.0);   // expired
    auto fresh_flare = make_decoy(w, f4::geo::WorldPosition{0, 0, 0},
                   f4::math::Vec3<double>{0, 0, 0}, "blue",
                   DecoyKind::Flare, 99.0, 3.5); // alive
    (void)old_chaff;
    (void)fresh_flare;

    EXPECT_EQ(count_live_decoys(w.world, DecoyKind::Chaff, now), 0u);
    EXPECT_EQ(count_live_decoys(w.world, DecoyKind::Flare, now), 1u);

    const auto removed = sweep_expired_decoys(w.world, now);
    EXPECT_EQ(removed, 1u);
    EXPECT_EQ(w.world.with_component<DecoyComponent>().size(), 1u);
}

// ============================================================================
// the decoy-aware seeker source
// ============================================================================
TEST(SeekerSeduction, NoDecoysIsTheDirectTargetRead) {
    SeekerRig rig({0.0, 2000.0, 0.0}, 1u);
    MissileSimComponent::set_sim_time(1.0);
    const auto snap = rig.snap();
    ASSERT_TRUE(snap.valid);
    EXPECT_DOUBLE_EQ(snap.position.y, 5000.0);   // the target's position
}

TEST(SeekerSeduction, FlareInsideConeSeducesOnTheRoll) {
    // Seed sweep: mt19937's first draw with some seeds succeeds (the
    // model's 0.4 chance). Calibrate once, then PIN — the seduction
    // decision is deterministic given the seed.
    std::uint32_t seducing_seed = 0;
    bool found = false;
    for (std::uint32_t s = 1; s <= 16 && !found; ++s) {
        SeekerRig probe({0.0, 2000.0, 0.0}, s);
        // A flare dead ahead, inside the cone and range.
        make_decoy(probe.w, f4::geo::WorldPosition{0.0, 3000.0, 20000.0},
                   f4::math::Vec3<double>{0, 0, 0}, "blue",
                   DecoyKind::Flare, 0.5, 3.5);
        MissileSimComponent::set_sim_time(1.0);
        if (probe.snap().valid) {
            const auto snap = probe.snap();
            // Seduced iff the picture is the FLARE's, not the target's.
            if (std::abs(snap.position.y - 3000.0) < 1e-6) {
                seducing_seed = s;
                found = true;
            }
        }
    }
    ASSERT_NE(seducing_seed, 0u) << "no seed seduced in 1..16";

    // Pin the calibrated seed's full behavior.
    SeekerRig rig({0.0, 2000.0, 0.0}, seducing_seed);
    make_decoy(rig.w, f4::geo::WorldPosition{0.0, 3000.0, 20000.0},
                   f4::math::Vec3<double>{0, 0, 0}, "blue",
               DecoyKind::Flare, 0.5, 3.5);
    MissileSimComponent::set_sim_time(1.0);
    const auto snap = rig.snap();
    ASSERT_TRUE(snap.valid);
    EXPECT_DOUBLE_EQ(snap.position.y, 3000.0);   // the flare
}

TEST(SeekerSeduction, FlareOutsideConeNeverSeduces) {
    // The AIM-9-class 20-deg cone: a flare due EAST of a northbound
    // missile is 90 deg off the velocity axis — out, whatever the roll.
    for (std::uint32_t s = 1; s <= 16; ++s) {
        SeekerRig rig({0.0, 2000.0, 0.0}, s);
        make_decoy(rig.w, f4::geo::WorldPosition{3000.0, 0.0, 20000.0},
                   f4::math::Vec3<double>{0, 0, 0}, "blue",
                   DecoyKind::Flare, 0.5, 3.5);
        MissileSimComponent::set_sim_time(1.0);
        const auto snap = rig.snap();
        ASSERT_TRUE(snap.valid);
        EXPECT_DOUBLE_EQ(snap.position.y, 5000.0);   // still the target
    }
}

TEST(SeekerSeduction, RadarMissileTakesChaffNotFlares) {
    // Flare-only world, radar seeker: never seduced, any seed — the
    // guidance-kind gate sends flares straight past an active radar.
    for (std::uint32_t s = 1; s <= 16; ++s) {
        SeekerRig rig({0.0, 2000.0, 0.0}, s, GuidanceKind::ActiveRadar);
        make_decoy(rig.w, f4::geo::WorldPosition{0.0, 3000.0, 20000.0},
                   f4::math::Vec3<double>{0, 0, 0}, "blue",
                   DecoyKind::Flare, 0.5, 3.5);
        MissileSimComponent::set_sim_time(1.0);
        const auto snap = rig.snap();
        ASSERT_TRUE(snap.valid);
        EXPECT_DOUBLE_EQ(snap.position.y, 5000.0);
    }
}

TEST(SeekerSeduction, SameTeamDecoyNeverSeduces) {
    for (std::uint32_t s = 1; s <= 16; ++s) {
        SeekerRig rig({0.0, 2000.0, 0.0}, s);
        rig.cfg.shooter_team = "red";
        // A RED flare (the shooter's own side) dead ahead.
        make_decoy(rig.w, f4::geo::WorldPosition{0.0, 3000.0, 20000.0},
                   f4::math::Vec3<double>{0, 0, 0}, "red",
                   DecoyKind::Flare, 0.5, 3.5);
        MissileSimComponent::set_sim_time(1.0);
        const auto snap = rig.snap();
        EXPECT_DOUBLE_EQ(snap.position.y, 5000.0);
    }
}

TEST(SeekerSeduction, ExpiredDecoyIsIgnored) {
    for (std::uint32_t s = 1; s <= 16; ++s) {
        SeekerRig rig({0.0, 2000.0, 0.0}, s);
        make_decoy(rig.w, f4::geo::WorldPosition{0.0, 3000.0, 20000.0},
                   f4::math::Vec3<double>{0, 0, 0}, "blue",
                   DecoyKind::Flare, 0.0, 3.5);   // born 0, ttl 3.5
        MissileSimComponent::set_sim_time(5.0);   // now 5.0: burnt out
        const auto snap = rig.snap();
        EXPECT_DOUBLE_EQ(snap.position.y, 5000.0);
    }
}

TEST(SeekerSeduction, SeductionIsStickyThroughTheDecoysLife) {
    // Once seduced, the seeker rides the flare on EVERY subsequent
    // tick (no re-rolls, no flip-flop) until the decoy burns out —
    // then the target picture returns.
    bool exercised = false;
    for (std::uint32_t s = 1; s <= 16 && !exercised; ++s) {
        SeekerRig rig({0.0, 2000.0, 0.0}, s);
        make_decoy(rig.w, f4::geo::WorldPosition{0.0, 3000.0, 20000.0},
                   f4::math::Vec3<double>{0, 0, 0}, "blue",
                   DecoyKind::Flare, 0.5, 3.5);

        // Build the closure ONCE (it carries the state).
        auto fn = make_decoy_aware_seeker_source(rig.cfg);

        MissileSimComponent::set_sim_time(1.0);
        auto s1 = fn(rig.w.world, rig.target.id().value);
        if (std::abs(s1.position.y - 3000.0) >= 1e-6) continue;

        // Seduced: sticky on every later tick while the flare lives.
        MissileSimComponent::set_sim_time(2.0);
        auto s2 = fn(rig.w.world, rig.target.id().value);
        EXPECT_DOUBLE_EQ(s2.position.y, 3000.0);
        MissileSimComponent::set_sim_time(3.9);
        auto s3 = fn(rig.w.world, rig.target.id().value);
        EXPECT_DOUBLE_EQ(s3.position.y, 3000.0);

        // Burnout (born 0.5 + ttl 3.5 = 4.0): the seeker re-arms.
        MissileSimComponent::set_sim_time(4.5);
        auto s4 = fn(rig.w.world, rig.target.id().value);
        EXPECT_DOUBLE_EQ(s4.position.y, 5000.0);
        exercised = true;
    }
    ASSERT_TRUE(exercised) << "no seed seduced in 1..16";
}

TEST(SeekerSeduction, FailedRollNeverReRolls) {
    // One honest chance per decoy: with a seed whose first draw fails
    // the 0.4 chance, the same flare never seduces on later ticks
    // either (a 60 Hz re-roll would be certain seduction).
    bool found_failing = false;
    for (std::uint32_t s = 1; s <= 16 && !found_failing; ++s) {
        SeekerRig rig({0.0, 2000.0, 0.0}, s);
        auto fn = make_decoy_aware_seeker_source(rig.cfg);
        make_decoy(rig.w, f4::geo::WorldPosition{0.0, 3000.0, 20000.0},
                   f4::math::Vec3<double>{0, 0, 0}, "blue",
                   DecoyKind::Flare, 0.5, 60.0);
        MissileSimComponent::set_sim_time(1.0);
        auto s1 = fn(rig.w.world, rig.target.id().value);
        if (std::abs(s1.position.y - 5000.0) < 1e-6) {
            found_failing = true;
            for (double t = 2.0; t <= 10.0; t += 1.0) {
                MissileSimComponent::set_sim_time(t);
                auto st = fn(rig.w.world, rig.target.id().value);
                EXPECT_DOUBLE_EQ(st.position.y, 5000.0)
                    << "re-roll happened at t=" << t;
            }
        }
    }
    ASSERT_TRUE(found_failing) << "no failing seed in 1..16";
}

// ============================================================================
// the integration that motivated the tranche
// ============================================================================
TEST(SeductionIntegration, SeducedMissileDetonatesAtTheFlareJetSurvives) {
    World w;
    MissileSimComponent::set_sim_time(0.0);

    // Target: a static blue jet at 5000 ft north (the assigned target).
    auto target = w.world.create();
    auto& ttf = target.add<entities::TransformComponent>();
    ttf.position = f4::geo::WorldPosition{0.0, 5000.0, 20000.0};
    target.set_tag(entities::tags::TEAM,
                   entities::TagValue::from(std::string("blue")));
    auto& tdmg = target.add<entities::DamageStateComponent>();
    tdmg.hit_points = 100.0;
    tdmg.max_hit_points = 100.0;

    // Shooter: a red jet at the origin flying north (the missile
    // inherits its velocity — a dead-stop launch would never gain
    // horizontal speed under the point-mass model), with an AIM-9M.
    auto shooter = w.world.create();
    auto& stf = shooter.add<entities::TransformComponent>();
    stf.position = f4::geo::WorldPosition{0.0, 0.0, 20000.0};
    stf.vy = 800.0;
    shooter.set_tag(entities::tags::TEAM,
                    entities::TagValue::from(std::string("red")));
    auto& store = shooter.add<weapons::WeaponStoreComponent>();
    const auto table = WeaponClassTable::with_builtins();
    const auto aim9 = table.find_by_name("AIM-9M");
    ASSERT_NE(aim9, kInvalidWeapon);
    store.add_station(aim9, 1, "wingtip");

    const auto missile_id = launch_missile(
        w.world, w.bus, shooter, entities::EntityId{target.id().value},
        table, aim9, 0.0);
    ASSERT_TRUE(missile_id.valid());

    // A flare on the missile's flight path (static — the unit-level
    // decoy; the motion is covered above). 3000 ft ahead of the shooter,
    // exactly on the line to the target.
    make_decoy(w, f4::geo::WorldPosition{0.0, 3000.0, 20000.0},
                   f4::math::Vec3<double>{0, 0, 0}, "blue",
               DecoyKind::Flare, 0.0, 60.0);

    // Arm the countermeasure-aware seeker with a seed that seduces
    // (calibrated the same way the unit test does).
    auto* mc = entities::EntityHandle(missile_id, &w.world)
                   .get<MissileComponent>();
    ASSERT_NE(mc, nullptr);
    const auto* rec = table.get(aim9);
    bool seduces = false;
    for (std::uint32_t s = 1; s <= 16 && !seduces; ++s) {
        SeekerCountermeasureConfig cfg;
        cfg.guidance = rec->guidance;
        cfg.flare_chance = 0.4;
        cfg.seeker_half_angle_rad =
            rec->seeker_half_angle_deg * (M_PI / 180.0);
        cfg.seeker_max_range_ft = rec->seeker_max_range_ft;
        cfg.missile_id = missile_id.value;
        cfg.shooter_team = "red";
        cfg.rng_seed = s;
        mc->seeker_source = make_decoy_aware_seeker_source(cfg);
        mc->decoy_aware_seeker = true;
        // Probe: the first tick's picture should be the flare.
        MissileSimComponent::set_sim_time(0.1);
        const auto probe = mc->seeker_source(w.world, mc->target_id);
        seduces = std::abs(probe.position.y - 3000.0) < 1e-6;
    }
    ASSERT_TRUE(seduces) << "no seed seduced in 1..16";

    // Fly the missile to terminal state (60 Hz minor frame).
    for (int i = 0; i < 600 && !mc->missile.terminal(); ++i) {
        MissileSimComponent::set_sim_time(MissileSimComponent::sim_time() +
                                          1.0 / 60.0);
        w.world.update_all(1.0 / 60.0, w.bus);
    }
    ASSERT_TRUE(mc->missile.terminal());

    // The fuze fired at the FLARE — 2000 ft short of the jet. The
    // countermeasure-aware terminal math measures that miss against
    // the ASSIGNED target: no damage, no kill.
    EXPECT_FALSE(tdmg.killed);
    EXPECT_GT(tdmg.hit_points, 99.0);
}
