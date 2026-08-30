// test_sensor_fusion.cpp — unit tests for the SensorFusion module.
//
// Covers the 6 validation tests from AI_IMPLEMENTATION_PLAN.md §5 Step 2:
//   1. Four detection sources (GCI/RWR/Radar/Visual) — any one enables canSee
//   2. Threat scoring: hostile +50 if combatClass 2-4
//   3. ATA > 90deg -> score /2
//   4. Combat class guess: speed>300kts OR alt<10000ft -> fighter(4)
//   5. Skill-dependent update interval: Recruit=10s, Ace=1s
//   6. EWMA smoothing on ataDot/rangeDot (0.85/0.15 blend)
//
// Plus structural tests: empty world, ownship-only, primary/threat/missile
// target selection, refresh-on-timer-expiry.

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstdint>

#include <f4/ai/f4_ai.hpp>
#include <f4/entities/f4_entities.hpp>
#include <f4/geo/f4_geo.hpp>
#include <f4/messaging/f4_messaging.hpp>

using namespace f4::ai;
using namespace f4::entities;
using namespace f4::geo;
using namespace f4::messaging;

namespace {

// 1 nautical mile in feet (for placing targets at known ranges).
constexpr double FT_PER_NM = 6076.11548;
// 1 knot in ft/s.
constexpr double FPS_PER_KT = 1.687809857;

// Build an entity with a TransformComponent at the given position.
// Optionally set team/role tags. Returns the EntityId (uint64_t).
struct EntitySpec {
    WorldPosition pos{};
    WorldPosition vel{};          // ft/s
    const char*   team = nullptr; // "red", "blue", ...
    const char*   role = nullptr; // "fighter", "missile", ...
};

std::uint64_t add_entity(EntityWorld& w, const EntitySpec& s) {
    EntityHandle h = w.create();
    auto& tf = h.add<TransformComponent>();
    tf.position = s.pos;
    tf.vx = s.vel.x;  tf.vy = s.vel.y;  tf.vz = s.vel.z;
    if (s.team) h.set_tag(tags::TEAM, TagValue::from(std::string(s.team)));
    if (s.role) h.set_tag(tags::ROLE, TagValue::from(std::string(s.role)));
    return h.id().value;
}

// Helper: place ownship at the origin, heading north (vel = +y), at 20000 ft.
// Targets are placed relative to this.
struct OwnshipSpec {
    WorldPosition pos{0.0, 0.0, 20000.0};
    WorldPosition vel{0.0, 600.0 * FPS_PER_KT, 0.0};  // 600 kts northbound
};

} // anonymous namespace

// ============================================================================
// Structural tests
// ============================================================================

TEST(SensorFusion, EmptyWorldProducesEmptyTargetList) {
    EntityWorld world;
    MessageBus bus;
    SensorFusion sf;
    sf.initialize(/*ownship_id=*/1, world, bus, SkillLevel::Veteran);
    sf.update(1.0);
    EXPECT_EQ(sf.target_count(), 0u);
    EXPECT_EQ(sf.primary_target(), nullptr);
    EXPECT_EQ(sf.threat_target(), nullptr);
    EXPECT_EQ(sf.missile_threat(), nullptr);
}

TEST(SensorFusion, OwnshipOnlyProducesNoTargets) {
    EntityWorld world;
    MessageBus bus;
    OwnshipSpec os;
    const auto own_id = add_entity(world, {os.pos, os.vel, "blue", "fighter"});

    SensorFusion sf;
    sf.initialize(own_id, world, bus, SkillLevel::Veteran);
    sf.update(1.0);
    // Ownship has a TransformComponent but is filtered out by id.
    EXPECT_EQ(sf.target_count(), 0u);
}

TEST(SensorFusion, OneTargetIsDetected) {
    EntityWorld world;
    MessageBus bus;
    OwnshipSpec os;
    const auto own_id = add_entity(world, {os.pos, os.vel, "blue", "fighter"});
    // Hostile 50 NM north of ownship, stationary.
    WorldPosition tgt_pos{0.0, 50.0 * FT_PER_NM, 20000.0};
    const auto tgt_id = add_entity(world, {tgt_pos, {}, "red", "fighter"});

    SensorFusion sf;
    sf.initialize(own_id, world, bus, SkillLevel::Ace);
    sf.update(1.0);

    ASSERT_EQ(sf.target_count(), 1u);
    EXPECT_EQ(sf.targets()[0].entity_id, tgt_id);
    EXPECT_NEAR(sf.targets()[0].range_nm, 50.0, 0.01);
}

