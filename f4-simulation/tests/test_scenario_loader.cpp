// f4-simulation/tests/test_scenario_loader.cpp
//
// Unit tests for the scenario JSON loader.
//
// These tests verify:
//   1. A valid scenario JSON loads with all fields populated correctly
//   2. A missing file throws with a helpful message
//   3. An empty scenario JSON throws (missing required fields)
//   4. A scenario with no aircraft throws
//   5. A taxi route with < 2 waypoints throws
//
// The test loads the bundled fixture `takeoff_kunsan.json` from the
// F4_SIMULATION_TEST_FIXTURES_DIR compile definition (set in CMakeLists.txt).

#include <gtest/gtest.h>

#include "f4/simulation/scenario.hpp"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

using namespace f4::simulation;

namespace {
std::string fixture_dir() {
    const char* env = std::getenv("F4_SIMULATION_TEST_FIXTURES_DIR");
    if (env) return env;
#ifdef F4_SIMULATION_TEST_FIXTURES_DIR
    return F4_SIMULATION_TEST_FIXTURES_DIR;
#else
    return ".";
#endif
}

std::string load_fixture(const std::string& name) {
    const auto path = std::filesystem::path(fixture_dir()) / name;
    std::ifstream f(path);
    if (!f) {
        ADD_FAILURE() << "Could not open fixture: " << path;
        return {};
    }
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}
} // namespace

TEST(ScenarioLoader, LoadsValidScenarioWithAllFields) {
    const auto json = load_fixture("takeoff_kunsan.json");
    ASSERT_FALSE(json.empty());

    auto s = load_scenario_from_string(json);

    EXPECT_EQ(s.name, "takeoff_kunsan");
    EXPECT_EQ(s.theater, "korea");
    EXPECT_EQ(s.sim_dt, 1.0 / 60.0);
    EXPECT_EQ(s.total_ticks, 600);
    EXPECT_TRUE(s.record);
    EXPECT_EQ(s.record_path, "trace.json");

    // Asset paths (NOT resolved by load_scenario_from_string — only by load_scenario)
    EXPECT_EQ(s.models_hdr_path, "KoreaObj.HDR");
    EXPECT_EQ(s.models_lod_path, "KoreaObj.LOD");
    EXPECT_EQ(s.models_tex_path, "KoreaObj.TEX");

    // Aircraft
    ASSERT_EQ(s.aircraft.size(), 1u);
    const auto& a = s.aircraft.front();
    EXPECT_EQ(a.callsign, "EAGLE1");
    EXPECT_EQ(a.aircraft_config_path, "f16.json");
    EXPECT_EQ(a.aircraft_name, "F-16C_50");
    EXPECT_EQ(a.vis_type_index, 1052);
    EXPECT_DOUBLE_EQ(a.parking_spot.x, 0.0);
    EXPECT_DOUBLE_EQ(a.parking_spot.y, 0.0);
    EXPECT_DOUBLE_EQ(a.parking_spot.z, 50.0);
    EXPECT_DOUBLE_EQ(a.heading_rad, 0.0);
    EXPECT_DOUBLE_EQ(a.initial_fuel_lbs, 6500.0);

    // Airfield
    EXPECT_EQ(s.airfield.active_runway_id, 36);
    EXPECT_EQ(s.airfield.active_runway_name, "Rwy 36L");
    EXPECT_DOUBLE_EQ(s.airfield.runway_heading_rad, 0.0);
    EXPECT_DOUBLE_EQ(s.airfield.threshold_position.x, 500.0);
    EXPECT_DOUBLE_EQ(s.airfield.threshold_position.y, 8000.0);
    EXPECT_DOUBLE_EQ(s.airfield.departure_altitude_ft, 2500.0);

    // Taxi route
    ASSERT_EQ(s.airfield.taxi_route.size(), 5u);
    EXPECT_DOUBLE_EQ(s.airfield.taxi_route.front().x, 0.0);    // parking
    // The route ENDS at the hold-short point (offset west of the runway
    // centerline, short of the threshold) — the aircraft stops here and
    // requests takeoff clearance before lining up.
    EXPECT_DOUBLE_EQ(s.airfield.taxi_route.back().x, 330.0);   // hold-short
    EXPECT_DOUBLE_EQ(s.airfield.taxi_route.back().y, 7600.0);
}

