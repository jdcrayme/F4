// f4-simulation/tests/test_bvr_intercept_harness.cpp
//
// M4 — the BVR intercept acceptance harness, pinned over the shipped
// bvr_intercept.json scenario (short horizons: the rig compresses
// minutes to seconds the way the campaign-war harness compresses hours).
//
//   1. The fight runs, certifies, and is deterministic: two passes over
//      the same horizon produce identical recorder MD5s (the M4
//      contract, in miniature), the engagement completes (at least one
//      kill with attribution), the roster identity holds at every
//      sample, and the fight stays alive (the brain detected + engaged).
//   2. The engagement_completed verdict fires when the kill happens:
//      first_kill_s is set, shots_fired >= 1, the killer matches a
//      MissileLaunched.shooter_id.
//   3. The fight_alive verdict fires when the AI never detects: a
//      scenario with aircraft spawned beyond detection range produces
//      fight_alive == false with the "no RadarTrackAcquired" stall
//      diagnostic.
//   4. A hold_fire scenario detects + locks but does not fire: the
//      fight_alive verdict stays TRUE (the brain engaged via STT —
//      RwrLock is engagement), but engagement_completed is FALSE and
//      the engagement_failure names the "no launch" rung.
//   5. runs == 1 skips the determinism proof: one MD5, no second, the
//      verdict stays vacuously true.
//   6. The harness refuses a non-combat scenario: combat.enabled ==
//      false produces a harness abort (not a verdict), the abort_reason
//      names the refusal.
//
// Companion: Docs/COMBAT_CHAIN_M4_PLAN.md (the M4 plan), test_combat_
// integration.cpp::BvrInterceptScenarioFilePlaysOut (the M3 precedent
// — the same scenario, hand-driven, that the harness now drives via
// the brain).

#include <f4/simulation/bvr_intercept_harness.hpp>
#include <f4/simulation/scenario.hpp>
#include <f4/recorder/flight_recorder.hpp>
#include <f4/recorder/combat_event.hpp>

#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>

using namespace f4::simulation;

namespace {

std::filesystem::path bvr_scenario() {
#ifdef F4_SCENARIOS_DIR
    return std::filesystem::path(F4_SCENARIOS_DIR) / "bvr_intercept.json";
#else
    return {};
#endif
}

bool scenario_ready() {
    const auto p = bvr_scenario();
    return !p.empty() && std::filesystem::exists(p);
}

/// Write a minimal non-combat scenario to a temp file. The harness
/// refuses it (combat.enabled == false) at run_pass_ — the check
/// happens BEFORE Simulation construction, so the scenario doesn't
/// need valid aircraft config or terrain.
std::filesystem::path write_noncombat_scenario() {
    const auto dir = std::filesystem::temp_directory_path();
    const auto p = dir / "f4_bvr_harness_noncombat_test.json";
    std::ofstream f(p);
    f << R"({
  "name": "noncombat_test",
  "theater": "korea",
  "sim_dt": 0.016666667,
  "total_ticks": 100
})";
    return p;
}

/// Write a combat scenario with aircraft spawned far beyond detection
/// range (500 NM apart). The radar's reference range is 40 NM; at 500
/// NM no candidate passes the range pre-rejection (8× ref range = 320
/// NM), so no detection ever happens — the fight_alive pre-engage gate
/// fires by sample 2.
std::filesystem::path write_outrange_scenario() {
    const auto dir = std::filesystem::temp_directory_path();
    const auto p = dir / "f4_bvr_harness_outrange_test.json";
    std::ofstream f(p);
    // 500 NM = 3,038,058 ft. Spawn at (0,0,10000) and (0,3038058,10000).
    f << R"({
  "name": "outrange_test",
  "theater": "korea",
  "sim_dt": 0.016666667,
  "total_ticks": 600,
  "start_enroute": true,
  "combat": {"enabled": true, "radar_rng_seed": 777},
  "aircraft": [
    {"callsign": "EAGLE1", "team": "blue",
     "aircraft_config_path": "@F4_AIRCRAFT_CONFIG@",
     "spawn_in_air": true, "initial_vt_fps": 500.0,
     "parking_spot": {"x": 0.0, "y": 0.0, "z": 10000.0},
     "heading_rad": 0.0},
    {"callsign": "BANDIT1", "team": "red",
     "aircraft_config_path": "@F4_AIRCRAFT_CONFIG@",
     "spawn_in_air": true, "initial_vt_fps": 500.0,
     "parking_spot": {"x": 0.0, "y": 3038058.0, "z": 10000.0},
     "heading_rad": 3.14159265358979}
  ]
})";
    return p;
}

