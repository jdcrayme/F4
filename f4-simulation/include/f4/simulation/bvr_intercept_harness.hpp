// f4-simulation/include/f4/simulation/bvr_intercept_harness.hpp
//
// PUBLIC HEADER — M4, the end-to-end BVR intercept acceptance harness
// (see Docs/COMBAT_CHAIN_M4_PLAN.md). The combat-chain sibling of
// CampaignWarHarness (C5, the 24-hour war — see campaign_war_harness.hpp):
// where C5 certifies the CAMPAIGN loop (the ledger, the tasking, the
// resupply, the ground war), M4 certifies the COMBAT chain at scenario
// scale (detection → employment → launch → crank → defeat → kill).
//
// WHAT THIS IS. M1 (f4-weapons) + M2 (f4-sensors) + M3 (f4-ai combat
// modules) make a fight MEANINGFUL: the brain detects via the radar,
// classifies via the RadarBackedDetectionPolicy, employs via BVRModule,
// fires via MissileModule, defends via MissileModule's defeat role, and
// kills via the missile flyout + apply_damage chain. Every link is
// unit-tested; what's missing is the END-TO-END artifact with a verdict.
// This harness RUNS that fight — headless, deterministic, instrumented —
// and issues the acceptance verdicts:
//
//   * DETERMINISM            — the fight runs TWICE in-process (fresh
//                              Simulation per pass); the two
//                              FlightRecorder::to_json() byte streams
//                              must be identical (compared as bytes,
//                              certified as MD5 — the number a human
//                              can re-derive with md5sum on the
//                              bvr_intercept_result.json artifact).
//   * ENGAGEMENT_COMPLETED   — at least one EntityKilled event within
//                              the horizon, with correct attribution
//                              (subject_id == victim, object_id ==
//                              killer). The M4 contract: a fight that
//                              runs to a kill, or fails with a named
//                              failure class.
//   * ROSTER_BOUNDED         — the roster identity holds at every
//                              sample: live_entities == initial +
//                              spawned − retired. Missiles count;
//                              sweep_spent_missiles's removals are
//                              observed as retirements. Catches the
//                              MissileSimComponent leak class that
//                              the per-tick sweep would otherwise mask
//                              across the harness's sample boundary.
//   * FIGHT_ALIVE            — the shooter's brain reached at least
//                              BVRState::Entering AND the bus carried
//                              at least one RadarTrackAcquiredMessage.
//                              A scenario where the AI never detects
//                              is a stalled fight, not a quiet one.
//                              A scenario that detects but never
//                              fires (MAR/Pk gate mis-tuned) is a
//                              stalled fight at a different rung —
//                              the engagement_summary block names
//                              which rung (no first_launch_s).
//
// WHAT RUNS. The harness composes Simulation directly — the same object
// the scenario player and the campaign session drive. There is no
// CampaignSession at scenario-list scale (no campaign, no ledger, no
// tasking ladder). The harness calls sim.tick(scenario.sim_dt) in
// 4-sim-second batches (the same 240-tick drain discipline C5 uses —
// byte-equivalent to any other split of the same ticks, pinned by the
// C2 contract).
//
// ARTIFACTS. The harness produces three, all written by the HOST
// (bvr_intercept_qc): bvr_intercept_result.json (run 0's recorder JSON
// — the byte-stable certificate), bvr_intercept_summary.json (verdicts
// + counters + MD5 + the engagement_summary block — deterministic
// content ONLY; no wall-clock, no RSS, no ticks/sec), and the diary
// (bvr_intercept_diary.json — per-sample telemetry, explicitly NOT
// byte-stable, lives in its own file for exactly that reason).
//
// Determinism: no RNG in the harness path (the combat RNG is the
// scenario's radar_rng_seed, baked into RadarSimComponent at attach
// time); no host clocks in the loop's decisions. The only wall-clock
// reads (chrono + RSS) feed the diary's telemetry columns, never a
// verdict. C++20.

#pragma once