TEST(ScenarioLoader, MissingFileThrows) {
    const std::filesystem::path nope = "/tmp/this_scenario_does_not_exist.json";
    EXPECT_THROW(load_scenario(nope), std::runtime_error);
}

// ============================================================================
// Flight-plan waypoints + taxi-in route
// ============================================================================

namespace {

// Minimal valid scenario body with the given JSON fragment spliced in
// (kept in the style of the fixture: aircraft + airfield).
std::string with_extra_block(const std::string& extra) {
    return R"({
        "name": "wp_test",
        "aircraft": [
            {"callsign":"EAGLE1","aircraft_config_path":"f16.json",
             "aircraft_name":"F-16C_50","vis_type_index":1052,
             "parking_spot":{"x":0,"y":0,"z":50},"heading_rad":0,"initial_fuel_lbs":6500}
        ],
        "airfield": {
            "active_runway_id": 36,
            "active_runway_name": "Rwy 36L",
            "runway_heading_rad": 0.0,
            "threshold_position": {"x":500,"y":8000,"z":50},
            "runway_end_position": {"x":500,"y":13000,"z":50},
            "taxi_route": [
                {"x":0,"y":0,"z":50},
                {"x":330,"y":7600,"z":50}
            ]
        },
        )" + extra + R"(
    })";
}

} // namespace

TEST(ScenarioLoader, WaypointsAreParsedInOrder) {
    const auto json = with_extra_block(R"(
        "waypoints": [
            {"name":"WP1","position":{"x":0,"y":40000,"z":10000},"speed_kts":400},
            {"name":"APCH_FIX","position":{"x":500,"y":-25000,"z":3000},"speed_kts":250}
        ]
    )");
    auto s = load_scenario_from_string(json);
    ASSERT_EQ(s.waypoints.size(), 2u);
    EXPECT_EQ(s.waypoints[0].name, "WP1");
    EXPECT_DOUBLE_EQ(s.waypoints[0].position.y, 40000.0);
    EXPECT_DOUBLE_EQ(s.waypoints[0].position.z, 10000.0);
    EXPECT_DOUBLE_EQ(s.waypoints[0].speed_kts, 400.0);
    EXPECT_EQ(s.waypoints[1].name, "APCH_FIX");
    EXPECT_DOUBLE_EQ(s.waypoints[1].position.x, 500.0);
    EXPECT_DOUBLE_EQ(s.waypoints[1].speed_kts, 250.0);
}

TEST(ScenarioLoader, WaypointSpeedDefaultsTo350) {
    const auto json = with_extra_block(R"(
        "waypoints": [ {"name":"W","position":{"x":0,"y":10000,"z":5000}} ]
    )");
    auto s = load_scenario_from_string(json);
    ASSERT_EQ(s.waypoints.size(), 1u);
    EXPECT_DOUBLE_EQ(s.waypoints[0].speed_kts, 350.0);
}

TEST(ScenarioLoader, WaypointsAreOptional) {
    auto s = load_scenario_from_string(with_extra_block(R"("record": false)"));
    EXPECT_TRUE(s.waypoints.empty());
    EXPECT_TRUE(s.airfield.taxi_in_route.empty());
}

TEST(ScenarioLoader, WaypointWithNonPositiveSpeedThrows) {
    const auto json = with_extra_block(R"(
        "waypoints": [ {"name":"BAD","position":{"x":0,"y":10000,"z":5000},"speed_kts":0} ]
    )");
    EXPECT_THROW(load_scenario_from_string(json), std::runtime_error);
}

