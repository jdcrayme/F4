// f4-simulation/tools/wvr_merge_qc.cpp
//
// wvr_merge_qc — the M5a end-to-end WVR / guns merge acceptance tool
// (see Docs/archive/COMBAT_CHAIN_M5_PLAN.md). The combat-chain sibling of
// bvr_intercept_qc (M4): where M4 certifies the BVR intercept, M5a
// certifies the inside-the-band fight — the merge, the guns, the
// heaters, the disengage.
//
// One command runs a WVR merge scenario headless, in-process, through
// WvrMergeHarness (f4-simulation). The harness runs the fight TWICE
// (fresh Simulation per pass) and certifies four verdicts:
//
//   * DETERMINISTIC            — the recorder's to_json() byte stream is
//                                 identical across the two passes
//                                 (certified as MD5).
//   * ENGAGEMENT_COMPLETED     — at least one EntityKilled within the
//                                 horizon, with attribution (missile OR
//                                 gun path).
//   * ROSTER_BOUNDED           — live == initial + spawned − retired at
//                                 every sample (no entity leak).
//   * FIGHT_ALIVE              — detection (RadarTrackAcquired) AND band
//                                 entry (>= 1 WvrEngaged event).
//
// ARTIFACTS (three, the campaign_qc --war / bvr_intercept_qc pattern):
//   1. wvr_merge_result.json  — run 0's recorder JSON verbatim (the
//      byte-stable certificate — what a human md5sums).
//   2. wvr_merge_summary.json — DETERMINISTIC CONTENT ONLY: verdicts,
//      counters, MD5s, the engagement window. NO wall-clock, NO RSS,
//      NO ticks/sec (those live in the diary).
//   3. wvr_merge_diary.json   — per-sample telemetry. Explicitly NOT
//      byte-stable; that's why it's a separate artifact.
//
// Exit code (the acceptance surface — mirrors M4's table):
//   0  all four verdicts green.
//   1  usage / IO error / scenario-load failure / harness abort.
//   2  scenario has combat.enabled == false (the harness refuses —
//      "wrong scenario" vs "broken fight" must be distinguishable).
//   3  fight_alive violated — no detection within horizon (pre-engage
//      stall). first_detect_s < 0 is the discriminator.
//   4  fight_alive violated — detected but never entered the WVR band
//      (no WvrEngaged event; the verdict's fight_stall names the rung).
//   5  engagement_completed violated — no kill within horizon, or kill
//      unattributed. The verdict's engagement_failure names the rung.
//   6  roster_bounded violated — entity leak.
//   9  deterministic violated — run 1's recorder bytes differ from run
//      0's. Skipped when --runs 1.
//
// The exit-code priority follows the table above (numeric): fight_alive
// (3/4) → engagement_completed (5) → roster_bounded (6) → deterministic
// (9) → 0.
//
// Usage:
//   wvr_merge_qc <scenario.json> [options]
//     --horizon-sec <N>     sim-seconds of fight (default 300)
//     --sample-sec <N>      diary sample cadence (default 30)
//     --runs <N>            determinism passes (default 2; 1 skips the
//                            proof)
//     --out-dir <path>      artifact directory (default: scenario's
//                            parent)
//     --max-wall <sec>      wall-clock watchdog across all runs (default
//                            0 = off)
//     --quiet               suppress per-sample progress (stderr)
//     --help                show this message

#include <f4/simulation/wvr_merge_harness.hpp>
#include <f4/json/f4_json.hpp>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

