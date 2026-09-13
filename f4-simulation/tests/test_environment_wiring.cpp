// f4-simulation/tests/test_environment_wiring.cpp
//
// Task 73: the Simulation-level environment wiring — the zero-change
// rule (no blocks: no WeatherSystem, fusions at 1.0), and the push path
// (a night scenario's scale lands on the roster brains' SensorFusion).

#include <f4/simulation/simulation.hpp>
#include <f4/simulation/weather_system.hpp>

#include <f4/ai/brain_component.hpp>

#include <gtest/gtest.h>

#include <filesystem>
#include <string>

using namespace f4::simulation;

namespace {

std::filesystem::path find_f16() {
    std::filesystem::path f16;
    const char* gen = std::getenv("F4_GENERATED_FIXTURES_DIR");
    if (gen) f16 = std::filesystem::path(gen) / "f16.json";
    if (f16.empty() || !std::filesystem::exists(f16)) {
#ifdef F4_SOURCE_FIXTURES_DIR
        f16 = std::filesystem::path(F4_SOURCE_FIXTURES_DIR) / "f16.json";
#endif
    }
    if (f16.empty() || !std::filesystem::exists(f16))
        f16 = std::filesystem::path("../../../Data/Aircraft/f16.json");
    return f16;
}

// One airborne aircraft over a flat field — the cheapest roster the
// wiring can be exercised against. spawn_in_air keeps the FM from
// needing a full ground phase for the ticks this test runs.
std::string scenario_json(const std::filesystem::path& f16,
                          const std::string& env_blocks) {
    return R"({
        "name": "env_wiring",
        "aircraft": [
            {"callsign":"E1","aircraft_config_path":")" + f16.string() +
           R"(","aircraft_name":"F-16","vis_type_index":1052,
             "spawn_in_air": true,
             "position":{"x":0,"y":10000,"z":10000},
             "heading_rad":0,"initial_fuel_lbs":5000}
        ],
        "airfield": {
            "active_runway_id": 36,
            "threshold_position": {"x":0,"y":0,"z":0},
            "runway_end_position": {"x":0,"y":1000,"z":0},
            "taxi_route": [{"x":0,"y":0,"z":0},{"x":0,"y":1000,"z":0}]
        })" + env_blocks + R"(
    })";
}

} // namespace

TEST(EnvironmentWiring, AbsentBlocksMeansNoSystemAndScaleOne) {
    const auto f16 = find_f16();
    if (!std::filesystem::exists(f16)) GTEST_SKIP() << "f16.json not found";

    Simulation sim(load_scenario_from_string(scenario_json(f16, "")),
                   std::filesystem::path("."));
    sim.initialize();
    ASSERT_EQ(sim.aircraft_entities().size(), 1u);
    EXPECT_EQ(sim.environment(), nullptr); // zero-change: no system at all

    for (int i = 0; i < 10; ++i) sim.tick(1.0 / 60.0);

    // The roster's fusion scale never moved off its default.
    f4::entities::EntityHandle h(sim.aircraft_entities().front(),
                                 const_cast<f4::entities::EntityWorld*>(
                                     &sim.world()));
    const auto* brain = h.get<f4::ai::BrainComponent>();
    ASSERT_NE(brain, nullptr);
    EXPECT_DOUBLE_EQ(brain->sensors().visual_range_scale(), 1.0);
}

TEST(EnvironmentWiring, NightScenarioPushesNightScaleToFusions) {
    const auto f16 = find_f16();
    if (!std::filesystem::exists(f16)) GTEST_SKIP() << "f16.json not found";

    // Locked clear at midnight: the scale is the pure night factor (0.1)
    // — no weather jitter in the way.
    const auto json = scenario_json(
        f16, R"(,
        "weather": {"locked": true},
        "time": {"start_seconds": 0, "advance": false}
    )");
    Simulation sim(load_scenario_from_string(json),
                   std::filesystem::path("."));
    sim.initialize();
    ASSERT_EQ(sim.aircraft_entities().size(), 1u);
    ASSERT_NE(sim.environment(), nullptr);
    EXPECT_EQ(sim.environment()->band(),
              f4::world_types::DaylightBand::Night);

    for (int i = 0; i < 10; ++i) sim.tick(1.0 / 60.0);

    f4::entities::EntityHandle h(sim.aircraft_entities().front(),
                                 const_cast<f4::entities::EntityWorld*>(
                                     &sim.world()));
    const auto* brain = h.get<f4::ai::BrainComponent>();
    ASSERT_NE(brain, nullptr);
    EXPECT_DOUBLE_EQ(brain->sensors().visual_range_scale(),
                     sim.environment()->visual_scale());
    EXPECT_DOUBLE_EQ(brain->sensors().visual_range_scale(), 0.1);
}

TEST(EnvironmentWiring, NoClockWithoutTimeBlock) {
    // A weather-only scenario: the system exists, but the clock stays at
    // its noon default (band Day) — the two blocks are independent
    // switches.
    const auto f16 = find_f16();
    if (!std::filesystem::exists(f16)) GTEST_SKIP() << "f16.json not found";

    const auto json = scenario_json(
        f16, R"(,
        "weather": {"condition": "hazy", "locked": true}
    )");
    Simulation sim(load_scenario_from_string(json),
                   std::filesystem::path("."));
    sim.initialize();
    ASSERT_NE(sim.environment(), nullptr);
    EXPECT_EQ(sim.environment()->band(),
              f4::world_types::DaylightBand::Day);
    // Hazy visibility (12 NM) against the 40 NM reference: scale 0.3.
    EXPECT_DOUBLE_EQ(sim.environment()->visual_scale(), 0.3);
    for (int i = 0; i < 5; ++i) sim.tick(1.0 / 60.0);
    f4::entities::EntityHandle h(sim.aircraft_entities().front(),
                                 const_cast<f4::entities::EntityWorld*>(
                                     &sim.world()));
    const auto* brain = h.get<f4::ai::BrainComponent>();
    ASSERT_NE(brain, nullptr);
    EXPECT_DOUBLE_EQ(brain->sensors().visual_range_scale(), 0.3);
}