TEST(ScenarioLoader, TaxiInRouteIsParsed) {
    // taxi_in_route lives inside the airfield block — splice a full airfield.
    const auto with_route = R"({
        "name": "taxi_in_test",
        "aircraft": [
            {"callsign":"EAGLE1","aircraft_config_path":"f16.json",
             "aircraft_name":"F-16C_50","vis_type_index":1052,
             "parking_spot":{"x":0,"y":0,"z":50},"heading_rad":0,"initial_fuel_lbs":6500}
        ],
        "airfield": {
            "active_runway_id": 36,
            "active_runway_name": "Rwy 36L",
            "runway_heading_rad": 0.0,
            "threshold_position": {"x":500,"y":8000,"z":50},
            "runway_end_position": {"x":500,"y":13000,"z":50},
            "taxi_route": [
                {"x":0,"y":0,"z":50},
                {"x":330,"y":7600,"z":50}
            ],
            "taxi_in_route": [
                {"x":330,"y":7900,"z":50},
                {"x":300,"y":5000,"z":50},
                {"x":0,"y":0,"z":50}
            ]
        }
    })";
    auto s = load_scenario_from_string(with_route);
    ASSERT_EQ(s.airfield.taxi_in_route.size(), 3u);
    EXPECT_DOUBLE_EQ(s.airfield.taxi_in_route.back().x, 0.0);
    EXPECT_DOUBLE_EQ(s.airfield.taxi_in_route.back().y, 0.0);
    // taxi_route unchanged
    ASSERT_EQ(s.airfield.taxi_route.size(), 2u);
}

TEST(ScenarioLoader, EmptyScenarioThrows) {
    EXPECT_THROW(load_scenario_from_string("{}"), std::runtime_error);
}

TEST(ScenarioLoader, ScenarioWithNoAircraftThrows) {
    const std::string json = R"({
        "name": "no_aircraft",
        "theater": "korea",
        "aircraft": [],
        "airfield": {
            "active_runway_id": 36,
            "taxi_route": [{"x":0,"y":0,"z":0}, {"x":0,"y":100,"z":0}]
        }
    })";
    EXPECT_THROW(load_scenario_from_string(json), std::runtime_error);
}

TEST(ScenarioLoader, TaxiRouteWithTooFewWaypointsThrows) {
    const std::string json = R"({
        "name": "short_route",
        "theater": "korea",
        "aircraft": [
            {"callsign":"EAGLE1","aircraft_config_path":"f16.json","vis_type_index":1052,
             "parking_spot":{"x":0,"y":0,"z":50},"heading_rad":0,"initial_fuel_lbs":6500}
        ],
        "airfield": {
            "active_runway_id": 36,
            "taxi_route": [{"x":0,"y":0,"z":0}]
        }
    })";
    EXPECT_THROW(load_scenario_from_string(json), std::runtime_error);
}

// === Phase 2: spawn_mode tests ===

TEST(ScenarioLoader, DefaultsToScenarioListSpawnMode) {
    // When "spawn_mode" is omitted, the default is ScenarioList (Phase 1
    // behavior — backward compatible).
    const std::string json = R"({
        "name": "default_mode",
        "theater": "korea",
        "aircraft": [
            {"callsign":"EAGLE1","aircraft_config_path":"f16.json","vis_type_index":1052,
             "parking_spot":{"x":0,"y":0,"z":50},"heading_rad":0,"initial_fuel_lbs":6500}
        ],
        "airfield": {
            "active_runway_id": 36,
            "taxi_route": [{"x":0,"y":0,"z":0}, {"x":0,"y":100,"z":0}]
        }
    })";
    auto s = load_scenario_from_string(json);
    EXPECT_EQ(s.spawn_mode, SpawnMode::ScenarioList);
}

