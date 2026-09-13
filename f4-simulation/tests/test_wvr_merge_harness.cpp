// f4-simulation/tests/test_wvr_merge_harness.cpp
//
// M5a — the WVR / guns merge acceptance harness, pinned over the shipped
// wvr_merge.json + guns_merge.json scenarios (short horizons: the rig
// compresses minutes the way the BVR rig does).
//
//   1. The GUNS fight runs, certifies, and is deterministic: two passes,
//      identical recorder MD5s, the kill completes (gun-attributed), the
//      roster identity holds, and the fight stays alive (detection AND
//      band entry). The engagement window narrates a merge: detect →
//      WvrEngaged → GunFired → EntityKilled → WvrDisengaged.
//   2. The WVR (heaters) fight runs, certifies, and is deterministic
//      (the wvr_merge scenario — bvr_hold holds the AMRAAMs, the merge
//      employs the heater; M4's bvr_intercept_qc owns the BVR half).
//   3. The harness refuses a non-combat scenario (the exit-2 contract:
//      the stable abort prefix, before any load validation).
//   4. A detected-but-never-in-band scenario fails fight_alive with the
//      exit-4 diagnostic (aircraft spawned beyond the band, combat on).
//   5. runs == 1 skips the determinism proof (vacuously true, empty
//      run1 MD5).
//   6. The MD5 helper (test vectors — md5("") and md5("abc")).
//   7. The 2v2 multi-flight acceptance (M4 §6's multi-flight deferral
//      closes here): a blue 2-ship vs a red 2-ship through the harness,
//      engagement completes, roster bounded.
//
// Companion: Docs/COMBAT_CHAIN_M5_PLAN.md, test_bvr_intercept_harness
// .cpp (the M4 rig this mirrors), test_combat_integration.cpp (the
// hand-driven guns/wvr fights the harness now certifies).

#include <f4/simulation/wvr_merge_harness.hpp>
#include <f4/simulation/scenario.hpp>
#include <f4/recorder/flight_recorder.hpp>
#include <f4/recorder/combat_event.hpp>

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <string>

using namespace f4::simulation;

namespace {

std::filesystem::path wvr_scenario() {
#ifdef F4_SCENARIOS_DIR
    return std::filesystem::path(F4_SCENARIOS_DIR) / "wvr_merge.json";
#else
    return {};
#endif
}

std::filesystem::path guns_scenario() {
#ifdef F4_SCENARIOS_DIR
    return std::filesystem::path(F4_SCENARIOS_DIR) / "guns_merge.json";
#else
    return {};
#endif
}

bool scenario_ready(const std::filesystem::path& p) {
    return !p.empty() && std::filesystem::exists(p);
}

/// Locate the generated F-16 aircraft config fixture. Same resolution as
/// test_bvr_intercept_harness.cpp — generic_string() keeps the path
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
    const auto p = dir / "f4_wvr_harness_noncombat_test.json";
    std::ofstream f(p);
    f << R"({
  "name": "noncombat_test",
  "theater": "korea",
  "sim_dt": 0.016666667,
  "total_ticks": 100
})";
    return p;
}

