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
#include <f4/ai/modules/wingman_module.hpp>
#include <f4/ai/sensor_fusion.hpp>
#include <f4/entities/entity.hpp>
#include <f4/flight/flight_model_component.hpp>
#include <f4/messaging/bus.hpp>
#include <f4/recorder/flight_recorder.hpp>
#include <f4/sensors/f4_sensors.hpp>
#include <f4/weapons/f4_weapons.hpp>

#include <algorithm>
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
    // generic_string: the path is embedded in scenario JSON documents
    // (combat_scenario_json) — backslashes would JSON-escape ("\f" =
    // form feed); forward slashes are valid on Windows filesystems too.
    return std::filesystem::exists(path) ? path.generic_string() : "";
}

constexpr double kDt = 1.0 / 60.0;

// The scenario: two fighters in a stern chase at 10,000 ft. The bandit is
// ~13.2 NM (80,000 ft) north of the shooter. Both fly the same north route.
// `record_path` non-empty flips "record" on and points the trace at it
// (used by the M4 recording E2E). `fuel_block` non-empty is inserted as
// the scenario's "fuel" policy block (the arbiter's fuel check E2Es).
std::string combat_scenario_json(const std::string& f16_path,
                                 bool combat_enabled,
                                 const std::string& record_path = {},
                                 const std::string& fuel_block = {}) {
    const std::string record_block = record_path.empty()
        ? "\"record\": false"
        : "\"record\": true,\n  \"record_path\": \"" + record_path + "\"";
    const std::string fuel_seg = fuel_block.empty()
        ? ""
        : ",\n  \"fuel\": " + fuel_block;
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
  )" + record_block + fuel_seg + R"(,
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
    std::vector<weapons::GunFiredMessage> gun_fired;

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
        bus.subscribe<weapons::GunFiredMessage>(
            [this](const weapons::GunFiredMessage& m) { gun_fired.push_back(m); });
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
        EXPECT_NE(h.get<weapons::GunComponent>(), nullptr);
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

// ============================================================================
// 8. M4 — the fight REPLAYS HEADLESS. The same BVR engagement as (5), run
//    with recording on, written to disk, and re-loaded through the
//    FlightRecorder JSON round-trip. The recording must carry BOTH halves
//    of the fight: the kinematic tracks (two aircraft AND the missile
//    flyout) and the combat event stream (detect -> lock -> launch ->
//    detonation -> damage -> kill), with events tick-aligned to the
//    snapshots so a replay host can scrub and narrate coherently.
// ============================================================================
TEST(CombatIntegration, CombatRecordingReplaysTheFight) {
    const auto f16 = f16_config_path();
    if (f16.empty()) GTEST_SKIP() << "f16.json fixture not generated";

    // Record into a temp file (unique-ish name; removed at the end).
    const auto trace_path =
        std::filesystem::temp_directory_path() / "f4_combat_replay_test.json";
    auto scenario = load_scenario_from_string(
        combat_scenario_json(f16, true, trace_path.generic_string()));
    Simulation sim(std::move(scenario), std::filesystem::path("."));
    sim.initialize();

    // The recorder exists and is queryable live (the M4 accessor).
    ASSERT_NE(sim.recorder(), nullptr);

    const auto shooter_id = sim.aircraft_entities()[0];   // EAGLE1 (blue)
    const auto bandit_id  = sim.aircraft_entities()[1];   // BANDIT1 (red)

    // Fly the fight — same shape as (5): budget 150 s, stop once the
    // follow-up missile is swept.
    bool killed = false;
    const int max_ticks = static_cast<int>(150.0 / kDt);
    for (int i = 0; i < max_ticks; ++i) {
        sim.tick(kDt);
        const auto& events = sim.recorder()->combat_events();
        for (const auto& e : events) {
            if (e.kind == f4::recorder::CombatEventKind::EntityKilled) {
                killed = true;
            }
        }
        if (killed && weapons::count_live_missiles(sim.world()) == 0u) break;
    }
    ASSERT_TRUE(killed) << "the fight never resolved; nothing to replay";

    // The live recording already carries the chain (the bus bridge worked).
    const auto live_event_count = sim.recorder()->combat_event_count();
    ASSERT_GE(live_event_count, 6u)
        << "expected at least acquire/lock/launch/rwr-launch/detonate/kill";

    // Persist + reload: the file is the deliverable a replay host consumes.
    ASSERT_NO_THROW(sim.write_recording());
    ASSERT_TRUE(std::filesystem::exists(trace_path));

    const auto replay = [&trace_path]() {
        return f4::recorder::FlightRecorder::load_json(trace_path);
    };
    const auto loaded = replay();
    ASSERT_EQ(loaded.combat_event_count(), live_event_count)
        << "the JSON round-trip lost events";

    // --- Kinematic half: two aircraft tracks + the missile flyout ---------
    std::vector<std::uint64_t> aircraft_ids;
    std::vector<std::uint64_t> missile_ids;
    std::uint64_t max_tick = 0;
    for (const auto& s : loaded.snapshots()) {
        max_tick = std::max(max_tick, s.tick);
        if (s.missile) {
            if (std::find(missile_ids.begin(), missile_ids.end(),
                          s.entity_id) == missile_ids.end()) {
                missile_ids.push_back(s.entity_id);
            }
        } else {
            if (std::find(aircraft_ids.begin(), aircraft_ids.end(),
                          s.entity_id) == aircraft_ids.end()) {
                aircraft_ids.push_back(s.entity_id);
            }
        }
    }
    ASSERT_EQ(aircraft_ids.size(), 2u)
        << "the recording lost one of the two aircraft tracks";
    ASSERT_FALSE(missile_ids.empty())
        << "the missile flyout was never recorded — replays would show "
           "jets dying to invisible missiles";

    // The missile track reads as a missile: weapon name + status.
    bool missile_track_ok = false;
    for (const auto& s : loaded.snapshots()) {
        if (!s.missile) continue;
        if (s.callsign == "AIM-120C" && s.ai_mode == "Missile" &&
            (s.ai_state == "guided" || s.ai_state == "ballistic")) {
            missile_track_ok = true;
        }
    }
    EXPECT_TRUE(missile_track_ok)
        << "missile snapshots missing weapon name / flyout status";

    // --- Event half: the full engagement chain, in order -------------------
    const auto& ev = loaded.combat_events();
    auto find_event = [&ev](f4::recorder::CombatEventKind kind,
                            std::uint64_t subject, std::uint64_t object) {
        for (const auto& e : ev) {
            if (e.kind == kind && e.subject_id == subject &&
                e.object_id == object) {
                return &e;
            }
        }
        return static_cast<const f4::recorder::CombatEvent*>(nullptr);
    };

    // Detection: EAGLE1's radar acquired BANDIT1.
    ASSERT_NE(find_event(f4::recorder::CombatEventKind::TrackAcquired,
                         shooter_id.value, bandit_id.value),
              nullptr)
        << "the radar acquisition never made it into the recording";

    // The lock warning on the victim.
    ASSERT_NE(find_event(f4::recorder::CombatEventKind::RwrLock,
                         bandit_id.value, shooter_id.value),
              nullptr)
        << "the RWR lock warning never made it into the recording";

    // The shot: shooter, target, and the weapon's NAME (resolved at capture
    // time — a replay must not need the weapon table).
    const auto* launch = find_event(
        f4::recorder::CombatEventKind::MissileLaunched,
        shooter_id.value, bandit_id.value);
    ASSERT_NE(launch, nullptr) << "the launch event is missing";
    EXPECT_EQ(launch->weapon_name, "AIM-120C");
    EXPECT_GT(launch->speed_ft_s, 0.0);
    EXPECT_NE(launch->missile_id, 0u);
    // The launched missile has a track in the snapshot stream.
    EXPECT_TRUE(std::find(missile_ids.begin(), missile_ids.end(),
                          launch->missile_id) != missile_ids.end())
        << "the launching missile never produced a recorded track";

    // The victim saw the launch (the defeat trigger).
    ASSERT_NE(find_event(f4::recorder::CombatEventKind::RwrLaunch,
                         bandit_id.value, launch->missile_id),
              nullptr)
        << "the victim's RWR launch warning never made it into the recording";

    // The terminal events: hit -> damage -> kill, all attributed.
    const auto* det = find_event(
        f4::recorder::CombatEventKind::MissileDetonated,
        shooter_id.value, bandit_id.value);
    ASSERT_NE(det, nullptr) << "the detonation event is missing";
    EXPECT_EQ(det->end_cause, "target_hit");
    EXPECT_EQ(det->missile_id, launch->missile_id);
    EXPECT_GT(det->flight_time_s, 0.0);

    const auto* dmg = find_event(
        f4::recorder::CombatEventKind::DamageApplied,
        bandit_id.value, shooter_id.value);
    ASSERT_NE(dmg, nullptr) << "the damage event is missing";
    EXPECT_TRUE(dmg->killed);
    EXPECT_DOUBLE_EQ(dmg->hit_points_after, 0.0);

    const auto* kill = find_event(
        f4::recorder::CombatEventKind::EntityKilled,
        bandit_id.value, shooter_id.value);
    ASSERT_NE(kill, nullptr) << "the kill event is missing";

    // --- Timing coherence: events align with the snapshot stream ----------
    double prev_time = -1.0;
    for (const auto& e : ev) {
        // Monotonic: bus events arrive in publish order.
        EXPECT_GE(e.sim_time_s, prev_time);
        prev_time = e.sim_time_s;
        // Tick-aligned: every event belongs to a tick the snapshots cover
        // (the +1 bridge stamping — see attach_combat_event_recorder).
        EXPECT_GE(e.tick, 1u);
        EXPECT_LE(e.tick, max_tick);
    }
    // Cause precedes effect: launch before detonation; detonation and kill
    // may share a stamp (the fuze, damage, and kill all publish on the
    // missile's terminal tick) — the vector ORDER still has det first.
    EXPECT_LT(launch->sim_time_s, det->sim_time_s);
    EXPECT_LE(det->sim_time_s, kill->sim_time_s);

    // The combat debrief section: launch outcome + kill attribution in the
    // LLM-facing summary of the same recording.
    const auto summary = loaded.to_summary_json();
    EXPECT_NE(summary.find("\"weapon\":\"AIM-120C\""), std::string::npos);
    EXPECT_NE(summary.find("\"end_cause\":\"target_hit\""), std::string::npos);

    std::filesystem::remove(trace_path);  // tidy: the round-trip is proven
}

// ============================================================================
// 9. M3 tactics Step 11 — the 2-SHIP. Schema: "lead_callsign" resolves,
//    validates (unknown / cross-team / self all rejected loudly), and the
//    spawn path marks the wingman brain.
// ============================================================================
TEST(CombatIntegration, LeadCallsignSchemaRoundTripsAndValidates) {
    const auto f16 = f16_config_path();
    if (f16.empty()) GTEST_SKIP() << "f16.json fixture not generated";

    // Round-trip: EAGLE2 follows EAGLE1; parse + validate at initialize().
    const std::string base = R"({
  "name": "two_ship_schema", "theater": "korea",
  "aircraft": [
    { "callsign": "EAGLE1", "aircraft_config_path": ")" + f16 + R"(",
      "aircraft_name": "F-16C_50", "vis_type_index": 1052,
      "parking_spot": { "x": 0.0, "y": 0.0, "z": 15000.0 },
      "heading_rad": 0.0, "initial_fuel_lbs": 6500.0,
      "initial_vt_fps": 506.0, "spawn_in_air": true, "team": "blue" },
    { "callsign": "EAGLE2", "aircraft_config_path": ")" + f16 + R"(",
      "aircraft_name": "F-16C_50", "vis_type_index": 1052,
      "parking_spot": { "x": 2000.0, "y": -2500.0, "z": 15000.0 },
      "heading_rad": 0.0, "initial_fuel_lbs": 6500.0,
      "initial_vt_fps": 506.0, "spawn_in_air": true, "team": "blue",
      "lead_callsign": "EAGLE1" }
  ],
  "airfield": {
    "active_runway_id": 36, "active_runway_name": "Rwy 36",
    "runway_heading_rad": 0.0,
    "threshold_position": { "x": 0.0, "y": -5000.0, "z": 0.0 },
    "runway_end_position":  { "x": 0.0, "y": 5000.0, "z": 0.0 },
    "threshold_altitude_ft": 0.0, "departure_altitude_ft": 15000.0,
    "taxi_route": [ { "x": 0.0, "y": -5000.0, "z": 0.0 },
                    { "x": 0.0, "y": 0.0, "z": 0.0 } ] },
  "waypoints": [ { "name": "N", "position": { "x": 0.0, "y": 500000.0, "z": 15000.0 },
                   "speed_kts": 420.0 } ],
  "start_enroute": true, "sim_dt": 0.016666666666666,
  "total_ticks": 600, "record": false
})";

    auto scenario = load_scenario_from_string(base);
    ASSERT_EQ(scenario.aircraft.size(), 2u);
    EXPECT_EQ(scenario.aircraft[0].lead_callsign, "");
    EXPECT_EQ(scenario.aircraft[1].lead_callsign, "EAGLE1");

    Simulation sim(std::move(scenario), std::filesystem::path("."));
    sim.initialize();

    auto* lead_brain = entities::EntityHandle(sim.aircraft_entities()[0],
                                              &sim.world())
        .get<f4::ai::BrainComponent>();
    auto* wing_brain = entities::EntityHandle(sim.aircraft_entities()[1],
                                              &sim.world())
        .get<f4::ai::BrainComponent>();
    ASSERT_NE(lead_brain, nullptr);
    ASSERT_NE(wing_brain, nullptr);
    EXPECT_FALSE(lead_brain->is_wingman());
    EXPECT_TRUE(wing_brain->is_wingman());
    EXPECT_EQ(wing_brain->flight_lead_id(),
              sim.aircraft_entities()[0].value);

    // Unknown lead callsign -> initialize() fails loudly.
    {
        std::string bad = base;
        bad.replace(bad.find("\"lead_callsign\": \"EAGLE1\""),
                    sizeof("\"lead_callsign\": \"EAGLE1\"") - 1,
                    "\"lead_callsign\": \"NOBODY\"");
        Simulation sim_bad(load_scenario_from_string(bad),
                           std::filesystem::path("."));
        EXPECT_THROW(sim_bad.initialize(), std::runtime_error);
    }
    // Self-lead -> rejected.
    {
        std::string bad = base;
        bad.replace(bad.find("\"lead_callsign\": \"EAGLE1\""),
                    sizeof("\"lead_callsign\": \"EAGLE1\"") - 1,
                    "\"lead_callsign\": \"EAGLE2\"");
        Simulation sim_bad(load_scenario_from_string(bad),
                           std::filesystem::path("."));
        EXPECT_THROW(sim_bad.initialize(), std::runtime_error);
    }
    // Cross-team lead -> rejected (a red wingman of a blue lead).
    {
        std::string bad = base;
        bad.replace(bad.find("\"team\": \"blue\",\n      \"lead_callsign\""),
                    sizeof("\"team\": \"blue\",\n      \"lead_callsign\"") - 1,
                    "\"team\": \"red\",\n      \"lead_callsign\"");
        Simulation sim_bad(load_scenario_from_string(bad),
                           std::filesystem::path("."));
        EXPECT_THROW(sim_bad.initialize(), std::runtime_error);
    }
}