TEST(ScenarioLoader, ParsesCampaignFlightsSpawnMode) {
    const std::string json = R"({
        "name": "campaign_mode",
        "theater": "korea",
        "spawn_mode": "campaign_flights",
        "world_json_path": "save1.world.json",
        "class_table_path": "Falcon4.CT",
        "aircraft": [
            {"callsign":"TEMPLATE","aircraft_config_path":"f16.json","vis_type_index":1052,
             "parking_spot":{"x":0,"y":0,"z":50},"heading_rad":0,"initial_fuel_lbs":6500}
        ],
        "airfield": {
            "active_runway_id": 36,
            "taxi_route": [{"x":0,"y":0,"z":0}, {"x":0,"y":100,"z":0}]
        }
    })";
    auto s = load_scenario_from_string(json);
    EXPECT_EQ(s.spawn_mode, SpawnMode::CampaignFlights);
    EXPECT_EQ(s.world_json_path, "save1.world.json");
    EXPECT_EQ(s.class_table_path, "Falcon4.CT");
}

TEST(ScenarioLoader, UnknownSpawnModeThrows) {
    const std::string json = R"({
        "name": "bad_mode",
        "theater": "korea",
        "spawn_mode": "magic_unicorns",
        "aircraft": [
            {"callsign":"EAGLE1","aircraft_config_path":"f16.json","vis_type_index":1052,
             "parking_spot":{"x":0,"y":0,"z":50},"heading_rad":0,"initial_fuel_lbs":6500}
        ],
        "airfield": {
            "active_runway_id": 36,
            "taxi_route": [{"x":0,"y":0,"z":0}, {"x":0,"y":100,"z":0}]
        }
    })";
    EXPECT_THROW(load_scenario_from_string(json), std::runtime_error);
}

TEST(ScenarioLoader, CampaignFlightsModeRequiresWorldJsonPath) {
    // spawn_mode=campaign_flights without world_json_path must fail validation.
    const std::string json = R"({
        "name": "missing_world",
        "theater": "korea",
        "spawn_mode": "campaign_flights",
        "class_table_path": "Falcon4.CT",
        "aircraft": [
            {"callsign":"T","aircraft_config_path":"f16.json","vis_type_index":1052,
             "parking_spot":{"x":0,"y":0,"z":50},"heading_rad":0,"initial_fuel_lbs":6500}
        ],
        "airfield": {
            "active_runway_id": 36,
            "taxi_route": [{"x":0,"y":0,"z":0}, {"x":0,"y":100,"z":0}]
        }
    })";
    EXPECT_THROW(load_scenario_from_string(json), std::runtime_error);
}

TEST(ScenarioLoader, CampaignFlightsModeRequiresClassTablePath) {
    const std::string json = R"({
        "name": "missing_ct",
        "theater": "korea",
        "spawn_mode": "campaign_flights",
        "world_json_path": "save1.world.json",
        "aircraft": [
            {"callsign":"T","aircraft_config_path":"f16.json","vis_type_index":1052,
             "parking_spot":{"x":0,"y":0,"z":50},"heading_rad":0,"initial_fuel_lbs":6500}
        ],
        "airfield": {
            "active_runway_id": 36,
            "taxi_route": [{"x":0,"y":0,"z":0}, {"x":0,"y":100,"z":0}]
        }
    })";
    EXPECT_THROW(load_scenario_from_string(json), std::runtime_error);
}

TEST(ScenarioLoader, CampaignFlightsModeRequiresAircraftTemplate) {
    // Even in campaign_flights mode, aircraft[0] must exist with an
    // aircraft_config_path (used as the shared config for all spawned aircraft).
    const std::string json = R"({
        "name": "missing_template",
        "theater": "korea",
        "spawn_mode": "campaign_flights",
        "world_json_path": "save1.world.json",
        "class_table_path": "Falcon4.CT",
        "aircraft": [],
        "airfield": {
            "active_runway_id": 36,
            "taxi_route": [{"x":0,"y":0,"z":0}, {"x":0,"y":100,"z":0}]
        }
    })";
    EXPECT_THROW(load_scenario_from_string(json), std::runtime_error);
}

