// f4-simulation/tools/bvr_intercept_qc.cpp
//
// bvr_intercept_qc — the M4 end-to-end BVR intercept acceptance tool
// (see Docs/COMBAT_CHAIN_M4_PLAN.md). The combat-chain sibling of
// campaign_qc's --war mode: where campaign_qc --war certifies the
// CAMPAIGN loop (the 24-hour war — ledger, tasking, resupply, the ground
// war), bvr_intercept_qc certifies the COMBAT chain at scenario scale
// (detection → employment → launch → crank → defeat → kill).
//
// One command runs a BVR intercept scenario headless, in-process, through
// BvrInterceptHarness (f4-simulation — the same Simulation the scenario
// player drives, composed directly without a CampaignSession). The harness
// runs the fight TWICE (fresh Simulation per pass) and certifies four
// verdicts:
//
//   * DETERMINISTIC            — the recorder's to_json() byte stream is
//                                 identical across the two passes (certified
//                                 as MD5 — the number a human can re-derive
//                                 with md5sum on bvr_intercept_result.json).
//   * ENGAGEMENT_COMPLETED     — at least one EntityKilled within the
//                                 horizon, with correct attribution.
//   * ROSTER_BOUNDED           — live == initial + spawned − retired at
//                                 every sample (no entity leak).
//   * FIGHT_ALIVE              — the shooter's brain reached Entering AND
//                                 the bus carried at least one detect.
//
// ARTIFACTS (three, mirroring campaign_qc --war's pattern):
//   1. bvr_intercept_result.json  — run 0's recorder JSON verbatim (the
//      byte-stable certificate — what a human md5sums). Written as raw
//      bytes; the harness owns the recorder interaction (f4-recorder is
//      NOT a direct link of this tool — it comes transitively through
//      f4-simulation).
//   2. bvr_intercept_summary.json — DETERMINISTIC CONTENT ONLY: verdicts,
//      counters, MD5s, the engagement window, precondition flags. Written
//      with f4::json::Writer (the same writer campaign_qc uses). NO
//      wall-clock, NO RSS, NO ticks/sec — those are host-dependent and
//      live in the diary.
//   3. bvr_intercept_diary.json   — per-sample telemetry (the diary vector
//      with wall_sec / ticks_per_sec / rss_kb). This file is explicitly
//      NOT byte-stable — that's why it's a separate artifact.
//
// Exit code (the acceptance surface — see COMBAT_CHAIN_M4_PLAN.md §4):
//   0  all four verdicts green (deterministic, engagement_completed,
//      roster_bounded, fight_alive).
//   1  usage / IO error / scenario-load failure / harness abort (watchdog,
//      load failure — a DIFFERENT failure class than the four verdicts;
//      the harness's abort_reason names it).
//   2  scenario has combat.enabled == false (the harness refuses to run a
//      non-combat scenario — silent success would be the worst failure
//      class; the tool surfaces the harness's abort as this dedicated
//      code so a CI can distinguish "wrong scenario" from "broken fight").
//   3  fight_alive violated — no detection within horizon (pre-engage
//      stall: the shooter's radar never acquired, the brain never reached
//      Entering). first_detect_s < 0 is the discriminator; the verdict's
//      fight_stall names the rung.
//   4  fight_alive violated — detected but never engaged (the brain
//      detected but never fired: MAR/Pk gate mis-tuned, tactic selection
//      broken, state machine stuck). first_detect_s >= 0 but the chain
//      stalled past detection; the verdict's fight_stall names the rung.
//   5  engagement_completed violated — no kill within horizon, or kill
//      unattributed. The verdict's engagement_failure names the rung.
//   6  roster_bounded violated — entity leak (live != initial + spawned
//      − retired at some sample). The verdict's roster_leak names the
//      sample.
//   9  deterministic violated — run 1's recorder bytes differ from run
//      0's. The two MD5s in the summary name the divergence; diff
//      bvr_intercept_result.json against a re-run to find the first
//      diverging event. Skipped when --runs 1 (the proof is opt-in).
//
// The exit-code priority follows the table above (numeric): fight_alive
// (3/4) → engagement_completed (5) → roster_bounded (6) → deterministic
// (9) → 0. Each gate names its rung via the verdict's diagnostic string,
// surfaced to stderr AND the summary's diagnostics block, so a CI log and
// a saved artifact tell the same story.
//
// Usage:
//   bvr_intercept_qc <scenario.json> [options]
//     --horizon-sec <N>     sim-seconds of fight (default 300 — 5 min at
//                            60 Hz, well past the AMRAAM's TOF)
//     --sample-sec <N>      diary sample cadence (default 30)
//     --runs <N>            determinism passes (default 2; 1 skips the
//                            proof — deterministic is then vacuously true
//                            and recorder_md5_run1 is empty)
//     --out-dir <path>      artifact directory (default: scenario's parent)
//     --max-wall <sec>      wall-clock watchdog across all runs (default 0
//                            = off; a hung fight aborts as exit 1)
//     --quiet               suppress per-sample progress (stderr)
//     --help                show this message

