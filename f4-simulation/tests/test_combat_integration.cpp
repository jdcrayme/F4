// test_combat_integration.cpp — the M3 "integration first" end-to-end test.
//
// COMBAT_CHAIN_PLAN.md milestone M3 starts by wiring f4-weapons + f4-sensors
// into f4-simulation (they were leaf libraries with zero consumers since
// M1/M2 landed). This file proves the chain moves THROUGH the Simulation:
//
//   1. combat components attach at spawn (scenario "combat": {"enabled": true})
//   2. the tick drives RadarSimComponent (priority 45) + MissileSimComponent
//      (priority 40) inside update_all, plus the update_rwr + sweep sweeps
//   3. detection -> track -> STT lock -> RWR lock warning, all through the
//      sim's own bus
//   4. launch_missile through the sim's weapon table -> flyout -> fuze ->
//      damage -> kill -> EntityKilledMessage -> missile swept
//   5. RadarBackedDetectionPolicy feeds SensorFusion from the track store
//      (the M2 hook made real — GCI-omniscience OFF under the policy)
//   6. combat disabled (default): none of the combat components exist —
//      the world is exactly what it was before M3
//
// Geometry: a stern chase. EAGLE1 (blue) and BANDIT1 (red) both fly north
// at ~10,000 ft; the bandit starts ~13 NM ahead. Both inside the default
// 120-degree north-centered scan bar; 13 NM is inside the 0.75*R_det knee
// (0.75 * 40 NM = 30 NM) where detection probability is 1.0 — the rolls
// are deterministic. The AMRAAM closes on the bandit's ~420 kt tail.
//
// The scenario is built from an in-memory JSON string (load_scenario_from_
// string); aircraft_config_path points at the generated f16.json fixture
// (FlightModelComponent::init requires real aero tables).

#include <gtest/gtest.h>

#include "f4/simulation/simulation.hpp"
#include "f4/simulation/combat_bridge.hpp"

#include <f4/ai/brain_component.hpp>
#include <f4/ai/modules/bvr_module.hpp>
#include <f4/ai/modules/wvr_module.hpp>
#include <f4/ai/sensor_fusion.hpp>
#include <f4/entities/entity.hpp>
#include <f4/messaging/bus.hpp>
#include <f4/sensors/f4_sensors.hpp>
#include <f4/weapons/f4_weapons.hpp>

#include <cstdlib>
#include <filesystem>
#include <string>
#include <vector>

using namespace f4::simulation;
namespace entities = f4::entities;
namespace messaging = f4::messaging;
namespace sensors = f4::sensors;
namespace weapons = f4::weapons;

namespace {

// Locate the generated F-16 aircraft config fixture (built by f4-convert
// from f4-convert/tests/fixtures/f16.dat). Empty string = not found; the
// caller skips.
std::string f16_config_path() {
    const char* env = std::getenv("F4_GENERATED_FIXTURES_DIR");
    std::string dir = env ? env : "";
#ifdef F4_GENERATED_FIXTURES_DIR
    if (dir.empty()) dir = F4_GENERATED_FIXTURES_DIR;
#endif
    if (dir.empty()) return "";
    const auto path = std::filesystem::path(dir) / "f16.json";
    return std::filesystem::exists(path) ? path.string() : "";
}

constexpr double kDt = 1.0 / 60.0;

// The scenario: two fighters in a stern chase at 10,000 ft. The bandit is
// ~13.2 NM (80,000 ft) north of the shooter. Both fly the same north route.
std::string combat_scenario_json(const std::string& f16_path,
                                 bool combat_enabled) {
    return R"({
  "name": "combat_integration",
  "theater": "korea",
  "aircraft": [
    { "callsign": "EAGLE1", "aircraft_config_path": ")" + f16_path + R"(",
      "aircraft_name": "F-16C_50", "vis_type_index": 1052,
      "parking_spot": { "x": 0.0, "y": 0.0, "z": 10000.0 },
      "heading_rad": 0.0, "initial_fuel_lbs": 6500.0,
      "initial_vt_fps": 506.0, "spawn_in_air": true, "team": "blue" },
    { "callsign": "BANDIT1", "aircraft_config_path": ")" + f16_path + R"(",
      "aircraft_name": "F-16C_50", "vis_type_index": 1052,
      "parking_spot": { "x": 0.0, "y": 80000.0, "z": 10000.0 },
      "heading_rad": 0.0, "initial_fuel_lbs": 6500.0,
      "initial_vt_fps": 420.0, "spawn_in_air": true, "team": "red" }
  ],
  "airfield": {
    "active_runway_id": 36, "active_runway_name": "Rwy 36",
    "runway_heading_rad": 0.0,
    "threshold_position": { "x": 0.0, "y": -5000.0, "z": 0.0 },
    "runway_end_position":  { "x": 0.0, "y": 5000.0, "z": 0.0 },
    "threshold_altitude_ft": 0.0, "departure_altitude_ft": 10000.0,
    "taxi_route": [ { "x": 0.0, "y": -5000.0, "z": 0.0 },
                    { "x": 0.0, "y": 0.0, "z": 0.0 } ]
  },
  "waypoints": [
    { "name": "FAR_NORTH", "position": { "x": 0.0, "y": 500000.0, "z": 10000.0 },
      "speed_kts": 420.0 }
  ],
  "start_enroute": true,
  "sim_dt": 0.016666666666666,
  "total_ticks": 30000,
  "record": false,
  "combat": { "enabled": )" + (combat_enabled ? "true" : "false") + R"(,
              "radar_rng_seed": 777 }
})";
}

