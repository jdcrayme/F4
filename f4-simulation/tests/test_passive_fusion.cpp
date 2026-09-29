// test_passive_fusion.cpp — the blind-spot geometry companion to the
// landed SENSOR-FUSION-1 tranche (the passive-sensor fusion leg).
//
// test_sensor_fidelity pins the tranche's SEMANTICS (the policy's
// passive verdicts, the attach gates, the throttle band) over a
// hand-rolled policy world. THIS file pins the GEOMETRY end to end
// through Simulation::tick — the aft-quarter blind-spot chase that is
// the tranche's whole reason to exist:
//
//   1. the gate: combat.passive_sensors parses, defaults FALSE, and the
//      golden identity holds — gate off attaches no passive component
//      and the detection picture is exactly the pre-FUSE one
//   2. the attach: gate on arms both sensors (team tags, derived seeds,
//      the airframe IRST card resolved from the configured library —
//      the library hookup the attach path consumes)
//   3. the fold: an IRST/eyeball contact lights detected_by_visual on
//      the fusion's TargetInfo while the radar leg stays false
//   4. the fight: the spawned brain's own fusion (the policy the sim
//      installed) makes the bandit its threat target with a blind radar
//   5. the corpse rule: killed entities do not paint through the
//      passive legs (classify's corpse early-out covers both legs)
//
// Geometry (the aft-quarter blind-spot chase — see the helper): EAGLE1
// (blue) heading east at the origin, BANDIT1 (red) 6 NM away at 115° off
// the shooter's track, heading west. Both default radar bars are
// track-centered (±60°, the C6 boresight) — NEITHER radar ever holds a
// track on the other, and nothing in the AI steers the bar directly. The
// IRST (±120° off the nose, 10 NM nominal, flat-1.0 default signature =>
// sure-thing inside 7.5 NM) and the eyeball (181° — everything, 10 NM
// threshold) both see the bandit. Clean separation: passive legs on,
// radar leg dead, no RWR contamination (the bandit's radar never scans
// the shooter either).

#include <gtest/gtest.h>

#include "f4/simulation/simulation.hpp"
#include "f4/simulation/combat_bridge.hpp"

#include <f4/ai/brain_component.hpp>
#include <f4/ai/sensor_fusion.hpp>
#include <f4/entities/entity.hpp>
#include <f4/sensors/f4_sensors.hpp>

#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

using namespace f4::simulation;
namespace entities = f4::entities;
namespace sensors = f4::sensors;

namespace {

constexpr double kDt = 1.0 / 60.0;

// Locate the generated F-16 aircraft config fixture (the combat tests'
// shape — FlightModelComponent::init requires real aero tables). Empty
// string = not generated; the caller skips.
std::string f16_config_path() {
    const char* env = std::getenv("F4_GENERATED_FIXTURES_DIR");
    std::string dir = env ? env : "";
#ifdef F4_GENERATED_FIXTURES_DIR
    if (dir.empty()) dir = F4_GENERATED_FIXTURES_DIR;
#endif
    if (dir.empty()) return "";
    const auto path = std::filesystem::path(dir) / "f16.json";
    return std::filesystem::exists(path) ? path.generic_string() : "";
}

// Geometry (the aft-quarter blind-spot chase): EAGLE1 (blue) at the
// origin heading east, BANDIT1 (red) 6 NM away at bearing 205 (115° off
// the shooter's ground track) heading WEST. The C6 host boresights every
// search bar onto its owner's ground track (±60°), so the shooter's bar
// (centered east) misses the bandit by 55° — and nothing in the AI
// re-centers the bar; it chases the track, which only rotates after the
// brain DECIDES to turn, which under this tranche requires the passive
// contact first. The IRST (±120° off the nose, 10 NM nominal, flat-1.0
// default signature => sure-thing inside 7.5 NM) holds the bandit with
// 5° of margin. The bandit's bar (centered west) misses the shooter by
// the same 55°, so no RWR strobe contaminates either picture. With the
// gate OFF neither brain sees anything, neither turns, and the picture
// stays exactly the pre-FUSE silence — the golden identity.
std::string east_chase_json(const std::string& f16_path,
                            bool passive,
                            const std::string& extra_combat = {}) {
    return R"({
  "name": "passive_fusion_blind_spot",
  "theater": "korea",
  "aircraft": [
    { "callsign": "EAGLE1", "aircraft_config_path": ")" + f16_path + R"(",
      "aircraft_name": "F-16C_50", "vis_type_index": 1052,
      "parking_spot": { "x": 0.0, "y": 0.0, "z": 10000.0 },
      "heading_rad": 1.5707963267948966, "initial_fuel_lbs": 6500.0,
      "initial_vt_fps": 506.0, "spawn_in_air": true, "team": "blue" },
    { "callsign": "BANDIT1", "aircraft_config_path": ")" + f16_path + R"(",
      "aircraft_name": "F-16C_50", "vis_type_index": 1052,
      "parking_spot": { "x": -15406.0, "y": -33043.0, "z": 10000.0 },
      "heading_rad": 4.712388980384690, "initial_fuel_lbs": 6500.0,
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
    { "name": "FAR_WEST", "position": { "x": -500000.0, "y": 0.0, "z": 10000.0 },
      "speed_kts": 420.0 }
  ],
  "start_enroute": true,
  "sim_dt": 0.016666666666666,
  "total_ticks": 30000,
  "combat": { "enabled": true,
              "radar_rng_seed": 777,
              "bvr_hold": true,
              "missiles_hold": true,
              "passive_sensors": )" + (passive ? "true" : "false") + extra_combat + R"( }
})";
}

