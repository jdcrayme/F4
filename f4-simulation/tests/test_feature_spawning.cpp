// f4-simulation/tests/test_feature_spawning.cpp
//
// Phase 2A integration tests for Simulation::spawn_airfield_features().
//
// These tests verify that:
//   1. A scenario with airfield_features[] spawns one entity per feature
//   2. Each feature entity has TransformComponent + VisualModelComponent
//   3. Feature entities have NO FlightModelComponent (they're static)
//   4. The Simulation tracks feature_entities_ separately from aircraft_entities_
//   5. tick() does not modify feature entity transforms (static)
//   6. Multiple features sharing the same vis_type all get model_record pointers
//      to the same ModelRecord (mesh cache will dedupe)
//   7. A scenario with empty airfield_features spawns zero feature entities
//      (backward compatibility with Phase 1 scenarios)
//
// The tests construct a Simulation with a real ModelDatabase (loaded from
// the bundled KoreaObj.HDR/.LOD fixtures) so vis_type lookups actually
// resolve. The aircraft config is loaded from the generated f16.json
// fixture (built by f4-convert from f4-convert/tests/fixtures/f16.dat).

#include <gtest/gtest.h>

#include "f4/simulation/simulation.hpp"
#include "f4/simulation/visual_model_component.hpp"

#include <f4/entities/entity.hpp>
#include <f4/flight/flight_model_component.hpp>
#include <f4/models/model_database.hpp>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

#ifdef _WIN32
#include <process.h>
#else
#include <unistd.h>
#endif

using namespace f4::simulation;
using namespace f4::entities;

namespace {

std::string env_or(const char* key, const char* fallback) {
    const char* v = std::getenv(key);
    return v ? v : fallback;
}

std::string models_dir() {
    return env_or("F4_MODELS_DIR", F4_MODELS_DIR);
}

std::string generated_fixtures_dir() {
    return env_or("F4_GENERATED_FIXTURES_DIR", F4_GENERATED_FIXTURES_DIR);
}

std::string scenario_dir() {
    // Write the temp scenario JSON next to the test binary so relative
    // asset paths can be resolved.
    return std::filesystem::current_path().string();
}

// Build a minimal valid scenario JSON with the given airfield_features[]
// block (as a raw JSON array snippet). The aircraft block uses the
// generated f16.json fixture so the simulation can load a valid
// AircraftConfig.
std::string build_scenario_json(const std::string& features_json_array,
                                 bool include_aircraft = true) {
    std::ostringstream ss;
    ss << R"({
        "name": "feature_spawn_test",
        "theater": "korea",
        "models_hdr_path": ")" << models_dir() << "/KoreaObj.HDR",
    ss << R"(",
        "models_lod_path": ")" << models_dir() << "/KoreaObj.LOD",
    ss << R"(",
        "models_tex_path": ")" << models_dir() << "/KoreaObj.TEX",
    ss << R"(",
        )";
    if (include_aircraft) {
        ss << R"(
        "aircraft": [
            {"callsign":"EAGLE1","aircraft_config_path":")"
            << generated_fixtures_dir() << "/f16.json"
            << R"(","aircraft_name":"F-16C_50","vis_type_index":1052,
             "parking_spot":{"x":0,"y":0,"z":50},"heading_rad":0,"initial_fuel_lbs":6500}
        ],
        )";
    }
    ss << R"(
        "airfield": {
            "active_runway_id": 36,
            "active_runway_name": "Rwy 36",
            "runway_heading_rad": 0.0,
            "threshold_position": {"x":500,"y":8000,"z":50},
            "runway_end_position": {"x":500,"y":13000,"z":50},
            "threshold_altitude_ft": 50.0,
            "departure_altitude_ft": 2500.0,
            "taxi_route": [
                {"x":0,"y":0,"z":50},
                {"x":100,"y":200,"z":50},
                {"x":500,"y":8000,"z":50}
            ]
        },
        "airfield_features": )";
    ss << features_json_array;
    ss << R"(,
        "sim_dt": 0.016666666666666666,
        "total_ticks": 10,
        "record": false
    })";
    return ss.str();
}

// Write a temp scenario JSON file and return its path.
// The filename includes the PID: gtest_discover_tests() registers each
// TEST as a separate ctest entry, so `ctest -j N` runs this exe multiple
// times CONCURRENTLY — a fixed filename made the processes clobber each
// other's scenario file.
std::string write_temp_scenario(const std::string& json) {
#ifdef _WIN32
    const long pid = _getpid();
#else
    const long pid = static_cast<long>(getpid());
#endif
    const auto path = std::filesystem::path(scenario_dir())
        / ("temp_feature_scenario_" + std::to_string(pid) + ".json");
    std::ofstream f(path);
    f << json;
    return path.string();
}

} // namespace