/// Every combat event observed on the sim's bus.
struct CombatEventLog {
    std::vector<sensors::RadarTrackAcquiredMessage> acquired;
    std::vector<sensors::RwrWarningMessage> rwr;
    std::vector<weapons::MissileLaunchedMessage> launched;
    std::vector<weapons::MissileDetonatedMessage> detonated;
    std::vector<weapons::DamageAppliedMessage> damage;
    std::vector<weapons::EntityKilledMessage> killed;

    void attach(messaging::MessageBus& bus) {
        bus.subscribe<sensors::RadarTrackAcquiredMessage>(
            [this](const sensors::RadarTrackAcquiredMessage& m) { acquired.push_back(m); });
        bus.subscribe<sensors::RwrWarningMessage>(
            [this](const sensors::RwrWarningMessage& m) { rwr.push_back(m); });
        bus.subscribe<weapons::MissileLaunchedMessage>(
            [this](const weapons::MissileLaunchedMessage& m) { launched.push_back(m); });
        bus.subscribe<weapons::MissileDetonatedMessage>(
            [this](const weapons::MissileDetonatedMessage& m) { detonated.push_back(m); });
        bus.subscribe<weapons::DamageAppliedMessage>(
            [this](const weapons::DamageAppliedMessage& m) { damage.push_back(m); });
        bus.subscribe<weapons::EntityKilledMessage>(
            [this](const weapons::EntityKilledMessage& m) { killed.push_back(m); });
    }
};

} // namespace

// ============================================================================
// 1. Spawn-side: the combat component set exists iff combat is enabled.
// ============================================================================
TEST(CombatIntegration, CombatComponentsAttachWhenEnabled) {
    const auto f16 = f16_config_path();
    if (f16.empty()) GTEST_SKIP() << "f16.json fixture not generated";

    auto scenario =
        load_scenario_from_string(combat_scenario_json(f16, true));
    Simulation sim(std::move(scenario), std::filesystem::path("."));
    sim.initialize();

    ASSERT_EQ(sim.aircraft_entities().size(), 2u);
    for (const auto eid : sim.aircraft_entities()) {
        entities::EntityHandle h(eid, &sim.world());
        EXPECT_NE(h.get<weapons::WeaponStoreComponent>(), nullptr);
        EXPECT_NE(h.get<sensors::SignatureComponent>(), nullptr);
        EXPECT_NE(h.get<sensors::RadarSimComponent>(), nullptr);
        EXPECT_NE(h.get<sensors::RwrComponent>(), nullptr);
        EXPECT_NE(h.get<entities::DamageStateComponent>(), nullptr);
    }

    // IFF plumbing: teams came from the scenario JSON.
    entities::EntityHandle shooter(sim.aircraft_entities()[0], &sim.world());
    entities::EntityHandle bandit(sim.aircraft_entities()[1], &sim.world());
    EXPECT_EQ(shooter.get<sensors::RadarSimComponent>()->own_team, "blue");
    EXPECT_EQ(bandit.get<sensors::RadarSimComponent>()->own_team, "red");

    // Damage state initialized to the configured fighter strength.
    EXPECT_DOUBLE_EQ(
        shooter.get<entities::DamageStateComponent>()->hit_points, 25.0);
    EXPECT_DOUBLE_EQ(
        bandit.get<entities::DamageStateComponent>()->hit_points, 25.0);

    // The store carries the standard fighter loadout (8 AMRAAMs).
    const auto amraam =
        sim.weapon_table().find_by_name("AIM-120C");
    ASSERT_NE(amraam, weapons::kInvalidWeapon);
    EXPECT_EQ(shooter.get<weapons::WeaponStoreComponent>()->count_for(amraam), 8);

    // Determinism plumbing: per-aircraft seeds derived from the base seed.
    EXPECT_EQ(shooter.get<sensors::RadarSimComponent>()->rng_seed, 777u);
    EXPECT_EQ(bandit.get<sensors::RadarSimComponent>()->rng_seed, 778u);
}

TEST(CombatIntegration, NoCombatComponentsWhenDisabled) {
    const auto f16 = f16_config_path();
    if (f16.empty()) GTEST_SKIP() << "f16.json fixture not generated";

    auto scenario =
        load_scenario_from_string(combat_scenario_json(f16, false));
    Simulation sim(std::move(scenario), std::filesystem::path("."));
    sim.initialize();

    ASSERT_EQ(sim.aircraft_entities().size(), 2u);
    for (const auto eid : sim.aircraft_entities()) {
        entities::EntityHandle h(eid, &sim.world());
        EXPECT_EQ(h.get<weapons::WeaponStoreComponent>(), nullptr);
        EXPECT_EQ(h.get<sensors::RadarSimComponent>(), nullptr);
        EXPECT_EQ(h.get<sensors::RwrComponent>(), nullptr);
        EXPECT_EQ(h.get<entities::DamageStateComponent>(), nullptr);
    }

    // The TEAM tag still rides on every aircraft (harmless, future-proof).
    entities::EntityHandle bandit(sim.aircraft_entities()[1], &sim.world());
    // (Tag read back through the world API — teams are tags, not components.)
    // A couple of ticks must not invent combat state either.
    for (int i = 0; i < 10; ++i) sim.tick(kDt);
    EXPECT_EQ(weapons::count_live_missiles(sim.world()), 0u);
}