// ============================================================================
// Validation test 1: Four detection sources (GCI/RWR/Radar/Visual)
//   "any one enables canSee" — sfusion.cpp
// ============================================================================

TEST(SensorFusion, FourDetectionSourcesEnableCanSee) {
    EntityWorld world;
    MessageBus bus;
    OwnshipSpec os;
    const auto own_id = add_entity(world, {os.pos, os.vel, "blue", "fighter"});

    // Place a hostile at 5 NM (inside all detection ranges).
    WorldPosition close_pos{0.0, 5.0 * FT_PER_NM, 20000.0};
    add_entity(world, {close_pos, {}, "red", "fighter"});

    SensorFusion sf;
    sf.initialize(own_id, world, bus, SkillLevel::Ace);
    sf.update(1.0);

    ASSERT_EQ(sf.target_count(), 1u);
    const auto& t = sf.targets()[0];
    // All four sources should fire for a close hostile.
    EXPECT_TRUE(SensorFusion::detected_by_radar(t));
    EXPECT_TRUE(SensorFusion::detected_by_rwr(t));
    EXPECT_TRUE(SensorFusion::detected_by_visual(t));
    EXPECT_TRUE(SensorFusion::detected_by_gci(t));
    EXPECT_TRUE(SensorFusion::can_see(t));
}

TEST(SensorFusion, DetectionSourcesAreRangeGated) {
    EntityWorld world;
    MessageBus bus;
    OwnshipSpec os;
    const auto own_id = add_entity(world, {os.pos, os.vel, "blue", "fighter"});

    // Hostile at 60 NM: inside radar (80) and rwr (50)? -> rwr NO, radar YES.
    // Outside visual (10). GCI always YES.
    WorldPosition far_pos{0.0, 60.0 * FT_PER_NM, 20000.0};
    add_entity(world, {far_pos, {}, "red", "fighter"});

    SensorFusion sf;
    sf.initialize(own_id, world, bus, SkillLevel::Ace);
    sf.update(1.0);

    ASSERT_EQ(sf.target_count(), 1u);
    const auto& t = sf.targets()[0];
    EXPECT_TRUE(SensorFusion::detected_by_radar(t));   // 60 < 80
    EXPECT_FALSE(SensorFusion::detected_by_rwr(t));    // 60 > 50
    EXPECT_FALSE(SensorFusion::detected_by_visual(t)); // 60 > 10
    EXPECT_TRUE(SensorFusion::detected_by_gci(t));     // always
    EXPECT_TRUE(SensorFusion::can_see(t));             // any one is enough
}

TEST(SensorFusion, FriendlyTargetIsNotRadarDetected) {
    // Radar/RWR detection requires is_hostile (simulates IFF — friendly
    // transponders don't trigger RWR, and friendly radar returns are
    // filtered out of the threat picture).
    EntityWorld world;
    MessageBus bus;
    OwnshipSpec os;
    const auto own_id = add_entity(world, {os.pos, os.vel, "blue", "fighter"});
    WorldPosition friendly_pos{0.0, 5.0 * FT_PER_NM, 20000.0};
    add_entity(world, {friendly_pos, {}, "blue", "fighter"});

    SensorFusion sf;
    sf.initialize(own_id, world, bus, SkillLevel::Ace);
    sf.update(1.0);

    ASSERT_EQ(sf.target_count(), 1u);
    const auto& t = sf.targets()[0];
    EXPECT_FALSE(SensorFusion::detected_by_radar(t));
    EXPECT_FALSE(SensorFusion::detected_by_rwr(t));
    EXPECT_TRUE(SensorFusion::detected_by_visual(t));  // visual is unaffiliated
    EXPECT_TRUE(SensorFusion::detected_by_gci(t));     // GCI sees all
    EXPECT_TRUE(SensorFusion::can_see(t));
}

// ============================================================================
// Validation test 2: Threat scoring — hostile +50 if combatClass 2-4
//   sfusion.cpp threat scoring
// ============================================================================