// ============================================================================
// 10. M3 tactics Step 11 E2E: the 2v2. Two blues in formation (EAGLE1 lead,
//     EAGLE2 wingman on its FightingWing station) stern-chase two red
//     bandits 13 NM north (hold_fire drones — the fight resolves with the
//     blues alive, deterministically). The full wingman contract:
//       a. formation: EAGLE2 flies the FORMATION rung pre-detection
//          (WingmanFormation / Following, on station);
//       b. the SORT: once the lead engages, EAGLE2 engages the OTHER
//          bandit (sorted_threat_target);
//       c. both bandits die, both blues survive;
//       d. rejoin: after the fight EAGLE2 returns to the lead's wing
//          (Formation / Following, station error converging).
// ============================================================================
TEST(CombatIntegration, AiVersusAiTwoShipBvrFight) {
    const auto f16 = f16_config_path();
    if (f16.empty()) GTEST_SKIP() << "f16.json fixture not generated";

    const std::string json = R"({
  "name": "two_ship_integration", "theater": "korea",
  "aircraft": [
    { "callsign": "EAGLE1", "aircraft_config_path": ")" + f16 + R"(",
      "aircraft_name": "F-16C_50", "vis_type_index": 1052,
      "parking_spot": { "x": 0.0, "y": 0.0, "z": 15000.0 },
      "heading_rad": 0.0, "initial_fuel_lbs": 6500.0,
      "initial_vt_fps": 506.0, "spawn_in_air": true, "team": "blue" },
    { "callsign": "EAGLE2", "aircraft_config_path": ")" + f16 + R"(",
      "aircraft_name": "F-16C_50", "vis_type_index": 1052,
      "parking_spot": { "x": 2000.0, "y": -2500.0, "z": 15000.0 },
      "heading_rad": 0.0, "initial_fuel_lbs": 6500.0,
      "initial_vt_fps": 506.0, "spawn_in_air": true, "team": "blue",
      "lead_callsign": "EAGLE1" },
    { "callsign": "BANDIT1", "aircraft_config_path": ")" + f16 + R"(",
      "aircraft_name": "F-16C_50", "vis_type_index": 1052,
      "parking_spot": { "x": 0.0, "y": 79000.0, "z": 15000.0 },
      "heading_rad": 0.0, "initial_fuel_lbs": 6500.0,
      "initial_vt_fps": 460.0, "spawn_in_air": true, "team": "red",
      "hold_fire": true },
    { "callsign": "BANDIT2", "aircraft_config_path": ")" + f16 + R"(",
      "aircraft_name": "F-16C_50", "vis_type_index": 1052,
      "parking_spot": { "x": 5000.0, "y": 79000.0, "z": 15000.0 },
      "heading_rad": 0.0, "initial_fuel_lbs": 6500.0,
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
                    { "x": 0.0, "y": 0.0, "z": 0.0 } ] },
  "waypoints": [ { "name": "FAR_NORTH",
                   "position": { "x": 0.0, "y": 500000.0, "z": 15000.0 },
                   "speed_kts": 420.0 } ],
  "start_enroute": true, "sim_dt": 0.016666666666666,
  "total_ticks": 30000, "record": false,
  "combat": { "enabled": true, "radar_rng_seed": 777,
              "fighter_hit_points": 20 }
})";
    auto scenario = load_scenario_from_string(json);
    Simulation sim(std::move(scenario), std::filesystem::path("."));
    sim.initialize();

    CombatEventLog log;
    log.attach(sim.bus());

    const auto lead_id  = sim.aircraft_entities()[0];  // EAGLE1
    const auto wing_id  = sim.aircraft_entities()[1];  // EAGLE2
    const auto bandit1  = sim.aircraft_entities()[2];
    const auto bandit2  = sim.aircraft_entities()[3];
    entities::EntityHandle lead(lead_id, &sim.world());
    entities::EntityHandle wing(wing_id, &sim.world());
    entities::EntityHandle b1(bandit1, &sim.world());
    entities::EntityHandle b2(bandit2, &sim.world());

    auto* lead_brain = lead.get<f4::ai::BrainComponent>();
    auto* wing_brain = wing.get<f4::ai::BrainComponent>();
    auto* fm_wing = wing.get<f4::flight::FlightModelComponent>();
    ASSERT_NE(lead_brain, nullptr);
    ASSERT_NE(wing_brain, nullptr);
    ASSERT_NE(fm_wing, nullptr);
    ASSERT_TRUE(wing_brain->is_wingman());

    bool formation_before_fight = false;
    bool sort_separation_seen = false;
    int kills = 0;
    int last_kill_tick = -1;
    bool rejoin_following = false;
    double rejoin_station_dist_ft = 1.0e9;
    std::string diag;   // failure diagnostics: a 5 s timeline

    // True 3D distance wingman -> formation station (the along-track
    // error alone reads wildly during the lead's post-fight turn — the
    // station frame rotates under the wingman).
    const auto station_dist = [&]() {
        const auto st = wing_brain->wingman().formation_position();
        const double ddx = fm_wing->position_east_ft() - st.x;
        const double ddy = fm_wing->position_north_ft() - st.y;
        return std::sqrt(ddx * ddx + ddy * ddy);
    };

    const int max_ticks = static_cast<int>(300.0 / kDt);
    for (int i = 0; i < max_ticks; ++i) {
        sim.tick(kDt);

        // 5 s timeline, appended to the failure message below (a rejoin
        // regression without the timeline is undebuggable).
        if (i % (5 * 60) == 0) {
            diag += "\n t=" + std::to_string(i / 60) +
                "s lead=" + lead_brain->combat_mode_name() +
                " wing=" + wing_brain->combat_mode_name() +
                "/" + wing_brain->wingman().state_name() +
                " dist=" + std::to_string(station_dist()) +
                " kills=" + std::to_string(kills);
        }

        // (a) pre-detection: the wingman flies the formation rung.
        const bool any_fight =
            lead_brain->combat_mode() != f4::ai::BrainComponent::CombatMode::None
            && lead_brain->combat_mode() !=
                   f4::ai::BrainComponent::CombatMode::Formation;
        if (!any_fight && log.acquired.empty() &&
            wing_brain->combat_mode() ==
                f4::ai::BrainComponent::CombatMode::Formation &&
            wing_brain->wingman().state() ==
                f4::ai::modules::WingState::Following) {
            if (station_dist() < 2500.0) formation_before_fight = true;
        }

        // (b) the sort: while BOTH fight, different bandits.
        const auto lead_tgt = lead_brain->combat_engagement_id();
        const auto wing_tgt = wing_brain->combat_engagement_id();
        if (lead_tgt != 0 && wing_tgt != 0 && lead_tgt != wing_tgt) {
            sort_separation_seen = true;
        }

        // (c) kills.
        if (kills < 2) {
            kills = 0;
            for (const auto eid : {bandit1, bandit2}) {
                const auto* dmg = entities::EntityHandle(eid, &sim.world())
                    .get<entities::DamageStateComponent>();
                if (dmg != nullptr && dmg->killed) ++kills;
            }
            if (kills == 2 && last_kill_tick < 0) last_kill_tick = i;
        }

        // (d) rejoin: after the fight, back on the wing.
        if (kills == 2 && last_kill_tick >= 0 &&
            i > last_kill_tick + static_cast<int>(5.0 / kDt) &&
            wing_brain->combat_mode() ==
                f4::ai::BrainComponent::CombatMode::Formation &&
            wing_brain->wingman().state() ==
                f4::ai::modules::WingState::Following) {
            const double dist = station_dist();
            rejoin_station_dist_ft = std::min(rejoin_station_dist_ft, dist);
            if (dist < 4000.0) rejoin_following = true;
        }

        if (kills == 2 && rejoin_following &&
            weapons::count_live_missiles(sim.world()) == 0u) {
            break;  // the fight resolved AND the wing reformed — done
        }
    }

    // --- (a) formation flew before the fight --------------------------------
    EXPECT_TRUE(formation_before_fight)
        << "the wingman never flew the formation rung pre-detection";

    // --- (b) the sort split the flight --------------------------------------
    ASSERT_TRUE(sort_separation_seen)
        << "lead and wingman never engaged different bandits (sort broken)";

    // --- (c) the fight resolved ---------------------------------------------
    ASSERT_EQ(kills, 2) << "the two-ship never killed both bandits";
    for (const auto& k : log.killed) {
        EXPECT_TRUE(k.target_id == bandit1.value || k.target_id == bandit2.value)
            << "a non-bandit died";
        EXPECT_TRUE(k.shooter_id == lead_id.value || k.shooter_id == wing_id.value)
            << "a hold_fire bandit scored a kill";
    }
    EXPECT_EQ(log.killed.size(), 2u);
    const auto* lead_dmg = lead.get<entities::DamageStateComponent>();
    const auto* wing_dmg = wing.get<entities::DamageStateComponent>();
    ASSERT_NE(lead_dmg, nullptr);
    ASSERT_NE(wing_dmg, nullptr);
    EXPECT_FALSE(lead_dmg->killed) << "the lead died to a hold_fire drone";
    EXPECT_FALSE(wing_dmg->killed) << "the wingman died to a hold_fire drone";
    EXPECT_EQ(weapons::count_live_missiles(sim.world()), 0u)
        << "live missiles left after the fight";

    // --- (d) the wing reformed ----------------------------------------------
    ASSERT_TRUE(rejoin_following)
        << "the wingman never rejoined formation after the fight" << diag;
    EXPECT_LT(rejoin_station_dist_ft, 4000.0);
}

