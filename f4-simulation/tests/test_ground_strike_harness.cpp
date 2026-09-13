// f4-simulation/tests/test_ground_strike_harness.cpp
//
// M5b — the air-to-ground strike acceptance harness, pinned over the
// shipped ground_strike.json scenario (short horizons: the rig
// compresses minutes the way the M4/M5a rigs do) and over synthetic
// temp-dir scenarios (the negative cases).
//
//   1. The strike runs, certifies, and is deterministic: two passes,
//      identical recorder MD5s, the chain completes (release → on-target
//      impact → feature damage), the roster identity holds, and the
//      ledger agrees with the events. The strike window narrates the
//      pass: BombReleased → BombImpact(impact, on target) → damage.
//   2. The harness refuses a non-combat scenario (the exit-2 contract:
//      the stable abort prefix, before any load validation).
//   3. A striker with no ordnance never releases (the exit-3 class:
//      release_occurred violated, the arming rung named).
//   4. A scenario with no A/G delivery waypoint aborts (the wrong-shape
//      scenario is a harness abort, not a verdict).
//   5. runs == 1 skips the determinism proof (vacuously true, empty
//      run1 MD5).
//   6. The certificate's MD5 (self-consistency over a real run's bytes —
//      the file-private helper, the M5a rig's approach).
//
// Companion: Docs/COMBAT_CHAIN_M4_PLAN.md §6, Docs/COMBAT_CHAIN_M5_PLAN
// .md §6 (the deferral this closes), test_wvr_merge_harness.cpp (the M5a
// rig this mirrors), test_bomb.cpp / test_bomb_unit.cpp (the A/G chain
// links this certifies end to end).

#include <f4/simulation/ground_strike_harness.hpp>
#include <f4/simulation/scenario.hpp>
#include <f4/recorder/flight_recorder.hpp>
#include <f4/recorder/combat_event.hpp>

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <string>

using namespace f4::simulation;

namespace {

std::filesystem::path strike_scenario() {
#ifdef F4_SCENARIOS_DIR
    return std::filesystem::path(F4_SCENARIOS_DIR) / "ground_strike.json";
#else
    return {};
#endif
}

bool scenario_ready(const std::filesystem::path& p) {
    return !p.empty() && std::filesystem::exists(p);
}

/// Locate the generated F-16 aircraft config fixture. Same resolution as
/// test_wvr_merge_harness.cpp — generic_string() keeps the path
/// JSON-safe.
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

/// Write a minimal non-combat scenario to a temp file (the harness's
/// refusal is pre-scan — the scenario needs no aircraft list).
std::filesystem::path write_noncombat_scenario() {
    const auto dir = std::filesystem::temp_directory_path();
    const auto p = dir / "f4_strike_harness_noncombat_test.json";
    std::ofstream f(p);
    f << R"({
  "name": "noncombat_test",
  "theater": "korea",
  "sim_dt": 0.016666667,
  "total_ticks": 100
})";
    return p;
}

/// A single-ship strike: one blue F-16 spawning in air at 8,000 ft
/// (675 fps — the doctrine reference delivery speed), flying IP → STRIKE
/// (action 17, the campwp.h WP_STRIKE the brain's rung keys on) →
/// EGRESS. The strike target itself is the HARNESS's injection (the M5b
/// composition contract) at the option defaults — (0, 30000, 0), the
/// ground point under the STRIKE waypoint.
std::filesystem::path write_strike_scenario(const std::string& f16) {
    const auto dir = std::filesystem::temp_directory_path();
    const auto p = dir / "f4_strike_harness_test.json";
    std::ofstream f(p);
    f << R"({
  "name": "strike_test",
  "theater": "korea",
  "sim_dt": 0.016666667,
  "total_ticks": 36000,
  "start_enroute": true,
  "combat": {"enabled": true, "radar_rng_seed": 4242,
             "fighter_hit_points": 10, "guns_hold": true},
  "waypoints": [
    {"name": "IP", "position": {"x": 0.0, "y": 15000.0, "z": 8000.0},
     "speed_kts": 400.0},
    {"name": "STRIKE", "position": {"x": 0.0, "y": 30000.0, "z": 8000.0},
     "speed_kts": 400.0, "action": 17},
    {"name": "EGRESS", "position": {"x": 0.0, "y": 45000.0, "z": 8000.0},
     "speed_kts": 400.0}
  ],
  "aircraft": [
    {"callsign": "STRIKE1", "team": "blue",
     "aircraft_config_path": ")" + f16 + R"(",
     "aircraft_name": "F-16C_50", "vis_type_index": 1052,
     "spawn_in_air": true, "initial_fuel_lbs": 6500.0,
     "initial_vt_fps": 675.0,
     "parking_spot": {"x": 0.0, "y": -30000.0, "z": 8000.0},
     "heading_rad": 0.0}
  ],
  "airfield": {
    "active_runway_id": 36, "active_runway_name": "Rwy 36",
    "runway_heading_rad": 0.0,
    "threshold_position": {"x": 0.0, "y": -5000.0, "z": 0.0},
    "runway_end_position": {"x": 0.0, "y": 5000.0, "z": 0.0},
    "threshold_altitude_ft": 0.0, "departure_altitude_ft": 8000.0,
    "taxi_route": [{"x": 0.0, "y": -5000.0, "z": 0.0},
                   {"x": 0.0, "y": 0.0, "z": 0.0}]
  }
})";
    return p;
}