// ============================================================================
// 2. The chain: detect -> track -> lock -> RWR warning -> launch -> kill.
// ============================================================================
TEST(CombatIntegration, DetectTrackLockLaunchKillSweep) {
    const auto f16 = f16_config_path();
    if (f16.empty()) GTEST_SKIP() << "f16.json fixture not generated";

    auto scenario =
        load_scenario_from_string(combat_scenario_json(f16, true));
    Simulation sim(std::move(scenario), std::filesystem::path("."));
    sim.initialize();

    CombatEventLog log;
    log.attach(sim.bus());

    const auto shooter_id = sim.aircraft_entities()[0];
    const auto bandit_id = sim.aircraft_entities()[1];
    entities::EntityHandle shooter(shooter_id, &sim.world());
    entities::EntityHandle bandit(bandit_id, &sim.world());

    // --- Detection: run ~5 s. The shooter's radar scans every 1 s; the
    // bandit sits at ~13 NM, deep inside the 0.75*40 NM Pd=1.0 knee, dead
    // ahead in the default north-centered bar. Two detections promote the
    // track to Established.
    for (int i = 0; i < 5 * 60; ++i) sim.tick(kDt);

    ASSERT_GE(shooter.get<sensors::RadarSimComponent>()->scans_performed(), 2u)
        << "radar never scanned — tick does not drive RadarSimComponent";
    const auto* tf =
        shooter.get<sensors::RadarSimComponent>()->tracks().find(bandit_id.value);
    ASSERT_NE(tf, nullptr) << "no track on the bandit after 5 s";
    EXPECT_EQ(tf->state, sensors::TrackState::Established);
    EXPECT_FALSE(log.acquired.empty())
        << "RadarTrackAcquiredMessage never flowed on the sim bus";

    // IFF: the bandit is red, the shooter's team is blue.
    EXPECT_TRUE(tf->hostile_by_iff);

    // --- Lock: STT on the bandit (requires the live track we just proved).
    EXPECT_TRUE(shooter.get<sensors::RadarSimComponent>()
                    ->command_track(bandit_id.value));

    // --- RWR: the next tick's update_rwr sweep must paint the bandit's
    // RWR with the shooter's lock and publish the transition message.
    for (int i = 0; i < 5; ++i) sim.tick(kDt);
    const auto* bandit_rwr = bandit.get<sensors::RwrComponent>();
    ASSERT_NE(bandit_rwr, nullptr);
    EXPECT_TRUE(bandit_rwr->lock_active) << "victim RWR never saw the lock";
    EXPECT_FALSE(log.rwr.empty())
        << "RwrWarningMessage never flowed on the sim bus";
    EXPECT_EQ(log.rwr.back().victim_id, bandit_id.value);
    EXPECT_EQ(log.rwr.back().emitter_id, shooter_id.value);

    // --- Launch: through the sim's own world/bus/weapon table. This is
    // the exact call a future BVRModule makes.
    const auto amraam = sim.weapon_table().find_by_name("AIM-120C");
    ASSERT_NE(amraam, weapons::kInvalidWeapon);
    const auto missile = weapons::launch_missile(
        sim.world(), sim.bus(), shooter, bandit_id,
        sim.weapon_table(), amraam, sim.sim_time_s());
    ASSERT_TRUE(missile.valid()) << "launch refused (store/dry?)";
    EXPECT_EQ(log.launched.size(), 1u);

    // Store debited 8 -> 7.
    EXPECT_EQ(shooter.get<weapons::WeaponStoreComponent>()->count_for(amraam), 7);

    // --- Flyout + kill: tick until the bandit dies (90 s budget). The
    // missile's MissileSimComponent (priority 40) runs inside update_all;
    // every combat tick also sweeps terminal missiles.
    int kill_tick = -1;
    for (int i = 0; i < static_cast<int>(90.0 / kDt); ++i) {
        sim.tick(kDt);
        if (!log.killed.empty()) { kill_tick = i; break; }
    }
    ASSERT_NE(kill_tick, -1) << "missile never killed the bandit";

    // Exactly one kill, correct attribution, damage state transitioned.
    ASSERT_EQ(log.killed.size(), 1u);
    EXPECT_EQ(log.killed[0].target_id, bandit_id.value);
    EXPECT_EQ(log.killed[0].shooter_id, shooter_id.value);
    const auto* dmg = bandit.get<entities::DamageStateComponent>();
    ASSERT_NE(dmg, nullptr);
    EXPECT_TRUE(dmg->killed);
    EXPECT_DOUBLE_EQ(dmg->hit_points, 0.0);
    EXPECT_EQ(dmg->killed_by, shooter_id.value);
    ASSERT_FALSE(log.detonated.empty());
    EXPECT_EQ(log.detonated.back().cause,
              weapons::MissileEndCause::TargetHit);

    // The spent missile is swept by the tick (host-side sweep, combat only).
    EXPECT_FALSE(sim.world().alive(missile));
    EXPECT_EQ(weapons::count_live_missiles(sim.world()), 0u);

    // The corpse stays (death semantics belong to higher layers) and keeps
    // flying under its FM — the documented M2 simplification.
    EXPECT_TRUE(sim.world().alive(bandit_id));
}