// ============================================================================
// 11. The SHIPPED two_ship scenario plays out: same fight, through the
//     file the scenario-player loads (build/scenarios/two_ship.json).
// ============================================================================
TEST(CombatIntegration, TwoShipScenarioFilePlaysOut) {
#ifdef F4_SCENARIOS_DIR
    const std::filesystem::path file =
        std::filesystem::path(F4_SCENARIOS_DIR) / "two_ship.json";
    if (!std::filesystem::exists(file)) GTEST_SKIP()
        << "two_ship.json not configured (build it first)";

    const auto scenario = load_scenario(file);

    // The shipped file must be the 2-ship: lead + wingman + two bandits.
    ASSERT_TRUE(scenario.combat.enabled);
    ASSERT_EQ(scenario.aircraft.size(), 4u);
    EXPECT_EQ(scenario.aircraft[0].lead_callsign, "");
    EXPECT_EQ(scenario.aircraft[1].lead_callsign, "EAGLE1");
    EXPECT_EQ(scenario.aircraft[0].team, "blue");
    EXPECT_EQ(scenario.aircraft[1].team, "blue");
    EXPECT_EQ(scenario.aircraft[2].team, "red");
    EXPECT_EQ(scenario.aircraft[3].team, "red");
    EXPECT_TRUE(scenario.aircraft[2].hold_fire);
    EXPECT_TRUE(scenario.aircraft[3].hold_fire);

    Simulation sim(scenario, file.parent_path());
    sim.initialize();

    CombatEventLog log;
    log.attach(sim.bus());

    const auto wing_id = sim.aircraft_entities()[1];
    const auto bandit1 = sim.aircraft_entities()[2];
    const auto bandit2 = sim.aircraft_entities()[3];
    auto* wing_brain = entities::EntityHandle(wing_id, &sim.world())
        .get<f4::ai::BrainComponent>();
    ASSERT_NE(wing_brain, nullptr);
    ASSERT_TRUE(wing_brain->is_wingman());

    bool formation_seen = false;
    int kills = 0;
    bool rejoin_seen = false;
    const int max_ticks = static_cast<int>(300.0 / kDt);
    for (int i = 0; i < max_ticks; ++i) {
        sim.tick(kDt);

        if (wing_brain->combat_mode() ==
            f4::ai::BrainComponent::CombatMode::Formation) {
            formation_seen = true;
        }

        kills = 0;
        for (const auto eid : {bandit1, bandit2}) {
            const auto* dmg = entities::EntityHandle(eid, &sim.world())
                .get<entities::DamageStateComponent>();
            if (dmg != nullptr && dmg->killed) ++kills;
        }
        if (kills == 2 &&
            wing_brain->combat_mode() ==
                f4::ai::BrainComponent::CombatMode::Formation &&
            wing_brain->wingman().state() ==
                f4::ai::modules::WingState::Following) {
            rejoin_seen = true;
        }
        if (kills == 2 && rejoin_seen &&
            weapons::count_live_missiles(sim.world()) == 0u) {
            break;
        }
    }

    ASSERT_TRUE(formation_seen) << "the wingman never flew formation";
    ASSERT_EQ(kills, 2) << "the shipped two_ship scenario never resolved";
    ASSERT_TRUE(rejoin_seen) << "the wingman never rejoined";
    EXPECT_EQ(weapons::count_live_missiles(sim.world()), 0u);
#else
    GTEST_SKIP() << "F4_SCENARIOS_DIR not defined for this target";
#endif
}

