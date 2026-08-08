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
    EXPECT_DOUBLE_EQ(s.airfield.taxi_route.back().x, 500.0);   // threshold
    EXPECT_DOUBLE_EQ(s.airfield.taxi_route.back().y, 8000.0);
}

TEST(ScenarioLoader, MissingFileThrows) {
    const std::filesystem::path nope = "/tmp/this_scenario_does_not_exist.json";
    EXPECT_THROW(load_scenario(nope), std::runtime_error);
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