// ============================================================================
// 3. The policy adapter: SensorFusion sees radar truth, not GCI truth.
// ============================================================================
TEST(CombatIntegration, RadarBackedPolicyFlipsSensorFusionOffGci) {
    const auto f16 = f16_config_path();
    if (f16.empty()) GTEST_SKIP() << "f16.json fixture not generated";

    auto scenario =
        load_scenario_from_string(combat_scenario_json(f16, true));
    Simulation sim(std::move(scenario), std::filesystem::path("."));
    sim.initialize();

    const auto shooter_id = sim.aircraft_entities()[0];
    const auto bandit_id = sim.aircraft_entities()[1];

    // A SensorFusion (as BVRModule will construct at M3) with the policy
    // installed. No ticks yet: no tracks, no RWR => every detection flag on
    // the bandit is FALSE (legacy GCI rules would see it at theater scale;
    // the target list itself still carries the candidate — visibility is
    // flag-based, see SensorFusion::can_see).
    f4::ai::SensorFusion sf;
    sf.initialize(shooter_id.value, sim.world(), sim.bus(),
                  f4::ai::SkillLevel::Veteran);
    RadarBackedDetectionPolicy policy(sim.world(), shooter_id.value);
    sf.set_detection_policy(&policy);
    sf.force_refresh();
    {
        const f4::ai::TargetInfo* t0 = nullptr;
        for (const auto& t : sf.targets()) {
            if (t.entity_id == bandit_id.value) { t0 = &t; break; }
        }
        ASSERT_NE(t0, nullptr);
        EXPECT_FALSE(f4::ai::SensorFusion::can_see(*t0))
            << "policy leaked detection before any radar scan";
    }

    // After a few seconds of radar operation the track exists; the policy
    // must report the bandit through radar and NOT through GCI.
    for (int i = 0; i < 5 * 60; ++i) sim.tick(kDt);
    sf.force_refresh();

    const f4::ai::TargetInfo* bandit_info = nullptr;
    for (const auto& t : sf.targets()) {
        if (t.entity_id == bandit_id.value) { bandit_info = &t; break; }
    }
    ASSERT_NE(bandit_info, nullptr) << "SensorFusion blind to a tracked bandit";
    EXPECT_TRUE(bandit_info->detected_by_radar);
    EXPECT_FALSE(bandit_info->detected_by_gci) << "GCI-omniscience leaked";
    EXPECT_FALSE(bandit_info->detected_by_visual);

    // A clean SensorFusion (no policy) still uses legacy GCI rules — the
    // default flip is deliberate and deferred until tactics exist.
    f4::ai::SensorFusion legacy;
    legacy.initialize(shooter_id.value, sim.world(), sim.bus(),
                      f4::ai::SkillLevel::Veteran);
    legacy.force_refresh();
    const f4::ai::TargetInfo* legacy_info = nullptr;
    for (const auto& t : legacy.targets()) {
        if (t.entity_id == bandit_id.value) { legacy_info = &t; break; }
    }
    ASSERT_NE(legacy_info, nullptr);
    EXPECT_TRUE(legacy_info->detected_by_gci);
}

// ============================================================================
// 4. Scenario JSON: the combat block parses + validates.
// ============================================================================
TEST(CombatIntegration, CombatBlockParsingAndValidation) {
    const auto f16 = f16_config_path();

    // Defaults when the block is absent entirely.
    const std::string no_block = R"({
      "name": "no_combat", "sim_dt": 0.016, "total_ticks": 100,
      "aircraft": [ { "callsign": "E", "aircraft_config_path": ")" + f16 + R"(",
                      "vis_type_index": 1 } ],
      "airfield": { "taxi_route": [ {"x":0,"y":0,"z":0}, {"x":0,"y":1,"z":0} ] }
    })";
    auto s1 = load_scenario_from_string(no_block);
    EXPECT_FALSE(s1.combat.enabled);
    EXPECT_EQ(s1.combat.radar_rng_seed, 0x46344u);
    EXPECT_DOUBLE_EQ(s1.combat.fighter_hit_points, 25.0);
    EXPECT_EQ(s1.aircraft[0].team, "blue");   // default team

    // Explicit values round-trip (team + the whole combat block).
    auto s2 = load_scenario_from_string(
        combat_scenario_json(f16, true));
    EXPECT_TRUE(s2.combat.enabled);
    EXPECT_EQ(s2.combat.radar_rng_seed, 777u);
    EXPECT_EQ(s2.aircraft[0].team, "blue");
    EXPECT_EQ(s2.aircraft[1].team, "red");

    // combat.enabled=false still parses the rest of the block.
    auto s3 = load_scenario_from_string(
        combat_scenario_json(f16, false));
    EXPECT_FALSE(s3.combat.enabled);
    EXPECT_EQ(s3.combat.radar_rng_seed, 777u);

    // Unknown team is rejected loudly.
    const std::string bad = R"({
      "name": "bad", "sim_dt": 0.016, "total_ticks": 100,
      "aircraft": [ { "callsign": "X", "aircraft_config_path": ")" + f16 + R"(",
                      "vis_type_index": 1, "team": "purple" } ],
      "airfield": { "taxi_route": [ {"x":0,"y":0,"z":0}, {"x":0,"y":1,"z":0} ] }
    })";
    EXPECT_THROW(load_scenario_from_string(bad), std::runtime_error);
}