// A distinctive airframe IRST card: NOT the defaults (az 120 / el 60 /
// 10 NM / ground 1.0) — the attach must provably read the library.
const char* kIrstCardJson = R"({
  "kind": "f4.irstdata",
  "version": 1,
  "sensors": [
    { "name": "generic", "az_limit_deg": 90.0, "el_limit_deg": 40.0,
      "nominal_range_nm": 8.0, "ground_factor": 0.5, "flare_chance": 0.0 },
    { "name": "aim9l", "az_limit_deg": 60.0, "el_limit_deg": 60.0,
      "nominal_range_nm": 10.0, "ground_factor": 0.001, "flare_chance": 0.2 }
  ]
})";

std::string write_temp_irst_cards() {
    const auto path = std::filesystem::temp_directory_path() /
                      "f4_passive_fusion_irst_cards.json";
    std::ofstream out(path, std::ios::binary);
    out << kIrstCardJson;
    return path.generic_string();
}

const f4::ai::TargetInfo* find_target(const f4::ai::SensorFusion& sf,
                                      std::uint64_t id) {
    for (const auto& t : sf.targets()) {
        if (t.entity_id == id) return &t;
    }
    return nullptr;
}

} // namespace

// ============================================================================
// 1. The gate: parse, default, golden identity.
// ============================================================================

TEST(PassiveFusion, GateParsesAndDefaultsOff) {
    const auto f16 = f16_config_path();
    ASSERT_FALSE(f16.empty()) << "f16.json fixture not generated";

    // Absent key = false (the golden identity default).
    auto s1 = load_scenario_from_string(east_chase_json(f16, false));
    EXPECT_FALSE(s1.combat.passive_sensors);

    // Explicit true round-trips.
    auto s2 = load_scenario_from_string(east_chase_json(f16, true));
    EXPECT_TRUE(s2.combat.passive_sensors);
    EXPECT_TRUE(s2.combat.enabled);
}

TEST(PassiveFusion, GateOffAttachesNoPassiveComponents) {
    const auto f16 = f16_config_path();
    if (f16.empty()) GTEST_SKIP() << "f16.json fixture not generated";

    auto scenario = load_scenario_from_string(east_chase_json(f16, false));
    Simulation sim(std::move(scenario), std::filesystem::path("."));
    sim.initialize();

    ASSERT_GE(sim.aircraft_entities().size(), 2u);
    for (const auto eid : sim.aircraft_entities()) {
        entities::EntityHandle h(eid, &sim.world());
        EXPECT_EQ(h.get<sensors::IrstComponent>(), nullptr)
            << "IRST attached with the gate OFF — the golden identity broke";
        EXPECT_EQ(h.get<sensors::VisualComponent>(), nullptr)
            << "eyeball attached with the gate OFF — the golden identity "
               "broke";
        // The rest of the combat set is intact (the gate only touches the
        // passive pair).
        EXPECT_NE(h.get<sensors::RadarSimComponent>(), nullptr);
        EXPECT_NE(h.get<sensors::RwrComponent>(), nullptr);
    }
}

TEST(PassiveFusion, GateOnAttachesPassiveComponents) {
    const auto f16 = f16_config_path();
    if (f16.empty()) GTEST_SKIP() << "f16.json fixture not generated";

    auto scenario = load_scenario_from_string(east_chase_json(f16, true));
    Simulation sim(std::move(scenario), std::filesystem::path("."));
    sim.initialize();

    ASSERT_GE(sim.aircraft_entities().size(), 2u);
    std::uint32_t seed0 = 0, seed1 = 0;
    for (std::size_t i = 0; i < sim.aircraft_entities().size(); ++i) {
        entities::EntityHandle h(sim.aircraft_entities()[i], &sim.world());
        const auto* irst = h.get<sensors::IrstComponent>();
        const auto* visual = h.get<sensors::VisualComponent>();
        ASSERT_NE(irst, nullptr);
        ASSERT_NE(visual, nullptr);
        // The IFF reference rides the spawn team.
        const bool blue = i == 0;
        EXPECT_EQ(irst->own_team, blue ? "blue" : "red");
        EXPECT_EQ(visual->own_team, blue ? "blue" : "red");
        // The IRST stream is per-aircraft deterministic (base + the
        // 0x2000 passive-slot constant + index) — co-mounted sensors
        // never share a sequence, same-seed scenarios replay identically.
        (blue ? seed0 : seed1) = irst->rng_seed;
    }
    EXPECT_EQ(seed0, 777u + 0x2000u + 0u);
    EXPECT_EQ(seed1, 777u + 0x2000u + 1u);
    EXPECT_NE(seed0, seed1);
}