// === Test 1: A scenario with features spawns one entity per feature ===
TEST(FeatureSpawning, ScenarioWithFeaturesSpawnsOneEntityPerFeature) {
    const std::string features = R"([
        {"name":"Runway 1","vis_type_index":121,"position":{"x":500,"y":8500,"z":50},"heading_rad":0},
        {"name":"Tower",   "vis_type_index":143,"position":{"x":800,"y":6500,"z":50},"heading_rad":0},
        {"name":"Hangar",  "vis_type_index":169,"position":{"x":-200,"y":500,"z":50},"heading_rad":0}
    ])";
    const auto json = build_scenario_json(features);
    const auto path = write_temp_scenario(json);

    Simulation sim(load_scenario(path), path);
    sim.initialize();

    EXPECT_EQ(sim.feature_entities().size(), 3u);
    EXPECT_EQ(sim.aircraft_entities().size(), 1u);  // the F-16

    std::filesystem::remove(path);
}

// === Test 2: Each feature entity has Transform + VisualModel ===
TEST(FeatureSpawning, FeatureEntityHasTransformAndVisualModel) {
    const std::string features = R"([
        {"name":"Tower","vis_type_index":143,"position":{"x":800,"y":6500,"z":50},"heading_rad":0}
    ])";
    const auto json = build_scenario_json(features);
    const auto path = write_temp_scenario(json);

    Simulation sim(load_scenario(path), path);
    sim.initialize();

    ASSERT_EQ(sim.feature_entities().size(), 1u);
    auto h = EntityHandle(sim.feature_entities().front(), &sim.world());

    EXPECT_NE(h.get<TransformComponent>(), nullptr);
    EXPECT_NE(h.get<VisualModelComponent>(), nullptr);
    EXPECT_NE(h.get<VisualModelComponent>()->model_record, nullptr);

    std::filesystem::remove(path);
}

// === Test 3: Feature entities have NO FlightModelComponent (static) ===
TEST(FeatureSpawning, FeatureEntityHasNoFlightModel) {
    const std::string features = R"([
        {"name":"Hangar","vis_type_index":169,"position":{"x":-200,"y":500,"z":50},"heading_rad":0}
    ])";
    const auto json = build_scenario_json(features);
    const auto path = write_temp_scenario(json);

    Simulation sim(load_scenario(path), path);
    sim.initialize();

    ASSERT_EQ(sim.feature_entities().size(), 1u);
    auto h = EntityHandle(sim.feature_entities().front(), &sim.world());

    // Features are static — no FM, no brain.
    EXPECT_EQ(h.get<f4::flight::FlightModelComponent>(), nullptr);

    std::filesystem::remove(path);
}

// === Test 4: tick() does not modify feature entity transforms ===
TEST(FeatureSpawning, TickDoesNotModifyFeatureTransforms) {
    const std::string features = R"([
        {"name":"Tower","vis_type_index":143,"position":{"x":800,"y":6500,"z":50},"heading_rad":0.5}
    ])";
    const auto json = build_scenario_json(features);
    const auto path = write_temp_scenario(json);

    Simulation sim(load_scenario(path), path);
    sim.initialize();

    ASSERT_EQ(sim.feature_entities().size(), 1u);
    auto h = EntityHandle(sim.feature_entities().front(), &sim.world());
    auto* tf = h.get<TransformComponent>();
    ASSERT_NE(tf, nullptr);

    // Snapshot the transform before tick.
    const auto pre_x = tf->position.x;
    const auto pre_y = tf->position.y;
    const auto pre_z = tf->position.z;
    const auto pre_qw = tf->qw;
    const auto pre_qz = tf->qz;

    // Tick several times. The aircraft's FM will update its own transform,
    // but the feature's transform should remain unchanged.
    for (int i = 0; i < 5; ++i) sim.tick(0.016666666666666666);

    EXPECT_DOUBLE_EQ(tf->position.x, pre_x);
    EXPECT_DOUBLE_EQ(tf->position.y, pre_y);
    EXPECT_DOUBLE_EQ(tf->position.z, pre_z);
    EXPECT_DOUBLE_EQ(tf->qw, pre_qw);
    EXPECT_DOUBLE_EQ(tf->qz, pre_qz);

    std::filesystem::remove(path);
}