// ============================================================================
// 5. M3 tactics E2E: the AI fights the war. NO test-driven lock or launch —
//    the two brains see each other through the radar-backed policy, the
//    shooter's BVRModule locks (intent -> driver) and fires (intent ->
//    launch_missile through the sim's weapon table), the bandit's
//    MissileModule beams against the incoming AMRAAM off its RWR launch
//    warning, and the DamageState ends the fight. This is the
//    COMBAT_CHAIN_PLAN M3 acceptance scenario: two formations detect,
//    engage, and kill — with only tick() called.
// ============================================================================
TEST(CombatIntegration, AiVersusAiBvrEngagement) {
    const auto f16 = f16_config_path();
    if (f16.empty()) GTEST_SKIP() << "f16.json fixture not generated";

    auto scenario =
        load_scenario_from_string(combat_scenario_json(f16, true));
    Simulation sim(std::move(scenario), std::filesystem::path("."));
    sim.initialize();

    CombatEventLog log;
    log.attach(sim.bus());

    const auto shooter_id = sim.aircraft_entities()[0];   // EAGLE1 (blue)
    const auto bandit_id  = sim.aircraft_entities()[1];   // BANDIT1 (red)
    entities::EntityHandle shooter(shooter_id, &sim.world());
    entities::EntityHandle bandit(bandit_id, &sim.world());

    // The M3 wiring the spawn path did: combat brains + radar policies +
    // table-derived envelopes. Verify it before flying.
    auto* shooter_brain = shooter.get<f4::ai::BrainComponent>();
    auto* bandit_brain  = bandit.get<f4::ai::BrainComponent>();
    ASSERT_NE(shooter_brain, nullptr);
    ASSERT_NE(bandit_brain, nullptr);
    EXPECT_TRUE(shooter_brain->combat_enabled());
    EXPECT_TRUE(bandit_brain->combat_enabled());
    EXPECT_NE(shooter_brain->sensors().detection_policy(), nullptr)
        << "radar-backed policy not installed on the shooter brain";
    EXPECT_NE(bandit_brain->sensors().detection_policy(), nullptr)
        << "radar-backed policy not installed on the bandit brain";
    // AIM-120C: max 40 NM boundary -> 20 NM doctrine envelope, ~0.5 NM min.
    EXPECT_NEAR(shooter_brain->bvr().fire().config().max_pk_range_nm,
                20.0, 0.01);
    EXPECT_NEAR(shooter_brain->bvr().fire().config().min_pk_range_nm,
                0.5, 0.01);

    // Fly the fight. Budget 150 s (detect ~5 s, launch ~6 s, flyout ~35 s).
    bool shooter_bvr_seen = false;
    bool shooter_employing_seen = false;
    bool bandit_defense_seen = false;
    bool disengaged_after_kill = false;
    int kill_tick = -1;
    const int max_ticks = static_cast<int>(150.0 / kDt);
    for (int i = 0; i < max_ticks; ++i) {
        sim.tick(kDt);

        if (shooter_brain->combat_mode() ==
            f4::ai::BrainComponent::CombatMode::BVR) {
            shooter_bvr_seen = true;
            if (shooter_brain->bvr().state() ==
                f4::ai::modules::BVRState::Employing) {
                shooter_employing_seen = true;
            }
        }
        if (bandit_brain->combat_mode() ==
            f4::ai::BrainComponent::CombatMode::Defensive) {
            bandit_defense_seen = true;
        }

        if (kill_tick < 0 && !log.killed.empty()) kill_tick = i;

        // After the kill: the corpse stops painting (the policy's corpse
        // filter), the shooter's BVR sees LostTarget, and the brain falls
        // off the ladder back to navigation.
        if (kill_tick >= 0 && i > kill_tick + static_cast<int>(10.0 / kDt)) {
            if (shooter_brain->combat_mode() ==
                f4::ai::BrainComponent::CombatMode::None) {
                disengaged_after_kill = true;
            }
        }
    }

    // --- The OODA loop ran on its own --------------------------------------
    ASSERT_GE(log.launched.size(), 1u) << "the AI never fired";
    for (const auto& m : log.launched) {
        EXPECT_EQ(m.shooter_id, shooter_id.value)
            << "only the radar-tracked shooter may launch (RWR-only "
               "pictures must not fire)";
    }
    EXPECT_LE(log.launched.size(), 2u);  // shoot-shoot doctrine
    ASSERT_TRUE(shooter_bvr_seen) << "shooter brain never entered BVR";
    ASSERT_TRUE(shooter_employing_seen)
        << "shooter never reached the employment state";
    ASSERT_TRUE(bandit_defense_seen)
        << "bandit never defended against the incoming AMRAAM";

    // The victim's RWR saw the launch (the defeat trigger).
    bool bandit_saw_launch = false;
    for (const auto& w : log.rwr) {
        if (w.type == sensors::RwrWarningType::Launch &&
            w.victim_id == bandit_id.value) {
            bandit_saw_launch = true;
        }
    }
    EXPECT_TRUE(bandit_saw_launch) << "victim RWR never saw the launch";

    // --- The outcome --------------------------------------------------------
    ASSERT_NE(kill_tick, -1) << "the AI's missile never killed the bandit";
    ASSERT_GE(log.killed.size(), 1u);
    for (const auto& k : log.killed) {
        EXPECT_EQ(k.target_id, bandit_id.value);
        EXPECT_EQ(k.shooter_id, shooter_id.value);
    }
    const auto* dmg = bandit.get<entities::DamageStateComponent>();
    ASSERT_NE(dmg, nullptr);
    EXPECT_TRUE(dmg->killed);

    // The shooter survives, holds its damage state, and went home.
    const auto* shooter_dmg = shooter.get<entities::DamageStateComponent>();
    ASSERT_NE(shooter_dmg, nullptr);
    EXPECT_FALSE(shooter_dmg->killed);
    EXPECT_TRUE(disengaged_after_kill)
        << "shooter never disengaged after the kill (corpse filter / "
           "LostTarget not working)";

    // Every missile the AI fired was swept (no live rounds left flying).
    EXPECT_EQ(weapons::count_live_missiles(sim.world()), 0u);
}