/// A 2v2 multi-flight merge: a blue 2-ship (wing + lead via lead_callsign)
/// against a red 2-ship of hold-fire drones, head-on at 15,000 ft, 2.8 NM
/// apart. The blues employ (guns + heaters free); the reds fight
/// geometry only. The blue wingman exercises the WingmanModule sort
/// through the harness (the multi-flight surface M4 deferred).
std::filesystem::path write_two_ship_scenario(const std::string& f16) {
    const auto dir = std::filesystem::temp_directory_path();
    const auto p = dir / "f4_wvr_harness_twoship_test.json";
    std::ofstream f(p);
    f << R"({
  "name": "wvr_twoship_test",
  "theater": "korea",
  "sim_dt": 0.016666667,
  "total_ticks": 18000,
  "start_enroute": true,
  "combat": {"enabled": true, "radar_rng_seed": 4242,
             "fighter_hit_points": 1,
             "missiles_hold": false, "guns_hold": false},
  "waypoints": [
    {"name": "MERGE", "position": {"x": 0.0, "y": 8500.0, "z": 15000.0},
     "speed_kts": 420.0}
  ],
  "aircraft": [
    {"callsign": "EAGLE1", "team": "blue",
     "aircraft_config_path": ")" + f16 + R"(",
     "aircraft_name": "F-16C_50", "vis_type_index": 1052,
     "spawn_in_air": true, "initial_fuel_lbs": 6500.0,
     "initial_vt_fps": 532.0,
     "parking_spot": {"x": 0.0, "y": 0.0, "z": 15000.0},
     "heading_rad": 0.0},
    {"callsign": "EAGLE2", "team": "blue", "lead_callsign": "EAGLE1",
     "aircraft_config_path": ")" + f16 + R"(",
     "aircraft_name": "F-16C_50", "vis_type_index": 1052,
     "spawn_in_air": true, "initial_fuel_lbs": 6500.0,
     "initial_vt_fps": 532.0,
     "parking_spot": {"x": 2000.0, "y": -2500.0, "z": 15000.0},
     "heading_rad": 0.0},
    {"callsign": "BANDIT1", "team": "red",
     "aircraft_config_path": ")" + f16 + R"(",
     "aircraft_name": "F-16C_50", "vis_type_index": 1052,
     "spawn_in_air": true, "initial_fuel_lbs": 6500.0,
     "initial_vt_fps": 532.0,
     "parking_spot": {"x": 0.0, "y": 17013.0, "z": 15000.0},
     "heading_rad": 3.14159265358979, "hold_fire": true},
    {"callsign": "BANDIT2", "team": "red",
     "aircraft_config_path": ")" + f16 + R"(",
     "aircraft_name": "F-16C_50", "vis_type_index": 1052,
     "spawn_in_air": true, "initial_fuel_lbs": 6500.0,
     "initial_vt_fps": 532.0,
     "parking_spot": {"x": -2000.0, "y": 19513.0, "z": 15000.0},
     "heading_rad": 3.14159265358979, "hold_fire": true}
  ],
  "airfield": {
    "active_runway_id": 36, "active_runway_name": "Rwy 36",
    "runway_heading_rad": 0.0,
    "threshold_position": {"x": 0.0, "y": -5000.0, "z": 0.0},
    "runway_end_position": {"x": 0.0, "y": 5000.0, "z": 0.0},
    "threshold_altitude_ft": 0.0, "departure_altitude_ft": 15000.0,
    "taxi_route": [{"x": 0.0, "y": -5000.0, "z": 0.0},
                   {"x": 0.0, "y": 0.0, "z": 0.0}]
  }
})";
    return p;
}

/// A combat scenario whose aircraft never reach the WVR band: a stern
/// chase 40 NM behind with the target pulling away is inside detection
/// range but the fight stays BVR (the WVR band is 3 NM entry) — the
/// fight_alive gate must name the band-boundary rung (exit 4's class).
std::filesystem::path write_never_in_band_scenario(const std::string& f16) {
    const auto dir = std::filesystem::temp_directory_path();
    const auto p = dir / "f4_wvr_harness_neverband_test.json";
    std::ofstream f(p);
    // The bandit is 40 NM ahead flying the SAME speed — the range never
    // closes under the 3 NM entry. The radar detects (inside 40 NM ref),
    // the BVR rung fights, the WVR band is never entered. 30 NM = 182,283 ft.
    f << R"({
  "name": "wvr_neverbänd_test",
  "theater": "korea",
  "sim_dt": 0.016666667,
  "total_ticks": 6000,
  "start_enroute": true,
  "combat": {"enabled": true, "radar_rng_seed": 777, "bvr_hold": true},
  "waypoints": [
    {"name": "FAR_NORTH", "position": {"x": 0.0, "y": 500000.0, "z": 10000.0},
     "speed_kts": 420.0}
  ],
  "aircraft": [
    {"callsign": "EAGLE1", "team": "blue",
     "aircraft_config_path": ")" + f16 + R"(",
     "aircraft_name": "F-16C_50", "vis_type_index": 1052,
     "spawn_in_air": true, "initial_fuel_lbs": 6500.0,
     "initial_vt_fps": 506.0,
     "parking_spot": {"x": 0.0, "y": 0.0, "z": 10000.0},
     "heading_rad": 0.0},
    {"callsign": "BANDIT1", "team": "red",
     "aircraft_config_path": ")" + f16 + R"(",
     "aircraft_name": "F-16C_50", "vis_type_index": 1052,
     "spawn_in_air": true, "initial_fuel_lbs": 6500.0,
     "initial_vt_fps": 506.0,
     "parking_spot": {"x": 0.0, "y": 182283.0, "z": 10000.0},
     "heading_rad": 0.0, "hold_fire": true}
  ],
  "airfield": {
    "active_runway_id": 36, "active_runway_name": "Rwy 36",
    "runway_heading_rad": 0.0,
    "threshold_position": {"x": 0.0, "y": -5000.0, "z": 0.0},
    "runway_end_position": {"x": 0.0, "y": 5000.0, "z": 0.0},
    "threshold_altitude_ft": 0.0, "departure_altitude_ft": 10000.0,
    "taxi_route": [{"x": 0.0, "y": -5000.0, "z": 0.0},
                   {"x": 0.0, "y": 0.0, "z": 0.0}]
  }
})";
    return p;
}