/// The same geometry with NO A/G delivery waypoint on the route — the
/// wrong-shape scenario (the injection aborts, it never becomes a
/// verdict).
std::filesystem::path write_no_delivery_scenario(const std::string& f16) {
    const auto dir = std::filesystem::temp_directory_path();
    const auto p = dir / "f4_strike_harness_nodelivery_test.json";
    std::ofstream f(p);
    f << R"({
  "name": "strike_nodelivery_test",
  "theater": "korea",
  "sim_dt": 0.016666667,
  "total_ticks": 3600,
  "start_enroute": true,
  "combat": {"enabled": true, "radar_rng_seed": 4242},
  "waypoints": [
    {"name": "IP", "position": {"x": 0.0, "y": 15000.0, "z": 8000.0},
     "speed_kts": 400.0},
    {"name": "FAR", "position": {"x": 0.0, "y": 45000.0, "z": 8000.0},
     "speed_kts": 400.0}
  ],
  "aircraft": [
    {"callsign": "STRIKE1", "team": "blue",
     "aircraft_config_path": ")" + f16 + R"(",
     "aircraft_name": "F-16C_50", "vis_type_index": 1052,
     "spawn_in_air": true, "initial_fuel_lbs": 6500.0,
     "initial_vt_fps": 675.0,
     "parking_spot": {"x": 0.0, "y": -30000.0, "z": 8000.0},
     "heading_rad": 0.0}
  ],
  "airfield": {
    "active_runway_id": 36, "active_runway_name": "Rwy 36",
    "runway_heading_rad": 0.0,
    "threshold_position": {"x": 0.0, "y": -5000.0, "z": 0.0},
    "runway_end_position": {"x": 0.0, "y": 5000.0, "z": 0.0},
    "threshold_altitude_ft": 0.0, "departure_altitude_ft": 8000.0,
    "taxi_route": [{"x": 0.0, "y": -5000.0, "z": 0.0},
                   {"x": 0.0, "y": 0.0, "z": 0.0}]
  }
})";
    return p;
}

GroundStrikeHarnessOptions make_opts(const std::filesystem::path& scenario) {
    GroundStrikeHarnessOptions o;
    o.scenario_json = scenario;
    o.asset_dir = scenario.parent_path();
    o.horizon_sec = 300;
    o.sample_sec = 30.0;
    o.runs = 2;
    return o;
}

} // namespace