// ============================================================================
// 9. GUNS (Steps 11-12) — the last unflown weapon.
//
//    ROE wiring (the no-surprise default: guns TIGHT unless a scenario
//    arms them), then the AI-vs-AI guns merge: missiles tight, guns free,
//    a drone bandit — the fight resolves at the trigger.
// ============================================================================

TEST(CombatIntegration, GunsRoeDefaultsAndWiring) {
    const auto f16 = f16_config_path();
    if (f16.empty()) GTEST_SKIP() << "f16.json fixture not generated";

    // The DEFAULT combat block (no gun fields): guns tight, everything
    // else as it was — the pre-gun scenarios fly unchanged.
    auto scenario =
        load_scenario_from_string(combat_scenario_json(f16, true));
    Simulation sim(std::move(scenario), std::filesystem::path("."));
    sim.initialize();

    const auto eagle = entities::EntityHandle(sim.aircraft_entities()[0],
                                              &sim.world());
    auto* brain = eagle.get<f4::ai::BrainComponent>();
    ASSERT_NE(brain, nullptr);
    EXPECT_TRUE(brain->wvr().guns().config().hold_fire)
        << "guns must default TIGHT (no-surprise for pre-gun scenarios)";
    EXPECT_FALSE(brain->wvr().fire().config().hold_fire);
    EXPECT_FALSE(brain->bvr().fire().config().hold_fire);

    // The GunComponent attached with the M61A1's card: handle, muzzle
    // velocity, lethal radius.
    auto* gun = eagle.get<weapons::GunComponent>();
    ASSERT_NE(gun, nullptr);
    const auto m61 = sim.weapon_table().find_by_name("M61A1");
    ASSERT_NE(m61, weapons::kInvalidWeapon);
    EXPECT_EQ(gun->weapon_handle, m61);
    EXPECT_TRUE(gun->stream.tracers().empty());   // idle at spawn

    // The gun fire control configured from the card + the store's drum,
    // capped at the employment doctrine: envelope [~0.08, 0.35] NM,
    // 3400 ft/s, 511 rounds.
    EXPECT_NEAR(brain->wvr().guns().config().max_range_nm, 0.35, 0.01);
    EXPECT_NEAR(brain->wvr().guns().config().muzzle_velocity_fps,
                3400.0, 1.0);
    EXPECT_EQ(brain->wvr().guns().rounds_remaining(), 511);
}

TEST(CombatIntegration, GunsRoeMissilesTightGunsFree) {
    const auto f16 = f16_config_path();
    if (f16.empty()) GTEST_SKIP() << "f16.json fixture not generated";

    // missiles_hold + guns armed: BVR + IR fire controls tight, the
    // trigger free. Subsumes bvr_hold.
    const std::string json = R"({
  "name": "guns_roe",
  "theater": "korea",
  "aircraft": [
    { "callsign": "EAGLE1", "aircraft_config_path": ")" + f16 + R"(",
      "aircraft_name": "F-16C_50", "vis_type_index": 1052,
      "parking_spot": { "x": 0.0, "y": 0.0, "z": 15000.0 },
      "heading_rad": 0.0, "initial_fuel_lbs": 6500.0,
      "initial_vt_fps": 760.0, "spawn_in_air": true, "team": "blue" }
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
  "sim_dt": 0.016666666666666,
  "total_ticks": 600,
  "record": false,
  "combat": { "enabled": true, "missiles_hold": true,
              "guns_hold": false }
})";
    auto scenario = load_scenario_from_string(json);
    Simulation sim(std::move(scenario), std::filesystem::path("."));
    sim.initialize();

    auto* brain = entities::EntityHandle(sim.aircraft_entities()[0],
                                         &sim.world())
                      .get<f4::ai::BrainComponent>();
    ASSERT_NE(brain, nullptr);
    EXPECT_TRUE(brain->bvr().fire().config().hold_fire);
    EXPECT_TRUE(brain->wvr().fire().config().hold_fire);
    EXPECT_FALSE(brain->wvr().guns().config().hold_fire);
    EXPECT_TRUE(brain->bvr_hold());   // missiles_hold subsumes it
}

