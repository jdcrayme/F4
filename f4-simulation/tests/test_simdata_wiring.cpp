// f4-simulation/tests/test_simdata_wiring.cpp
//
// SimData AI scenario wiring — the Data/ side of the f4-convert pipeline
// (SimData.zip's BRAINDAT.brn + FORMDAT.FIL, converted to canonical JSON
// at build time) flowing all the way into spawned brains:
//
//   1. Scenario schema: "brain_profile" / "formation" on aircraft,
//      "brain_data_path" / "formation_library_path" on the scenario;
//      validation (formation requires lead_callsign); optional fields
//      keep every pre-SimData scenario loading byte-for-byte.
//   2. Simulation::initialize wiring: profiles resolve against the
//      generated fixtures (or the explicit paths) and inject —
//      BrainComponent::set_brain_archetype (doctrine: which combat
//      modes are armed) and WingmanModule::command_formation_slot
//      (the game's own FORMDAT station geometry). Unknown names and
//      unloadable files fail LOUDLY (authoring errors, not runtime).
//   3. The lazy-load contract: nothing references the data -> nothing
//      loads, no behavior changes.
//   4. Behavior: a SEAD wingman holds weapons while its (archetype-free)
//      lead fights — the .brn design intent, through the whole chain
//      (radar -> fusion -> ladder -> CombatIntent -> launch); and a
//      "spread" wingman converges on the file's own station geometry
//      (0.5 NM LEFT of the lead, 100 ft stacked — bvrengage.cpp:3378).
//
// Fixture needs: the generated f16.json (FlightModelComponent::init
// requires real aero tables) plus the generated braindata.json /
// formdat.json (f4-convert converts the shipped SimData fixtures at
// build time). Tests skip when the fixtures are absent.

#include <gtest/gtest.h>

#include "f4/simulation/simulation.hpp"

#include <f4/ai/brain_component.hpp>
#include <f4/ai/modules/wingman_module.hpp>
#include <f4/entities/entity.hpp>
#include <f4/messaging/bus.hpp>
#include <f4/weapons/f4_weapons.hpp>
#include <f4/sensors/f4_sensors.hpp>

#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <vector>

using namespace f4::simulation;
namespace entities = f4::entities;
namespace messaging = f4::messaging;
namespace weapons = f4::weapons;
namespace sensors = f4::sensors;

namespace {

// The f4-convert build-time output dir (f16.json + the SimData JSONs).
std::string fixtures_dir() {
    const char* env = std::getenv("F4_GENERATED_FIXTURES_DIR");
    std::string dir = env ? env : "";
#ifdef F4_GENERATED_FIXTURES_DIR
    if (dir.empty()) dir = F4_GENERATED_FIXTURES_DIR;
#endif
    return dir;
}

bool f16_config_path(std::string& out) {
    const auto p = std::filesystem::path(fixtures_dir()) / "f16.json";
    if (!std::filesystem::exists(p)) return false;
    // generic_string: embedded in scenario JSON (backslash would
    // JSON-escape); forward slashes work on Windows filesystems too.
    out = p.generic_string();
    return true;
}

constexpr double kDt = 1.0 / 60.0;
constexpr double kNmToFt = 6076.211;  // f4::data::kNmToFt (phyconst.h)

// The two-ship scenario template (both airborne, enroute north at
// 10,000 ft — the wingman starts ~11 kft displaced from the spread
// slot). Placeholders splice the variable parts (no raw-string quote
// seams to get wrong):
//   @LEAD@      — EAGLE2's lead_callsign line (empty = independent)
//   @WING@      — EAGLE2's extra fields (brain_profile / formation)
//   @SCENARIO@  — top-level fields (brain_data_path / combat block)
// Each field block carries its own leading comma and newline.
std::string two_ship_json(const std::string& f16_path,
                          const std::string& wingman_fields = {},
                          const std::string& scenario_fields = {},
                          const std::string& lead_callsign = "EAGLE1") {
    const std::string lead_line = lead_callsign.empty()
        ? std::string()
        : (",\n      \"lead_callsign\": \"" + lead_callsign + "\"");
    const std::string wing_line = wingman_fields.empty()
        ? std::string()
        : (",\n      " + wingman_fields);
    std::string json = R"({
  "name": "simdata_wiring",
  "theater": "korea",
  "aircraft": [
    { "callsign": "EAGLE1", "aircraft_config_path": "@F16@",
      "aircraft_name": "F-16C_50", "vis_type_index": 1052,
      "parking_spot": { "x": 0.0, "y": 0.0, "z": 10000.0 },
      "heading_rad": 0.0, "initial_fuel_lbs": 6500.0,
      "initial_vt_fps": 506.0, "spawn_in_air": true, "team": "blue" },
    { "callsign": "EAGLE2", "aircraft_config_path": "@F16@",
      "aircraft_name": "F-16C_50", "vis_type_index": 1052,
      "parking_spot": { "x": -9000.0, "y": -6000.0, "z": 10000.0 },
      "heading_rad": 0.0, "initial_fuel_lbs": 6500.0,
      "initial_vt_fps": 506.0, "spawn_in_air": true, "team": "blue"@LEAD@@WING@ }
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
  "record": false@SCENARIO@
})";
    json.replace(json.find("@F16@"), 5, f16_path);
    json.replace(json.find("@LEAD@"), 6, lead_line);
    json.replace(json.find("@WING@"), 6, wing_line);
    json.replace(json.find("@SCENARIO@"), 11, scenario_fields);
    return json;
}

} // namespace

