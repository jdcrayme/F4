// f4-simulation/tools/ground_strike_qc.cpp
//
// ground_strike_qc — the M5b end-to-end AIR-TO-GROUND strike acceptance
// tool (the harness both COMBAT_CHAIN_M4_PLAN §6 and
// COMBAT_CHAIN_M5_PLAN §6 deferred as "the certified A/G rung is its own
// tranche"). The combat-chain sibling of bvr_intercept_qc (M4) and
// wvr_merge_qc (M5a): where those certify the A/A ladder, M5b certifies
// the A/G chain — arm → fly the delivery waypoint → CCIP release →
// ballistic flyout → impact on the objective → feature damage → the
// fstatus ledger.
//
// One command runs a strike scenario headless, in-process, through
// GroundStrikeHarness (f4-simulation). The harness composes the strike
// world (the objective, the bomb station, the StrikeModule + mission-plan
// binding — see ground_strike_harness.hpp), runs the strike TWICE (fresh
// Simulation per pass) and certifies five verdicts:
//
//   * DETERMINISTIC      — the recorder's to_json() byte stream is
//                          identical across the two passes (certified as
//                          MD5).
//   * RELEASE_OCCURRED   — at least one BombReleased by an armed striker.
//   * IMPACT_ON_TARGET   — at least one BombImpact with end_cause
//                          "impact" against the objective.
//   * DAMAGE_APPLIED     — the on-target impacts destroyed at least one
//                          feature (cross-checked against the objective's
//                          damage ledger in the summary).
//   * ROSTER_BOUNDED     — live == initial + spawned − retired at every
//                          sample (no entity leak; bombs count).
//
// ARTIFACTS (three, the campaign_qc / bvr_intercept_qc / wvr_merge_qc
// pattern):
//   1. ground_strike_result.json  — run 0's recorder JSON verbatim (the
//      byte-stable certificate — what a human md5sums).
//   2. ground_strike_summary.json — DETERMINISTIC CONTENT ONLY: verdicts,
//      counters, MD5s, the strike window. NO wall-clock, NO RSS, NO
//      ticks/sec (those live in the diary).
//   3. ground_strike_diary.json   — per-sample telemetry. Explicitly NOT
//      byte-stable; that's why it's a separate artifact.
//
// Exit code (the acceptance surface — mirrors the M4/M5a tables):
//   0  all five verdicts green.
//   1  usage / IO error / scenario-load failure / harness abort.
//   2  scenario has combat.enabled == false (the harness refuses —
//      "wrong scenario" vs "broken strike" must be distinguishable).
//   3  release_occurred violated — no BombReleased within horizon (the
//      arming/trigger stall; release_stall names the rung).
//   4  impact_on_target violated — released but never hit the objective
//      (the flyout stall; impact_failure names the rung).
//   5  damage_applied violated — hit the objective but destroyed no
//      features (the terminal-effectiveness stall; damage_failure names
//      the rung).
//   6  roster_bounded violated — entity leak.
//   9  deterministic violated — run 1's recorder bytes differ from run
//      0's. Skipped when --runs 1.
//
// The exit-code priority follows the table above (numeric): release (3)
// → impact (4) → damage (5) → roster (6) → deterministic (9) → 0.
//
// Usage:
//   ground_strike_qc <scenario.json> [options]
//     --horizon-sec <N>     sim-seconds of run (default 300)
//     --sample-sec <N>      diary sample cadence (default 30)
//     --runs <N>            determinism passes (default 2; 1 skips the
//                            proof)
//     --out-dir <path>      artifact directory (default: scenario's
//                            parent)
//     --max-wall <sec>      wall-clock watchdog across all runs (default
//                            0 = off)
//     --quiet               suppress per-sample progress (stderr)
//     --help                show this message

#include "qc_tool_support.hpp"
#include <f4/simulation/ground_strike_harness.hpp>
#include <f4/json/f4_json.hpp>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

[[noreturn]] void usage(const char* prog) {
    std::fprintf(stderr,
        "usage: %s <scenario.json> [options]\n"
        "  --horizon-sec <N>     sim-seconds of run (default 300)\n"
        "  --sample-sec <N>      diary sample cadence (default 30)\n"
        "  --runs <N>            determinism passes (default 2; 1 skips\n"
        "                         the proof)\n"
        "  --out-dir <path>      artifact directory (default: scenario's\n"
        "                         parent)\n"
        "  --max-wall <sec>      wall-clock watchdog (default 0 = off)\n"
        "  --quiet               suppress per-sample progress\n"
        "  --help                show this message\n",
        prog);
    std::exit(1);
}

namespace {

// ---------------------------------------------------------------------------
// CLI
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// JSON Writer helpers (mirroring wvr_merge_qc).
// ---------------------------------------------------------------------------

} // namespace