TEST(CombatIntegration, AiVersusAiGunsMergeFight) {
    const auto f16 = f16_config_path();
    if (f16.empty()) GTEST_SKIP() << "f16.json fixture not generated";

    // The guns fight: head-on at 15,000 ft, 2.8 NM apart — INSIDE the
    // WVR band from the first tick (both nose-on at spawn: no BVR crank
    // transient to fly through before the gun envelope opens). EAGLE1:
    // missiles tight, guns FREE (the only armed weapon). BANDIT1: a
    // full drone (hold_fire — fights geometry, jinks, never shoots).
    // Hit points 1: the drone's light hull — one burst of 20 mm HEI
    // (measured ~50 hits at the falloff edge ≈ 1.6 damage on this seed)
    // finishes it. The hp is a SCENARIO calibration like the heater
    // E2E's 10-for-one-shot; a missile-calibrated fighter hull (25) is
    // a missile exchange, not a gun fight.
    const std::string json = R"({
  "name": "guns_merge_integration",
  "theater": "korea",
  "aircraft": [
    { "callsign": "EAGLE1", "aircraft_config_path": ")" + f16 + R"(",
      "aircraft_name": "F-16C_50", "vis_type_index": 1052,
      "parking_spot": { "x": 0.0, "y": 0.0, "z": 15000.0 },
      "heading_rad": 0.0, "initial_fuel_lbs": 6500.0,
      "initial_vt_fps": 760.0, "spawn_in_air": true, "team": "blue" },
    { "callsign": "BANDIT1", "aircraft_config_path": ")" + f16 + R"(",
      "aircraft_name": "F-16C_50", "vis_type_index": 1052,
      "parking_spot": { "x": 0.0, "y": 17013.0, "z": 15000.0 },
      "heading_rad": 3.14159265358979, "initial_fuel_lbs": 6500.0,
      "initial_vt_fps": 720.0, "spawn_in_air": true, "team": "red",
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
    { "name": "MERGE", "position": { "x": 0.0, "y": 8500.0, "z": 15000.0 },
      "speed_kts": 420.0 }
  ],
  "start_enroute": true,
  "sim_dt": 0.016666666666666,
  "total_ticks": 30000,
  "record": false,
  "combat": { "enabled": true, "radar_rng_seed": 999,
              "fighter_hit_points": 1,
              "missiles_hold": true, "guns_hold": false }
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

    // The ROE: the eagle's guns are the only free weapon on the field.
    EXPECT_FALSE(eagle_brain->wvr().guns().config().hold_fire);
    EXPECT_TRUE(eagle_brain->wvr().fire().config().hold_fire);
    EXPECT_TRUE(eagle_brain->bvr().fire().config().hold_fire);
    EXPECT_TRUE(bandit_brain->wvr().guns().config().hold_fire);

    // Fly the guns fight: detection ~3 s, closure to the gun envelope
    // ~28 s, merges with snapshot bursts, re-attacks, kill, disengage.
    // Budget 180 s.
    bool eagle_wvr_seen = false;
    bool disengaged_after_kill = false;
    // The cavoid/midair trade, watched: inside the gun employment band
    // the pass owns the geometry — the WVR target is exempt from the
    // collision-avoid rung (the brain feeds wvr().gun_pass_target_id()
    // to collision_avoid). If this wiring ever breaks, the merge becomes
    // a mutual 0.7-s break and the gun envelope never opens.
    bool gun_pass_exempt_seen = false;
    bool eagle_ca_ever_avoided = false;
    int kill_tick = -1;
    const int max_ticks = static_cast<int>(180.0 / kDt);
    for (int i = 0; i < max_ticks; ++i) {
        sim.tick(kDt);

        if (eagle_brain->combat_mode() ==
            f4::ai::BrainComponent::CombatMode::WVR) {
            eagle_wvr_seen = true;
            if (eagle_brain->collision_avoid().exempt_id() ==
                bandit_id.value) {
                gun_pass_exempt_seen = true;
            }
        }
        if (eagle_brain->collision_avoid().is_avoiding()) {
            eagle_ca_ever_avoided = true;
        }
        if (kill_tick < 0 && !log.killed.empty()) kill_tick = i;
        if (kill_tick >= 0 && i > kill_tick + static_cast<int>(10.0 / kDt)) {
            if (eagle_brain->combat_mode() ==
                f4::ai::BrainComponent::CombatMode::None) {
                disengaged_after_kill = true;
            }
        }
        if (kill_tick >= 0 &&
            i > kill_tick + static_cast<int>(5.0 / kDt) &&
            disengaged_after_kill &&
            log.gun_fired.size() >= 1u) {
            break;
        }
    }

    // --- The fight ran through the WVR band ------------------------------
    ASSERT_TRUE(eagle_wvr_seen)
        << "EAGLE1 never entered the WVR rung (no guns fight possible)";

    // --- The committed firing pass exemption ------------------------------
    // (the documented midair trade: inside the employment band the
    // weapons own the merge geometry — the cavoid break stays out)
    EXPECT_TRUE(gun_pass_exempt_seen)
        << "the WVR gun-pass target was never exempted from cavoid";
    EXPECT_FALSE(eagle_ca_ever_avoided)
        << "collision avoid broke the committed gun pass (merge geometry "
           "is owned by the weapons inside the employment band)";

    // --- Missiles never left the rail (missiles_hold) ---------------------
    ASSERT_TRUE(log.launched.empty())
        << "a missile launched despite missiles_hold";
    const auto* eagle_store = eagle.get<weapons::WeaponStoreComponent>();
    ASSERT_NE(eagle_store, nullptr);
    const auto amraam = sim.weapon_table().find_by_name("AIM-120C");
    const auto heater = sim.weapon_table().find_by_name("AIM-9M");
    const auto m61 = sim.weapon_table().find_by_name("M61A1");
    ASSERT_NE(amraam, weapons::kInvalidWeapon);
    ASSERT_NE(heater, weapons::kInvalidWeapon);
    ASSERT_NE(m61, weapons::kInvalidWeapon);
    EXPECT_EQ(eagle_store->count_for(amraam), 8);
    EXPECT_EQ(eagle_store->count_for(heater), 2);

    // --- The guns fired, by the eagle only, and the drum ran down --------
    ASSERT_GE(log.gun_fired.size(), 1u) << "the guns never fired";
    int rounds_expended = 0;
    for (const auto& g : log.gun_fired) {
        EXPECT_EQ(g.shooter_id, eagle_id.value)
            << "the hold_fire drone must never fire guns";
        EXPECT_EQ(g.target_id, bandit_id.value)
            << "bursts must carry the aim hint";
        const auto* rec = sim.weapon_table().get(g.weapon_handle);
        ASSERT_NE(rec, nullptr);
        EXPECT_EQ(rec->name, "M61A1");
        rounds_expended += g.rounds;
    }
    EXPECT_GT(rounds_expended, 0);
    EXPECT_LT(eagle_store->count_for(m61), 511)
        << "no gun rounds were expended from the store";
    EXPECT_EQ(eagle_store->count_for(m61), 511 - rounds_expended)
        << "the store debit must match the rounds that left the muzzle";

    // --- The outcome: a GUN kill (damage with missile_id == 0) ----------
    ASSERT_NE(kill_tick, -1) << "the guns never killed the bandit";
    bool gun_kill = false;
    for (const auto& d : log.damage) {
        if (d.target_id == bandit_id.value && d.missile_id == 0) {
            gun_kill = true;
        }
    }
    ASSERT_TRUE(gun_kill) << "the kill was not by gun damage";
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
        << "EAGLE1 never disengaged after the gun kill";
}

// 10. The SHIPPED scenario file plays out: the player's guns_merge.json —
//     same fight as the in-memory E2E, keeping the file the
//     scenario-player actually reads honest.
TEST(CombatIntegration, GunsMergeScenarioFilePlaysOut) {
#ifdef F4_SCENARIOS_DIR
    const auto path =
        std::filesystem::path(F4_SCENARIOS_DIR) / "guns_merge.json";
    ASSERT_TRUE(std::filesystem::exists(path))
        << "guns_merge.json not configured (build it first)";

    auto scenario = load_scenario(path);
    ASSERT_EQ(scenario.aircraft.size(), 2u);
    EXPECT_TRUE(scenario.combat.missiles_hold);
    EXPECT_FALSE(scenario.combat.guns_hold);

    Simulation sim(std::move(scenario), std::filesystem::path("."));
    sim.initialize();

    CombatEventLog log;
    log.attach(sim.bus());

    const auto eagle_id  = sim.aircraft_entities()[0];
    const auto bandit_id = sim.aircraft_entities()[1];
    auto* eagle_brain = entities::EntityHandle(eagle_id, &sim.world())
                            .get<f4::ai::BrainComponent>();
    ASSERT_NE(eagle_brain, nullptr);
    EXPECT_FALSE(eagle_brain->wvr().guns().config().hold_fire);

    int kill_tick = -1;
    const int max_ticks = static_cast<int>(180.0 / kDt);
    for (int i = 0; i < max_ticks; ++i) {
        sim.tick(kDt);
        if (kill_tick < 0 && !log.killed.empty()) kill_tick = i;
        if (kill_tick >= 0 && i > kill_tick + static_cast<int>(5.0 / kDt)) {
            break;
        }
    }
    ASSERT_NE(kill_tick, -1) << "the shipped guns scenario never resolved";
    ASSERT_GE(log.gun_fired.size(), 1u);
    EXPECT_TRUE(log.launched.empty());   // a pure guns fight
    for (const auto& k : log.killed) {
        EXPECT_EQ(k.target_id, bandit_id.value);
        EXPECT_EQ(k.shooter_id, eagle_id.value);
    }
#else
    GTEST_SKIP() << "F4_SCENARIOS_DIR not defined for this target";
#endif
}

// ============================================================================
// 11. The DigitalBrain arbiter (M3 final rungs — AI_IMPLEMENTATION_PLAN
//     Step 12, collapsed into the BrainComponent ladder): ground avoid +
//     collision avoid (FreeFalcon priorities 1-2, always armed) and the
//     fuel check (FrameExec step 2, joker/bingo). Safety is not a tactic:
//     these run with combat ON or OFF.
// ============================================================================

/// A synthetic ridge for the ground-avoid E2E: flat 0 MSL, a 1.9 ft/ft
/// ramp climbing between y=25,000 and y=30,000, then a 9,500-ft plateau.
/// A northbound jet at 10,000 ft MSL meets the predicted-clearance floor
/// (MIN_ALTT 1,500) ~8 s before the ramp — the pull-up must fly.
class RidgeTerrain final : public f4::terrain::TerrainSource {
public:
    [[nodiscard]] double elevation_at_ft(double, double y_ft) const override {
        if (y_ft <= 25000.0) return 0.0;
        if (y_ft <= 30000.0) return (y_ft - 25000.0) * 1.9;
        return 9500.0;
    }
};

TEST(CombatIntegration, GroundAvoidPullsUpOverTheRidge) {
    const auto f16 = f16_config_path();
    if (f16.empty()) GTEST_SKIP() << "f16.json fixture not generated";

    // One jet, combat DISABLED (safety is not a tactic): 10,000 ft MSL
    // northbound at ~420 kt into the ridge. The route's leg altitude
    // (16,000) is above the plateau — after the recovery the nav module
    // finishes the climb, proving the rung ENDED instead of flapping.
    const std::string json = R"({
  "name": "ground_avoid_ridge",
  "theater": "korea",
  "aircraft": [
    { "callsign": "EAGLE1", "aircraft_config_path": ")" + f16 + R"(",
      "aircraft_name": "F-16C_50", "vis_type_index": 1052,
      "parking_spot": { "x": 0.0, "y": 20000.0, "z": 10000.0 },
      "heading_rad": 0.0, "initial_fuel_lbs": 6500.0,
      "initial_vt_fps": 700.0, "spawn_in_air": true, "team": "blue" }
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
    { "name": "FAR_NORTH", "position": { "x": 0.0, "y": 250000.0, "z": 16000.0 },
      "speed_kts": 420.0 }
  ],
  "start_enroute": true,
  "sim_dt": 0.016666666666666,
  "total_ticks": 30000,
  "record": false
})";
    auto scenario = load_scenario_from_string(json);
    Simulation sim(std::move(scenario), std::filesystem::path("."));

    RidgeTerrain ridge;
    sim.set_terrain_source(&ridge);  // before initialize: spawn reads it too
    sim.initialize();

    const auto eid = sim.aircraft_entities()[0];
    entities::EntityHandle jet(eid, &sim.world());
    auto* brain = jet.get<f4::ai::BrainComponent>();
    ASSERT_NE(brain, nullptr);
    auto* fm = jet.get<f4::flight::FlightModelComponent>();
    ASSERT_NE(fm, nullptr);

    // Fly the encounter: trigger ~8.5 s in, ramp crossing ~15 s, release
    // over the plateau, then the nav climb resumes. 40 s budget.
    bool ga_seen = false;
    bool airborne_throughout = true;
    double min_clearance_ft = 1.0e9;
    const int max_ticks = static_cast<int>(40.0 / kDt);
    for (int i = 0; i < max_ticks; ++i) {
        sim.tick(kDt);
        if (brain->safety_mode() ==
            f4::ai::BrainComponent::SafetyMode::GroundAvoid) {
            ga_seen = true;
        }
        const auto* tf = jet.get<entities::TransformComponent>();
        ASSERT_NE(tf, nullptr);
        const double clearance =
            tf->position.z - ridge.elevation_at_ft(tf->position.x,
                                                   tf->position.y);
        min_clearance_ft = std::min(min_clearance_ft, clearance);
        if (!fm->state().gear.inAir) airborne_throughout = false;
    }

    // --- The rung fired ---------------------------------------------------
    ASSERT_TRUE(ga_seen) << "the ridge never tripped ground avoid";

    // --- The recovery held the MIN_ALTT doctrine --------------------------
    // The pull-up must keep the jet clear of the terrain for the whole
    // ride: the recovery target is terrain + 2x the 1,500 ft floor.
    EXPECT_GT(min_clearance_ft, 1000.0)
        << "the recovery dipped below a safe clearance (min "
        << min_clearance_ft << " ft)";
    EXPECT_TRUE(airborne_throughout) << "the jet touched down on the ridge";

    // --- The rung released: terrain cleared, nav owns the climb -----------
    EXPECT_EQ(brain->safety_mode(), f4::ai::BrainComponent::SafetyMode::None)
        << "ground avoid never released after the ridge";
    const auto* tf = jet.get<entities::TransformComponent>();
    ASSERT_NE(tf, nullptr);
    EXPECT_GT(tf->position.z, 12000.0)
        << "the jet did not climb out after the recovery";
    EXPECT_EQ(brain->mode_name(), "NavigationMode")
        << "the navigation module did not take the jet back";
}