// === Phase 2A: airfield_features tests ===

TEST(ScenarioLoader, LoadsAirfieldFeaturesFromFixture) {
    // The bundled takeoff_kunsan.json fixture now includes an
    // airfield_features[] block (Phase 2A). Verify it parses.
    const auto json = load_fixture("takeoff_kunsan.json");
    ASSERT_FALSE(json.empty());

    auto s = load_scenario_from_string(json);
    ASSERT_EQ(s.airfield_features.size(), 4u);

    // First feature: Runway Section 1
    const auto& f0 = s.airfield_features[0];
    EXPECT_EQ(f0.name, "Runway Section 1");
    EXPECT_EQ(f0.vis_type_index, 121);
    EXPECT_DOUBLE_EQ(f0.position.x, 500.0);
    EXPECT_DOUBLE_EQ(f0.position.y, 8500.0);
    EXPECT_DOUBLE_EQ(f0.position.z, 50.0);
    EXPECT_DOUBLE_EQ(f0.heading_rad, 0.0);

    // Second feature: another runway section (same vis_type — tests that
    // multiple features with the same model are allowed)
    const auto& f1 = s.airfield_features[1];
    EXPECT_EQ(f1.name, "Runway Section 2");
    EXPECT_EQ(f1.vis_type_index, 121);
    EXPECT_DOUBLE_EQ(f1.position.y, 10500.0);

    // Control Tower
    const auto& f2 = s.airfield_features[2];
    EXPECT_EQ(f2.name, "Control Tower");
    EXPECT_EQ(f2.vis_type_index, 143);

    // Hangar
    const auto& f3 = s.airfield_features[3];
    EXPECT_EQ(f3.name, "Hangar 1");
    EXPECT_EQ(f3.vis_type_index, 169);
}

TEST(ScenarioLoader, EmptyAirfieldFeaturesIsAllowed) {
    // airfield_features is optional — a scenario without it should still
    // load (backward compatibility with Phase 1 scenarios).
    const std::string json = R"({
        "name": "no_features",
        "theater": "korea",
        "aircraft": [
            {"callsign":"EAGLE1","aircraft_config_path":"f16.json","vis_type_index":1052,
             "parking_spot":{"x":0,"y":0,"z":50},"heading_rad":0,"initial_fuel_lbs":6500}
        ],
        "airfield": {
            "active_runway_id": 36,
            "taxi_route": [{"x":0,"y":0,"z":0}, {"x":0,"y":100,"z":0}]
        }
    })";
    auto s = load_scenario_from_string(json);
    EXPECT_TRUE(s.airfield_features.empty());
}

TEST(ScenarioLoader, FeatureWithInvalidVisTypeThrows) {
    // A feature with vis_type_index <= 0 is invalid — the model database
    // can't resolve it. Validation should catch this at load time.
    const std::string json = R"({
        "name": "bad_feature",
        "theater": "korea",
        "aircraft": [
            {"callsign":"EAGLE1","aircraft_config_path":"f16.json","vis_type_index":1052,
             "parking_spot":{"x":0,"y":0,"z":50},"heading_rad":0,"initial_fuel_lbs":6500}
        ],
        "airfield": {
            "active_runway_id": 36,
            "taxi_route": [{"x":0,"y":0,"z":0}, {"x":0,"y":100,"z":0}]
        },
        "airfield_features": [
            {"name":"Bad","vis_type_index":0,"position":{"x":0,"y":0,"z":0},"heading_rad":0}
        ]
    })";
    EXPECT_THROW(load_scenario_from_string(json), std::runtime_error);
}

