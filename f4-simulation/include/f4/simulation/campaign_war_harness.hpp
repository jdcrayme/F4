// f4-simulation/include/f4/simulation/campaign_war_harness.hpp
//
// PUBLIC HEADER — C5, the 24-hour war: the long-horizon acceptance
// harness (see Docs/CAMPAIGN_LOOP_PLAN.md §5, "C5 — the 24-hour war
// (the acceptance)").
//
// WHAT THIS IS. The C1 ledger + the C2 one-pool tasking + the C3
// routed generation + the C4 ATM pipeline make a long war MEANINGFUL:
// every hour's packages, draws, losses, recoveries, and resupply
// reshape the next hour's force. This harness RUNS that war — both
// sides generate, fly, fight, attrite, recover, and resupply for
// hours of sim time — headless, deterministic, and instrumented, and
// then issues the acceptance verdicts:
//
//   * DETERMINISM   — the whole war runs TWICE (in-process, fresh
//                     sessions); the two campaign_result.json byte
//                     streams must be identical (compared as bytes,
//                     certified as MD5 — the number a human can
//                     re-derive with md5sum on the artifact).
//   * LEDGER DRIFT  — the one-pool accounting identities hold at
//                     every sample: per-team pool bounds, the
//                     tasking view ≤ the existence view, the team
//                     books agree with the squadron books, and the
//                     monotone counters never go backwards.
//   * ENTITY LEAK   — the roster identity holds at every sample:
//                     live == initial + spawned − retired. The wreck
//                     reaper (CampaignSessionOptions::wreck_hold_sec,
//                     armed by this harness) is what keeps the right
//                     side bounded over a 24-hour churn of ~10^5
//                     lifecycle events.
//   * WAR ALIVE     — the campaign clock advances the whole horizon
//                     (the C5-FIX-1 class), tasking cycles fire every
//                     sample, and every belligerent that has EVER drawn
//                     this war keeps drawing hour after hour while its
//                     pool is taskable — a side that flew and then went
//                     silent with aircraft available is a stalled war,
//                     not a quiet one (a side that never drew from t0 is
//                     a fixture-data matter — role mix — visible in the
//                     diary's per-team rows, not gated).
//
// WHAT RUNS. The harness composes CampaignSession — the same V-CAMP
// object the world viewer drives frame by frame — so the acceptance
// run exercises EXACTLY the interactive loop (one world, one clock,
// the spawner feeding the sim's own roster). The session's advance()
// is called in 4-sim-second batches (the 240-tick per-advance cap,
// fully drained — no debt dropped), which is byte-equivalent to any
// other split of the same ticks (the C2 pin: one big tick == N small
// ones).
//
// ARTIFACTS. The harness produces three, all written by the HOST
// (campaign_qc --war): campaign_result.json (run 0's ledger — the
// byte-stable certificate), a summary "war" block (deterministic
// content ONLY — verdicts, counters, MD5; no wall-clock, no RSS, no
// ticks/sec), and the war DIARY (one row per sample with the
// performance telemetry — wall-clock, ticks/sec, RSS — which is
// explicitly NOT byte-stable and lives in its own file for exactly
// that reason).
//
// Determinism: no RNG, no host clocks in the loop's decisions; the
// only wall-clock reads (chrono + RSS) feed the diary's telemetry
// columns, never a verdict. C++20.

#pragma once