TEST(CombatIntegration, CollisionAvoidBreaksHeadOnConvergence) {
    const auto f16 = f16_config_path();
    if (f16.empty()) GTEST_SKIP() << "f16.json fixture not generated";

    // Two BLUE jets, nose-on at 15,000 ft, combat DISABLED — the purest
    // statement of the rung (safety is not a tactic; the traffic picture
    // includes friendlies by design — the formation mate you are about to
    // fly through is exactly the aircraft cavoid exists for). Same
    // geometry as the guns merge, 2.8 NM apart, converging on the MERGE
    // waypoint; after the break both round the corner east onto FAR_EAST
    // in loose trail (the corner arcs are on opposite sides — no second
    // convergence).
    const std::string json = R"({
  "name": "collision_avoid_head_on",
  "theater": "korea",
  "aircraft": [
    { "callsign": "EAGLE1", "aircraft_config_path": ")" + f16 + R"(",
      "aircraft_name": "F-16C_50", "vis_type_index": 1052,
      "parking_spot": { "x": 0.0, "y": 0.0, "z": 15000.0 },
      "heading_rad": 0.0, "initial_fuel_lbs": 6500.0,
      "initial_vt_fps": 700.0, "spawn_in_air": true, "team": "blue" },
    { "callsign": "HAWK1", "aircraft_config_path": ")" + f16 + R"(",
      "aircraft_name": "F-16C_50", "vis_type_index": 1052,
      "parking_spot": { "x": 0.0, "y": 17013.0, "z": 15000.0 },
      "heading_rad": 3.14159265358979, "initial_fuel_lbs": 6500.0,
      "initial_vt_fps": 660.0, "spawn_in_air": true, "team": "blue" }
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
    { "name": "MERGE", "position": { "x": 0.0, "y": 8500.0, "z": 15000.0 },
      "speed_kts": 420.0 },
    { "name": "FAR_EAST", "position": { "x": 250000.0, "y": 8500.0, "z": 15000.0 },
      "speed_kts": 420.0 }
  ],
  "start_enroute": true,
  "sim_dt": 0.016666666666666,
  "total_ticks": 30000,
  "record": false
})";
    auto scenario = load_scenario_from_string(json);
    Simulation sim(std::move(scenario), std::filesystem::path("."));
    sim.initialize();

    const auto eagle_id = sim.aircraft_entities()[0];
    const auto hawk_id  = sim.aircraft_entities()[1];
    entities::EntityHandle eagle(eagle_id, &sim.world());
    entities::EntityHandle hawk(hawk_id, &sim.world());
    auto* eagle_brain = eagle.get<f4::ai::BrainComponent>();
    auto* hawk_brain  = hawk.get<f4::ai::BrainComponent>();
    ASSERT_NE(eagle_brain, nullptr);
    ASSERT_NE(hawk_brain, nullptr);

    // Fly the convergence. What the E2E measures (the honest ledger —
    // see the assert block below for the reference's late-react
    // character): the range at which the FIRST brain arms (the
    // prediction must fire OUTSIDE the protected bubble), the minimum
    // range at the pass, the range 1.5 s AFTER the pass (the escape +
    // divergence owning the post-pass geometry), and the release.
    bool eagle_ca_seen = false;
    bool hawk_ca_seen = false;
    bool ca_mode_name_reported = false;
    double first_avoid_range_ft = -1.0;
    double min_range_ft = 1.0e9;
    double post_pass_range_ft = -1.0;
    int min_tick = -1;
    std::uint64_t eagle_intruder = 0;
    std::uint64_t hawk_intruder = 0;
    const int max_ticks = static_cast<int>(45.0 / kDt);
    for (int i = 0; i < max_ticks; ++i) {
        sim.tick(kDt);
        const auto* etf = eagle.get<entities::TransformComponent>();
        const auto* htf = hawk.get<entities::TransformComponent>();
        ASSERT_NE(etf, nullptr);
        ASSERT_NE(htf, nullptr);
        const double dx = htf->position.x - etf->position.x;
        const double dy = htf->position.y - etf->position.y;
        const double dz = htf->position.z - etf->position.z;
        const double rng = std::sqrt(dx * dx + dy * dy + dz * dz);
        if (rng < min_range_ft) {
            min_range_ft = rng;
            min_tick = i;
        }
        if (min_tick >= 0 && i == min_tick + 90) {
            post_pass_range_ft = rng;   // 1.5 s after the tightest point
        }
        const bool any_avoiding =
            eagle_brain->collision_avoid().is_avoiding() ||
            hawk_brain->collision_avoid().is_avoiding();
        if (any_avoiding && first_avoid_range_ft < 0.0) {
            first_avoid_range_ft = rng;
        }
        if (eagle_brain->safety_mode() ==
            f4::ai::BrainComponent::SafetyMode::CollisionAvoid) {
            eagle_ca_seen = true;
            eagle_intruder = eagle_brain->collision_avoid().intruder_id();
            if (eagle_brain->mode_name() == "CollisionAvoid") {
                ca_mode_name_reported = true;
            }
        }
        if (hawk_brain->safety_mode() ==
            f4::ai::BrainComponent::SafetyMode::CollisionAvoid) {
            hawk_ca_seen = true;
            hawk_intruder = hawk_brain->collision_avoid().intruder_id();
        }
    }

    // --- Both jets took the rung ------------------------------------------
    ASSERT_TRUE(eagle_ca_seen) << "EAGLE1 never flew the collision break";
    ASSERT_TRUE(hawk_ca_seen) << "HAWK1 never flew the collision break";
    EXPECT_EQ(eagle_intruder, hawk_id.value)
        << "the break was not against the converging traffic";
    EXPECT_EQ(hawk_intruder, eagle_id.value)
        << "the break was not against the converging traffic";
    EXPECT_TRUE(ca_mode_name_reported)
        << "the HUD's mode line never reported CollisionAvoid";

    // --- The prediction fired OUTSIDE the protected bubble -----------------
    // (the 200-ft hRange bubble is the protected volume; the module's
    // job is to PREDICT the collision before it arrives — the first
    // avoidance must precede contact.)
    EXPECT_GT(first_avoid_range_ft, 200.0)
        << "the first break armed inside the protected bubble (the "
           "prediction never ran before contact)";

    // --- The pass + the post-pass geometry ---------------------------------
    // The honest ledger, measured on this exact-head-on worst case: the
    // reference's reactTime doctrine (0.55-0.7 s of warning — FreeFalcon
    // GS_LIMIT/maxGs*reactFact) arms the escape with under a second on
    // the clock, and this sim's FCS G-lag (~2 s stick-to-load-factor)
    // spends most of THAT on roll-in and pitch development. The miss at
    // the tightest point is tens of feet — the same character the
    // reference's digi has (FreeFalcon merges occasionally end in the
    // midair; the module header documents the trade). What the escape
    // DOES own: the jets pass clean (no airframe contact), and 1.5 s
    // later the geometry has opened decisively — both jets flying their
    // max-performance escapes apart, no re-convergence.
    EXPECT_GT(min_range_ft, 0.0) << "the tracks never crossed";
    EXPECT_NE(min_tick, -1);
    ASSERT_GT(post_pass_range_ft, 400.0)
        << "the geometry did not open after the pass ("
        << post_pass_range_ft << " ft at +1.5 s)";

    // --- The rung released after the pass ---------------------------------
    EXPECT_EQ(eagle_brain->safety_mode(),
              f4::ai::BrainComponent::SafetyMode::None)
        << "collision avoid never released after the pass";
    EXPECT_EQ(hawk_brain->safety_mode(),
              f4::ai::BrainComponent::SafetyMode::None)
        << "collision avoid never released after the pass";
    for (const auto* h : {&eagle, &hawk}) {
        const auto* fm = h->get<f4::flight::FlightModelComponent>();
        ASSERT_NE(fm, nullptr);
        EXPECT_TRUE(fm->state().gear.inAir) << "a jet did not stay airborne";
    }
}