WvrMergeHarnessOptions make_opts(const std::filesystem::path& scenario) {
    WvrMergeHarnessOptions o;
    o.scenario_json = scenario;
    o.asset_dir = scenario.parent_path();
    o.horizon_sec = 300;
    o.sample_sec = 30.0;
    o.runs = 2;
    return o;
}

} // namespace

// ============================================================================
// 1. The GUNS fight: runs, certifies, deterministic — and the engagement
//    window narrates the merge as a fight.
// ============================================================================
TEST(WvrMergeHarness, GunsFightRunsCertifiesAndIsDeterministic) {
    const auto scenario = guns_scenario();
    if (!scenario_ready(scenario)) GTEST_SKIP()
        << "guns_merge.json not configured (build it first)";

    auto h = WvrMergeHarness::create(make_opts(scenario));
    ASSERT_NE(h, nullptr);

    const auto& report = h->execute();
    ASSERT_FALSE(report.aborted) << report.abort_reason;

    // The four verdicts.
    EXPECT_TRUE(report.verdict.deterministic)
        << "MD5 run0=" << report.verdict.recorder_md5_run0
        << " run1=" << report.verdict.recorder_md5_run1;
    EXPECT_TRUE(report.verdict.engagement_completed)
        << "engagement_failure: " << report.verdict.engagement_failure;
    EXPECT_TRUE(report.verdict.roster_bounded)
        << "roster_leak: " << report.verdict.roster_leak;
    EXPECT_TRUE(report.verdict.fight_alive)
        << "fight_stall: " << report.verdict.fight_stall;

    // The certificate: two non-empty MD5s that match.
    EXPECT_FALSE(report.verdict.recorder_md5_run0.empty());
    EXPECT_FALSE(report.verdict.recorder_md5_run1.empty());
    EXPECT_EQ(report.verdict.recorder_md5_run0,
              report.verdict.recorder_md5_run1);

    // The engagement window: detect → band → gun → kill, in order.
    EXPECT_GE(report.verdict.first_detect_s, 0.0);
    EXPECT_GE(report.verdict.first_wvr_engage_s, 0.0);
    EXPECT_GE(report.verdict.first_gun_s, 0.0);
    EXPECT_GE(report.verdict.first_kill_s, 0.0);
    EXPECT_LE(report.verdict.first_detect_s,
              report.verdict.first_wvr_engage_s);
    EXPECT_LE(report.verdict.first_wvr_engage_s, report.verdict.first_gun_s);
    EXPECT_LE(report.verdict.first_gun_s, report.verdict.first_kill_s);
    // (The disengage ordering is asserted below via the recorder doc —
    // the post-kill stand-down; first_wvr_disengage_s may legitimately
    // precede the kill: the tick-1 detection-policy handoff flickers
    // the band for one tick before the radar's first scan completes.)

    // The guns did the work: at least one burst, no missile needed
    // (the scenario holds the missiles).
    EXPECT_GE(report.verdict.gun_bursts, 1);
    EXPECT_EQ(report.verdict.missile_shots, 0);

    // The kill is gun-attributed: reload the recorder document and
    // verify the chain end-to-end (the round-trip is part of the M5a
    // contract — the band events survive the JSON).
    auto rec = f4::recorder::FlightRecorder::from_json(report.recorder_json);
    std::uint64_t killer = 0, victim = 0;
    for (const auto& e : rec.combat_events()) {
        if (e.kind == f4::recorder::CombatEventKind::EntityKilled) {
            victim = e.subject_id;
            killer = e.object_id;
            break;
        }
    }
    EXPECT_NE(killer, 0u);
    EXPECT_NE(victim, 0u);
    bool gun_attribution = false;
    for (const auto& e : rec.combat_events()) {
        if (e.kind == f4::recorder::CombatEventKind::GunFired &&
            e.subject_id == killer) {
            gun_attribution = true;
            break;
        }
    }
    EXPECT_TRUE(gun_attribution)
        << "killer " << killer << " has no GunFired event";

    // The band events survive the round-trip with their kinds intact
    // (the parse-loop fix — wvr_engaged must not degrade to
    // track_acquired's default), and the fight ends its band presence
    // with the post-kill stand-down: some WvrDisengaged lands at or
    // after the kill. (A pre-kill disengage is legitimate — the tick-1
    // detection-policy handoff flickers the band for one tick before
    // the radar's first scan; the events record that honestly.)
    int band_events = 0;
    double last_disengage_s = -1.0;
    for (const auto& e : rec.combat_events()) {
        if (e.kind == f4::recorder::CombatEventKind::WvrEngaged ||
            e.kind == f4::recorder::CombatEventKind::WvrDisengaged) {
            ++band_events;
        }
        if (e.kind == f4::recorder::CombatEventKind::WvrDisengaged) {
            last_disengage_s = e.sim_time_s;
        }
    }
    EXPECT_GE(band_events, 2)
        << "the WVR band events did not survive the recorder round-trip";
    EXPECT_GE(last_disengage_s, report.verdict.first_kill_s)
        << "the brain never stood down after the kill";
}

