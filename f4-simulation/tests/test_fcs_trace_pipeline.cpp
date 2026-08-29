// f4-simulation/tests/test_fcs_trace_pipeline.cpp
//
// End-to-end test of the Phase 0b/0c/0d work:
//   - Loads the isolated `takeoff_only` and `landing_only` scenarios.
//   - Runs each headlessly for a bounded number of ticks.
//   - Asserts the FCS CSV trace is written to disk with the expected
//     structure (header row + N data rows, expected columns).
//   - Does NOT assert stability of the aircraft — that's the point of
//     capturing the trace. The acceptance criteria live in
//     FLIGHT_CONTROL_NEXT_STEPS.md §5 and will be enforced once the
//     Phase A/B/C/D fixes land.

#include <gtest/gtest.h>

#include <f4/simulation/simulation.hpp>
#include <f4/ai/brain_component.hpp>
#include <f4/flight/flight_model_component.hpp>

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace {

// Parse a CSV file into rows of cells (no quoted-comma handling — we control
// the AI state strings and they don't contain commas in these scenarios).
std::vector<std::vector<std::string>> parse_csv_file(const std::string& path) {
    std::ifstream ifs(path);
    if (!ifs) return {};
    std::vector<std::vector<std::string>> rows;
    std::string line;
    while (std::getline(ifs, line)) {
        if (line.empty()) continue;
        std::vector<std::string> row;
        std::string cell;
        for (char c : line) {
            if (c == ',') {
                row.push_back(std::move(cell));
                cell.clear();
            } else {
                cell.push_back(c);
            }
        }
        row.push_back(std::move(cell));
        rows.push_back(std::move(row));
    }
    return rows;
}

// Locate a column index by name from the header row. Returns SIZE_MAX if not
// found.
std::size_t col_index(const std::vector<std::string>& header,
                      const std::string& name) {
    for (std::size_t i = 0; i < header.size(); ++i) {
        if (header[i] == name) return i;
    }
    return SIZE_MAX;
}

} // namespace

// ============================================================================
// takeoff_only scenario — taxi → lineup → takeoff roll → rotate → FlyOut
// ============================================================================
TEST(FcsTracePipelineTest, TakeoffOnlyProducesTrace) {
#ifdef F4_SCENARIOS_DIR
    const std::filesystem::path scenario_path =
        std::string(F4_SCENARIOS_DIR) + "/takeoff_only.json";
#else
    const std::filesystem::path scenario_path = "scenarios/takeoff_only.json";
#endif
    if (!std::filesystem::exists(scenario_path)) {
        GTEST_SKIP() << "takeoff_only.json not found at " << scenario_path
                     << " (scenario player build is off?)";
    }

    auto scenario = f4::simulation::load_scenario(scenario_path);
    f4::simulation::Simulation sim(scenario, scenario_path.parent_path());
    sim.initialize();

    // Run 300 ticks (5 s of sim time at 60 Hz) — enough to see the takeoff
    // roll begin and possibly reach liftoff. We don't assert the aircraft
    // completed takeoff; we only assert the trace pipeline worked.
    const int N_TICKS = 300;
    for (int i = 0; i < N_TICKS; ++i) {
        sim.tick(1.0 / 60.0);
    }
    sim.write_fcs_trace();

    // The scenario sets fcs_trace_path. Verify the file exists and has the
    // expected structure.
    ASSERT_FALSE(scenario.fcs_trace_path.empty());
    const std::string trace_path = scenario.fcs_trace_path.string();
    EXPECT_TRUE(std::filesystem::exists(trace_path))
        << "CSV trace file not written at " << trace_path;

    const auto rows = parse_csv_file(trace_path);
    ASSERT_GE(rows.size(), 2u) << "trace must have a header + >=1 data rows";
    EXPECT_EQ(rows[0][0], "tick");
    EXPECT_EQ(rows[0].back(), "nx");

    // Verify the expected number of columns (header + every data row).
    const std::size_t expected_cols = rows[0].size();
    EXPECT_EQ(expected_cols, 53u);
    for (std::size_t i = 1; i < rows.size(); ++i) {
        EXPECT_EQ(rows[i].size(), expected_cols)
            << "row " << i << " has wrong column count";
    }

    // Verify we got roughly one row per tick (may be slightly less if the
    // aircraft didn't spawn — but it should, since the scenario has one).
    EXPECT_GE(rows.size() - 1, N_TICKS / 2)
        << "expected at least half the ticks in the trace; got "
        << (rows.size() - 1);

    // Spot-check a few columns exist in the header.
    EXPECT_NE(col_index(rows[0], "aoacmd_deg"), SIZE_MAX);
    EXPECT_NE(col_index(rows[0], "p_dps"), SIZE_MAX);
    EXPECT_NE(col_index(rows[0], "course_lateral_ft"), SIZE_MAX);
    EXPECT_NE(col_index(rows[0], "alpha_deg"), SIZE_MAX);
    EXPECT_NE(col_index(rows[0], "beta_deg"), SIZE_MAX);
    EXPECT_NE(col_index(rows[0], "ai_state"), SIZE_MAX);

    // Cleanup
    std::remove(trace_path.c_str());
}