// === Test 5: Multiple features with same vis_type share ModelRecord ===
TEST(FeatureSpawning, FeaturesWithSameVisTypeShareModelRecord) {
    // Two runway sections with the same vis_type=121. The ModelDatabase
    // returns a pointer to the same ModelRecord for both — the renderer's
    // mesh cache relies on this to dedupe GPU uploads.
    const std::string features = R"([
        {"name":"Runway A","vis_type_index":121,"position":{"x":500,"y":8500,"z":50},"heading_rad":0},
        {"name":"Runway B","vis_type_index":121,"position":{"x":500,"y":10500,"z":50},"heading_rad":0}
    ])";
    const auto json = build_scenario_json(features);
    const auto path = write_temp_scenario(json);

    Simulation sim(load_scenario(path), path);
    sim.initialize();

    ASSERT_EQ(sim.feature_entities().size(), 2u);
    auto h0 = EntityHandle(sim.feature_entities()[0], &sim.world());
    auto h1 = EntityHandle(sim.feature_entities()[1], &sim.world());
    auto* v0 = h0.get<VisualModelComponent>();
    auto* v1 = h1.get<VisualModelComponent>();

    ASSERT_NE(v0->model_record, nullptr);
    ASSERT_NE(v1->model_record, nullptr);
    EXPECT_EQ(v0->model_record, v1->model_record);  // same pointer

    std::filesystem::remove(path);
}

// === Test 6: Scenario with empty airfield_features spawns zero feature entities ===
TEST(FeatureSpawning, EmptyFeaturesArraySpawnsNothing) {
    const auto json = build_scenario_json("[]");
    const auto path = write_temp_scenario(json);

    Simulation sim(load_scenario(path), path);
    sim.initialize();

    EXPECT_TRUE(sim.feature_entities().empty());
    EXPECT_EQ(sim.aircraft_entities().size(), 1u);  // aircraft still spawns

    std::filesystem::remove(path);
}

// === Test 7: Feature transform's heading_rad is encoded as a Z-up quaternion ===
TEST(FeatureSpawning, FeatureHeadingIsEncodedAsZUpQuaternion) {
    // A feature with heading_rad = pi/2 (90 degrees, pointing east).
    // COMPASS convention (see f4/simulation/frames.hpp): headings are
    // clockwise from north, which in ENU is a NEGATIVE rotation about
    // +Z-up: q = (cos(pi/4), 0, 0, -sin(pi/4)). (The old positive-sign
    // expectation encoded the mirrored-heading rendering bug.)
    const std::string features = R"([
        {"name":"Rotated","vis_type_index":143,"position":{"x":0,"y":0,"z":0},"heading_rad":1.5707963267948966}
    ])";
    const auto json = build_scenario_json(features);
    const auto path = write_temp_scenario(json);

    Simulation sim(load_scenario(path), path);
    sim.initialize();

    ASSERT_EQ(sim.feature_entities().size(), 1u);
    auto h = EntityHandle(sim.feature_entities().front(), &sim.world());
    auto* tf = h.get<TransformComponent>();
    ASSERT_NE(tf, nullptr);

    // q = (cos(h/2), 0, 0, -sin(h/2)) for compass heading h.
    EXPECT_NEAR(tf->qw, 0.7071067811865476, 1e-9);
    EXPECT_NEAR(tf->qx, 0.0, 1e-9);
    EXPECT_NEAR(tf->qy, 0.0, 1e-9);
    EXPECT_NEAR(tf->qz, -0.7071067811865476, 1e-9);

    std::filesystem::remove(path);
}

// === Test 8: Renderer-visible — all VMC-bearing entities are discoverable ===
TEST(FeatureSpawning, AllVisualModelEntitiesAreDiscoverableViaWithComponent) {
    // The renderer uses EntityWorld::with_component<VisualModelComponent>() to
    // find all renderable entities. This test verifies that both the aircraft
    // and the features appear in that query — confirming the renderer will
    // draw them all.
    const std::string features = R"([
        {"name":"F1","vis_type_index":121,"position":{"x":500,"y":8500,"z":50},"heading_rad":0},
        {"name":"F2","vis_type_index":143,"position":{"x":800,"y":6500,"z":50},"heading_rad":0},
        {"name":"F3","vis_type_index":169,"position":{"x":-200,"y":500,"z":50},"heading_rad":0}
    ])";
    const auto json = build_scenario_json(features);
    const auto path = write_temp_scenario(json);

    Simulation sim(load_scenario(path), path);
    sim.initialize();

    // 1 aircraft + 3 features = 4 VMC-bearing entities total.
    const auto vmc_entities = sim.world().with_component<VisualModelComponent>();
    EXPECT_EQ(vmc_entities.size(), 4u);

    std::filesystem::remove(path);
}