// ============================================================================
// 1. Scenario schema (no fixtures needed)
// ============================================================================

TEST(SimDataScenario, ParsesBrainProfileAndFormationFields) {
    const auto s = load_scenario_from_string(two_ship_json(
        "f16.json",
        R"("brain_profile": "SEAD", "formation": "spread")"));
    ASSERT_EQ(s.aircraft.size(), 2u);
    EXPECT_EQ(s.aircraft[0].brain_profile, "");
    EXPECT_EQ(s.aircraft[0].formation, "");
    EXPECT_EQ(s.aircraft[1].brain_profile, "SEAD");
    EXPECT_EQ(s.aircraft[1].formation, "spread");
    EXPECT_EQ(s.aircraft[1].lead_callsign, "EAGLE1");
    // The scenario-level data paths default to empty (the build-time
    // fixtures apply); explicit strings parse verbatim.
    EXPECT_TRUE(s.brain_data_path.empty());
    EXPECT_TRUE(s.formation_library_path.empty());
}

TEST(SimDataScenario, ParsesDataPathOverrides) {
    const auto s = load_scenario_from_string(two_ship_json(
        "f16.json", R"("brain_profile": "Generic")",
        R"(,
           "brain_data_path": "Data/braindata.json",
           "formation_library_path": "Data/formdat.json")"));
    EXPECT_EQ(s.brain_data_path.string(), "Data/braindata.json");
    EXPECT_EQ(s.formation_library_path.string(), "Data/formdat.json");
}

TEST(SimDataScenario, FormationWithoutLeadCallsignIsRejected) {
    // A formation is flown by a WINGMAN — validate() catches the
    // authoring error before any entity spawns.
    EXPECT_THROW(
        load_scenario_from_string(two_ship_json(
            "f16.json", R"("formation": "spread")", {}, "")),
        std::runtime_error);
    try {
        (void)load_scenario_from_string(two_ship_json(
            "f16.json", R"("formation": "spread")", {}, ""));
    } catch (const std::runtime_error& e) {
        EXPECT_NE(std::string(e.what()).find("formation"), std::string::npos);
        EXPECT_NE(std::string(e.what()).find("lead_callsign"),
                  std::string::npos);
    }
}

TEST(SimDataScenario, NewFieldsAreOptional) {
    // The pre-SimData world: no profiles, no paths, same load.
    const auto s = load_scenario_from_string(two_ship_json("f16.json"));
    EXPECT_EQ(s.aircraft[1].brain_profile, "");
    EXPECT_EQ(s.aircraft[1].formation, "");
}

// ============================================================================
// 2. initialize() wiring (fixtures required)
// ============================================================================

TEST(SimDataWiring, BrainProfileInjectsArchetypeFromDefaultDir) {
    std::string f16;
    if (!f16_config_path(f16)) GTEST_SKIP() << "fixtures not generated";

    auto scenario = load_scenario_from_string(two_ship_json(
        f16, R"("brain_profile": "SEAD")"));
    Simulation sim(std::move(scenario), std::filesystem::path("."));
    sim.initialize();

    // The lazy contract, per side: brain data loaded, formations did not.
    EXPECT_TRUE(sim.brain_data_loaded());
    EXPECT_FALSE(sim.formation_library_loaded());

    ASSERT_EQ(sim.aircraft_entities().size(), 2u);
    entities::EntityHandle lead(sim.aircraft_entities()[0], &sim.world());
    entities::EntityHandle wing(sim.aircraft_entities()[1], &sim.world());
    auto* lead_brain = lead.get<f4::ai::BrainComponent>();
    auto* wing_brain = wing.get<f4::ai::BrainComponent>();
    ASSERT_NE(lead_brain, nullptr);
    ASSERT_NE(wing_brain, nullptr);

    // The wingman carries the SEAD doctrine; the lead (no profile)
    // keeps the built-in (null archetype).
    ASSERT_NE(wing_brain->brain_archetype(), nullptr);
    EXPECT_EQ(wing_brain->brain_archetype()->name, "SEAD");
    EXPECT_EQ(lead_brain->brain_archetype(), nullptr);

    // The shipped SEAD table: defensive modes armed, every engagement
    // mode disarmed (the .brn design intent — mission aircraft do not
    // pick fights). Verified through the SAME loaded rows the ladder
    // reads at runtime.
    const auto* a = wing_brain->brain_archetype();
    EXPECT_TRUE(a->mode_enabled(f4::data::BrainModeKey::MissileDefeat));
    EXPECT_FALSE(a->mode_enabled(f4::data::BrainModeKey::BVREngage));
    EXPECT_FALSE(a->mode_enabled(f4::data::BrainModeKey::WVREngage));
    EXPECT_FALSE(a->mode_enabled(f4::data::BrainModeKey::MissileEngage));
    EXPECT_FALSE(a->mode_enabled(f4::data::BrainModeKey::GunsEngage));
    EXPECT_TRUE(a->mode_enabled(f4::data::BrainModeKey::Wingy));
}

TEST(SimDataWiring, FormationInjectsFormdatSlotFromDefaultDir) {
    std::string f16;
    if (!f16_config_path(f16)) GTEST_SKIP() << "fixtures not generated";

    auto scenario = load_scenario_from_string(two_ship_json(
        f16, R"("formation": "spread")"));
    Simulation sim(std::move(scenario), std::filesystem::path("."));
    sim.initialize();

    // Lazy contract, the other side: formations loaded, brain data not.
    EXPECT_FALSE(sim.brain_data_loaded());
    EXPECT_TRUE(sim.formation_library_loaded());

    entities::EntityHandle wing(sim.aircraft_entities()[1], &sim.world());
    auto* brain = wing.get<f4::ai::BrainComponent>();
    ASSERT_NE(brain, nullptr);
    ASSERT_TRUE(brain->is_wingman());

    // The FORMDAT slot drives the station (the game's own spread:
    // 2-ship slot 0.5 NM at -90 deg, inherited from slot[0]).
    auto& wingman = brain->wingman();
    EXPECT_TRUE(wingman.formation_slot_active());
    EXPECT_EQ(wingman.formation_name(), "spread");
}

TEST(SimDataWiring, NothingReferencesMeansNothingLoads) {
    std::string f16;
    if (!f16_config_path(f16)) GTEST_SKIP() << "fixtures not generated";

    auto scenario = load_scenario_from_string(two_ship_json(f16));
    Simulation sim(std::move(scenario), std::filesystem::path("."));
    sim.initialize();

    EXPECT_FALSE(sim.brain_data_loaded());
    EXPECT_FALSE(sim.formation_library_loaded());

    entities::EntityHandle wing(sim.aircraft_entities()[1], &sim.world());
    auto* brain = wing.get<f4::ai::BrainComponent>();
    ASSERT_NE(brain, nullptr);
    EXPECT_EQ(brain->brain_archetype(), nullptr);
    EXPECT_FALSE(brain->wingman().formation_slot_active());
}

TEST(SimDataWiring, ExplicitBrainDataPathIsHonored) {
    std::string f16;
    if (!f16_config_path(f16)) GTEST_SKIP() << "fixtures not generated";

    // generic_string: embedded in scenario JSON below (backslash would
    // JSON-escape); forward slashes work on Windows filesystems too.
    const auto brain_json =
        (std::filesystem::path(fixtures_dir()) / "simdata" / "braindata.json").generic_string();
    ASSERT_TRUE(std::filesystem::exists(brain_json));

    // Explicit path (the "Data/" layout a real install would use) +
    // a different archetype — proves the path branch, not the default.
    auto scenario = load_scenario_from_string(two_ship_json(
        f16, R"("brain_profile": "Intercepter")",
        R"(,
           "brain_data_path": ")" + brain_json + R"(")"));
    Simulation sim(std::move(scenario), std::filesystem::path("."));
    sim.initialize();

    entities::EntityHandle wing(sim.aircraft_entities()[1], &sim.world());
    auto* brain = wing.get<f4::ai::BrainComponent>();
    ASSERT_NE(brain, nullptr);
    ASSERT_NE(brain->brain_archetype(), nullptr);
    EXPECT_EQ(brain->brain_archetype()->name, "Intercepter");
}

TEST(SimDataWiring, UnknownBrainProfileFailsLoudly) {
    std::string f16;
    if (!f16_config_path(f16)) GTEST_SKIP() << "fixtures not generated";

    auto scenario = load_scenario_from_string(two_ship_json(
        f16, R"("brain_profile": "Bogus")"));
    Simulation sim(std::move(scenario), std::filesystem::path("."));

    try {
        sim.initialize();
        FAIL() << "unknown brain_profile must fail initialize()";
    } catch (const std::runtime_error& e) {
        const std::string msg(e.what());
        EXPECT_NE(msg.find("Bogus"), std::string::npos);
        // The error carries the known-name list (actionable authoring).
        EXPECT_NE(msg.find("Generic"), std::string::npos);
        EXPECT_NE(msg.find("SEAD"), std::string::npos);
    }
}

TEST(SimDataWiring, UnknownFormationFailsLoudly) {
    std::string f16;
    if (!f16_config_path(f16)) GTEST_SKIP() << "fixtures not generated";

    auto scenario = load_scenario_from_string(two_ship_json(
        f16, R"("formation": "vic")"));
    Simulation sim(std::move(scenario), std::filesystem::path("."));

    try {
        sim.initialize();
        FAIL() << "unknown formation must fail initialize()";
    } catch (const std::runtime_error& e) {
        const std::string msg(e.what());
        EXPECT_NE(msg.find("vic"), std::string::npos);
        EXPECT_NE(msg.find("spread"), std::string::npos);
    }
}

TEST(SimDataWiring, MissingBrainDataFileFailsLoudly) {
    std::string f16;
    if (!f16_config_path(f16)) GTEST_SKIP() << "fixtures not generated";

    auto scenario = load_scenario_from_string(two_ship_json(
        f16, R"("brain_profile": "SEAD")",
        R"(,
           "brain_data_path": "no/such/braindata.json")"));
    Simulation sim(std::move(scenario), std::filesystem::path("."));

    try {
        sim.initialize();
        FAIL() << "missing brain data file must fail initialize()";
    } catch (const std::runtime_error& e) {
        const std::string msg(e.what());
        EXPECT_NE(msg.find("brain data"), std::string::npos);
        EXPECT_NE(msg.find("no/such/braindata.json"), std::string::npos);
    }
}

// ============================================================================
// 3. Behavior: the SEAD stand-down + the spread station, end to end
// ============================================================================

TEST(SimDataBehavior, SeadWingmanHoldsWeaponsWhileLeadFights) {
    std::string f16;
    if (!f16_config_path(f16)) GTEST_SKIP() << "fixtures not generated";

    // Stern chase: EAGLE1 (lead, blue) + EAGLE2 (SEAD wingman, blue)
    // vs BANDIT1 (red) ~13 NM ahead — inside the deterministic-detection
    // knee (the combat integration test's geometry). The lead carries NO
    // archetype (built-in doctrine: weapons free); the wingman carries
    // the shipped SEAD table (every engagement mode disarmed).
    auto json = two_ship_json(
        f16, R"("brain_profile": "SEAD")",
        R"(,
           "combat": { "enabled": true, "radar_rng_seed": 777 })");
    // Splice in the red bandit ahead of the flight: insert as the last
    // element of the AIRCRAFT array (the first "\n  ]," close — the
    // taxi-route and waypoint arrays close differently).
    const std::string bandit =
        R"({ "callsign": "BANDIT1", "aircraft_config_path": ")" + f16 +
        R"(", "aircraft_name": "F-16C_50", "vis_type_index": 1052,
      "parking_spot": { "x": 0.0, "y": 80000.0, "z": 10000.0 },
      "heading_rad": 0.0, "initial_fuel_lbs": 6500.0,
      "initial_vt_fps": 420.0, "spawn_in_air": true, "team": "red" })";
    const auto arr_close = json.find("\n  ],");
    ASSERT_NE(arr_close, std::string::npos);
    json.insert(arr_close, ",\n    " + bandit);

    auto scenario = load_scenario_from_string(json);
    Simulation sim(std::move(scenario), std::filesystem::path("."));
    sim.initialize();
    ASSERT_EQ(sim.aircraft_entities().size(), 3u);

    const auto lead_id = sim.aircraft_entities()[0];
    const auto wing_id = sim.aircraft_entities()[1];

    // Every launch on the bus during the run.
    std::vector<weapons::MissileLaunchedMessage> launched;
    sim.bus().subscribe<weapons::MissileLaunchedMessage>(
        [&launched](const weapons::MissileLaunchedMessage& m) {
            launched.push_back(m);
        });

    // 60 s: enough for detection -> track -> lock -> release at this
    // geometry (the combat test's timeline) and the wingman's rejoin.
    for (int i = 0; i < 60 * 60; ++i) sim.tick(kDt);

    entities::EntityHandle wing(wing_id, &sim.world());
    auto* brain = wing.get<f4::ai::BrainComponent>();
    ASSERT_NE(brain, nullptr);
    ASSERT_NE(brain->brain_archetype(), nullptr);
    EXPECT_EQ(brain->brain_archetype()->name, "SEAD");

    // The wingman NEVER entered an engagement mode: BVR/WVR are the
    // disarmed rungs; what remains is the formation rung (Wingy stays
    // armed in the shipped SEAD table — the mission aircraft still
    // flies the flight) or a defensive save (MissileDefeat armed).
    const std::string mode = brain->combat_mode_name();
    EXPECT_NE(mode, "BVREngage")
        << "SEAD wingman entered BVR (disarmed archetype)";
    EXPECT_NE(mode, "WVREngage")
        << "SEAD wingman entered WVR (disarmed archetype)";

    // The lead fought — at least one AMRAAM left the rail.
    bool lead_launched = false;
    for (const auto& m : launched) {
        if (m.shooter_id == lead_id.value) lead_launched = true;
    }
    EXPECT_TRUE(lead_launched)
        << "the archetype-free lead must employ weapons in this geometry";

    // The SEAD wingman never released anything — the .brn doctrine held
    // through the whole chain (fusion -> ladder -> intent -> launch).
    for (const auto& m : launched) {
        EXPECT_NE(m.shooter_id, wing_id.value)
            << "SEAD wingman released a weapon (disarmed archetype)";
    }
}