TEST(SensorFusion, ThreatScoringHostilePlusFighterBonus) {
    // A hostile fighter (combat_class 4) at close range, nose-on.
    // Expected score: 100 (hostile base) + 50 (fighter bonus) = 150.
    EntityWorld world;
    MessageBus bus;
    OwnshipSpec os;
    const auto own_id = add_entity(world, {os.pos, os.vel, "blue", "fighter"});

    // Target: 5 NM north, 600 kts northbound (away from ownship, but ownship
    // is also northbound — same direction, so target is "running away").
    // To make ATA = 0 (nose-on), target should be heading TOWARD ownship,
    // i.e., southbound (negative y velocity).
    WorldPosition tgt_pos{0.0, 5.0 * FT_PER_NM, 20000.0};
    WorldPosition tgt_vel{0.0, -600.0 * FPS_PER_KT, 0.0};  // southbound
    add_entity(world, {tgt_pos, tgt_vel, "red", "fighter"});

    SensorFusion sf;
    sf.initialize(own_id, world, bus, SkillLevel::Ace);
    sf.update(1.0);

    ASSERT_EQ(sf.target_count(), 1u);
    const auto& t = sf.targets()[0];
    EXPECT_EQ(t.combat_class, 4);       // 600 kts > 300 -> fighter
    EXPECT_TRUE(t.is_hostile);
    // ATA should be ~0 (target is heading south, we're north, target is
    // north of us -> target is pointed at us).
    EXPECT_NEAR(t.ata_rad, 0.0, 0.01);
    // 100 (hostile) + 50 (fighter) = 150. ATA < pi/2 so no halving.
    EXPECT_NEAR(t.threat_score, 150.0, 0.01);
}

TEST(SensorFusion, ThreatScoringHostileNonFighterNoBonus) {
    // A hostile stationary target (combat_class 0): score = 100, no +50 bonus.
    EntityWorld world;
    MessageBus bus;
    OwnshipSpec os;
    const auto own_id = add_entity(world, {os.pos, os.vel, "blue", "fighter"});

    WorldPosition tgt_pos{0.0, 5.0 * FT_PER_NM, 20000.0};
    add_entity(world, {tgt_pos, {}, "red", "transport"});  // stationary

    SensorFusion sf;
    sf.initialize(own_id, world, bus, SkillLevel::Ace);
    sf.update(1.0);

    ASSERT_EQ(sf.target_count(), 1u);
    const auto& t = sf.targets()[0];
    EXPECT_EQ(t.combat_class, 0);
    EXPECT_TRUE(t.is_hostile);
    EXPECT_NEAR(t.threat_score, 100.0, 0.01);
}

TEST(SensorFusion, ThreatScoringFriendlyHasZeroScore) {
    EntityWorld world;
    MessageBus bus;
    OwnshipSpec os;
    const auto own_id = add_entity(world, {os.pos, os.vel, "blue", "fighter"});
    WorldPosition tgt_pos{0.0, 5.0 * FT_PER_NM, 20000.0};
    add_entity(world, {tgt_pos, {}, "blue", "fighter"});

    SensorFusion sf;
    sf.initialize(own_id, world, bus, SkillLevel::Ace);
    sf.update(1.0);

    ASSERT_EQ(sf.target_count(), 1u);
    EXPECT_NEAR(sf.targets()[0].threat_score, 0.0, 0.01);
}

// ============================================================================
// Validation test 3: ATA > 90deg -> score /= 2
//   sfusion.cpp: if (ataFrom > 90) score /= 2
// ============================================================================

TEST(SensorFusion, AtaGt90HalvesThreatScore) {
    // A hostile fighter pointed AWAY from ownship (ATA ~ 180 deg).
    // Expected: (100 + 50) / 2 = 75.
    EntityWorld world;
    MessageBus bus;
    OwnshipSpec os;
    const auto own_id = add_entity(world, {os.pos, os.vel, "blue", "fighter"});

    // Target 5 NM north, heading north (away from ownship).
    WorldPosition tgt_pos{0.0, 5.0 * FT_PER_NM, 20000.0};
    WorldPosition tgt_vel{0.0, 600.0 * FPS_PER_KT, 0.0};  // northbound = away
    add_entity(world, {tgt_pos, tgt_vel, "red", "fighter"});

    SensorFusion sf;
    sf.initialize(own_id, world, bus, SkillLevel::Ace);
    sf.update(1.0);

    ASSERT_EQ(sf.target_count(), 1u);
    const auto& t = sf.targets()[0];
    // Target is north of us, heading north -> pointed away -> ATA ~ pi.
    EXPECT_GT(t.ata_rad, PI / 2.0);
    EXPECT_NEAR(t.ata_rad, PI, 0.01);
    // (100 + 50) / 2 = 75.
    EXPECT_NEAR(t.threat_score, 75.0, 0.01);
}