// ============================================================================
// 1. The strike: runs, certifies, deterministic — and the strike window
//    narrates the pass as a chain.
// ============================================================================
TEST(GroundStrikeHarness, StrikeRunsCertifiesAndIsDeterministic) {
    const auto scenario = strike_scenario();
    ASSERT_TRUE(scenario_ready(scenario))
        << "the synthetic strike scenario failed to write";

    auto h = GroundStrikeHarness::create(make_opts(scenario));
    ASSERT_NE(h, nullptr);

    const auto& report = h->execute();
    ASSERT_FALSE(report.aborted) << report.abort_reason;

    // Preconditions: the injection armed the single blue ship and bound
    // the objective.
    EXPECT_EQ(report.combat_enabled, true);
    EXPECT_EQ(report.aircraft_count, 1);
    EXPECT_EQ(report.blue_aircraft, 1);
    EXPECT_EQ(report.red_aircraft, 0);
    EXPECT_EQ(report.strikers_armed, 1);
    EXPECT_NE(report.target_entity_id, 0u);
    EXPECT_EQ(report.target_features, 5);

    // The five verdicts.
    EXPECT_TRUE(report.verdict.deterministic)
        << "MD5 run0=" << report.verdict.recorder_md5_run0
        << " run1=" << report.verdict.recorder_md5_run1;
    EXPECT_TRUE(report.verdict.release_occurred)
        << "release_stall: " << report.verdict.release_stall;
    EXPECT_TRUE(report.verdict.impact_on_target)
        << "impact_failure: " << report.verdict.impact_failure;
    EXPECT_TRUE(report.verdict.damage_applied)
        << "damage_failure: " << report.verdict.damage_failure;
    EXPECT_TRUE(report.verdict.roster_bounded)
        << "roster_leak: " << report.verdict.roster_leak;

    // The certificate: two non-empty MD5s that match.
    EXPECT_FALSE(report.verdict.recorder_md5_run0.empty());
    EXPECT_FALSE(report.verdict.recorder_md5_run1.empty());
    EXPECT_EQ(report.verdict.recorder_md5_run0,
              report.verdict.recorder_md5_run1);

    // The strike window: release → on-target impact → damage, in order.
    EXPECT_GE(report.verdict.first_release_s, 0.0);
    EXPECT_GE(report.verdict.first_on_target_s, 0.0);
    EXPECT_GE(report.verdict.first_damage_s, 0.0);
    EXPECT_LE(report.verdict.first_release_s,
              report.verdict.first_on_target_s);
    EXPECT_LE(report.verdict.first_on_target_s,
              report.verdict.first_damage_s);
    // The bomb took SOME time to fall (a release is not an impact).
    EXPECT_GT(report.verdict.first_on_target_s,
              report.verdict.first_release_s);

    // The chain completed more than trivially: the stick walked the
    // target, the misses are inside the lethal radius, and features
    // died.
    EXPECT_GE(report.verdict.bombs_released, 2)
        << "a stick of 4 should release, got "
        << report.verdict.bombs_released;
    EXPECT_GE(report.verdict.impacts_on_target, 1);
    EXPECT_GE(report.verdict.features_destroyed_max, 1.0);
    EXPECT_GT(report.verdict.destroyed_pct_max, 0.0);
    // Every on-target impact landed close enough to threaten the
    // objective (the CCIP gate's promise).
    EXPECT_GE(report.verdict.min_miss_distance_ft, 0.0);
    EXPECT_LT(report.verdict.min_miss_distance_ft, 300.0)
        << "min miss " << report.verdict.min_miss_distance_ft
        << " ft exceeds the MK-82 lethal radius";

    // The ledger agrees with the events (the event says what happened,
    // the ledger says what stuck).
    EXPECT_GE(report.verdict.features_destroyed_final, 1.0);
    EXPECT_GT(report.verdict.destroyed_pct_final, 0.0);

    // The chain end-to-end through the recorder round-trip (the M5a
    // contract shape — the bomb events survive the JSON with their
    // kinds and the on-target binding intact).
    auto rec = f4::recorder::FlightRecorder::from_json(report.recorder_json);
    int released = 0, on_target = 0, damaging = 0;
    for (const auto& e : rec.combat_events()) {
        if (e.kind == f4::recorder::CombatEventKind::BombReleased) {
            ++released;
            // The shooter is the striker; the object is the objective.
            EXPECT_EQ(e.object_id, report.target_entity_id);
        }
        if (e.kind == f4::recorder::CombatEventKind::BombImpact &&
            e.end_cause == "impact" && e.object_id == report.target_entity_id) {
            ++on_target;
            if (e.damage > 0.0) ++damaging;
        }
    }
    EXPECT_EQ(released, report.verdict.bombs_released);
    EXPECT_EQ(on_target, report.verdict.impacts_on_target);
    EXPECT_GE(damaging, 1);
}