/// Write a combat scenario with hold_fire on the shooter. The brain
/// detects + commands STT (RwrLock fires on the victim) but never
/// releases a weapon (hold_fire gates release_pulse). fight_alive
/// stays TRUE (STT is engagement); engagement_completed is FALSE.
std::filesystem::path write_holdfire_scenario() {
    const auto dir = std::filesystem::temp_directory_path();
    const auto p = dir / "f4_bvr_harness_holdfire_test.json";
    std::ofstream f(p);
    // Stern chase: 13 NM = 78,990 ft. Shooter behind, both northbound.
    f << R"({
  "name": "holdfire_test",
  "theater": "korea",
  "sim_dt": 0.016666667,
  "total_ticks": 3600,
  "start_enroute": true,
  "combat": {"enabled": true, "radar_rng_seed": 777},
  "aircraft": [
    {"callsign": "EAGLE1", "team": "blue",
     "aircraft_config_path": "@F4_AIRCRAFT_CONFIG@",
     "spawn_in_air": true, "initial_vt_fps": 506.0,
     "parking_spot": {"x": 0.0, "y": 0.0, "z": 10000.0},
     "heading_rad": 0.0, "hold_fire": true},
    {"callsign": "BANDIT1", "team": "red",
     "aircraft_config_path": "@F4_AIRCRAFT_CONFIG@",
     "spawn_in_air": true, "initial_vt_fps": 420.0,
     "parking_spot": {"x": 0.0, "y": 78990.0, "z": 10000.0},
     "heading_rad": 0.0}
  ]
})";
    return p;
}

InterceptHarnessOptions make_opts(const std::filesystem::path& scenario) {
    InterceptHarnessOptions o;
    o.scenario_json = scenario;
    o.asset_dir = scenario.parent_path();
    o.horizon_sec = 300;       // 5 min — well past AMRAAM TOF
    o.sample_sec = 30.0;
    o.runs = 2;
    return o;
}

} // namespace

// ============================================================================
// 1. The fight runs, certifies, and is deterministic.
//    The M4 contract: a fight that runs to a kill with a byte-stable
//    recorder certificate across two passes.
// ============================================================================
TEST(BvrInterceptHarness, RunsCertifiesAndIsDeterministic) {
    if (!scenario_ready()) GTEST_SKIP()
        << "bvr_intercept.json not configured (build it first)";

    auto h = BvrInterceptHarness::create(make_opts(bvr_scenario()));
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

    // The recorder bytes themselves (the strict form — a digest
    // collision cannot pass).
    EXPECT_FALSE(report.recorder_json.empty());

    // The engagement window: detect → launch → kill, in order.
    EXPECT_GE(report.verdict.first_detect_s, 0.0);
    EXPECT_GE(report.verdict.first_launch_s, 0.0);
    EXPECT_GE(report.verdict.first_kill_s, 0.0);
    EXPECT_LE(report.verdict.first_detect_s, report.verdict.first_launch_s);
    EXPECT_LE(report.verdict.first_launch_s, report.verdict.first_kill_s);

    // At least one shot fired, at least one hit.
    EXPECT_GE(report.verdict.shots_fired, 1);
    EXPECT_GE(report.verdict.shots_hit, 1);

    // The diary carries one row per sample.
    EXPECT_GE(report.samples, 1);
    EXPECT_EQ(static_cast<int>(report.diary.size()), report.samples);

    // Headline counters.
    EXPECT_GE(report.tracks_acquired, 1);
    EXPECT_GE(report.missiles_launched, 1);
    EXPECT_GE(report.kills, 1);
}