TEST(SimDataBehavior, SpreadWingmanConvergesOnFormdatStation) {
    std::string f16;
    if (!f16_config_path(f16)) GTEST_SKIP() << "fixtures not generated";

    auto scenario = load_scenario_from_string(two_ship_json(
        f16, R"("formation": "spread")"));
    Simulation sim(std::move(scenario), std::filesystem::path("."));
    sim.initialize();

    const auto lead_id = sim.aircraft_entities()[0];
    const auto wing_id = sim.aircraft_entities()[1];

    // 150 s of enroute flight: the wingman starts 9+ kft displaced
    // (southwest), rejoins, and settles on the file's own station.
    for (int i = 0; i < 150 * 60; ++i) sim.tick(kDt);

    entities::EntityHandle lead(lead_id, &sim.world());
    entities::EntityHandle wing(wing_id, &sim.world());
    const auto* lead_tf = lead.get<entities::TransformComponent>();
    const auto* wing_tf = wing.get<entities::TransformComponent>();
    ASSERT_NE(lead_tf, nullptr);
    ASSERT_NE(wing_tf, nullptr);

    auto* brain = wing.get<f4::ai::BrainComponent>();
    ASSERT_NE(brain, nullptr);
    EXPECT_TRUE(brain->wingman().formation_slot_active());

    // The file's spread station: 0.5 NM at -90 deg (LEFT of the lead's
    // north heading), 100 ft low (flightIdx stack). Compare the LIVE
    // wingman against the slot geometry relative to the live lead.
    const double range_ft = 0.5 * kNmToFt;
    const double dx = wing_tf->position.x - lead_tf->position.x;
    const double dy = wing_tf->position.y - lead_tf->position.y;
    const double dz = wing_tf->position.z - lead_tf->position.z;

    // Direction + distance: LEFT (west) of the lead, ~0.5 NM out.
    EXPECT_LT(dx, -0.5 * range_ft)
        << "wingman is not on the spread slot's LEFT side (dx = " << dx
        << " ft)";
    const double lateral_err = std::hypot(dx + range_ft, dy);
    EXPECT_LT(lateral_err, 900.0)
        << "wingman did not converge on the spread station (lateral error "
        << lateral_err << " ft)";
    // The stack: ~100 ft below (allow the capture transient).
    EXPECT_NEAR(dz, -100.0, 350.0);
}
