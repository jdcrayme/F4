// f4-simulation/include/f4/simulation/wvr_merge_harness.hpp
//
// PUBLIC HEADER — M5a, the end-to-end WVR / guns merge acceptance harness
// (see Docs/COMBAT_CHAIN_M5_PLAN.md). The combat-chain sibling of
// BvrInterceptHarness (M4 — see bvr_intercept_harness.hpp): where M4
// certifies the beyond-visual-range chain (detection → employment →
// launch → crank → kill), M5a certifies the INSIDE-the-band fight — the
// last un-certified combat surface (M4 §6 deferred it deliberately:
// "the verdict gates differ — WVR cares about WVRState transitions, guns
// cares about GunModule::burst_count").
//
// WHAT THIS IS. The merge fight: the brain detects (radar), hands the
// fight from BVR to WVR at the band boundary, commits the merge (the
// collision-avoid exemption — the weapons own the pass geometry), employs
// heaters (WvrMerge) and/or the gun (guns_merge) inside their envelopes,
// kills or defeats, and disengages. Every link is unit-tested (M1
// weapons, M2 sensors, M3 tactics) and the guns-merge E2E exists as a
// hand-driven integration test; what M5a adds is the ACCEPTANCE ARTIFACT:
// a headless, deterministic, instrumented run with verdicts and a
// byte-stable certificate — the same shape the campaign war (C5) and the
// BVR intercept (M4) already have.
//
// THE M5a DELTA vs M4 — the band transition as first-class evidence.
// The WVR fight's "engaged" rung is not a weapon event: a merge that
// commits and never fires is a stalled fight at a DIFFERENT rung than
// one that never detected. The recorder gained WvrEngaged/WvrDisengaged
// combat events (f4-recorder — the sim appends them on the brain's
// combat-mode crossing; see simulation.cpp::record_wvr_band_flips), and
// this harness's fight_alive gate reads them: fight_alive = detection
// AND band entry. The engagement window reports first_wvr_engage_s /
// first_wvr_disengage_s so the summary narrates the merge as a fight:
// entered the band → fired → killed → disengaged.
//
// The four verdicts (M4's shapes, WVR's contents):
//
//   * DETERMINISTIC            — the fight runs TWICE in-process (fresh
//                                Simulation per pass); the two
//                                FlightRecorder::to_json() byte streams
//                                must be identical (compared as bytes,
//                                certified as MD5 — the number a human
//                                re-derives with md5sum on the
//                                wvr_merge_result.json artifact).
//   * ENGAGEMENT_COMPLETED     — at least one EntityKilled within the
//                                horizon with attribution. WVR
//                                attribution accepts BOTH weapon paths:
//                                the killer matches the subject of a
//                                MissileLaunched OR a GunFired event
//                                (the guns fight's kill rides the gun
//                                damage path — missile_id == 0).
//   * ROSTER_BOUNDED           — the roster identity holds at every
//                                sample: live == initial + spawned −
//                                retired (the M4 identity, unchanged —
//                                missiles count, retirements observed).
//   * FIGHT_ALIVE              — WVR contents: the bus carried at least
//                                one RadarTrackAcquired (the pre-engage
//                                gate, by the second sample) AND at
//                                least one WvrEngaged event (the brain
//                                reached the band — the Entering
//                                analog). A scenario that detects but
//                                never enters the band (geometry, band
//                                constants) is a stalled fight at a
//                                named rung; so is one that enters but
//                                never employs (first_gun_s /
//                                first_launch_s unset — the summary
//                                names which).
//
// WHAT RUNS. The harness composes Simulation directly — the same object
// the scenario player drives; there is no CampaignSession at scenario
// scale. sim.tick() runs in 4-sim-second batches (the 240-tick drain
// discipline C5/M4 use — byte-equivalent to any other split). The roster
// is scenario-defined (1v1 shipped; the harness is N-vs-M — the 2v2
// multi-flight acceptance rides the same code, M4 §6's multi-flight
// deferral closes here too).
//
// ARTIFACTS (host-written, the M4 three-file pattern):
//   wvr_merge_result.json  — run 0's recorder JSON (the byte-stable
//                            certificate)
//   wvr_merge_summary.json — verdicts + counters + MD5s + the engagement
//                            window (deterministic content ONLY)
//   wvr_merge_diary.json   — per-sample telemetry (NOT byte-stable, its
//                            own file for exactly that reason)
//
// Determinism: no RNG in the harness path (the combat RNG is the
// scenario's radar_rng_seed); no host clocks in the verdicts. The
// wall-clock reads feed the diary's telemetry columns only. C++20.

#pragma once