TEST(PassiveFusion, IrstAirframeCardResolvesFromLibrary) {
    const auto f16 = f16_config_path();
    if (f16.empty()) GTEST_SKIP() << "f16.json fixture not generated";

    const auto cards_path = write_temp_irst_cards();
    auto scenario = load_scenario_from_string(
        east_chase_json(f16, true,
                        ",\n              \"ir_seeker_data_path\": \"" +
                            cards_path + "\""));
    Simulation sim(std::move(scenario), std::filesystem::path("."));
    sim.initialize();

    entities::EntityHandle h(sim.aircraft_entities()[0], &sim.world());
    const auto* irst = h.get<sensors::IrstComponent>();
    ASSERT_NE(irst, nullptr);
    // The "generic" row's shape — NOT the component defaults.
    EXPECT_DOUBLE_EQ(irst->params.az_limit_deg, 90.0);
    EXPECT_DOUBLE_EQ(irst->params.el_limit_deg, 40.0);
    EXPECT_DOUBLE_EQ(irst->params.nominal_range_nm, 8.0);
    EXPECT_DOUBLE_EQ(irst->params.ground_factor, 0.5);

    std::filesystem::remove(cards_path);
}

// ============================================================================
// 2. The fold: passive contacts light the fusion's VISUAL flag.
// ============================================================================

TEST(PassiveFusion, PassiveLegsLightVisualFlagWithBlindRadar) {
    const auto f16 = f16_config_path();
    if (f16.empty()) GTEST_SKIP() << "f16.json fixture not generated";

    auto scenario = load_scenario_from_string(east_chase_json(f16, true));
    Simulation sim(std::move(scenario), std::filesystem::path("."));
    sim.initialize();

    const auto own_id = sim.aircraft_entities()[0];
    const auto bandit_id = sim.aircraft_entities()[1];

    // A standalone fusion + policy (the test-3 shape in
    // test_combat_integration). Poll at first contact: the first tick
    // any passive leg holds the bandit, read the flags — BEFORE the
    // brain's own reaction (which starts a turn that would eventually
    // swing the boresighted bar onto the bandit and light the radar leg
    // too; the passive-first ordering is itself part of the contract).
    f4::ai::SensorFusion sf;
    sf.initialize(own_id.value, sim.world(), sim.bus(),
                  f4::ai::SkillLevel::Veteran);
    RadarBackedDetectionPolicy policy(sim.world(), own_id.value);
    sf.set_detection_policy(&policy);

    const f4::ai::TargetInfo* bandit = nullptr;
    for (int i = 0; i < 10 * 60 && bandit == nullptr; ++i) {
        sim.tick(kDt);
        if (i % 15 != 0) continue;   // poll every quarter second
        sf.force_refresh();
        const auto* t = find_target(sf, bandit_id.value);
        if (t != nullptr && f4::ai::SensorFusion::can_see(*t)) bandit = t;
    }
    ASSERT_NE(bandit, nullptr)
        << "no passive leg ever held the bandit in 10 s of flight";
    EXPECT_TRUE(bandit->detected_by_visual)
        << "an IRST/eyeball contact at 6 NM aft-quarter did not fold in";
    EXPECT_FALSE(bandit->detected_by_radar)
        << "the track-centered bar must NOT see a bandit 55° off the beam";
    EXPECT_FALSE(bandit->detected_by_rwr)
        << "no emitter painted the shooter in this geometry";
    EXPECT_FALSE(bandit->detected_by_gci) << "GCI-omniscience leaked";
}