TEST(ScenarioLoader, FeatureWithNonZeroHeadingParses) {
    // Verify the heading_rad field is parsed correctly (not just defaulted to 0).
    const std::string json = R"({
        "name": "rotated_feature",
        "theater": "korea",
        "aircraft": [
            {"callsign":"EAGLE1","aircraft_config_path":"f16.json","vis_type_index":1052,
             "parking_spot":{"x":0,"y":0,"z":50},"heading_rad":0,"initial_fuel_lbs":6500}
        ],
        "airfield": {
            "active_runway_id": 36,
            "taxi_route": [{"x":0,"y":0,"z":0}, {"x":0,"y":100,"z":0}]
        },
        "airfield_features": [
            {"name":"Rotated Tower","vis_type_index":143,
             "position":{"x":100,"y":200,"z":50},"heading_rad":1.5707963267948966}
        ]
    })";
    auto s = load_scenario_from_string(json);
    ASSERT_EQ(s.airfield_features.size(), 1u);
    EXPECT_DOUBLE_EQ(s.airfield_features[0].heading_rad, 1.5707963267948966);
    EXPECT_DOUBLE_EQ(s.airfield_features[0].position.x, 100.0);
}

// ============================================================================
// airbase_source block
// ============================================================================

TEST(ScenarioLoader, AirbaseSourceIsParsed) {
    const auto json = R"({
        "name": "src_test",
        "aircraft": [
            {"callsign":"EAGLE1","aircraft_config_path":"f16.json",
             "aircraft_name":"F-16C_50","vis_type_index":1052,
             "parking":"auto","parking_index":2,
             "parking_spot":{"x":0,"y":0,"z":0},"heading_rad":0,"initial_fuel_lbs":6500}
        ],
        "airbase_source": {
            "world_json": "korea.world.json",
            "grid_x": 626, "grid_y": 475,
            "active_heading_deg": 20
        }
    })";
    auto s = load_scenario_from_string(json);
    EXPECT_TRUE(s.has_airbase_source);
    EXPECT_EQ(s.airbase_source.grid_x, 626);
    EXPECT_EQ(s.airbase_source.grid_y, 475);
    EXPECT_EQ(s.airbase_source.active_heading_deg, 20);
    EXPECT_EQ(s.airbase_source.world_json_path, "korea.world.json");
    ASSERT_EQ(s.aircraft.size(), 1u);
    EXPECT_TRUE(s.aircraft[0].parking_auto);
    EXPECT_EQ(s.aircraft[0].parking_index, 2);
}

TEST(ScenarioLoader, AirbaseSourceWithoutWorldJsonThrows) {
    const auto json = R"({
        "name": "bad_src",
        "aircraft": [
            {"callsign":"E1","aircraft_config_path":"f16.json",
             "aircraft_name":"F-16","vis_type_index":1052,
             "parking_spot":{"x":0,"y":0,"z":0},"heading_rad":0,"initial_fuel_lbs":1}
        ],
        "airbase_source": { "grid_x": 1, "grid_y": 2 }
    })";
    EXPECT_THROW(load_scenario_from_string(json), std::runtime_error);
}

TEST(ScenarioLoader, AirbaseSourceRelaxesTaxiRouteRequirement) {
    // With airbase_source, a scenario may omit the hand-authored airfield
    // entirely (runway/taxi come from the world JSON at sim init).
    const auto json = R"({
        "name": "derived_only",
        "aircraft": [
            {"callsign":"E1","aircraft_config_path":"f16.json",
             "aircraft_name":"F-16","vis_type_index":1052,
             "parking":"auto","parking_spot":{"x":0,"y":0,"z":0},"heading_rad":0,"initial_fuel_lbs":1}
        ],
        "airbase_source": { "world_json": "w.json" }
    })";
    auto s = load_scenario_from_string(json);
    EXPECT_TRUE(s.airfield.taxi_route.empty());   // derived later, at sim init
}