// ============================================================================
// Validation test 4: Combat class guess — speed>300kts OR alt<10000ft -> 4
//   sfusion.cpp:504
// ============================================================================

TEST(SensorFusion, CombatClassHeuristicAtBoundaries) {
    EntityWorld world;
    MessageBus bus;
    OwnshipSpec os;
    const auto own_id = add_entity(world, {os.pos, os.vel, "blue", "fighter"});

    // (a) speed > 300 kts, high altitude -> fighter (4)
    add_entity(world,
        {WorldPosition{0.0, 5.0 * FT_PER_NM, 20000.0},
         WorldPosition{0.0, 400.0 * FPS_PER_KT, 0.0},
         "red", "fighter"});

    // (b) speed < 300 kts, alt < 10000 ft -> fighter (4)
    add_entity(world,
        {WorldPosition{0.0, 6.0 * FT_PER_NM, 5000.0},
         WorldPosition{0.0, 100.0 * FPS_PER_KT, 0.0},
         "red", "fighter"});

    // (c) speed < 300 kts, alt > 10000 ft, speed > 150 kts -> maneuvering (3)
    add_entity(world,
        {WorldPosition{0.0, 7.0 * FT_PER_NM, 20000.0},
         WorldPosition{0.0, 200.0 * FPS_PER_KT, 0.0},
         "red", "fighter"});

    // (d) speed 0, alt > 10000 ft -> stationary (0)
    add_entity(world,
        {WorldPosition{0.0, 8.0 * FT_PER_NM, 20000.0},
         {}, "red", "transport"});

    SensorFusion sf;
    sf.initialize(own_id, world, bus, SkillLevel::Ace);
    sf.update(1.0);

    ASSERT_EQ(sf.target_count(), 4u);
    // Sort by range so we can identify them reliably.
    auto targets = sf.targets();
    std::sort(targets.begin(), targets.end(),
              [](const TargetInfo& a, const TargetInfo& b) {
                  return a.range_ft < b.range_ft;
              });
    EXPECT_EQ(targets[0].combat_class, 4);  // 5 NM, 400 kts
    EXPECT_EQ(targets[1].combat_class, 4);  // 6 NM, low+slow
    EXPECT_EQ(targets[2].combat_class, 3);  // 7 NM, 200 kts
    EXPECT_EQ(targets[3].combat_class, 0);  // 8 NM, stationary
}

// ============================================================================
// Validation test 5: Skill-dependent update interval
//   digimain.cpp:566 — Recruit=10s, Rookie=7s, Veteran=5s, Ace=1s
// ============================================================================

TEST(SensorFusion, SkillDependentUpdateInterval) {
    EXPECT_DOUBLE_EQ(SensorFusion::update_interval_sec(SkillLevel::Recruit), 10.0);
    EXPECT_DOUBLE_EQ(SensorFusion::update_interval_sec(SkillLevel::Rookie),   7.0);
    EXPECT_DOUBLE_EQ(SensorFusion::update_interval_sec(SkillLevel::Veteran),  5.0);
    EXPECT_DOUBLE_EQ(SensorFusion::update_interval_sec(SkillLevel::Ace),      1.0);
}

TEST(SensorFusion, SkillDependentReactionDelay) {
    EXPECT_DOUBLE_EQ(SensorFusion::reaction_delay_sec(SkillLevel::Recruit), 3.0);
    EXPECT_DOUBLE_EQ(SensorFusion::reaction_delay_sec(SkillLevel::Rookie),  2.0);
    EXPECT_DOUBLE_EQ(SensorFusion::reaction_delay_sec(SkillLevel::Veteran), 1.0);
    EXPECT_DOUBLE_EQ(SensorFusion::reaction_delay_sec(SkillLevel::Ace),     0.3);
}