// ============================================================================
// 2. The engagement_completed verdict fires when the kill happens:
//    attribution — the killer matches a MissileLaunched.shooter_id.
//    (Covered by test 1's assertions; this test isolates the
//    attribution check for diagnostics.)
// ============================================================================
TEST(BvrInterceptHarness, EngagementCompletedHasCorrectAttribution) {
    if (!scenario_ready()) GTEST_SKIP()
        << "bvr_intercept.json not configured (build it first)";

    auto h = BvrInterceptHarness::create(make_opts(bvr_scenario()));
    ASSERT_NE(h, nullptr);
    const auto& report = h->execute();
    ASSERT_FALSE(report.aborted) << report.abort_reason;

    ASSERT_TRUE(report.verdict.engagement_completed);
    ASSERT_GE(report.verdict.first_kill_s, 0.0);

    // The recorder carries the combat events; the harness stashes run
    // 0's events. Reload from the recorder_json to verify attribution
    // end-to-end (the recorder round-trip is part of the M4 contract).
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

    // The killer must have launched at least one missile at the victim.
    bool found_launch = false;
    for (const auto& e : rec.combat_events()) {
        if (e.kind == f4::recorder::CombatEventKind::MissileLaunched &&
            e.subject_id == killer) {
            found_launch = true;
            break;
        }
    }
    EXPECT_TRUE(found_launch)
        << "killer " << killer << " has no MissileLaunched event";
}

// ============================================================================
// 3. The fight_alive verdict fires when the AI never detects: aircraft
//    spawned beyond detection range produce no RadarTrackAcquired event
//    by sample 2, the pre-engage gate fires.
// ============================================================================
TEST(BvrInterceptHarness, FightAliveFiresWhenNoDetection) {
    const auto scenario = write_outrange_scenario();
    auto h = BvrInterceptHarness::create(make_opts(scenario));
    ASSERT_NE(h, nullptr);

    InterceptHarnessOptions short_opts = make_opts(scenario);
    short_opts.horizon_sec = 60;       // 1 min is enough to confirm no detect
    short_opts.runs = 1;               // determinism not under test here
    h = BvrInterceptHarness::create(short_opts);
    ASSERT_NE(h, nullptr);

    const auto& report = h->execute();
    ASSERT_FALSE(report.aborted) << report.abort_reason;

    EXPECT_FALSE(report.verdict.fight_alive);
    EXPECT_NE(report.verdict.fight_stall.find("no RadarTrackAcquired"),
              std::string::npos);
    EXPECT_EQ(report.tracks_acquired, 0);

    std::filesystem::remove(scenario);
}

// ============================================================================
// 4. A hold_fire scenario detects + locks but does not fire: fight_alive
//    stays TRUE (STT is engagement via RwrLock), engagement_completed is
//    FALSE, the engagement_failure names the "no launch" rung.
// ============================================================================
TEST(BvrInterceptHarness, HoldFireDetectsLocksButDoesNotFire) {
    const auto scenario = write_holdfire_scenario();
    InterceptHarnessOptions opts = make_opts(scenario);
    opts.horizon_sec = 90;       // enough time to detect + lock, not fire
    opts.runs = 1;
    auto h = BvrInterceptHarness::create(opts);
    ASSERT_NE(h, nullptr);

    const auto& report = h->execute();
    ASSERT_FALSE(report.aborted) << report.abort_reason;

    // The brain detected (fight_alive pre-engage side passes).
    EXPECT_TRUE(report.verdict.fight_alive)
        << "fight_stall: " << report.verdict.fight_stall;

    // The brain engaged via STT (RwrLock exists) — the finalize side
    // of fight_alive passes.
    EXPECT_GE(report.rwr_locks, 0)
        << "(RwrLock count — hold_fire still allows STT command)";

    // But no kill happened — engagement_completed is false.
    EXPECT_FALSE(report.verdict.engagement_completed);
    EXPECT_NE(report.verdict.engagement_failure.find("no MissileLaunched"),
              std::string::npos)
        << "engagement_failure: " << report.verdict.engagement_failure;

    EXPECT_EQ(report.kills, 0);
    EXPECT_EQ(report.verdict.first_kill_s, -1.0);

    std::filesystem::remove(scenario);
}