TEST(CombatIntegration, FuelJokerReportsButFightsOn) {
    const auto f16 = f16_config_path();
    if (f16.empty()) GTEST_SKIP() << "f16.json fixture not generated";

    // Joker ABOVE the start fuel (6,550 > 6,500): Joker from the first
    // gauge read — REPORTING ONLY, never a gate. The BVR stern chase
    // plays out exactly as it does without a fuel policy.
    auto scenario = load_scenario_from_string(combat_scenario_json(
        f16, true, {}, R"({ "joker_lbs": 6550.0, "bingo_lbs": 0.0 })"));
    Simulation sim(std::move(scenario), std::filesystem::path("."));
    sim.initialize();

    CombatEventLog log;
    log.attach(sim.bus());

    const auto shooter_id = sim.aircraft_entities()[0];   // EAGLE1 (blue)
    entities::EntityHandle shooter(shooter_id, &sim.world());
    auto* shooter_brain = shooter.get<f4::ai::BrainComponent>();
    ASSERT_NE(shooter_brain, nullptr);

    // The policy wired at spawn: verified POST-LOOP below (the fuel
    // check runs on the brain's first update, not at spawn).
    bool bvr_seen = false;
    const int max_ticks = static_cast<int>(150.0 / kDt);
    for (int i = 0; i < max_ticks; ++i) {
        sim.tick(kDt);
        if (shooter_brain->combat_mode() ==
            f4::ai::BrainComponent::CombatMode::BVR) {
            bvr_seen = true;
        }
        if (!log.launched.empty()) break;  // the point is made
    }

    // Joker is a report, not a gate: the fight happens.
    EXPECT_EQ(shooter_brain->fuel_state(), f4::ai::BrainComponent::FuelState::Joker);
    EXPECT_TRUE(bvr_seen) << "a joker-state jet never engaged (joker must not gate)";
    ASSERT_FALSE(log.launched.empty())
        << "a joker-state jet never fired (joker must not gate)";
    EXPECT_EQ(log.launched.front().shooter_id, shooter_id.value);
}