// ============================================================================
// Validation test 6: EWMA smoothing on ataDot/rangeDot (0.85/0.15 blend)
//   digimain.cpp (0.85/0.15 blend)
// ============================================================================

TEST(SensorFusion, EwmaConvergesToSteadyState) {
    // A target closing at constant rate. After enough updates the EWMA
    // rangedot should converge to the true closing rate.
    //
    // Setup: ownship stationary at origin. Target starts 100 NM north at
    // 20000 ft, moving south at 600 kts (closing). Each refresh the range
    // decreases by (600 kt * update_interval * FPS_PER_KT).
    //
    // We manually move the target between refreshes by recomputing its
    // position based on its velocity and the elapsed sim time.
    EntityWorld world;
    MessageBus bus;

    OwnshipSpec os;
    os.vel = WorldPosition{0.0, 0.0, 0.0};  // stationary ownship
    const auto own_id = add_entity(world, {os.pos, os.vel, "blue", "fighter"});

    // Target: starts 100 NM north, moving south (toward ownship).
    const double start_range_ft = 100.0 * FT_PER_NM;
    const double tgt_speed_fps = 600.0 * FPS_PER_KT;
    WorldPosition tgt_pos{0.0, start_range_ft, 20000.0};
    WorldPosition tgt_vel{0.0, -tgt_speed_fps, 0.0};
    EntityHandle tgt_h = world.create();
    auto& tgt_tf = tgt_h.add<TransformComponent>();
    tgt_tf.position = tgt_pos;
    tgt_tf.vx = tgt_vel.x; tgt_tf.vy = tgt_vel.y; tgt_tf.vz = tgt_vel.z;
    tgt_h.set_tag(tags::TEAM, TagValue::from(std::string("red")));
    tgt_h.set_tag(tags::ROLE, TagValue::from(std::string("fighter")));

    SensorFusion sf;
    sf.initialize(own_id, world, bus, SkillLevel::Ace);  // 1s update interval
    sf.update(1.0);  // first refresh — no prev snapshot, rangedot = 0

    ASSERT_EQ(sf.target_count(), 1u);
    EXPECT_NEAR(sf.targets()[0].rangedot, 0.0, 1e-6);  // first sample = 0

    // Simulate 20 refreshes, moving the target closer each time.
    // After each refresh, the true closing rate is +tgt_speed_fps (positive
    // because range is decreasing). The EWMA should converge to this.
    //
    // Math: with alpha=0.15 and constant input S, after n iterations the
    // EWMA value is S * (1 - (1-alpha)^n). At n=20: S * (1 - 0.85^20)
    //   = S * (1 - 0.039) = S * 0.961, i.e. within 5% of S.
    for (int i = 0; i < 20; ++i) {
        // Move the target closer by (speed * 1s).
        tgt_tf.position.y -= tgt_speed_fps * 1.0;
        sf.update(1.0);  // triggers refresh (timer expired)
        ASSERT_EQ(sf.target_count(), 1u);
    }

    // After 20 iterations with constant input, EWMA should be within ~5%
    // of the true closing rate.
    const double expected = tgt_speed_fps;  // positive = closing
    const double actual = sf.targets()[0].rangedot;
    EXPECT_NEAR(actual, expected, expected * 0.05);
}

// ============================================================================
// Target selection: primary / threat / missile
// ============================================================================

TEST(SensorFusion, PrimaryTargetIsHighestThreatScore) {
    EntityWorld world;
    MessageBus bus;
    OwnshipSpec os;
    const auto own_id = add_entity(world, {os.pos, os.vel, "blue", "fighter"});

    // (a) low-priority: hostile stationary transport (score 100)
    add_entity(world, {WorldPosition{0.0, 5.0 * FT_PER_NM, 20000.0},
                       {}, "red", "transport"});
    // (b) high-priority: hostile fighter nose-on (score 150)
    add_entity(world, {WorldPosition{0.0, 6.0 * FT_PER_NM, 20000.0},
                       WorldPosition{0.0, -600.0 * FPS_PER_KT, 0.0},
                       "red", "fighter"});
    // (c) friendly: score 0
    add_entity(world, {WorldPosition{0.0, 7.0 * FT_PER_NM, 20000.0},
                       {}, "blue", "fighter"});

    SensorFusion sf;
    sf.initialize(own_id, world, bus, SkillLevel::Ace);
    sf.update(1.0);

    ASSERT_EQ(sf.target_count(), 3u);
    const auto* p = sf.primary_target();
    ASSERT_NE(p, nullptr);
    EXPECT_NEAR(p->threat_score, 150.0, 0.01);
}