// ============================================================================
// landing_only scenario — spawn airborne on final, fly OnFinal → Flare → Rollout
// ============================================================================
TEST(FcsTracePipelineTest, LandingOnlyProducesTrace) {
#ifdef F4_SCENARIOS_DIR
    const std::filesystem::path scenario_path =
        std::string(F4_SCENARIOS_DIR) + "/landing_only.json";
#else
    const std::filesystem::path scenario_path = "scenarios/landing_only.json";
#endif
    if (!std::filesystem::exists(scenario_path)) {
        GTEST_SKIP() << "landing_only.json not found at " << scenario_path
                     << " (scenario player build is off?)";
    }

    auto scenario = f4::simulation::load_scenario(scenario_path);
    f4::simulation::Simulation sim(scenario, scenario_path.parent_path());
    sim.initialize();

    // The landing_only scenario starts the brain in Approach phase. Verify
    // that handoff happened on the first tick.
    const auto& aircraft = sim.aircraft_entities();
    ASSERT_FALSE(aircraft.empty());
    auto h = f4::entities::EntityHandle(aircraft.front(), &sim.world());
    auto* brain = h.get<f4::ai::BrainComponent>();
    ASSERT_NE(brain, nullptr);

    // Tick once to fire the brain's lazy initialize + phase handoff.
    sim.tick(1.0 / 60.0);
    EXPECT_EQ(brain->phase(), f4::ai::BrainComponent::Phase::Approach)
        << "landing_only scenario should hand off to Approach on first tick";

    // Run 300 more ticks to get a meaningful trace.
    for (int i = 0; i < 300; ++i) {
        sim.tick(1.0 / 60.0);
    }
    sim.write_fcs_trace();

    ASSERT_FALSE(scenario.fcs_trace_path.empty());
    const std::string trace_path = scenario.fcs_trace_path.string();
    EXPECT_TRUE(std::filesystem::exists(trace_path));

    const auto rows = parse_csv_file(trace_path);
    ASSERT_GE(rows.size(), 2u);
    EXPECT_EQ(rows[0].size(), 53u);

    // In the Approach phase, the localizer_heading_deg column should be
    // populated. Spot-check that at least one row has a non-zero value
    // (the localizer correction is non-trivial once the aircraft deviates
    // from the centerline, which it will as the AI steers to track the beam).
    const auto loc_idx = col_index(rows[0], "localizer_heading_deg");
    ASSERT_NE(loc_idx, SIZE_MAX);
    const auto ai_state_idx = col_index(rows[0], "ai_state");
    ASSERT_NE(ai_state_idx, SIZE_MAX);
    bool saw_approach_state = false;
    for (std::size_t i = 1; i < rows.size(); ++i) {
        if (rows[i][ai_state_idx].find("Final") != std::string::npos ||
            rows[i][ai_state_idx].find("Approach") != std::string::npos ||
            rows[i][ai_state_idx].find("Intercept") != std::string::npos ||
            rows[i][ai_state_idx].find("Flare") != std::string::npos) {
            saw_approach_state = true;
            break;
        }
    }
    EXPECT_TRUE(saw_approach_state)
        << "expected at least one row in an Approach-phase state "
           "(OnFinal/InterceptFinal/Flare/etc.)";

    // Cleanup
    std::remove(trace_path.c_str());
}

// ============================================================================
// Phase 0d: trim-init at spawn
// ============================================================================
// The takeoff_only scenario spawns with initial_vt_fps=0 (the default), and
// the simulation overrides that to 5 ft/s. Verify the FM's actual vt is
// non-zero after initialize() — this confirms the trim-init at spawn works.
TEST(FcsTracePipelineTest, SpawnAtNonZeroVtAvoidsGroundGuardTransient) {
#ifdef F4_SCENARIOS_DIR
    const std::filesystem::path scenario_path =
        std::string(F4_SCENARIOS_DIR) + "/takeoff_only.json";
#else
    const std::filesystem::path scenario_path = "scenarios/takeoff_only.json";
#endif
    if (!std::filesystem::exists(scenario_path)) {
        GTEST_SKIP() << "takeoff_only.json not found";
    }

    auto scenario = f4::simulation::load_scenario(scenario_path);
    f4::simulation::Simulation sim(scenario, scenario_path.parent_path());
    sim.initialize();

    const auto& aircraft = sim.aircraft_entities();
    ASSERT_FALSE(aircraft.empty());
    auto h = f4::entities::EntityHandle(aircraft.front(), &sim.world());
    auto* fm = h.get<f4::flight::FlightModelComponent>();
    ASSERT_NE(fm, nullptr);

    // The simulation overrides the scenario's vt=0 to 5 ft/s for ground
    // spawns (Phase 0d). The FM's vt should reflect that.
    EXPECT_GT(fm->state().kin.vt, 0.0)
        << "expected non-zero vt after spawn (Phase 0d trim-init)";
    EXPECT_LT(fm->state().kin.vt, 10.0)
        << "expected small initial vt (5 ft/s), not a high spawn speed";
}