#include <f4/simulation/scenario.hpp>

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace f4::simulation {

class Simulation;   // forward — the .cpp includes simulation.hpp; the
                    // header keeps the include surface minimal (only
                    // <filesystem> + std). The harness holds a bare
                    // pointer to the pass's live Simulation.

/// Harness tuning. The scenario carries the fight's SHAPE (aircraft,
/// teams, spawn positions, combat config, radar_rng_seed, hit_points,
/// ROE flags, sim_dt, record path); these carry the RUN (how long, how
/// many passes, how loud).
struct InterceptHarnessOptions {
    /// The scenario JSON to run. combat.enabled MUST be true (create()
    /// refuses non-combat scenarios — silent success on a non-combat
    /// scenario would be the worst failure class). Loaded via
    /// f4::simulation::load_scenario(path) — the same path the scenario
    /// player and the test suite consume.
    std::filesystem::path scenario_json;

    /// The asset directory for the scenario (the JSON's parent path,
    /// passed to Simulation's constructor — @asset: refs resolve
    /// against this). Defaults to scenario_json.parent_path() when
    /// empty.
    std::filesystem::path asset_dir;

    /// Sim-seconds of fight. The plan's acceptance horizon is 300
    /// (5 min at 60 Hz — well past the AMRAAM's expected
    /// time-of-flight). Smoke runs use 60 s.
    std::int64_t horizon_sec = 300;

    /// Diary + check cadence: one sample this many sim-seconds. The
    /// verdicts (roster_bounded, fight_alive) evaluate at every
    /// sample; engagement_completed and deterministic evaluate at the
    /// end of the run.
    double sample_sec = 30.0;

    /// How many times to run the whole fight. 2 (default) is the M4
    /// contract — the second pass exists only to re-derive the
    /// recorder bytes for the determinism proof. 1 skips the proof
    /// (wall-clock constrained hosts; the determinism verdict is then
    /// vacuously true and the summary says so via runs==1).
    int runs = 2;

    /// Total wall-clock watchdog across ALL runs (0 = off). When the
    /// fight exceeds it the harness ABORTS (report.aborted — a harness
    /// error, not a fight verdict; the host exits 1 on it). A hung or
    /// spiraling fight is a failure, but a DIFFERENT failure class
    /// than the four verdicts, and it must not masquerade as one.
    double max_wall_sec_total = 0.0;
};

/// One row of the fight diary — the world's state at one sample. All
/// counters are CUMULATIVE except where marked; the sample_*_delta
/// fields are the sample's activity pulse. wall_sec / ticks_per_sec /
/// rss_kb are TELEMETRY (not byte-stable across runs; they never feed
/// a verdict).
struct InterceptSample {
    int sample = 0;                 ///< 1-based
    double sim_time_s = 0.0;

    // --- roster identity (the roster_bounded gate) -------------------
    int initial_entities = 0;       ///< EntityWorld::size() at pass start
    int spawned_entities = 0;       ///< cumulative entities created mid-fight
    int retired_entities = 0;       ///< cumulative entities destroyed mid-fight
    int live_entities = 0;          ///< EntityWorld::size() now
    int live_missiles = 0;          ///< weapons::count_live_missiles(world)

    // --- combat chain progress (the fight_alive + engagement gates) --
    int tracks_acquired = 0;        ///< cumulative RadarTrackAcquired events
    int tracks_dropped = 0;         ///< cumulative RadarTrackDropped events
    int rwr_locks = 0;              ///< cumulative RwrLock events
    int rwr_launches = 0;           ///< cumulative RwrLaunch events
    int missiles_launched = 0;      ///< cumulative MissileLaunched events
    int missiles_detonated = 0;     ///< cumulative MissileDetonated events
    int damage_events = 0;          ///< cumulative DamageApplied events
    int kills = 0;                  ///< cumulative EntityKilled events

    // --- this sample's pulse -----------------------------------------
    int sample_launches = 0;        ///< missiles launched this sample
    int sample_detonations = 0;     ///< missiles detonated this sample
    int sample_kills = 0;           ///< kills this sample

    // --- performance telemetry (diary only; NEVER a verdict) -----------
    double wall_sec = 0.0;          ///< wall-clock spent on this sample
    double ticks_per_sec = 0.0;
    long rss_kb = 0;                ///< resident set (0 where unsupported)
};

/// The four M4 gates, with the first violation of each recorded as a
/// diagnostic string ("" when green).
struct InterceptVerdict {
    // -- the M4 gates proper --------------------------------------------
    bool deterministic = true;          ///< recorder bytes identical across runs
    bool engagement_completed = false;  ///< >= 1 EntityKilled with attribution
    bool roster_bounded = true;         ///< roster identity every sample
    bool fight_alive = true;            ///< brain reached Entering + bus had >= 1 detect

    // -- diagnostics (first violation per gate; empty when green) --------
    std::string engagement_failure;     ///< which rung the chain stopped at
    std::string roster_leak;            ///< sample N: live != initial + spawned - retired
    std::string fight_stall;            ///< sample N: no detection / no Entering

    // -- the certificate --------------------------------------------------
    std::string recorder_md5_run0;      ///< 32 lowercase hex ("" on abort)
    std::string recorder_md5_run1;      ///< "" when runs == 1

    // -- the engagement window (for the summary's engagement_summary) ---
    double first_detect_s = -1.0;       ///< earliest TrackAcquired sim_time_s (-1 = none)
    double first_launch_s = -1.0;       ///< earliest MissileLaunched sim_time_s
    double first_kill_s = -1.0;         ///< earliest EntityKilled sim_time_s
    int shots_fired = 0;                ///< MissileLaunched events (run 0)
    int shots_hit = 0;                  ///< MissileDetonated with end_cause == target_hit
    int shots_missed = 0;               ///< MissileDetonated with other causes
};

/// Everything the host needs after execute(): the verdicts, run 0's
/// diary, run 0's recorder JSON (the bvr_intercept_result.json bytes),
/// the final headline counters, and the abort state (watchdog / harness
/// error — never a fight verdict).
struct InterceptReport {
    InterceptVerdict verdict;
    std::vector<InterceptSample> diary;   ///< run 0's samples
    std::string recorder_json;            ///< run 0's (byte-stable)

    // Final cumulative counters (run 0, end of horizon).
    int tracks_acquired = 0;
    int tracks_dropped = 0;
    int rwr_locks = 0;
    int rwr_launches = 0;
    int missiles_launched = 0;
    int missiles_detonated = 0;
    int damage_events = 0;
    int kills = 0;
    int samples = 0;

    // Preconditions the gates key on.
    bool combat_enabled = false;          ///< scenario.combat.enabled
    int aircraft_count = 0;               ///< scenario.aircraft.size()
    int blue_aircraft = 0;
    int red_aircraft = 0;

    bool aborted = false;
    std::string abort_reason;
};

/// The BVR intercept acceptance harness. create() validates the options'
/// shape (scenario exists, combat.enabled, runs >= 1, horizon > 0);
/// execute() builds fresh Simulations, runs the horizon, samples the
/// diary, checks the identities, and derives the verdicts. One
/// execute() per harness; a second call re-runs the whole fight from
/// scratch (fresh report).
class BvrInterceptHarness {
public:
    /// Called once per diary sample during run 0 (run 1+ are silent —
    /// their only job is the recorder bytes). Runs on the harness
    /// thread, between tick batches; the sample is a copy.
    using ProgressFn = std::function<void(const InterceptSample&)>;

    [[nodiscard]] static std::unique_ptr<BvrInterceptHarness>
    create(const InterceptHarnessOptions& opts, std::string* error = nullptr);

    ~BvrInterceptHarness();

    BvrInterceptHarness(const BvrInterceptHarness&) = delete;
    BvrInterceptHarness& operator=(const BvrInterceptHarness&) = delete;

    /// Run the fight (opts.runs passes) and derive the verdicts. Safe
    /// to call once; returns the report (also report()).
    const InterceptReport& execute(ProgressFn on_sample = nullptr);

    [[nodiscard]] const InterceptReport& report() const noexcept {
        return report_;
    }

private:
    BvrInterceptHarness() = default;

    /// One full pass (run index 0..runs-1). Fills report_ fields the
    /// pass owns: run 0 the diary + checks + recorder json; every run
    /// its recorder MD5.
    void run_pass_(int run, const ProgressFn& on_sample);

    /// The sample walk inside run_pass_: collect, check, diary, notify.
    void sample_(int run);

    /// The two per-sample gates (run 0 only): roster_bounded,
    /// fight_alive. engagement_completed + deterministic evaluate at
    /// run end (finalize_).
    void check_sample_(const InterceptSample& s);

    /// Final verdict derivation (after the last pass): scan run 0's
    /// combat events for the engagement window + the
    /// engagement_completed verdict; compare the two recorder MD5s
    /// for the deterministic verdict.
    void finalize_();

    InterceptHarnessOptions opts_;
    InterceptReport report_;

    // --- per-pass state (run_pass_ / sample_ only) ----------------------
    // The pass's live Simulation. Owned in run_pass_ via unique_ptr;
    // bare pointer here for sample_/check_sample_ access. Null outside
    // a pass.
    Simulation* sim_ = nullptr;
    double pass_t0_ = 0.0;                 ///< sim time at pass start
    int pass_initial_entities_ = 0;        ///< EntityWorld::size() at pass start
    int pass_spawned_ = 0;                 ///< cumulative entities created mid-pass
    int pass_retired_ = 0;                 ///< cumulative entities destroyed mid-pass
    int pass_samples_ = 0;                 ///< samples collected this pass
    double pass_next_sample_t_ = 0.0;      ///< next sample's sim time
    std::chrono::steady_clock::time_point pass_sample_wall_{};

    // Cumulative combat-event counters observed THIS pass (the recorder
    // is the source of truth; these mirror it for sample-time checks).
    int pass_tracks_acquired_ = 0;
    int pass_tracks_dropped_ = 0;
    int pass_rwr_locks_ = 0;
    int pass_rwr_launches_ = 0;
    int pass_missiles_launched_ = 0;
    int pass_missiles_detonated_ = 0;
    int pass_damage_events_ = 0;
    int pass_kills_ = 0;

    // The previous sample's cumulative counters (for sample_*_delta).
    InterceptSample pass_prev_{};
    bool pass_first_sample_ = true;

    // Run 0's combat events (copied from the recorder at finalize_).
    // Used to derive the engagement window + the engagement_completed
    // verdict. Stored as raw event fields (the harness does NOT link
    // f4-recorder's CombatEvent type at the header level — the
    // forward declaration keeps the include surface minimal).
    struct EventRow {
        std::uint64_t tick;
        double sim_time_s;
        int kind;             // CombatEventKind as int (0..10)
        std::uint64_t subject_id;
        std::uint64_t object_id;
        std::uint64_t missile_id;
        std::string end_cause;
        std::string weapon_name;
    };
    std::vector<EventRow> run0_events_;
};

} // namespace f4::simulation