TEST(SensorFusion, ThreatTargetSkipsNonFighters) {
    // threat_target() only returns combat_class >= 2.
    EntityWorld world;
    MessageBus bus;
    OwnshipSpec os;
    const auto own_id = add_entity(world, {os.pos, os.vel, "blue", "fighter"});

    // (a) stationary hostile (combat_class 0, score 100) — highest score
    //     but not a "threat" in the fighter sense.
    add_entity(world, {WorldPosition{0.0, 5.0 * FT_PER_NM, 20000.0},
                       {}, "red", "transport"});
    // (b) hostile fighter (combat_class 4, score 75 because pointed away)
    add_entity(world, {WorldPosition{0.0, 6.0 * FT_PER_NM, 20000.0},
                       WorldPosition{0.0, 600.0 * FPS_PER_KT, 0.0},
                       "red", "fighter"});

    SensorFusion sf;
    sf.initialize(own_id, world, bus, SkillLevel::Ace);
    sf.update(1.0);

    ASSERT_EQ(sf.target_count(), 2u);
    const auto* p = sf.primary_target();
    const auto* t = sf.threat_target();
    ASSERT_NE(p, nullptr);
    ASSERT_NE(t, nullptr);
    // primary is the higher-score one (stationary transport, 100 > 75)
    EXPECT_NEAR(p->threat_score, 100.0, 0.01);
    EXPECT_EQ(p->combat_class, 0);
    // threat is the fighter (combat_class >= 2)
    EXPECT_GE(t->combat_class, 2);
    EXPECT_NEAR(t->threat_score, 75.0, 0.01);
    // They should be different entities.
    EXPECT_NE(p->entity_id, t->entity_id);
}

TEST(SensorFusion, MissileThreatIsNearestIncoming) {
    // missile_threat() returns the nearest is_missile target.
    EntityWorld world;
    MessageBus bus;
    OwnshipSpec os;
    const auto own_id = add_entity(world, {os.pos, os.vel, "blue", "fighter"});

    // Far missile (20 NM).
    add_entity(world, {WorldPosition{0.0, 20.0 * FT_PER_NM, 20000.0},
                       {}, "red", "missile"});
    // Near missile (5 NM).
    add_entity(world, {WorldPosition{0.0, 5.0 * FT_PER_NM, 20000.0},
                       {}, "red", "missile"});
    // A fighter closer than the far missile but not a missile.
    add_entity(world, {WorldPosition{0.0, 4.0 * FT_PER_NM, 20000.0},
                       {}, "red", "fighter"});

    SensorFusion sf;
    sf.initialize(own_id, world, bus, SkillLevel::Ace);
    sf.update(1.0);

    ASSERT_EQ(sf.target_count(), 3u);
    const auto* m = sf.missile_threat();
    ASSERT_NE(m, nullptr);
    EXPECT_NEAR(m->range_nm, 5.0, 0.01);  // nearest missile, not nearest entity
    EXPECT_TRUE(m->is_missile);
    EXPECT_NEAR(m->threat_score, 200.0, 0.01);  // missiles always 200
}

// ============================================================================
// Refresh timing
// ============================================================================

TEST(SensorFusion, RefreshOnTimerExpiry) {
    EntityWorld world;
    MessageBus bus;
    OwnshipSpec os;
    const auto own_id = add_entity(world, {os.pos, os.vel, "blue", "fighter"});

    SensorFusion sf;
    sf.initialize(own_id, world, bus, SkillLevel::Veteran);  // 5s interval
    EXPECT_NEAR(sf.time_until_refresh(), 0.0, 1e-9);  // initial = 0 (refresh now)

    // First update at dt=1s -> timer was 0, refresh fires, timer resets to 5s.
    sf.update(1.0);
    EXPECT_NEAR(sf.time_until_refresh(), 5.0, 1e-9);
    EXPECT_EQ(sf.target_count(), 0u);  // ownship only, no targets yet

    // Add a target. Without a refresh, SensorFusion won't see it.
    add_entity(world, {WorldPosition{0.0, 5.0 * FT_PER_NM, 20000.0},
                       {}, "red", "fighter"});
    sf.update(1.0);  // timer: 5 - 1 = 4s
    EXPECT_NEAR(sf.time_until_refresh(), 4.0, 1e-9);
    EXPECT_EQ(sf.target_count(), 0u);  // not refreshed yet

    // Skip ahead 4s — should refresh now.
    sf.update(4.0);
    EXPECT_NEAR(sf.time_until_refresh(), 5.0, 1e-9);  // reset
    EXPECT_EQ(sf.target_count(), 1u);  // target now visible
}