// ===========================================================================
// main — thin layer: parse args, GroundStrikeHarness::create, execute,
// write artifacts, derive exit code, return.
// ===========================================================================
int main(int argc, char** argv) {
    const Args args = parse_args(argc, argv);

    if (!std::filesystem::exists(args.scenario_json)) {
        std::fprintf(stderr,
                     "ground_strike_qc: scenario JSON not found: %s\n",
                     args.scenario_json.string().c_str());
        return 1;
    }

    f4::simulation::GroundStrikeHarnessOptions hopts;
    hopts.scenario_json = std::filesystem::absolute(args.scenario_json);
    hopts.horizon_sec = args.horizon_sec;
    hopts.sample_sec = args.sample_sec;
    hopts.runs = args.runs;
    hopts.max_wall_sec_total = args.max_wall_sec;

    std::printf("ground_strike: scenario=%s horizon=%llds sample=%.0fs "
                "runs=%d max_wall=%.0fs\n",
                args.scenario_json.string().c_str(),
                (long long)hopts.horizon_sec, hopts.sample_sec, hopts.runs,
                hopts.max_wall_sec_total);

    std::string err;
    auto harness = f4::simulation::GroundStrikeHarness::create(hopts, &err);
    if (harness == nullptr) {
        std::fprintf(stderr, "ground_strike_qc: %s\n", err.c_str());
        return 1;
    }

    // Progress: one line per sample to stderr (the harness only invokes
    // the callback during run 0). --quiet suppresses the line.
    const auto t_wall = std::chrono::steady_clock::now();
    const bool quiet = args.quiet;
    harness->execute([quiet](const f4::simulation::GroundStrikeSample& s) {
        if (quiet) return;
        std::fprintf(stderr,
                     "strike[s%02d] t=%.1fs rel=%d imp=%d on_tgt=%d "
                     "live_bombs=%d dmg_feat=%.0f\n",
                     s.sample, s.sim_time_s, s.bombs_released,
                     s.bombs_impacted, s.impacts_on_target, s.live_bombs,
                     s.features_destroyed_max);
        std::fflush(stderr);
    });
    const std::chrono::duration<double> wall =
        std::chrono::steady_clock::now() - t_wall;
    const auto& r = harness->report();

    // -----------------------------------------------------------------------
    // Abort: the harness refused / hit the watchdog / failed to load.
    // Exit 1, except combat-disabled which is exit 2. The prefix match is
    // the contract — the harness owns the wording, the tool matches it.
    // -----------------------------------------------------------------------
    if (r.aborted) {
        const std::string combat_disabled_prefix =
            "scenario combat.enabled is false";
        if (r.abort_reason.rfind(combat_disabled_prefix, 0) == 0) {
            std::fprintf(stderr,
                         "ground_strike_qc: scenario has combat.enabled == "
                         "false — the harness refuses to run a non-combat "
                         "scenario (exit 2). %s\n",
                         r.abort_reason.c_str());
            return 2;
        }
        std::fprintf(stderr, "ground_strike_qc: harness ABORTED — %s\n",
                     r.abort_reason.c_str());
        return 1;
    }

    // -----------------------------------------------------------------------
    // Artifacts (the three-file pattern).
    // -----------------------------------------------------------------------
    std::filesystem::create_directories(args.out_dir);

    // 1. The certificate — run 0's recorder JSON, byte-stable.
    const auto result_path = args.out_dir / "ground_strike_result.json";
    {
        std::ofstream out(result_path);
        out << r.recorder_json;
    }

    // 2. The summary — DETERMINISTIC CONTENT ONLY.
    const auto summary_path = args.out_dir / "ground_strike_summary.json";
    {
        f4::json::Writer w;
        w.put("{\n  \"format\": \"f4-ground-strike-summary\",\n  ");
        w.put("\"version\": 1,\n  \"scenario_json\": ");
        write_string(w, args.scenario_json.string());
        w.put(",\n  \"strike\": {\n    ");
        w.number_key("horizon_sec",
                     static_cast<std::int64_t>(hopts.horizon_sec));
        w.put(",    ");
        w.number_key("sample_sec", hopts.sample_sec);
        w.put(",    ");
        w.number_key("runs", hopts.runs);
        // Preconditions the gates key on.
        w.put(",\n    \"combat_enabled\": ");
        w.put(r.combat_enabled ? "true" : "false");
        w.put(",    ");
        w.number_key("aircraft_count", r.aircraft_count);
        w.put(",    ");
        w.number_key("blue_aircraft", r.blue_aircraft);
        w.put(",    ");
        w.number_key("red_aircraft", r.red_aircraft);
        w.put(",    ");
        w.number_key("strikers_armed", r.strikers_armed);
        w.put(",    ");
        w.number_key("target_entity_id",
                     static_cast<std::int64_t>(r.target_entity_id));
        w.put(",    ");
        w.number_key("target_features", r.target_features);
        // Final cumulative counters (run 0, end of horizon).
        w.put(",\n    ");
        w.number_key("bombs_released", r.bombs_released);
        w.put(",    ");
        w.number_key("bombs_impacted", r.bombs_impacted);
        w.put(",    ");
        w.number_key("samples", r.samples);
        // The five verdicts.
        w.put(",\n    \"deterministic\": ");
        w.put(r.verdict.deterministic ? "true" : "false");
        w.put(",    \"release_occurred\": ");
        w.put(r.verdict.release_occurred ? "true" : "false");
        w.put(",    \"impact_on_target\": ");
        w.put(r.verdict.impact_on_target ? "true" : "false");
        w.put(",    \"damage_applied\": ");
        w.put(r.verdict.damage_applied ? "true" : "false");
        w.put(",    \"roster_bounded\": ");
        w.put(r.verdict.roster_bounded ? "true" : "false");
        // Diagnostics (first violation per gate; empty when green).
        w.put(",\n    \"release_stall\": ");
        write_string(w, r.verdict.release_stall);
        w.put(",\n    \"impact_failure\": ");
        write_string(w, r.verdict.impact_failure);
        w.put(",\n    \"damage_failure\": ");
        write_string(w, r.verdict.damage_failure);
        w.put(",\n    \"roster_leak\": ");
        write_string(w, r.verdict.roster_leak);
        // The certificate: the two recorder MD5s.
        w.put(",\n    \"recorder_md5_run0\": ");
        write_string(w, r.verdict.recorder_md5_run0);
        w.put(",\n    \"recorder_md5_run1\": ");
        write_string(w, r.verdict.recorder_md5_run1);
        // The strike window (-1 = the event never happened).
        w.put(",\n    ");
        w.number_key("first_release_s", r.verdict.first_release_s);
        w.put(",    ");
        w.number_key("first_impact_s", r.verdict.first_impact_s);
        w.put(",    ");
        w.number_key("first_on_target_s", r.verdict.first_on_target_s);
        w.put(",    ");
        w.number_key("first_damage_s", r.verdict.first_damage_s);
        w.put(",\n    ");
        w.number_key("bombs_released_window", r.verdict.bombs_released);
        w.put(",    ");
        w.number_key("bombs_impacted_window", r.verdict.bombs_impacted);
        w.put(",    ");
        w.number_key("impacts_on_target", r.verdict.impacts_on_target);
        w.put(",    ");
        w.number_key("bomb_misses", r.verdict.bomb_misses);
        w.put(",    ");
        w.number_key("min_miss_distance_ft", r.verdict.min_miss_distance_ft);
        w.put(",\n    ");
        w.number_key("features_destroyed_max",
                     r.verdict.features_destroyed_max);
        w.put(",    ");
        w.number_key("destroyed_pct_max", r.verdict.destroyed_pct_max);
        w.put(",    ");
        w.number_key("features_destroyed_final",
                     r.verdict.features_destroyed_final);
        w.put(",    ");
        w.number_key("destroyed_pct_final", r.verdict.destroyed_pct_final);
        w.put("\n  }\n}\n");

        std::ofstream out(summary_path);
        out << w.str();
    }

    // 3. The diary — per-sample telemetry, NOT byte-stable.
    const auto diary_path = args.out_dir / "ground_strike_diary.json";
    {
        f4::json::Writer w;
        w.put("{\n  \"format\": \"f4-ground-strike-diary\",\n  ");
        w.put("\"version\": 1,\n  \"samples\": [\n    ");
        for (std::size_t i = 0; i < r.diary.size(); ++i) {
            const auto& s = r.diary[i];
            if (i > 0) w.put(",\n    ");
            w.put("{ ");
            w.number_key("sample", s.sample);
            w.put(", ");
            w.number_key("sim_time_s", s.sim_time_s);
            w.put(", ");
            w.number_key("initial_entities", s.initial_entities);
            w.put(", ");
            w.number_key("spawned_entities", s.spawned_entities);
            w.put(", ");
            w.number_key("retired_entities", s.retired_entities);
            w.put(", ");
            w.number_key("live_entities", s.live_entities);
            w.put(", ");
            w.number_key("live_bombs", s.live_bombs);
            w.put(", ");
            w.number_key("bombs_released", s.bombs_released);
            w.put(", ");
            w.number_key("bombs_impacted", s.bombs_impacted);
            w.put(", ");
            w.number_key("impacts_on_target", s.impacts_on_target);
            w.put(", ");
            w.number_key("damage_events", s.damage_events);
            w.put(", ");
            w.number_key("features_destroyed_max",
                         s.features_destroyed_max);
            w.put(", ");
            w.number_key("destroyed_pct_max", s.destroyed_pct_max);
            w.put(", ");
            w.number_key("min_miss_distance_ft", s.min_miss_distance_ft);
            w.put(", ");
            w.number_key("ledger_features_destroyed",
                         s.ledger_features_destroyed);
            w.put(", ");
            w.number_key("ledger_destroyed_pct", s.ledger_destroyed_pct);
            w.put(", ");
            w.number_key("sample_releases", s.sample_releases);
            w.put(", ");
            w.number_key("sample_impacts", s.sample_impacts);
            // Telemetry: the columns that make this file NOT byte-stable.
            char buf[128];
            std::snprintf(buf, sizeof(buf),
                          ", \"wall_sec\": %.3f, \"ticks_per_sec\": %.1f, "
                          "\"rss_kb\": %ld",
                          s.wall_sec, s.ticks_per_sec, s.rss_kb);
            w.put(buf);
            w.put(" }");
        }
        w.put(r.diary.empty() ? "]" : "\n  ]");
        w.put("\n}\n");

        std::ofstream out(diary_path);
        out << w.str();
    }

    // -----------------------------------------------------------------------
    // The headline + the gates
    // -----------------------------------------------------------------------
    std::printf("ground_strike: wall=%.1fs samples=%d released=%d "
                "impacted=%d on_target=%d ledger_destroyed=%d (%.1f%%)\n",
                wall.count(), r.samples, r.bombs_released,
                r.bombs_impacted, r.verdict.impacts_on_target,
                static_cast<int>(r.verdict.features_destroyed_final),
                r.verdict.destroyed_pct_final);
    std::printf("ground_strike: deterministic=%s release=%s impact=%s "
                "damage=%s roster=%s md5=%s\n",
                r.verdict.deterministic ? "yes" : "NO",
                r.verdict.release_occurred ? "ok" : "STALL",
                r.verdict.impact_on_target ? "ok" : "MISS",
                r.verdict.damage_applied ? "ok" : "NO EFFECT",
                r.verdict.roster_bounded ? "ok" : "LEAK",
                r.verdict.recorder_md5_run0.c_str());
    std::printf("wrote: %s\n", summary_path.string().c_str());
    std::printf("wrote: %s\n", result_path.string().c_str());
    std::printf("wrote: %s\n", diary_path.string().c_str());

    // -----------------------------------------------------------------------
    // Exit-code derivation (the acceptance surface). Priority: release
    // (3) → impact (4) → damage (5) → roster (6) → deterministic (9) → 0.
    // -----------------------------------------------------------------------

    if (!r.verdict.release_occurred) {
        std::fprintf(stderr,
                     "ground_strike_qc: QC FAILURE — release_occurred "
                     "violated, no attributed BombReleased within horizon "
                     "(exit 3, arming/trigger stall). %s Inspect the "
                     "diary's bombs_released column and the strike window "
                     "in ground_strike_summary.json.\n",
                     r.verdict.release_stall.c_str());
        return 3;
    }

    if (!r.verdict.impact_on_target) {
        std::fprintf(stderr,
                     "ground_strike_qc: QC FAILURE — impact_on_target "
                     "violated, released but never hit the objective "
                     "(exit 4, flyout stall). %s Inspect the strike "
                     "window's first_impact_s / bomb_misses in "
                     "ground_strike_summary.json.\n",
                     r.verdict.impact_failure.c_str());
        return 4;
    }

    if (!r.verdict.damage_applied) {
        std::fprintf(stderr,
                     "ground_strike_qc: QC FAILURE — damage_applied "
                     "violated, on-target impacts destroyed no features "
                     "(exit 5, terminal-effectiveness stall). %s Inspect "
                     "min_miss_distance_ft vs the target_features "
                     "placement in ground_strike_summary.json.\n",
                     r.verdict.damage_failure.c_str());
        return 5;
    }

    if (!r.verdict.roster_bounded) {
        std::fprintf(stderr,
                     "ground_strike_qc: QC FAILURE — roster_bounded "
                     "violated, entity leak (exit 6). %s Inspect the "
                     "diary's live_entities vs initial + spawned − "
                     "retired in ground_strike_diary.json.\n",
                     r.verdict.roster_leak.c_str());
        return 6;
    }

    if (hopts.runs >= 2 && !r.verdict.deterministic) {
        std::fprintf(stderr,
                     "ground_strike_qc: QC FAILURE — NOT deterministic: "
                     "run 0 md5 %s != run 1 md5 %s (exit 9). Diff "
                     "ground_strike_result.json against a re-run to find "
                     "the first diverging event.\n",
                     r.verdict.recorder_md5_run0.c_str(),
                     r.verdict.recorder_md5_run1.c_str());
        return 9;
    }

    return 0;
}