#include <f4/simulation/campaign_session.hpp>

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace f4::simulation {

/// Harness tuning. The session options carry the war's SHAPE (cycle
/// period, reinforcement cadence, ATM pipeline, filters, wreck hold);
/// these carry the RUN (how long, how many passes, how loud).
struct WarHarnessOptions {
    /// The session the war runs over (world JSON, class table, config,
    /// profiles, cycle period, reinforcement, ATM, filters...).
    /// wreck_hold_sec is FORCED to the harness's own wreck_hold below
    /// (the harness without a reaper is a memory-leak acceptance run
    /// — the exact failure class C5 exists to catch, not to ignore).
    CampaignSessionOptions session;

    /// Sim-seconds of war. The plan's acceptance horizon is 86400
    /// (24 h); smoke runs use minutes (0.1 h = 360 s).
    std::int64_t horizon_sec = 86400;

    /// Diary + check cadence: one sample this many sim-seconds. The
    /// verdicts (drift/leak/alive) evaluate at every sample.
    double sample_sec = 3600.0;

    /// How many times to run the whole war. 2 (default) is the C5
    /// contract — the second pass exists only to re-derive the ledger
    /// for the determinism proof. 1 skips the proof (wall-clock
    /// constrained hosts; the determinism verdict is then vacuously
    /// true and the summary says so via runs==1).
    int runs = 2;

    /// The wreck hold armed into the session (see
    /// CampaignSessionOptions::wreck_hold_sec — the harness overrides
    /// whatever the caller put in session.wreck_hold_sec; 300 s is
    /// enough for a viewer to watch the fall, short enough that a
    /// 24-hour war's roster stays bounded).
    double wreck_hold_sec = 300.0;

    /// Total wall-clock watchdog across ALL runs (0 = off). When the
    /// war exceeds it the harness ABORTS (report.aborted — a harness
    /// error, not a war verdict; the host exits 1 on it). A hung or
    /// spiraling war is a failure, but a DIFFERENT failure class than
    /// the four verdicts, and it must not masquerade as one.
    double max_wall_sec_total = 0.0;
};

/// One row of the war diary — the world's state at one sample. All
/// campaign/sim counters are CUMULATIVE except where marked; the
/// hour_* deltas are the sample's activity pulse. wall_sec /
/// ticks_per_sec / rss_kb are TELEMETRY (not byte-stable across runs;
/// they never feed a verdict).
struct WarHourSample {
    int sample = 0;                 ///< 1-based (hour 1 = first sample_sec)
    std::int64_t campaign_time = 0; ///< absolute (save epoch + clock)
    double sim_time_s = 0.0;

    // --- campaign side (cumulative) -----------------------------------
    int cycles = 0;                 ///< tasking cycles fired
    int intents = 0;                ///< missions generated
    int packages = 0;               ///< ATM packages built
    int escorts = 0;                ///< ATM support flights paired
    int routes_built = 0;
    int routes_failed = 0;
    int drawn = 0;                  ///< ledger: aircraft drawn (monotone)
    int air_losses = 0;             ///< ledger: air losses
    int recovered = 0;              ///< ledger: aircraft recovered
    int reinforced = 0;             ///< ledger: aircraft delivered
    int reinforce_fires = 0;        ///< ledger: cadence fires

    // --- sim side ------------------------------------------------------
    int synthetic_spawned = 0;      ///< cumulative generated flights
    int live_aircraft = 0;          ///< roster size now
    int airborne = 0;
    int retired = 0;                ///< wrecks reaped (cumulative)
    int world_entities = 0;         ///< EntityWorld::size() now

    // --- this sample's pulse -------------------------------------------
    int hour_spawns = 0;
    int hour_draws = 0;             ///< aircraft drawn this sample
    int hour_cycles = 0;            ///< cycles fired this sample

    // --- performance telemetry (diary only; NEVER a verdict) -----------
    double wall_sec = 0.0;          ///< wall-clock spent on this sample
    double ticks_per_sec = 0.0;
    long rss_kb = 0;                ///< resident set (0 where unsupported)

    /// One team's pool trajectory at this sample (the believable
    /// depletion/refill picture: existence, tasking, and the flows).
    struct TeamPool {
        int slot = 0;
        std::string name;
        int initial = 0;            ///< te_number_aircraft at snapshot
        int remaining = 0;          ///< existence view (deaths/resupply)
        int tasking = 0;            ///< remaining − outstanding draws
        int drawn = 0;              ///< outstanding draws (non-monotone)
        int drawn_total = 0;        ///< every draw ever (monotone, log)
        int losses = 0;
        int reinforced = 0;
        int recovered = 0;
    };
    std::vector<TeamPool> teams;
};

/// The four C5 gates + the inherited tasking classes, with the first
/// violation of each recorded as a diagnostic string ("" when green).
struct WarVerdict {
    // -- inherited tasking classes (the QC's exits 6/7/8, war edition) --
    bool drew_aircraft = false;     ///< the ladder drew >= 1 aircraft
    bool routes_built = false;      ///< >= 1 route (planner attached)
    bool materialized = false;      ///< >= 1 synthetic aircraft spawned
    bool packages_built = false;    ///< >= 1 ATM package (atm-armed only)

    // -- the C5 gates proper ---------------------------------------------
    bool deterministic = true;      ///< ledger bytes identical across runs
    bool ledger_consistent = true;  ///< one-pool identities every sample
    bool entities_bounded = true;   ///< roster identity every sample
    bool war_alive = true;          ///< clock + cycles + belligerent draws

    // -- diagnostics (first violation per gate; empty when green) --------
    std::string ledger_drift;
    std::string entity_leak;
    std::string war_stall;

    // -- the certificate --------------------------------------------------
    std::string ledger_md5_run0;    ///< 32 lowercase hex ("" on abort)
    std::string ledger_md5_run1;    ///< "" when runs == 1
};

/// Everything the host needs after execute(): the verdicts, run 0's
/// diary, run 0's ledger JSON (the campaign_result.json bytes), the
/// final headline counters, and the abort state (watchdog / harness
/// error — never a war verdict).
struct WarReport {
    WarVerdict verdict;
    std::vector<WarHourSample> diary;   ///< run 0's samples
    std::string ledger_json;            ///< run 0's (byte-stable)

    // Final cumulative counters (run 0, end of horizon).
    int cycles = 0;
    int intents = 0;
    int packages = 0;
    int escorts = 0;
    int routes_built = 0;
    int routes_failed = 0;
    int drawn = 0;
    int air_losses = 0;
    int recovered = 0;
    int reinforced = 0;
    int reinforce_fires = 0;
    int synthetic_spawned = 0;
    int retired = 0;
    int live_aircraft = 0;
    int airborne = 0;
    int samples = 0;

    // Preconditions the inherited gates key on (echoed for the host's
    // exit-code decisions — the QC's tasking_had_air, war edition).
    bool belligerent_air = false;   ///< a belligerent squadron had aircraft
    bool atm_armed = false;         ///< session ran the ATM pipeline

    /// The ledger's final per-team pool rows (the summary's teams
    /// block — the believable depletion/refill picture as numbers).
    std::vector<WarHourSample::TeamPool> ledger_teams;

    bool aborted = false;
    std::string abort_reason;
};

/// The 24-hour war harness. create() validates nothing but the
/// options' shape; execute() builds fresh CampaignSessions, runs the
/// horizon, samples the diary, checks the identities, and derives the
/// verdicts. One execute() per harness; a second call re-runs the
/// whole war from scratch (fresh report).
class CampaignWarHarness {
public:
    /// Called once per diary sample during run 0 (run 1+ are silent —
    /// their only job is the ledger bytes). Runs on the harness
    /// thread, between advance() batches; the sample is a copy.
    using ProgressFn = std::function<void(const WarHourSample&)>;

    [[nodiscard]] static std::unique_ptr<CampaignWarHarness>
    create(const WarHarnessOptions& opts, std::string* error = nullptr);

    ~CampaignWarHarness();

    CampaignWarHarness(const CampaignWarHarness&) = delete;
    CampaignWarHarness& operator=(const CampaignWarHarness&) = delete;

    /// Run the war (opts.runs passes) and derive the verdicts. Safe
    /// to call once; returns the report (also report()).
    const WarReport& execute(ProgressFn on_sample = nullptr);

    [[nodiscard]] const WarReport& report() const noexcept {
        return report_;
    }

private:
    CampaignWarHarness() = default;

    /// One full pass (run index 0..runs-1). Fills report_ fields the
    /// pass owns: run 0 the diary + checks + ledger json; every run
    /// its ledger MD5.
    void run_pass_(int run, const ProgressFn& on_sample);

    /// The sample walk inside run_pass_: collect, check, diary, notify.
    void sample_(int run);

    /// The three per-sample gates (run 0 only): ledger drift, entity
    /// leak, war alive. `books` carries the per-team squadron-side
    /// sums + unmatched-flow counts the drift equalities are guarded on.
    struct TeamBooks {
        int slot = 0;
        int run_draws = 0;
        int run_losses = 0;
        int run_reinforced = 0;
        int run_recoveries = 0;
        int unmatched_draws = 0;    ///< draws booked team-side only
        int unmatched_recov = 0;    ///< recoveries ditto
        int unresolved_losses = 0;  ///< air losses with no squadron
    };
    void check_sample_(const WarHourSample& s,
                       const std::vector<TeamBooks>& books);

    /// Final verdict derivation (after the last pass).
    void finalize_();

    WarHarnessOptions opts_;
    WarReport report_;

    // --- per-pass state (run_pass_ / sample_ only) ----------------------
    CampaignSession* session_ = nullptr;   ///< the pass's live session
    double pass_t0_ = 0.0;                 ///< sim time at pass start
    int pass_roster0_ = 0;                 ///< roster size at pass start
    std::vector<int> pass_belligerents_;   ///< slots at war
    std::vector<int> pass_expected_;       ///< belligerents that owe draws
    int pass_samples_ = 0;                 ///< samples collected this pass
    double pass_next_sample_t_ = 0.0;      ///< next sample's sim time
    std::chrono::steady_clock::time_point pass_sample_wall_{};
    // Previous sample's per-team monotone state (the drift checks).
    struct PrevTeam {
        int slot = 0;
        int losses = 0;
        int reinforced = 0;
        int recovered = 0;
        int drawn_total = 0;
        int tasking = 0;
    };
    std::vector<PrevTeam> pass_prev_teams_;
    WarHourSample pass_prev_{};            // for hour_* deltas
    bool pass_first_sample_ = true;
};

} // namespace f4::simulation