TEST(PassiveFusion, GoldenIdentityPictureUnchanged) {
    const auto f16 = f16_config_path();
    if (f16.empty()) GTEST_SKIP() << "f16.json fixture not generated";

    // The SAME blind-spot geometry with the gate OFF: no passive legs
    // (so neither brain sees anything and NEITHER aircraft ever turns —
    // the bars never chase), no radar track (both bars miss by 55°), no
    // RWR strobe — the bandit is invisible, exactly what the pre-FUSE
    // policy answered. The passive-armed twin of this test sees the
    // bandit through the visual leg: the gate is the whole difference.
    auto scenario = load_scenario_from_string(east_chase_json(f16, false));
    Simulation sim(std::move(scenario), std::filesystem::path("."));
    sim.initialize();

    const auto own_id = sim.aircraft_entities()[0];
    const auto bandit_id = sim.aircraft_entities()[1];
    for (int i = 0; i < 5 * 60; ++i) sim.tick(kDt);

    f4::ai::SensorFusion sf;
    sf.initialize(own_id.value, sim.world(), sim.bus(),
                  f4::ai::SkillLevel::Veteran);
    RadarBackedDetectionPolicy policy(sim.world(), own_id.value);
    sf.set_detection_policy(&policy);
    sf.force_refresh();

    const auto* bandit = find_target(sf, bandit_id.value);
    ASSERT_NE(bandit, nullptr);
    EXPECT_FALSE(f4::ai::SensorFusion::can_see(*bandit))
        << "the gate-off picture changed — the golden identity broke";
    EXPECT_FALSE(bandit->detected_by_visual);
    EXPECT_FALSE(bandit->detected_by_radar);
    EXPECT_FALSE(bandit->detected_by_rwr);
}

// ============================================================================
// 3. The fight: the spawned brain sees and engages on passive legs alone.
// ============================================================================

TEST(PassiveFusion, DeadRadarStillFights) {
    const auto f16 = f16_config_path();
    if (f16.empty()) GTEST_SKIP() << "f16.json fixture not generated";

    auto scenario = load_scenario_from_string(east_chase_json(f16, true));
    Simulation sim(std::move(scenario), std::filesystem::path("."));
    sim.initialize();

    const auto own_id = sim.aircraft_entities()[0];
    const auto bandit_id = sim.aircraft_entities()[1];
    auto* own_brain =
        entities::EntityHandle(own_id, &sim.world())
            .get<f4::ai::BrainComponent>();
    ASSERT_NE(own_brain, nullptr);

    // The brain's OWN fusion (the policy the sim installed) must make
    // the bandit its threat target — the WVR picture — with the radar
    // leg never lighting. Sampled at the FIRST threat tick: the passive
    // contact precedes the reaction (the bar chases the track, which
    // only rotates after this very moment), so the radar leg is still
    // 55° short of the bandit.
    const f4::ai::TargetInfo* threat = nullptr;
    for (int i = 0; i < 15 * 60 && threat == nullptr; ++i) {
        sim.tick(kDt);
        threat = own_brain->sensors().threat_target();
    }
    ASSERT_NE(threat, nullptr)
        << "the brain never engaged a bandit its passive legs could see";
    EXPECT_EQ(threat->entity_id, bandit_id.value);
    EXPECT_TRUE(threat->is_hostile);
    EXPECT_FALSE(threat->is_missile);
    EXPECT_TRUE(threat->detected_by_visual)
        << "the engagement arrived without the passive legs";
    EXPECT_FALSE(threat->detected_by_radar)
        << "the radar track appeared — the geometry stopped testing the "
           "passive-only path";
}

// ============================================================================
// 4. The corpse rule: the dead do not paint through the passive legs.
// ============================================================================

TEST(PassiveFusion, CorpsesDoNotPaintThroughPassiveLegs) {
    const auto f16 = f16_config_path();
    if (f16.empty()) GTEST_SKIP() << "f16.json fixture not generated";

    auto scenario = load_scenario_from_string(east_chase_json(f16, true));
    Simulation sim(std::move(scenario), std::filesystem::path("."));
    sim.initialize();

    const auto own_id = sim.aircraft_entities()[0];
    const auto bandit_id = sim.aircraft_entities()[1];

    // Let the passive sweeps make their contacts.
    for (int i = 0; i < 5 * 60; ++i) sim.tick(kDt);

    RadarBackedDetectionPolicy policy(sim.world(), own_id.value);
    f4::ai::TargetInfo t{};
    t.entity_id = bandit_id.value;
    const auto alive = policy.classify(t);
    EXPECT_TRUE(alive.visual)
        << "precondition: the bandit holds a passive contact";

    // Kill the bandit (the M2 corpse shape — the airframe flies, the
    // TARGET picture must blank) and classify again.
    entities::EntityHandle bandit(bandit_id, &sim.world());
    auto* dmg = bandit.get<entities::DamageStateComponent>();
    ASSERT_NE(dmg, nullptr);
    dmg->killed = true;
    const auto dead = policy.classify(t);
    EXPECT_FALSE(dead.visual);
    EXPECT_FALSE(dead.radar);
    EXPECT_FALSE(dead.rwr);
    EXPECT_FALSE(dead.gci);
}