#include <f4/simulation/scenario.hpp>

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace f4::simulation {

class Simulation;   // forward — the .cpp includes simulation.hpp (the
                    // M4 header's include-surface discipline)

/// Harness tuning. The scenario carries the fight's SHAPE (aircraft,
/// teams, spawn geometry, combat config, radar_rng_seed, ROE flags,
/// sim_dt); these carry the RUN. Mirrors InterceptHarnessOptions.
struct WvrMergeHarnessOptions {
    /// The scenario JSON to run. combat.enabled MUST be true (execute()
    /// refuses non-combat scenarios — the M4 refusal contract, same
    /// stable abort prefix for the QC's exit 2).
    std::filesystem::path scenario_json;

    /// The asset directory for the scenario (defaults to the scenario's
    /// parent — @asset: refs resolve against this).
    std::filesystem::path asset_dir;

    /// Sim-seconds of fight. The guns merge resolves in well under a
    /// minute (spawn-to-merge ~16 s at the shipped geometry); the
    /// acceptance horizon is 300 (5 min at 60 Hz) so a re-attack cycle
    /// fits. Smoke runs use 60 s.
    std::int64_t horizon_sec = 300;

    /// Diary + check cadence (sim-seconds per sample).
    double sample_sec = 30.0;

    /// Determinism passes. 2 (default) = the M4 contract — the second
    /// pass re-derives the recorder bytes. 1 skips the proof (the
    /// verdict is vacuously true; the summary says so via an empty
    /// recorder_md5_run1).
    int runs = 2;

    /// Total wall-clock watchdog across ALL runs (0 = off). Exceeded =
    /// ABORT (a harness error, never a fight verdict).
    double max_wall_sec_total = 0.0;
};

/// One row of the fight diary — the world's state at one sample.
/// Cumulative except the sample_* pulses; wall_sec / ticks_per_sec /
/// rss_kb are TELEMETRY (never a verdict).
struct WvrMergeSample {
    int sample = 0;                 ///< 1-based
    double sim_time_s = 0.0;

    // --- roster identity (the roster_bounded gate) -------------------
    int initial_entities = 0;       ///< EntityWorld::size() at pass start
    int spawned_entities = 0;       ///< cumulative entities created mid-fight
    int retired_entities = 0;       ///< cumulative entities destroyed mid-fight
    int live_entities = 0;          ///< EntityWorld::size() now
    int live_missiles = 0;          ///< weapons::count_live_missiles(world)

    // --- combat chain progress ----------------------------------------
    int tracks_acquired = 0;        ///< cumulative RadarTrackAcquired events
    int tracks_dropped = 0;         ///< cumulative RadarTrackDropped events
    int rwr_locks = 0;              ///< cumulative RwrLock events
    int rwr_launches = 0;           ///< cumulative RwrLaunch events
    int missiles_launched = 0;      ///< cumulative MissileLaunched events
    int missiles_detonated = 0;     ///< cumulative MissileDetonated events
    int damage_events = 0;          ///< cumulative DamageApplied events
    int kills = 0;                  ///< cumulative EntityKilled events
    int gun_bursts = 0;             ///< cumulative GunFired events
    int wvr_engagements = 0;        ///< cumulative WvrEngaged events

    // --- this sample's pulse -----------------------------------------
    int sample_launches = 0;
    int sample_detonations = 0;
    int sample_gun_bursts = 0;
    int sample_kills = 0;

    // --- performance telemetry (diary only; NEVER a verdict) -----------
    double wall_sec = 0.0;
    double ticks_per_sec = 0.0;
    long rss_kb = 0;
};

/// The four M5a gates, with the first violation of each recorded as a
/// diagnostic string ("" when green).
struct WvrMergeVerdict {
    // -- the gates proper ------------------------------------------------
    bool deterministic = true;          ///< recorder bytes identical across runs
    bool engagement_completed = false;  ///< >= 1 EntityKilled with attribution
    bool roster_bounded = true;         ///< roster identity every sample
    bool fight_alive = true;            ///< detection AND band entry

    // -- diagnostics (first violation per gate; empty when green) --------
    std::string engagement_failure;     ///< which rung the chain stopped at
    std::string roster_leak;            ///< sample N: the identity broke
    std::string fight_stall;            ///< sample N / finalize: no detect, no band

    // -- the certificate --------------------------------------------------
    std::string recorder_md5_run0;      ///< 32 lowercase hex ("" on abort)
    std::string recorder_md5_run1;      ///< "" when runs == 1

    // -- the engagement window (run 0's combat events) -------------------
    double first_detect_s = -1.0;       ///< earliest TrackAcquired (-1 = none)
    double first_wvr_engage_s = -1.0;   ///< earliest WvrEngaged
    double last_wvr_engage_s = -1.0;    ///< latest WvrEngaged (re-attack count)
    double first_wvr_disengage_s = -1.0;///< earliest WvrDisengaged
    double first_launch_s = -1.0;       ///< earliest MissileLaunched
    double first_gun_s = -1.0;          ///< earliest GunFired
    double first_kill_s = -1.0;         ///< earliest EntityKilled
    int missile_shots = 0;              ///< MissileLaunched events (run 0)
    int missile_hits = 0;               ///< MissileDetonated end_cause == target_hit
    int missile_misses = 0;             ///< other end causes
    int gun_bursts = 0;                 ///< GunFired events (run 0)
    int kills = 0;                      ///< EntityKilled events (run 0)
};

/// Everything the host needs after execute(): the verdicts, run 0's
/// diary, run 0's recorder JSON (the wvr_merge_result.json bytes), the
/// final headline counters, and the abort state.
struct WvrMergeReport {
    WvrMergeVerdict verdict;
    std::vector<WvrMergeSample> diary;   ///< run 0's samples
    std::string recorder_json;           ///< run 0's (byte-stable)

    // Final cumulative counters (run 0, end of horizon).
    int tracks_acquired = 0;
    int tracks_dropped = 0;
    int rwr_locks = 0;
    int rwr_launches = 0;
    int missiles_launched = 0;
    int missiles_detonated = 0;
    int damage_events = 0;
    int kills = 0;
    int gun_bursts = 0;
    int wvr_engagements = 0;
    int samples = 0;

    // Preconditions the gates key on.
    bool combat_enabled = false;          ///< scenario.combat.enabled
    int aircraft_count = 0;               ///< scenario.aircraft.size()
    int blue_aircraft = 0;
    int red_aircraft = 0;

    bool aborted = false;
    std::string abort_reason;
};

/// The WVR / guns merge acceptance harness. create() validates the
/// options' shape; execute() builds fresh Simulations, runs the horizon,
/// samples the diary, checks the identities, and derives the verdicts.
/// One execute() per harness; a second call re-runs the whole fight from
/// scratch (fresh report).
class WvrMergeHarness {
public:
    /// Called once per diary sample during run 0 (run 1+ are silent).
    using ProgressFn = std::function<void(const WvrMergeSample&)>;

    [[nodiscard]] static std::unique_ptr<WvrMergeHarness>
    create(const WvrMergeHarnessOptions& opts, std::string* error = nullptr);

    ~WvrMergeHarness();

    WvrMergeHarness(const WvrMergeHarness&) = delete;
    WvrMergeHarness& operator=(const WvrMergeHarness&) = delete;

    /// Run the fight (opts.runs passes) and derive the verdicts.
    const WvrMergeReport& execute(ProgressFn on_sample = nullptr);

    [[nodiscard]] const WvrMergeReport& report() const noexcept {
        return report_;
    }

private:
    WvrMergeHarness() = default;

    /// One full pass (run index 0..runs-1).
    void run_pass_(int run, const ProgressFn& on_sample);

    /// The sample walk inside run_pass_.
    void sample_(int run);

    /// The per-sample gates (run 0 only): roster_bounded, the pre-engage
    /// side of fight_alive.
    void check_sample_(const WvrMergeSample& s);

    /// Final verdict derivation (after the last pass).
    void finalize_();

    WvrMergeHarnessOptions opts_;
    WvrMergeReport report_;

    // --- per-pass state ---------------------------------------------------
    Simulation* sim_ = nullptr;
    double pass_t0_ = 0.0;
    int pass_initial_entities_ = 0;
    int pass_spawned_ = 0;
    int pass_retired_ = 0;
    int pass_samples_ = 0;
    double pass_next_sample_t_ = 0.0;
    std::chrono::steady_clock::time_point pass_sample_wall_{};

    // Cumulative combat-event counters observed THIS pass.
    int pass_tracks_acquired_ = 0;
    int pass_tracks_dropped_ = 0;
    int pass_rwr_locks_ = 0;
    int pass_rwr_launches_ = 0;
    int pass_missiles_launched_ = 0;
    int pass_missiles_detonated_ = 0;
    int pass_damage_events_ = 0;
    int pass_kills_ = 0;
    int pass_gun_bursts_ = 0;
    int pass_wvr_engagements_ = 0;

    // The previous sample's cumulative counters (for sample_*_delta).
    WvrMergeSample pass_prev_{};
    bool pass_first_sample_ = true;

    // Run 0's combat events (copied from the recorder at run end — the
    // EventRow shape keeps f4-recorder out of the header).
    struct EventRow {
        std::uint64_t tick;
        double sim_time_s;
        int kind;             // CombatEventKind as int (0..12)
        std::uint64_t subject_id;
        std::uint64_t object_id;
        std::uint64_t missile_id;
        std::string end_cause;
        std::string weapon_name;
    };
    std::vector<EventRow> run0_events_;
};

} // namespace f4::simulation