// ============================================================================
// 5b. M3 tactics Step 9 E2E: the MERGE. Two fighters start 5 NM apart,
//     head-on, with the AMRAAM exchange suppressed (combat block
//     "bvr_hold": radar missiles tight — SPINS-style). The bandit holds
//     all fire (per-aircraft "hold_fire": a maneuvering target drone —
//     RWR-only, it can see EAGLE1 through the lock warning but the
//     weapons-grade gate must keep it from firing blind). The fight is
//     decided INSIDE the 3 NM WVR entry band: EAGLE1's brain hands off
//     from BVR to WVR, the merge geometry classifies neutral, the IR fire
//     control takes the opportunity shot (FOX 2 off the wingtip — the
//     driver's WVR station preference), the bandit's RWR sees the launch
//     and MissileModule defends, and the heater ends it. This is the
//     plan's "Range band transitions (BVR->WVR at 3NM)" validation, run
//     through the whole stack.
// ============================================================================
TEST(CombatIntegration, AiVersusAiWvrMergeFight) {
    const auto f16 = f16_config_path();
    if (f16.empty()) GTEST_SKIP() << "f16.json fixture not generated";

    // Same shape as the BVR scenario, but: head-on (bandit southbound),
    // 5 NM apart (30,380 ft), 15,000 ft, hit points tuned so a heater
    // hit (24 lb warhead, ~14 damage at fuze range) ends the fight.
    const std::string json = R"({
  "name": "wvr_merge_integration",
  "theater": "korea",
  "aircraft": [
    { "callsign": "EAGLE1", "aircraft_config_path": ")" + f16 + R"(",
      "aircraft_name": "F-16C_50", "vis_type_index": 1052,
      "parking_spot": { "x": 0.0, "y": 0.0, "z": 15000.0 },
      "heading_rad": 0.0, "initial_fuel_lbs": 6500.0,
      "initial_vt_fps": 506.0, "spawn_in_air": true, "team": "blue" },
    { "callsign": "BANDIT1", "aircraft_config_path": ")" + f16 + R"(",
      "aircraft_name": "F-16C_50", "vis_type_index": 1052,
      "parking_spot": { "x": 0.0, "y": 30380.0, "z": 15000.0 },
      "heading_rad": 3.14159265358979, "initial_fuel_lbs": 6500.0,
      "initial_vt_fps": 460.0, "spawn_in_air": true, "team": "red",
      "hold_fire": true }
  ],
  "airfield": {
    "active_runway_id": 36, "active_runway_name": "Rwy 36",
    "runway_heading_rad": 0.0,
    "threshold_position": { "x": 0.0, "y": -5000.0, "z": 0.0 },
    "runway_end_position":  { "x": 0.0, "y": 5000.0, "z": 0.0 },
    "threshold_altitude_ft": 0.0, "departure_altitude_ft": 15000.0,
    "taxi_route": [ { "x": 0.0, "y": -5000.0, "z": 0.0 },
                    { "x": 0.0, "y": 0.0, "z": 0.0 } ]
  },
  "waypoints": [
    { "name": "FAR_NORTH", "position": { "x": 0.0, "y": 500000.0, "z": 15000.0 },
      "speed_kts": 420.0 }
  ],
  "start_enroute": true,
  "sim_dt": 0.016666666666666,
  "total_ticks": 30000,
  "record": false,
  "combat": { "enabled": true, "radar_rng_seed": 888,
              "fighter_hit_points": 10, "bvr_hold": true }
})";
    auto scenario = load_scenario_from_string(json);
    Simulation sim(std::move(scenario), std::filesystem::path("."));
    sim.initialize();

    CombatEventLog log;
    log.attach(sim.bus());

    const auto eagle_id  = sim.aircraft_entities()[0];  // EAGLE1 (blue)
    const auto bandit_id = sim.aircraft_entities()[1];  // BANDIT1 (red)
    entities::EntityHandle eagle(eagle_id, &sim.world());
    entities::EntityHandle bandit(bandit_id, &sim.world());

    auto* eagle_brain  = eagle.get<f4::ai::BrainComponent>();
    auto* bandit_brain = bandit.get<f4::ai::BrainComponent>();
    ASSERT_NE(eagle_brain, nullptr);
    ASSERT_NE(bandit_brain, nullptr);

    // The ROE wiring the spawn path did: EAGLE1 BVR-tight but heaters
    // free; the bandit fully tight (both modules).
    EXPECT_TRUE(eagle_brain->bvr().fire().config().hold_fire);
    EXPECT_FALSE(eagle_brain->wvr().fire().config().hold_fire);
    EXPECT_TRUE(bandit_brain->bvr().fire().config().hold_fire);
    EXPECT_TRUE(bandit_brain->wvr().fire().config().hold_fire);
    // The IR envelope came from the AIM-9M class: [0.5, 8] NM.
    EXPECT_NEAR(eagle_brain->wvr().fire().config().min_pk_range_nm,
                0.5, 0.01);
    EXPECT_NEAR(eagle_brain->wvr().fire().config().max_pk_range_nm,
                8.0, 0.01);

    // Fly the merge. Budget 90 s (detect ~3 s, closure to 3 NM ~30 s,
    // heater flyout ~5 s, sweep, disengage).
    bool eagle_wvr_seen = false;
    bool eagle_merge_seen = false;
    bool bandit_wvr_seen = false;
    bool bandit_defense_seen = false;
    bool disengaged_after_kill = false;
    int kill_tick = -1;
    const int max_ticks = static_cast<int>(90.0 / kDt);
    for (int i = 0; i < max_ticks; ++i) {
        sim.tick(kDt);

        if (eagle_brain->combat_mode() ==
            f4::ai::BrainComponent::CombatMode::WVR) {
            eagle_wvr_seen = true;
            if (eagle_brain->wvr().state() ==
                f4::ai::modules::WVRState::Merge) {
                eagle_merge_seen = true;
            }
        }
        if (bandit_brain->combat_mode() ==
            f4::ai::BrainComponent::CombatMode::WVR) {
            bandit_wvr_seen = true;
        }
        if (bandit_brain->combat_mode() ==
            f4::ai::BrainComponent::CombatMode::Defensive) {
            bandit_defense_seen = true;
        }
        if (kill_tick < 0 && !log.killed.empty()) kill_tick = i;
        if (kill_tick >= 0 && i > kill_tick + static_cast<int>(10.0 / kDt)) {
            if (eagle_brain->combat_mode() ==
                f4::ai::BrainComponent::CombatMode::None) {
                disengaged_after_kill = true;
            }
        }
        // Early exit once the fight is resolved, the sky is clean, AND
        // the shooter has had its disengage window (kill + 10 s: the
        // corpse filter drops the target at the next sensor refresh,
        // the WVR module sees LostTarget, the brain falls to nav).
        if (kill_tick >= 0 && weapons::count_live_missiles(sim.world()) == 0u
            && i > kill_tick + static_cast<int>(5.0 / kDt)
            && disengaged_after_kill) {
            break;
        }
    }

    // --- The band handoff ran on its own -----------------------------------
    ASSERT_TRUE(eagle_wvr_seen)
        << "EAGLE1 never entered the WVR rung (BVR->WVR handoff broken)";
    ASSERT_TRUE(eagle_merge_seen)
        << "EAGLE1's WVR module never classified the merge";
    ASSERT_TRUE(bandit_wvr_seen)
        << "the bandit (RWR-only picture) never entered the WVR rung";

    // --- The IR employment: heaters only, shooter only ----------------------
    ASSERT_GE(log.launched.size(), 1u) << "the merge never produced a shot";
    for (const auto& m : log.launched) {
        EXPECT_EQ(m.shooter_id, eagle_id.value)
            << "the hold_fire bandit must never launch";
        const auto* rec = sim.weapon_table().get(m.weapon_handle);
        ASSERT_NE(rec, nullptr);
        EXPECT_EQ(rec->guidance, weapons::GuidanceKind::Ir)
            << "the WVR shot must be a heater (driver station preference)";
    }
    EXPECT_LE(log.launched.size(), 2u);  // IR shoot-shoot doctrine

    // The AMRAAMs never left the rails despite BVR Employing (bvr_hold).
    const auto* eagle_store = eagle.get<weapons::WeaponStoreComponent>();
    ASSERT_NE(eagle_store, nullptr);
    const auto amraam_handle =
        sim.weapon_table().find_by_name("AIM-120C");
    const auto heater_handle = sim.weapon_table().find_by_name("AIM-9M");
    ASSERT_NE(amraam_handle, weapons::kInvalidWeapon);
    ASSERT_NE(heater_handle, weapons::kInvalidWeapon);
    EXPECT_EQ(eagle_store->count_for(amraam_handle), 8)
        << "an AMRAAM left the rail despite bvr_hold";
    EXPECT_LT(eagle_store->count_for(heater_handle), 2)
        << "no heater was expended";

    // --- The victim's defensive chain --------------------------------------
    ASSERT_TRUE(bandit_defense_seen)
        << "the bandit never defended against the heater";
    bool bandit_saw_launch = false;
    for (const auto& w : log.rwr) {
        if (w.type == sensors::RwrWarningType::Launch &&
            w.victim_id == bandit_id.value) {
            bandit_saw_launch = true;
        }
    }
    EXPECT_TRUE(bandit_saw_launch)
        << "the victim's RWR never saw the IR launch";

    // --- The outcome --------------------------------------------------------
    ASSERT_NE(kill_tick, -1) << "the heater never killed the bandit";
    for (const auto& k : log.killed) {
        EXPECT_EQ(k.target_id, bandit_id.value);
        EXPECT_EQ(k.shooter_id, eagle_id.value);
    }
    const auto* bandit_dmg = bandit.get<entities::DamageStateComponent>();
    ASSERT_NE(bandit_dmg, nullptr);
    EXPECT_TRUE(bandit_dmg->killed);

    const auto* eagle_dmg = eagle.get<entities::DamageStateComponent>();
    ASSERT_NE(eagle_dmg, nullptr);
    EXPECT_FALSE(eagle_dmg->killed);
    EXPECT_TRUE(disengaged_after_kill)
        << "EAGLE1 never disengaged after the merge kill";

    EXPECT_EQ(weapons::count_live_missiles(sim.world()), 0u);
}