#include <f4/simulation/bvr_intercept_harness.hpp>
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

    // --help can appear anywhere; handle it before the positional so
    // `bvr_intercept_qc --help` doesn't get mis-parsed as "scenario is
    // --help".
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
// JSON Writer helpers (f4::json::Writer has raw put/number/string_key;
// the string-key path needs a manual escape+quote, mirroring campaign_qc).
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
// main — thin layer: parse args, BvrInterceptHarness::create, execute,
// write artifacts, derive exit code, return. (Mirrors campaign_qc's
// run_war() shape, inlined to main because bvr_intercept_qc has one mode
// — campaign_qc factors run_war out because --war is one of several.)
// ===========================================================================
int main(int argc, char** argv) {
    const Args args = parse_args(argc, argv);

    if (!std::filesystem::exists(args.scenario_json)) {
        std::fprintf(stderr, "bvr_intercept_qc: scenario JSON not found: %s\n",
                     args.scenario_json.string().c_str());
        return 1;
    }

    f4::simulation::InterceptHarnessOptions hopts;
    hopts.scenario_json = std::filesystem::absolute(args.scenario_json);
    hopts.horizon_sec = args.horizon_sec;
    hopts.sample_sec = args.sample_sec;
    hopts.runs = args.runs;
    hopts.max_wall_sec_total = args.max_wall_sec;

    std::printf("bvr_intercept: scenario=%s horizon=%llds sample=%.0fs "
                "runs=%d max_wall=%.0fs\n",
                args.scenario_json.string().c_str(),
                (long long)hopts.horizon_sec, hopts.sample_sec, hopts.runs,
                hopts.max_wall_sec_total);

    std::string err;
    auto harness = f4::simulation::BvrInterceptHarness::create(hopts, &err);
    if (harness == nullptr) {
        std::fprintf(stderr, "bvr_intercept_qc: %s\n", err.c_str());
        return 1;
    }

    // Progress: one line per sample to stderr (sample N, sim_time,
    // live_missiles, kills). The harness only invokes the callback during
    // run 0 (run 1+ are silent — their only job is the recorder bytes).
    // --quiet suppresses the line entirely.
    const auto t_wall = std::chrono::steady_clock::now();
    const bool quiet = args.quiet;
    harness->execute([quiet](const f4::simulation::InterceptSample& s) {
        if (quiet) return;
        std::fprintf(stderr,
                     "bvr[s%02d] t=%.1fs missiles=%d kills=%d\n",
                     s.sample, s.sim_time_s, s.live_missiles, s.kills);
        std::fflush(stderr);
    });
    const std::chrono::duration<double> wall =
        std::chrono::steady_clock::now() - t_wall;
    const auto& r = harness->report();

    // -----------------------------------------------------------------------
    // Abort: the harness refused (combat disabled), hit the watchdog, or the
    // scenario failed to load. These are NOT fight verdicts — they're harness
    // errors. Exit 1, except combat-disabled which is exit 2 (the scenario is
    // structurally wrong for the harness, not a runtime failure). No
    // artifacts on abort — run 0's recorder is empty/incomplete, and writing
    // a half-formed certificate would be worse than writing none.
    // -----------------------------------------------------------------------
    if (r.aborted) {
        // The harness's combat-disabled abort reason is a stable prefix
        // (bvr_intercept_harness.cpp sets it verbatim: "scenario
        // combat.enabled is false (run N) — ..."). A load-failure abort
        // has a different prefix ("scenario load failed (run N): ...");
        // both surface as exit 1 vs 2 here. The prefix match is the
        // contract — the harness owns the wording, the tool matches it.
        const std::string combat_disabled_prefix =
            "scenario combat.enabled is false";
        if (r.abort_reason.rfind(combat_disabled_prefix, 0) == 0) {
            std::fprintf(stderr,
                         "bvr_intercept_qc: scenario has combat.enabled == "
                         "false — the harness refuses to run a non-combat "
                         "scenario (exit 2). %s\n",
                         r.abort_reason.c_str());
            return 2;
        }
        std::fprintf(stderr, "bvr_intercept_qc: harness ABORTED — %s\n",
                     r.abort_reason.c_str());
        return 1;
    }

    // -----------------------------------------------------------------------
    // Artifacts (the three-file pattern, mirroring campaign_qc --war).
    // -----------------------------------------------------------------------
    std::filesystem::create_directories(args.out_dir);

    // 1. The certificate — run 0's recorder JSON, byte-stable. The harness
    //    owns the recorder; the tool just writes the bytes it produced.
    const auto result_path = args.out_dir / "bvr_intercept_result.json";
    {
        std::ofstream out(result_path);
        out << r.recorder_json;
    }

    // 2. The summary — DETERMINISTIC CONTENT ONLY (verdicts, counters,
    //    MD5s, the engagement window, precondition flags). Written with
    //    f4::json::Writer — the same writer campaign_qc uses. NO
    //    wall-clock, NO RSS, NO ticks/sec (those live in the diary).
    const auto summary_path = args.out_dir / "bvr_intercept_summary.json";
    {
        f4::json::Writer w;
        w.put("{\n  \"format\": \"f4-bvr-intercept-summary\",\n  ");
        w.put("\"version\": 1,\n  \"scenario_json\": ");
        write_string(w, args.scenario_json.string());
        // The intercept block: DETERMINISTIC CONTENT ONLY (the byte-stable
        // certificate — wall-clock, ticks/sec, and RSS live in the diary,
        // which is explicitly not byte-stable).
        w.put(",\n  \"intercept\": {\n    ");
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
        // The certificate: the two recorder MD5s (run 1's is empty when
        // runs == 1 — the determinism proof was skipped).
        w.put(",\n    \"recorder_md5_run0\": ");
        write_string(w, r.verdict.recorder_md5_run0);
        w.put(",\n    \"recorder_md5_run1\": ");
        write_string(w, r.verdict.recorder_md5_run1);
        // The engagement window (from the verdict — derived from run 0's
        // combat events at finalize_). -1 means the event never happened
        // within the horizon.
        w.put(",\n    ");
        w.number_key("first_detect_s", r.verdict.first_detect_s);
        w.put(",    ");
        w.number_key("first_launch_s", r.verdict.first_launch_s);
        w.put(",    ");
        w.number_key("first_kill_s", r.verdict.first_kill_s);
        w.put(",    ");
        w.number_key("shots_fired", r.verdict.shots_fired);
        w.put(",    ");
        w.number_key("shots_hit", r.verdict.shots_hit);
        w.put(",    ");
        w.number_key("shots_missed", r.verdict.shots_missed);
        // The result artifact's path (for replay in the viewer / md5sum).
        w.put(",\n    \"result_json\": ");
        write_string(w, result_path.string());
        w.put("\n  }");
        w.put("\n}\n");

        std::ofstream out(summary_path);
        out << w.str();
    }

    // 3. The diary — per-sample telemetry (wall_sec / ticks_per_sec /
    //    rss_kb). Explicitly NOT byte-stable; that's why it's a separate
    //    file. The format mirrors campaign_qc's campaign_war_diary.json.
    const auto diary_path = args.out_dir / "bvr_intercept_diary.json";
    {
        f4::json::Writer w;
        w.put("{\n  \"format\": \"f4-bvr-intercept-diary\",\n  ");
        w.put("\"version\": 1,\n  ");
        w.put("\"note\": \"performance telemetry (wall-clock, ticks/sec, "
              "RSS) varies by host; the byte-stable artifacts are "
              "bvr_intercept_result.json and bvr_intercept_summary.json\",\n  ");
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
            w.number_key("tracks_dropped", s.tracks_dropped);
            w.put(", ");
            w.number_key("rwr_locks", s.rwr_locks);
            w.put(", ");
            w.number_key("rwr_launches", s.rwr_launches);
            w.put(", ");
            w.number_key("missiles_launched", s.missiles_launched);
            w.put(", ");
            w.number_key("missiles_detonated", s.missiles_detonated);
            w.put(", ");
            w.number_key("damage_events", s.damage_events);
            w.put(", ");
            w.number_key("kills", s.kills);
            w.put(", ");
            w.number_key("sample_launches", s.sample_launches);
            w.put(", ");
            w.number_key("sample_detonations", s.sample_detonations);
            w.put(", ");
            w.number_key("sample_kills", s.sample_kills);
            // Telemetry: wall-clock, ticks/sec, RSS — the columns that
            // make this file NOT byte-stable (hence its own artifact).
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
    std::printf("bvr_intercept: wall=%.1fs samples=%d tracks=%d launches=%d "
                "detonations=%d kills=%d\n",
                wall.count(), r.samples, r.tracks_acquired,
                r.missiles_launched, r.missiles_detonated, r.kills);
    std::printf("bvr_intercept: deterministic=%s engagement=%s roster=%s "
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
    // Exit-code derivation (the acceptance surface). Priority follows the
    // task's exit-code table (numeric): fight_alive (3/4) →
    // engagement_completed (5) → roster_bounded (6) → deterministic (9) →
    // 0. Each gate names its rung via the verdict's diagnostic string,
    // surfaced to stderr AND the summary's diagnostics block. The
    // verdicts describe run 0 (the diary + engagement window are run 0's);
    // the determinism gate is the cross-run seal (it fires last, only when
    // run 0 was otherwise clean but run 1 disagreed).
    // -----------------------------------------------------------------------

    // fight_alive: the pre-engage vs post-detect stall. The discriminator
    // is first_detect_s — no detection within horizon is exit 3 (the
    // shooter's radar never acquired); detection happened but the brain
    // never engaged is exit 4 (MAR/Pk gate, tactic selection, state
    // machine — report.verdict.fight_stall names the rung).
    if (!r.verdict.fight_alive) {
        if (r.verdict.first_detect_s < 0.0) {
            std::fprintf(stderr,
                         "bvr_intercept_qc: QC FAILURE — fight_alive "
                         "violated, no detection within horizon (exit 3, "
                         "pre-engage stall). %s Inspect the diary's "
                         "tracks_acquired column and the engagement window "
                         "in bvr_intercept_summary.json.\n",
                         r.verdict.fight_stall.c_str());
            return 3;
        }
        std::fprintf(stderr,
                     "bvr_intercept_qc: QC FAILURE — fight_alive violated, "
                     "detected but never engaged (exit 4). %s Inspect the "
                     "engagement window's first_launch_s in "
                     "bvr_intercept_summary.json.\n",
                     r.verdict.fight_stall.c_str());
        return 4;
    }

    // engagement_completed: no kill within horizon, or kill unattributed
    // (subject_id != victim / object_id != killer). The verdict's
    // engagement_failure names the rung.
    if (!r.verdict.engagement_completed) {
        std::fprintf(stderr,
                     "bvr_intercept_qc: QC FAILURE — engagement_completed "
                     "violated (exit 5). %s Inspect the engagement window's "
                     "first_kill_s and shots_* in "
                     "bvr_intercept_summary.json.\n",
                     r.verdict.engagement_failure.c_str());
        return 5;
    }

    // roster_bounded: entity leak (live != initial + spawned − retired at
    // some sample). sweep_spent_missiles's removals must be observed as
    // retirements; a MissileSimComponent leak across the sample boundary
    // is the class this catches. The verdict's roster_leak names the
    // sample.
    if (!r.verdict.roster_bounded) {
        std::fprintf(stderr,
                     "bvr_intercept_qc: QC FAILURE — roster_bounded "
                     "violated, entity leak (exit 6). %s Inspect the "
                     "diary's live_entities vs initial + spawned − retired "
                     "in bvr_intercept_diary.json.\n",
                     r.verdict.roster_leak.c_str());
        return 6;
    }

    // deterministic: run 1's recorder bytes differ from run 0's. Skipped
    // when runs == 1 (the proof is opt-in; deterministic is vacuously
    // true and the summary says so via recorder_md5_run1 == ""). The two
    // MD5s name the divergence; diff bvr_intercept_result.json against a
    // re-run to find the first diverging event.
    if (hopts.runs >= 2 && !r.verdict.deterministic) {
        std::fprintf(stderr,
                     "bvr_intercept_qc: QC FAILURE — NOT deterministic: "
                     "run 0 md5 %s != run 1 md5 %s (exit 9). Diff "
                     "bvr_intercept_result.json against a re-run to find "
                     "the first diverging event.\n",
                     r.verdict.recorder_md5_run0.c_str(),
                     r.verdict.recorder_md5_run1.c_str());
        return 9;
    }

    return 0;
}