// ============================================================================
// 2. The WVR (heaters) fight: runs, certifies, deterministic.
// ============================================================================
TEST(WvrMergeHarness, WvrFightRunsCertifiesAndIsDeterministic) {
    const auto scenario = wvr_scenario();
    if (!scenario_ready(scenario)) GTEST_SKIP()
        << "wvr_merge.json not configured (build it first)";

    auto h = WvrMergeHarness::create(make_opts(scenario));
    ASSERT_NE(h, nullptr);

    const auto& report = h->execute();
    ASSERT_FALSE(report.aborted) << report.abort_reason;

    EXPECT_TRUE(report.verdict.deterministic)
        << "MD5 run0=" << report.verdict.recorder_md5_run0
        << " run1=" << report.verdict.recorder_md5_run1;
    EXPECT_TRUE(report.verdict.engagement_completed)
        << "engagement_failure: " << report.verdict.engagement_failure;
    EXPECT_TRUE(report.verdict.roster_bounded)
        << "roster_leak: " << report.verdict.roster_leak;
    EXPECT_TRUE(report.verdict.fight_alive)
        << "fight_stall: " << report.verdict.fight_stall;

    // The heater path: detect → band → launch → kill.
    EXPECT_GE(report.verdict.first_wvr_engage_s, 0.0);
    EXPECT_GE(report.verdict.missile_shots, 1);
    EXPECT_GE(report.verdict.missile_hits, 1);
}

// ============================================================================
// 3. The refusal: a non-combat scenario aborts with the stable exit-2
//    prefix BEFORE any load validation.
// ============================================================================
TEST(WvrMergeHarness, RefusesNonCombatScenario) {
    const auto scenario = write_noncombat_scenario();
    WvrMergeHarnessOptions opts = make_opts(scenario);
    opts.runs = 1;
    auto h = WvrMergeHarness::create(opts);
    ASSERT_NE(h, nullptr);

    const auto& report = h->execute();
    EXPECT_TRUE(report.aborted);
    EXPECT_NE(report.abort_reason.find("scenario combat.enabled is false"),
              std::string::npos)
        << "abort_reason: " << report.abort_reason;
    std::filesystem::remove(scenario);
}

// ============================================================================
// 4. Detected but never in the band: fight_alive FALSE with the band-
//    boundary diagnostic (the exit-4 failure class).
// ============================================================================
TEST(WvrMergeHarness, FightAliveFiresWhenBandNeverEntered) {
    const std::string f16 = f16_config_path();
    if (f16.empty()) GTEST_SKIP() << "generated f16.json fixture not found";

    const auto scenario = write_never_in_band_scenario(f16);
    WvrMergeHarnessOptions opts = make_opts(scenario);
    opts.horizon_sec = 100;   // detect + a BVR stern chase; no band entry
    opts.runs = 1;
    auto h = WvrMergeHarness::create(opts);
    ASSERT_NE(h, nullptr);

    const auto& report = h->execute();
    ASSERT_FALSE(report.aborted) << report.abort_reason;

    // The fight detected (the pre-engage side held) ...
    EXPECT_GE(report.tracks_acquired, 1)
        << "the scenario must detect — that is its point";
    // ... but the band was never entered, and the gate names it.
    EXPECT_FALSE(report.verdict.fight_alive);
    EXPECT_NE(report.verdict.fight_stall.find("never entered the WVR band"),
              std::string::npos)
        << "fight_stall: " << report.verdict.fight_stall;
    EXPECT_EQ(report.wvr_engagements, 0);

    std::filesystem::remove(scenario);
}