TEST(SensorFusion, ForceRefreshIgnoresTimer) {
    EntityWorld world;
    MessageBus bus;
    OwnshipSpec os;
    const auto own_id = add_entity(world, {os.pos, os.vel, "blue", "fighter"});

    SensorFusion sf;
    sf.initialize(own_id, world, bus, SkillLevel::Veteran);  // 5s interval
    sf.update(1.0);  // refresh fires, timer = 5s
    EXPECT_NEAR(sf.time_until_refresh(), 5.0, 1e-9);

    add_entity(world, {WorldPosition{0.0, 5.0 * FT_PER_NM, 20000.0},
                       {}, "red", "fighter"});
    EXPECT_EQ(sf.target_count(), 0u);  // not refreshed

    sf.force_refresh();  // ignores timer
    EXPECT_NEAR(sf.time_until_refresh(), 5.0, 1e-9);  // reset
    EXPECT_EQ(sf.target_count(), 1u);  // target now visible
}

// ============================================================================
// M2 integration point: DetectionPolicy override
// ============================================================================

namespace {

// A policy that sees ONLY what the "radar track store" says — stands in for
// the f4-sensors-backed adapter that M3 wires in.
class TrackOnlyPolicy final : public SensorFusion::DetectionPolicy {
public:
    explicit TrackOnlyPolicy(std::uint64_t tracked_id) : tracked_id_(tracked_id) {}

    Verdict classify(const TargetInfo& t) override {
        Verdict v;
        v.radar = (t.entity_id == tracked_id_);  // only the tracked contact
        return v;  // no GCI omniscience, no RWR, no visual
    }

private:
    std::uint64_t tracked_id_;
};

} // namespace

TEST(SensorFusion, DetectionPolicyOverridesLegacySources) {
    EntityWorld world;
    MessageBus bus;
    OwnshipSpec os;
    const auto own_id = add_entity(world, {os.pos, os.vel, "blue", "fighter"});
    const auto enemy_id = add_entity(world, {WorldPosition{0.0, 20.0 * FT_PER_NM, 20000.0},
                                             {}, "red", "fighter"});

    // --- Without a policy: legacy rules (GCI sees everything) ---------------
    SensorFusion legacy;
    legacy.initialize(own_id, world, bus, SkillLevel::Veteran);
    legacy.update(1.0);
    ASSERT_EQ(legacy.target_count(), 1u);
    EXPECT_TRUE(SensorFusion::detected_by_gci(legacy.targets()[0]));
    EXPECT_TRUE(SensorFusion::can_see(legacy.targets()[0]));

    // --- With a policy: only the tracked contact is visible -----------------
    TrackOnlyPolicy policy(enemy_id);
    SensorFusion sf;
    sf.initialize(own_id, world, bus, SkillLevel::Veteran);
    sf.set_detection_policy(&policy);
    ASSERT_NE(sf.detection_policy(), nullptr);
    sf.update(1.0);
    ASSERT_EQ(sf.target_count(), 1u);
    const auto& t = sf.targets()[0];
    EXPECT_EQ(t.entity_id, enemy_id);
    EXPECT_TRUE(SensorFusion::detected_by_radar(t));
    EXPECT_FALSE(SensorFusion::detected_by_gci(t));
    EXPECT_FALSE(SensorFusion::detected_by_rwr(t));
    EXPECT_FALSE(SensorFusion::detected_by_visual(t));
    EXPECT_TRUE(SensorFusion::can_see(t));  // radar alone is enough

    // --- Clearing the policy restores legacy behavior ------------------------
    sf.set_detection_policy(nullptr);
    sf.force_refresh();
    ASSERT_EQ(sf.target_count(), 1u);
    EXPECT_TRUE(SensorFusion::detected_by_gci(sf.targets()[0]));
}