TEST(CombatIntegration, FuelBingoStandsDownTheEngagement) {
    const auto f16 = f16_config_path();
    if (f16.empty()) GTEST_SKIP() << "f16.json fixture not generated";

    // Bingo ABOVE the start fuel (7,000 > 6,500): both jets bingo from
    // the first gauge read — the engagement rungs never arm. Nothing
    // launches; the brains report RTB (the nav route IS the way home).
    auto scenario = load_scenario_from_string(combat_scenario_json(
        f16, true, {}, R"({ "joker_lbs": 0.0, "bingo_lbs": 7000.0 })"));
    Simulation sim(std::move(scenario), std::filesystem::path("."));
    sim.initialize();

    CombatEventLog log;
    log.attach(sim.bus());

    const auto shooter_id = sim.aircraft_entities()[0];   // EAGLE1 (blue)
    entities::EntityHandle shooter(shooter_id, &sim.world());
    auto* shooter_brain = shooter.get<f4::ai::BrainComponent>();
    ASSERT_NE(shooter_brain, nullptr);

    // The policy wired at spawn: verified POST-LOOP below (the fuel
    // check runs on the brain's first update, not at spawn).
    bool rtb_reported = false;
    bool engagement_seen = false;
    const int max_ticks = static_cast<int>(90.0 / kDt);
    for (int i = 0; i < max_ticks; ++i) {
        sim.tick(kDt);
        if (shooter_brain->mode_name() == "RTB") rtb_reported = true;
        if (shooter_brain->combat_mode() !=
            f4::ai::BrainComponent::CombatMode::None) {
            engagement_seen = true;
        }
    }

    // --- The engagement stood down ----------------------------------------
    EXPECT_FALSE(engagement_seen)
        << "a bingo jet took a combat rung (the fight is over at bingo)";
    EXPECT_TRUE(log.launched.empty())
        << "a bingo jet expended a weapon";
    EXPECT_TRUE(log.gun_fired.empty())
        << "a bingo jet expended a weapon";

    // --- The RTB report ----------------------------------------------------
    EXPECT_TRUE(rtb_reported)
        << "a bingoing enroute jet must report RTB (the route home)";
    EXPECT_EQ(shooter_brain->fuel_state(),
              f4::ai::BrainComponent::FuelState::Bingo);
    // The gauge is live: the reading sits at the spawned load, burned
    // down but positive (the fuel check reads the FM's real tank).
    EXPECT_GT(shooter_brain->fuel_lbs(), 0.0);
    EXPECT_LE(shooter_brain->fuel_lbs(), 6500.0);
}

// 12. The SHIPPED midair_merge scenario file plays out: the player's
//     watchable collision-avoid demo — two BLUE jets nose-on at 15,000 ft
//     (same team: the hostility rule keeps the fight OFF; the safety
//     rungs run with combat enabled). The brains break right at the
//     convergence, pass, release, and both continue their routes east.
TEST(CombatIntegration, MidairMergeScenarioFilePlaysOut) {
#ifdef F4_SCENARIOS_DIR
    const auto path =
        std::filesystem::path(F4_SCENARIOS_DIR) / "midair_merge.json";
    ASSERT_TRUE(std::filesystem::exists(path))
        << "midair_merge.json not configured (build it first)";

    auto scenario = load_scenario(path);
    ASSERT_EQ(scenario.aircraft.size(), 2u);
    EXPECT_EQ(scenario.aircraft[0].team, "blue");
    EXPECT_EQ(scenario.aircraft[1].team, "blue");
    EXPECT_TRUE(scenario.combat.enabled);
    EXPECT_TRUE(scenario.combat.missiles_hold);
    EXPECT_TRUE(scenario.combat.guns_hold);

    Simulation sim(std::move(scenario), std::filesystem::path("."));
    sim.initialize();

    CombatEventLog log;
    log.attach(sim.bus());

    const auto eagle_id = sim.aircraft_entities()[0];
    const auto hawk_id  = sim.aircraft_entities()[1];
    entities::EntityHandle eagle(eagle_id, &sim.world());
    entities::EntityHandle hawk(hawk_id, &sim.world());
    auto* eagle_brain = eagle.get<f4::ai::BrainComponent>();
    auto* hawk_brain  = hawk.get<f4::ai::BrainComponent>();
    ASSERT_NE(eagle_brain, nullptr);
    ASSERT_NE(hawk_brain, nullptr);

    bool eagle_ca_seen = false;
    bool hawk_ca_seen = false;
    double first_avoid_range_ft = -1.0;
    double min_range_ft = 1.0e9;
    int min_tick = -1;
    double post_pass_range_ft = -1.0;
    const int max_ticks = static_cast<int>(45.0 / kDt);
    for (int i = 0; i < max_ticks; ++i) {
        sim.tick(kDt);
        const auto* etf = eagle.get<entities::TransformComponent>();
        const auto* htf = hawk.get<entities::TransformComponent>();
        ASSERT_NE(etf, nullptr);
        ASSERT_NE(htf, nullptr);
        const double dx = htf->position.x - etf->position.x;
        const double dy = htf->position.y - etf->position.y;
        const double dz = htf->position.z - etf->position.z;
        const double rng = std::sqrt(dx * dx + dy * dy + dz * dz);
        if (rng < min_range_ft) {
            min_range_ft = rng;
            min_tick = i;
        }
        if (min_tick >= 0 && i == min_tick + 90) {
            post_pass_range_ft = rng;
        }
        if ((eagle_brain->collision_avoid().is_avoiding() ||
             hawk_brain->collision_avoid().is_avoiding()) &&
            first_avoid_range_ft < 0.0) {
            first_avoid_range_ft = rng;
        }
        if (eagle_brain->safety_mode() ==
            f4::ai::BrainComponent::SafetyMode::CollisionAvoid) {
            eagle_ca_seen = true;
        }
        if (hawk_brain->safety_mode() ==
            f4::ai::BrainComponent::SafetyMode::CollisionAvoid) {
            hawk_ca_seen = true;
        }
    }

    // The safety rung flew the break on both jets, outside the bubble,
    // the jets passed, the geometry opened, and nothing fired (blue on
    // blue — the hostility rule, plus the holds).
    ASSERT_TRUE(eagle_ca_seen) << "EAGLE1 never flew the collision break";
    ASSERT_TRUE(hawk_ca_seen) << "HAWK1 never flew the collision break";
    EXPECT_GT(first_avoid_range_ft, 200.0)
        << "the first break armed inside the protected bubble";
    EXPECT_GT(min_range_ft, 0.0) << "the tracks never crossed";
    ASSERT_GT(post_pass_range_ft, 400.0)
        << "the geometry did not open after the pass ("
        << post_pass_range_ft << " ft at +1.5 s)";
    EXPECT_EQ(eagle_brain->safety_mode(),
              f4::ai::BrainComponent::SafetyMode::None)
        << "collision avoid never released after the pass";
    EXPECT_EQ(hawk_brain->safety_mode(),
              f4::ai::BrainComponent::SafetyMode::None)
        << "collision avoid never released after the pass";
    EXPECT_TRUE(log.launched.empty()) << "blue-on-blue fired a missile";
    EXPECT_TRUE(log.gun_fired.empty()) << "blue-on-blue fired guns";
    for (const auto* h : {&eagle, &hawk}) {
        const auto* fm = h->get<f4::flight::FlightModelComponent>();
        ASSERT_NE(fm, nullptr);
        EXPECT_TRUE(fm->state().gear.inAir) << "a jet did not stay airborne";
    }
#else
    GTEST_SKIP() << "F4_SCENARIOS_DIR not defined for this target";
#endif
}