namespace {

// ---------------------------------------------------------------------------
// CLI
// ---------------------------------------------------------------------------
struct Args {
    std::filesystem::path scenario_json;
    std::filesystem::path out_dir;
    std::int64_t horizon_sec = 300;   // 5 min — the M4 acceptance horizon
    double sample_sec = 30.0;         // diary + check cadence
    int runs = 2;                     // 2 = the determinism proof; 1 = skip
    double max_wall_sec = 0.0;        // 0 = watchdog off
    bool quiet = false;
};

[[noreturn]] void usage(const char* prog) {
    std::fprintf(stderr,
        "usage: %s <scenario.json> [options]\n"
        "  --horizon-sec <N>     sim-seconds of fight (default 300)\n"
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

Args parse_args(int argc, char** argv) {
    Args a;
    if (argc < 2) usage(argv[0]);

    // --help can appear anywhere; handle it before the positional.
    for (int i = 1; i < argc; ++i) {
        const std::string tok = argv[i];
        if (tok == "--help" || tok == "-h") usage(argv[0]);
    }

    a.scenario_json = argv[1];
    for (int i = 2; i < argc; ++i) {
        const std::string k = argv[i];
        auto next = [&]() -> const char* {
            if (i + 1 >= argc) usage(argv[0]);
            return argv[++i];
        };
        if (k == "--horizon-sec") {
            a.horizon_sec = std::strtoll(next(), nullptr, 10);
        } else if (k == "--sample-sec") {
            a.sample_sec = std::atof(next());
        } else if (k == "--runs") {
            a.runs = std::max(1, std::atoi(next()));
        } else if (k == "--out-dir") {
            a.out_dir = next();
        } else if (k == "--max-wall") {
            a.max_wall_sec = std::atof(next());
        } else if (k == "--quiet") {
            a.quiet = true;
        } else {
            std::fprintf(stderr, "unknown option '%s'\n", k.c_str());
            usage(argv[0]);
        }
    }
    if (a.out_dir.empty()) a.out_dir = a.scenario_json.parent_path();
    return a;
}

// ---------------------------------------------------------------------------
// JSON Writer helpers (mirroring bvr_intercept_qc).
// ---------------------------------------------------------------------------
std::string json_escape(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
            default:   out += c;      break;
        }
    }
    return out;
}

void write_string(f4::json::Writer& w, const std::string& s) {
    w.put('"');
    w.put(json_escape(s));
    w.put('"');
}

} // namespace