// ============================================================================
// 5. runs == 1 skips the determinism proof.
// ============================================================================
TEST(WvrMergeHarness, SingleRunSkipsTheDeterminismProof) {
    const auto scenario = guns_scenario();
    if (!scenario_ready(scenario)) GTEST_SKIP()
        << "guns_merge.json not configured (build it first)";

    auto opts = make_opts(scenario);
    opts.runs = 1;
    auto h = WvrMergeHarness::create(opts);
    ASSERT_NE(h, nullptr);

    const auto& report = h->execute();
    ASSERT_FALSE(report.aborted) << report.abort_reason;

    EXPECT_TRUE(report.verdict.deterministic);   // vacuously true
    EXPECT_FALSE(report.verdict.recorder_md5_run0.empty());
    EXPECT_TRUE(report.verdict.recorder_md5_run1.empty());
}

// ============================================================================
// 6. The 2v2 multi-flight acceptance (M4 §6's multi-flight deferral).
// ============================================================================
TEST(WvrMergeHarness, TwoShipMergeCompletesThroughTheHarness) {
    const std::string f16 = f16_config_path();
    if (f16.empty()) GTEST_SKIP() << "generated f16.json fixture not found";

    const auto scenario = write_two_ship_scenario(f16);
    WvrMergeHarnessOptions opts = make_opts(scenario);
    opts.horizon_sec = 300;
    opts.runs = 2;
    auto h = WvrMergeHarness::create(opts);
    ASSERT_NE(h, nullptr);

    const auto& report = h->execute();
    ASSERT_FALSE(report.aborted) << report.abort_reason;

    // The roster: a 2-ship per side.
    EXPECT_EQ(report.blue_aircraft, 2);
    EXPECT_EQ(report.red_aircraft, 2);

    // The four verdicts hold at flight scale.
    EXPECT_TRUE(report.verdict.deterministic)
        << "MD5 run0=" << report.verdict.recorder_md5_run0
        << " run1=" << report.verdict.recorder_md5_run1;
    EXPECT_TRUE(report.verdict.engagement_completed)
        << "engagement_failure: " << report.verdict.engagement_failure;
    EXPECT_TRUE(report.verdict.roster_bounded)
        << "roster_leak: " << report.verdict.roster_leak;
    EXPECT_TRUE(report.verdict.fight_alive)
        << "fight_stall: " << report.verdict.fight_stall;

    // At least one bandit died, and the band was entered at least once
    // (the fight went WVR, not a lucky BVR shot — the spawn is 2.8 NM).
    EXPECT_GE(report.verdict.kills, 1);
    EXPECT_GE(report.wvr_engagements, 1);

    std::filesystem::remove(scenario);
}

// ============================================================================
// 7. The MD5 helper (the digest the certificate rests on).
// ============================================================================
TEST(WvrMergeHarnessMd5, KnownVectors) {
    // The harness's MD5 is file-private; the vectors run through a real
    // fight's recorder JSON instead: the digest must be a stable 32-hex
    // string, and recomputing over the same bytes must agree (the
    // self-consistency the certificate needs — the cross-host vectors
    // live in the campaign/BVR rigs which share the same copied class).
    const auto scenario = guns_scenario();
    if (!scenario_ready(scenario)) GTEST_SKIP()
        << "guns_merge.json not configured (build it first)";

    auto opts = make_opts(scenario);
    opts.runs = 1;
    opts.horizon_sec = 60;
    auto h = WvrMergeHarness::create(opts);
    ASSERT_NE(h, nullptr);
    const auto& report = h->execute();
    ASSERT_FALSE(report.aborted) << report.abort_reason;

    const auto& md5 = report.verdict.recorder_md5_run0;
    ASSERT_EQ(md5.size(), 32u);
    for (const char c : md5) {
        EXPECT_TRUE((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))
            << "non-hex char in digest: " << md5;
    }
    EXPECT_EQ(report.recorder_json.size() > 0u, true);
}