// ============================================================================
// 2. The refusal (the exit-2 contract, wvr_merge_harness verbatim).
// ============================================================================
TEST(GroundStrikeHarness, RefusesNonCombatScenario) {
    const auto scenario = write_noncombat_scenario();
    auto h = GroundStrikeHarness::create(make_opts(scenario));
    ASSERT_NE(h, nullptr);

    const auto& report = h->execute();
    EXPECT_TRUE(report.aborted);
    // The stable prefix is the QC tool's exit-2 contract — keep it
    // verbatim.
    EXPECT_EQ(report.abort_reason.rfind(
                  "scenario combat.enabled is false", 0), 0u)
        << report.abort_reason;
    EXPECT_TRUE(report.recorder_json.empty());
}

// ============================================================================
// 3. No ordnance, no release (the exit-3 class: the arming rung named).
// ============================================================================
TEST(GroundStrikeHarness, NoOrdnanceNeverReleases) {
    const auto scenario = strike_scenario();
    ASSERT_TRUE(scenario_ready(scenario));

    auto opts = make_opts(scenario);
    opts.bomb_rounds = 0;   // armed fire control, empty store
    opts.runs = 1;          // the negative case needs no proof pass
    auto h = GroundStrikeHarness::create(opts);
    ASSERT_NE(h, nullptr);

    const auto& report = h->execute();
    ASSERT_FALSE(report.aborted) << report.abort_reason;

    EXPECT_EQ(report.strikers_armed, 1);
    EXPECT_FALSE(report.verdict.release_occurred);
    EXPECT_EQ(report.verdict.bombs_released, 0);
    // The diagnostic names the trigger rung (the store was empty — the
    // fire control pulsed, the host could not fulfill).
    EXPECT_NE(report.verdict.release_stall.find("no BombReleased"),
              std::string::npos)
        << report.verdict.release_stall;
}

// ============================================================================
// 4. No delivery waypoint on the route: a harness abort (wrong shape),
//    never a verdict.
// ============================================================================
TEST(GroundStrikeHarness, NoDeliveryWaypointAborts) {
    const std::string f16 = f16_config_path();
    if (f16.empty()) GTEST_SKIP() << "no generated f16 fixture";

    const auto scenario = write_no_delivery_scenario(f16);
    auto h = GroundStrikeHarness::create(make_opts(scenario));
    ASSERT_NE(h, nullptr);

    const auto& report = h->execute();
    EXPECT_TRUE(report.aborted);
    EXPECT_NE(report.abort_reason.find("no blue aircraft could be armed"),
              std::string::npos)
        << report.abort_reason;
}

// ============================================================================
// 5. runs == 1 skips the determinism proof (the M5a contract).
// ============================================================================
TEST(GroundStrikeHarness, SingleRunSkipsTheDeterminismProof) {
    const auto scenario = strike_scenario();
    ASSERT_TRUE(scenario_ready(scenario));

    auto opts = make_opts(scenario);
    opts.runs = 1;
    auto h = GroundStrikeHarness::create(opts);
    ASSERT_NE(h, nullptr);

    const auto& report = h->execute();
    ASSERT_FALSE(report.aborted) << report.abort_reason;

    EXPECT_TRUE(report.verdict.deterministic);
    EXPECT_FALSE(report.verdict.recorder_md5_run0.empty());
    EXPECT_TRUE(report.verdict.recorder_md5_run1.empty());
}

// ============================================================================
// 6. The certificate's MD5 — self-consistency over a real run's bytes
//    (the file-private helper; the cross-host vectors live in the
//    campaign/BVR/WVR rigs which share the same copied class).
// ============================================================================
TEST(GroundStrikeHarnessMd5, StableDigestOverTheRecorderBytes) {
    const auto scenario = strike_scenario();
    ASSERT_TRUE(scenario_ready(scenario));

    auto opts = make_opts(scenario);
    opts.runs = 1;
    opts.horizon_sec = 120;
    auto h = GroundStrikeHarness::create(opts);
    ASSERT_NE(h, nullptr);
    const auto& report = h->execute();
    ASSERT_FALSE(report.aborted) << report.abort_reason;

    const auto& md5 = report.verdict.recorder_md5_run0;
    ASSERT_EQ(md5.size(), 32u);
    for (const char c : md5) {
        EXPECT_TRUE((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))
            << "non-hex char in digest: " << md5;
    }
    EXPECT_TRUE(report.recorder_json.size() > 0u);
}