// ============================================================================
// 6. The SHIPPED scenario file plays out: the player's bvr_intercept.json
//    (configured into <build>/scenarios by the root CMakeLists) must load
//    and produce the same AI-vs-AI engagement the in-memory E2E proved.
//    This keeps the file the scenario-player actually reads honest — a
//    typo in the JSON can't hide behind the in-memory test's copy.
// ============================================================================
TEST(CombatIntegration, BvrInterceptScenarioFilePlaysOut) {
#ifdef F4_SCENARIOS_DIR
    const std::filesystem::path file =
        std::filesystem::path(F4_SCENARIOS_DIR) / "bvr_intercept.json";
    if (!std::filesystem::exists(file)) GTEST_SKIP()
        << "bvr_intercept.json not configured (build it first)";

    const auto scenario = load_scenario(file);

    // The shipped file must ask for the fight.
    ASSERT_TRUE(scenario.combat.enabled);
    ASSERT_EQ(scenario.aircraft.size(), 2u);
    EXPECT_EQ(scenario.aircraft[0].team, "blue");
    EXPECT_EQ(scenario.aircraft[1].team, "red");
    EXPECT_EQ(scenario.aircraft[0].callsign, "EAGLE1");
    EXPECT_EQ(scenario.aircraft[1].callsign, "BANDIT1");

    Simulation sim(scenario, file.parent_path());
    sim.initialize();

    CombatEventLog log;
    log.attach(sim.bus());

    const auto shooter_id = sim.aircraft_entities()[0];
    const auto bandit_id = sim.aircraft_entities()[1];

    // Fly the fight (the in-memory twin resolves in ~60 s; 150 s budget).
    // After the kill keep flying until the shoot-shoot follow-up missile
    // is swept too — the in-memory twin asserts the same at 150 s.
    int kill_tick = -1;
    const int max_ticks = static_cast<int>(150.0 / kDt);
    for (int i = 0; i < max_ticks; ++i) {
        sim.tick(kDt);
        if (!log.killed.empty() && kill_tick < 0) kill_tick = i;
        if (kill_tick >= 0 && weapons::count_live_missiles(sim.world()) == 0u) {
            break;
        }
    }
    ASSERT_NE(kill_tick, -1) << "the shipped scenario's AI never scored a kill";
    ASSERT_GE(log.launched.size(), 1u);
    for (const auto& m : log.launched) {
        EXPECT_EQ(m.shooter_id, shooter_id.value);
    }
    for (const auto& k : log.killed) {
        EXPECT_EQ(k.target_id, bandit_id.value);
        EXPECT_EQ(k.shooter_id, shooter_id.value);
    }
    EXPECT_EQ(weapons::count_live_missiles(sim.world()), 0u);
#else
    GTEST_SKIP() << "F4_SCENARIOS_DIR not defined for this target";
#endif
}