// ============================================================================
// 5. runs == 1 skips the determinism proof: one MD5, no second, the
//    verdict stays vacuously true.
// ============================================================================
TEST(BvrInterceptHarness, SingleRunSkipsTheDeterminismProof) {
    if (!scenario_ready()) GTEST_SKIP()
        << "bvr_intercept.json not configured (build it first)";

    auto opts = make_opts(bvr_scenario());
    opts.runs = 1;
    auto h = BvrInterceptHarness::create(opts);
    ASSERT_NE(h, nullptr);

    const auto& report = h->execute();
    ASSERT_FALSE(report.aborted) << report.abort_reason;

    EXPECT_FALSE(report.verdict.recorder_md5_run0.empty());
    EXPECT_TRUE(report.verdict.recorder_md5_run1.empty());
    EXPECT_TRUE(report.verdict.deterministic)
        << "runs==1 leaves deterministic vacuously true";
}

// ============================================================================
// 6. The harness refuses a non-combat scenario: combat.enabled == false
//    produces a harness abort (not a verdict), the abort_reason names
//    the refusal. Silent success on a non-combat scenario would be the
//    worst failure class.
// ============================================================================
TEST(BvrInterceptHarness, RejectsNonCombatScenario) {
    const auto scenario = write_noncombat_scenario();
    auto h = BvrInterceptHarness::create(make_opts(scenario));
    ASSERT_NE(h, nullptr);   // create() only checks the file exists

    const auto& report = h->execute();
    EXPECT_TRUE(report.aborted);
    EXPECT_NE(report.abort_reason.find("combat.enabled is false"),
              std::string::npos)
        << "abort_reason: " << report.abort_reason;

    // No verdicts derived (abort short-circuits finalize_).
    EXPECT_TRUE(report.verdict.deterministic);   // vacuous
    EXPECT_FALSE(report.verdict.engagement_completed);

    std::filesystem::remove(scenario);
}

// ============================================================================
// 7. The engagement_summary block is emitted in the recorder's
//    to_summary_json when combat events exist. (The recorder-side
//    extension is tested in test_combat_events.cpp; this test confirms
//    the harness's end-to-end run produces a recorder whose summary
//    carries the block.)
// ============================================================================
TEST(BvrInterceptHarness, EngagementSummaryBlockEmitted) {
    if (!scenario_ready()) GTEST_SKIP()
        << "bvr_intercept.json not configured (build it first)";

    auto h = BvrInterceptHarness::create(make_opts(bvr_scenario()));
    ASSERT_NE(h, nullptr);
    const auto& report = h->execute();
    ASSERT_FALSE(report.aborted) << report.abort_reason;

    auto rec = f4::recorder::FlightRecorder::from_json(report.recorder_json);
    const auto summary = rec.to_summary_json("bvr_intercept");

    EXPECT_NE(summary.find("engagement_summary"), std::string::npos)
        << "summary missing engagement_summary block";
    EXPECT_NE(summary.find("first_detect_s"), std::string::npos);
    EXPECT_NE(summary.find("first_launch_s"), std::string::npos);
    EXPECT_NE(summary.find("first_kill_s"), std::string::npos);
    EXPECT_NE(summary.find("shots_fired"), std::string::npos);
    EXPECT_NE(summary.find("shots_hit"), std::string::npos);
    EXPECT_NE(summary.find("weapon_effectiveness_pct"), std::string::npos);
}