// ===========================================================================
// main — thin layer: parse args, WvrMergeHarness::create, execute, write
// artifacts, derive exit code, return.
// ===========================================================================
int main(int argc, char** argv) {
    const Args args = parse_args(argc, argv);

    if (!std::filesystem::exists(args.scenario_json)) {
        std::fprintf(stderr, "wvr_merge_qc: scenario JSON not found: %s\n",
                     args.scenario_json.string().c_str());
        return 1;
    }

    f4::simulation::WvrMergeHarnessOptions hopts;
    hopts.scenario_json = std::filesystem::absolute(args.scenario_json);
    hopts.horizon_sec = args.horizon_sec;
    hopts.sample_sec = args.sample_sec;
    hopts.runs = args.runs;
    hopts.max_wall_sec_total = args.max_wall_sec;

    std::printf("wvr_merge: scenario=%s horizon=%llds sample=%.0fs "
                "runs=%d max_wall=%.0fs\n",
                args.scenario_json.string().c_str(),
                (long long)hopts.horizon_sec, hopts.sample_sec, hopts.runs,
                hopts.max_wall_sec_total);

    std::string err;
    auto harness = f4::simulation::WvrMergeHarness::create(hopts, &err);
    if (harness == nullptr) {
        std::fprintf(stderr, "wvr_merge_qc: %s\n", err.c_str());
        return 1;
    }

    // Progress: one line per sample to stderr (the harness only invokes
    // the callback during run 0). --quiet suppresses the line.
    const auto t_wall = std::chrono::steady_clock::now();
    const bool quiet = args.quiet;
    harness->execute([quiet](const f4::simulation::WvrMergeSample& s) {
        if (quiet) return;
        std::fprintf(stderr,
                     "wvr[s%02d] t=%.1fs band=%d guns=%d missiles=%d "
                     "kills=%d\n",
                     s.sample, s.sim_time_s, s.wvr_engagements,
                     s.gun_bursts, s.live_missiles, s.kills);
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
                         "wvr_merge_qc: scenario has combat.enabled == "
                         "false — the harness refuses to run a non-combat "
                         "scenario (exit 2). %s\n",
                         r.abort_reason.c_str());
            return 2;
        }
        std::fprintf(stderr, "wvr_merge_qc: harness ABORTED — %s\n",
                     r.abort_reason.c_str());
        return 1;
    }

    // -----------------------------------------------------------------------
    // Artifacts (the three-file pattern).
    // -----------------------------------------------------------------------
    std::filesystem::create_directories(args.out_dir);

    // 1. The certificate — run 0's recorder JSON, byte-stable.
    const auto result_path = args.out_dir / "wvr_merge_result.json";
    {
        std::ofstream out(result_path);
        out << r.recorder_json;
    }

    // 2. The summary — DETERMINISTIC CONTENT ONLY.
    const auto summary_path = args.out_dir / "wvr_merge_summary.json";
    {
        f4::json::Writer w;
        w.put("{\n  \"format\": \"f4-wvr-merge-summary\",\n  ");
        w.put("\"version\": 1,\n  \"scenario_json\": ");
        write_string(w, args.scenario_json.string());
        w.put(",\n  \"merge\": {\n    ");
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
        // Final cumulative counters (run 0, end of horizon).
        w.put(",\n    ");
        w.number_key("tracks_acquired", r.tracks_acquired);
        w.put(",    ");
        w.number_key("tracks_dropped", r.tracks_dropped);
        w.put(",    ");
        w.number_key("rwr_locks", r.rwr_locks);
        w.put(",    ");
        w.number_key("rwr_launches", r.rwr_launches);
        w.put(",    ");
        w.number_key("missiles_launched", r.missiles_launched);
        w.put(",    ");
        w.number_key("missiles_detonated", r.missiles_detonated);
        w.put(",    ");
        w.number_key("damage_events", r.damage_events);
        w.put(",    ");
        w.number_key("gun_bursts", r.gun_bursts);
        w.put(",    ");
        w.number_key("wvr_engagements", r.wvr_engagements);
        w.put(",    ");
        w.number_key("kills", r.kills);
        w.put(",    ");
        w.number_key("samples", r.samples);
        // The four verdicts.
        w.put(",\n    \"deterministic\": ");
        w.put(r.verdict.deterministic ? "true" : "false");
        w.put(",    \"engagement_completed\": ");
        w.put(r.verdict.engagement_completed ? "true" : "false");
        w.put(",    \"roster_bounded\": ");
        w.put(r.verdict.roster_bounded ? "true" : "false");
        w.put(",    \"fight_alive\": ");
        w.put(r.verdict.fight_alive ? "true" : "false");
        // Diagnostics (first violation per gate; empty when green).
        w.put(",\n    \"engagement_failure\": ");
        write_string(w, r.verdict.engagement_failure);
        w.put(",\n    \"roster_leak\": ");
        write_string(w, r.verdict.roster_leak);
        w.put(",\n    \"fight_stall\": ");
        write_string(w, r.verdict.fight_stall);
        // The certificate: the two recorder MD5s.
        w.put(",\n    \"recorder_md5_run0\": ");
        write_string(w, r.verdict.recorder_md5_run0);
        w.put(",\n    \"recorder_md5_run1\": ");
        write_string(w, r.verdict.recorder_md5_run1);
        // The engagement window (-1 = the event never happened).
        w.put(",\n    ");
        w.number_key("first_detect_s", r.verdict.first_detect_s);
        w.put(",    ");
        w.number_key("first_wvr_engage_s", r.verdict.first_wvr_engage_s);
        w.put(",    ");
        w.number_key("last_wvr_engage_s", r.verdict.last_wvr_engage_s);
        w.put(",    ");
        w.number_key("first_wvr_disengage_s",
                     r.verdict.first_wvr_disengage_s);
        w.put(",    ");
        w.number_key("first_launch_s", r.verdict.first_launch_s);
        w.put(",    ");
        w.number_key("first_gun_s", r.verdict.first_gun_s);
        w.put(",    ");
        w.number_key("first_kill_s", r.verdict.first_kill_s);
        w.put(",    ");
        w.number_key("missile_shots", r.verdict.missile_shots);
        w.put(",    ");
        w.number_key("missile_hits", r.verdict.missile_hits);
        w.put(",    ");
        w.number_key("missile_misses", r.verdict.missile_misses);
        w.put(",    ");
        w.number_key("gun_bursts", r.verdict.gun_bursts);
        w.put(",    ");
        w.number_key("kills", r.verdict.kills);
        // The result artifact's path (for replay / md5sum).
        w.put(",\n    \"result_json\": ");
        write_string(w, result_path.string());
        w.put("\n  }");
        w.put("\n}\n");

        std::ofstream out(summary_path);
        out << w.str();
    }

    // 3. The diary — per-sample telemetry. Explicitly NOT byte-stable.
    const auto diary_path = args.out_dir / "wvr_merge_diary.json";
    {
        f4::json::Writer w;
        w.put("{\n  \"format\": \"f4-wvr-merge-diary\",\n  ");
        w.put("\"version\": 1,\n  ");
        w.put("\"note\": \"performance telemetry (wall-clock, ticks/sec, "
              "RSS) varies by host; the byte-stable artifacts are "
              "wvr_merge_result.json and wvr_merge_summary.json\",\n  ");
        w.number_key("samples", static_cast<std::int64_t>(r.diary.size()));
        w.put(",\n  \"rows\": [");
        bool first = true;
        for (const auto& s : r.diary) {
            w.put(first ? "\n    {" : ",\n    {");
            first = false;
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
            w.number_key("live_missiles", s.live_missiles);
            w.put(", ");
            w.number_key("tracks_acquired", s.tracks_acquired);
            w.put(", ");
            w.number_key("missiles_launched", s.missiles_launched);
            w.put(", ");
            w.number_key("gun_bursts", s.gun_bursts);
            w.put(", ");
            w.number_key("wvr_engagements", s.wvr_engagements);
            w.put(", ");
            w.number_key("damage_events", s.damage_events);
            w.put(", ");
            w.number_key("kills", s.kills);
            w.put(", ");
            w.number_key("sample_launches", s.sample_launches);
            w.put(", ");
            w.number_key("sample_gun_bursts", s.sample_gun_bursts);
            w.put(", ");
            w.number_key("sample_kills", s.sample_kills);
            // Telemetry: the columns that make this file NOT byte-stable.
            char buf[128];
            std::snprintf(buf, sizeof(buf),
                          ", \"wall_sec\": %.3f, \"ticks_per_sec\": %.1f, "
                          "\"rss_kb\": %ld",
                          s.wall_sec, s.ticks_per_sec, s.rss_kb);
            w.put(buf);
            w.put("\n    }");
        }
        w.put(r.diary.empty() ? "]" : "\n  ]");
        w.put("\n}\n");

        std::ofstream out(diary_path);
        out << w.str();
    }

    // -----------------------------------------------------------------------
    // The headline + the gates
    // -----------------------------------------------------------------------
    std::printf("wvr_merge: wall=%.1fs samples=%d tracks=%d band=%d "
                "guns=%d launches=%d kills=%d\n",
                wall.count(), r.samples, r.tracks_acquired,
                r.wvr_engagements, r.gun_bursts, r.missiles_launched,
                r.kills);
    std::printf("wvr_merge: deterministic=%s engagement=%s roster=%s "
                "alive=%s md5=%s\n",
                r.verdict.deterministic ? "yes" : "NO",
                r.verdict.engagement_completed ? "ok" : "FAIL",
                r.verdict.roster_bounded ? "ok" : "LEAK",
                r.verdict.fight_alive ? "ok" : "STALL",
                r.verdict.recorder_md5_run0.c_str());
    std::printf("wrote: %s\n", summary_path.string().c_str());
    std::printf("wrote: %s\n", result_path.string().c_str());
    std::printf("wrote: %s\n", diary_path.string().c_str());

    // -----------------------------------------------------------------------
    // Exit-code derivation (the acceptance surface). Priority: fight_alive
    // (3/4) → engagement_completed (5) → roster_bounded (6) →
    // deterministic (9) → 0.
    // -----------------------------------------------------------------------

    if (!r.verdict.fight_alive) {
        if (r.verdict.first_detect_s < 0.0) {
            std::fprintf(stderr,
                         "wvr_merge_qc: QC FAILURE — fight_alive violated, "
                         "no detection within horizon (exit 3, pre-engage "
                         "stall). %s Inspect the diary's tracks_acquired "
                         "column and the engagement window in "
                         "wvr_merge_summary.json.\n",
                         r.verdict.fight_stall.c_str());
            return 3;
        }
        std::fprintf(stderr,
                     "wvr_merge_qc: QC FAILURE — fight_alive violated, "
                     "detected but never entered the WVR band (exit 4). "
                     "%s Inspect the engagement window's "
                     "first_wvr_engage_s in wvr_merge_summary.json.\n",
                     r.verdict.fight_stall.c_str());
        return 4;
    }

    if (!r.verdict.engagement_completed) {
        std::fprintf(stderr,
                     "wvr_merge_qc: QC FAILURE — engagement_completed "
                     "violated (exit 5). %s Inspect the engagement "
                     "window's first_kill_s, first_gun_s and missile_* in "
                     "wvr_merge_summary.json.\n",
                     r.verdict.engagement_failure.c_str());
        return 5;
    }

    if (!r.verdict.roster_bounded) {
        std::fprintf(stderr,
                     "wvr_merge_qc: QC FAILURE — roster_bounded "
                     "violated, entity leak (exit 6). %s Inspect the "
                     "diary's live_entities vs initial + spawned − "
                     "retired in wvr_merge_diary.json.\n",
                     r.verdict.roster_leak.c_str());
        return 6;
    }

    if (hopts.runs >= 2 && !r.verdict.deterministic) {
        std::fprintf(stderr,
                     "wvr_merge_qc: QC FAILURE — NOT deterministic: "
                     "run 0 md5 %s != run 1 md5 %s (exit 9). Diff "
                     "wvr_merge_result.json against a re-run to find "
                     "the first diverging event.\n",
                     r.verdict.recorder_md5_run0.c_str(),
                     r.verdict.recorder_md5_run1.c_str());
        return 9;
    }

    return 0;
}