// ============================================================================
// 7. The SHIPPED scenario file plays out: the player's wvr_merge.json —
//    the merge the user watches in the scenario-player must be the same
//    fight the in-memory E2E (5b) proved: BVR-tight closure, WVR handoff
//    inside 3 NM, FOX 2 off the wingtip, the drone defends, the heater
//    ends it.
// ============================================================================
TEST(CombatIntegration, WvrMergeScenarioFilePlaysOut) {
#ifdef F4_SCENARIOS_DIR
    const std::filesystem::path file =
        std::filesystem::path(F4_SCENARIOS_DIR) / "wvr_merge.json";
    if (!std::filesystem::exists(file)) GTEST_SKIP()
        << "wvr_merge.json not configured (build it first)";

    const auto scenario = load_scenario(file);

    // The shipped file must ask for the merge.
    ASSERT_TRUE(scenario.combat.enabled);
    ASSERT_TRUE(scenario.combat.bvr_hold);
    ASSERT_EQ(scenario.aircraft.size(), 2u);
    EXPECT_EQ(scenario.aircraft[0].callsign, "EAGLE1");
    EXPECT_EQ(scenario.aircraft[1].callsign, "BANDIT1");
    EXPECT_FALSE(scenario.aircraft[0].hold_fire);
    EXPECT_TRUE(scenario.aircraft[1].hold_fire);

    Simulation sim(scenario, file.parent_path());
    sim.initialize();

    CombatEventLog log;
    log.attach(sim.bus());

    const auto eagle_id  = sim.aircraft_entities()[0];
    const auto bandit_id = sim.aircraft_entities()[1];

    bool wvr_seen = false;
    bool defense_seen = false;
    int kill_tick = -1;
    const int max_ticks = static_cast<int>(90.0 / kDt);
    for (int i = 0; i < max_ticks; ++i) {
        sim.tick(kDt);

        auto* eagle_brain = entities::EntityHandle(eagle_id, &sim.world())
            .get<f4::ai::BrainComponent>();
        if (eagle_brain != nullptr &&
            eagle_brain->combat_mode() ==
                f4::ai::BrainComponent::CombatMode::WVR) {
            wvr_seen = true;
        }
        auto* bandit_brain = entities::EntityHandle(bandit_id, &sim.world())
            .get<f4::ai::BrainComponent>();
        if (bandit_brain != nullptr &&
            bandit_brain->combat_mode() ==
                f4::ai::BrainComponent::CombatMode::Defensive) {
            defense_seen = true;
        }

        if (kill_tick < 0 && !log.killed.empty()) kill_tick = i;
        if (kill_tick >= 0 && i > kill_tick + static_cast<int>(10.0 / kDt)) {
            if (weapons::count_live_missiles(sim.world()) == 0u) break;
        }
    }

    ASSERT_NE(kill_tick, -1) << "the shipped merge scenario never resolved";
    ASSERT_TRUE(wvr_seen) << "EAGLE1 never reached the WVR rung";
    ASSERT_TRUE(defense_seen) << "the drone never defended the heater";
    ASSERT_GE(log.launched.size(), 1u);
    for (const auto& m : log.launched) {
        EXPECT_EQ(m.shooter_id, eagle_id.value);
        const auto* rec = sim.weapon_table().get(m.weapon_handle);
        ASSERT_NE(rec, nullptr);
        EXPECT_EQ(rec->guidance, weapons::GuidanceKind::Ir)
            << "the merge shot must be a heater";
    }
    for (const auto& k : log.killed) {
        EXPECT_EQ(k.target_id, bandit_id.value);
        EXPECT_EQ(k.shooter_id, eagle_id.value);
    }
    EXPECT_EQ(weapons::count_live_missiles(sim.world()), 0u);
#else
    GTEST_SKIP() << "F4_SCENARIOS_DIR not defined for this target";
#endif
}
