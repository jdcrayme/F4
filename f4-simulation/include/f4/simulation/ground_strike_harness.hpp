// f4-simulation/include/f4/simulation/ground_strike_harness.hpp
//
// PUBLIC HEADER — M5b, the end-to-end AIR-TO-GROUND strike acceptance
// harness (see Docs/archive/COMBAT_CHAIN_M4_PLAN.md §6 and
// Docs/archive/COMBAT_CHAIN_M5_PLAN.md §6 — the harness both plans deferred as
// "the certified A/G rung is its own tranche"). The combat-chain sibling
// of BvrInterceptHarness (M4 — the BVR chain) and WvrMergeHarness (M5a —
// the inside-the-band fight): where those certify the A/A ladder, M5b
// certifies the A/G chain —
//
//     arm → fly the delivery waypoint → CCIP release → ballistic flyout
//         → impact on the objective → feature damage → the fstatus ledger
//
// WHAT THIS IS. A scenario-scale strike: one (or more) blue strikers, a
// synthetic objective injected by the harness, a stick of bombs. Every
// link is unit-tested (M1 weapons: test_bomb/test_bomb_unit; the brain
// strike rung: test_strike_module; the ledger: test_campaign_result_sink)
// and the campaign-session surface books A/G losses (G2, the --unit-strike
// QC gate); what M5b adds is the ACCEPTANCE ARTIFACT: a headless,
// deterministic, instrumented run with verdicts and a byte-stable
// certificate — the same shape the campaign war (C5), the BVR intercept
// (M4) and the WVR merge (M5a) already have.
//
// THE COMPOSITION CONTRACT (the M5b scope decision). The scenario schema
// carries no loadout field, no waypoint target reference, and no strike
// target entity (those belong to a scenario-schema tranche of their own —
// they touch scenario.hpp, the loader, and every consumer). The harness
// therefore COMPOSES the strike world after Simulation::initialize(),
// identically on every pass (determinism is unaffected):
//
//   * the objective   — an entity with TransformComponent (the target
//                       position), FeatureSetComponent (the feature
//                       placements + hit points + values), a
//                       DamageBitmapComponent (the fstatus wire), and a
//                       red TEAM tag — the same shape the world loader
//                       builds from campaign data;
//   * the ordnance    — a Bomb-category station added to every blue
//                       scenario aircraft's WeaponStoreComponent (the
//                       combat loadout's A/A-only store keeps its
//                       stations; the strike station is additive);
//   * the fire control— the brain's StrikeModule configured from the
//                       weapon card (drag factor from the card's own
//                       ballistics at the delivery condition, salvo from
//                       the loadout, CCIP tolerance from the lethal
//                       radius) and the mission plan's delivery waypoint
//                       bound to the objective's entity id — exactly the
//                       wiring the campaign bridge performs for a saved
//                       strike flight (campaign_bridge.hpp
//                       arm_flight_strike), exercised here at scenario
//                       scale.
//
// The scenario itself declares the GEOMETRY (the delivery waypoint's
// WP_ACTION — 14/15/17/18/19, the campwp.h A/G delivery actions — is
// threaded into the brain's mission plan by Simulation::initialize) and
// the ROE (combat.enabled — mechanically required: the static bomb clock
// and the spent-bomb sweep are gated on it). A scenario whose route
// carries no delivery waypoint is a harness ABORT (wrong scenario shape),
// not a verdict.
//
// The five verdicts (M4/M5a's shapes, A/G's contents):
//
//   * DETERMINISTIC      — the strike runs TWICE in-process (fresh
//                          Simulation per pass); the two
//                          FlightRecorder::to_json() byte streams must be
//                          identical (compared as bytes, certified as
//                          MD5 — the number a human re-derives with
//                          md5sum on the ground_strike_result.json
//                          artifact).
//   * RELEASE_OCCURRED   — at least one BombReleased whose shooter is one
//                          of the armed strikers (rung 1: arming/envelope
//                          — an empty store, a waypoint the trigger never
//                          armed, or a geometry that never satisfies the
//                          CCIP gate each name their rung).
//   * IMPACT_ON_TARGET   — at least one BombImpact with end_cause
//                          "impact" against the objective (rung 2: the
//                          flyout — an "expired" bomb is a named failure,
//                          not a pass).
//   * DAMAGE_APPLIED     — the on-target impacts destroyed at least one
//                          feature (rung 3: terminal effectiveness),
//                          cross-checked against the objective's own
//                          damage ledger (objective_damage_summary — the
//                          event says what happened, the ledger says what
//                          stuck).
//   * ROSTER_BOUNDED     — the roster identity holds at every sample:
//                          live == initial + spawned − retired (the M4
//                          identity — bombs count: they spawn at release
//                          and are swept at terminal state).
//
// WHAT RUNS. The harness composes Simulation directly — the same object
// the scenario player drives; there is no CampaignSession at scenario
// scale. sim.tick() runs in 4-sim-second batches (the 240-tick drain
// discipline C5/M4/M5a use). The striker is scenario-defined (1 shipped;
// every blue scenario aircraft is armed and flies its own delivery
// waypoint — a package strike rides the same code).
//
// ARTIFACTS (host-written, the M4 three-file pattern):
//   ground_strike_result.json  — run 0's recorder JSON (the byte-stable
//                                certificate)
//   ground_strike_summary.json — verdicts + counters + MD5s + the strike
//                                window (deterministic content ONLY)
//   ground_strike_diary.json   — per-sample telemetry (NOT byte-stable,
//                                its own file for exactly that reason)
//
// Determinism: no RNG in the harness path (the injected world is
// identical every pass; the damage roll rides the blast model's fixed
// default roll — the same value every pass). No host clocks in the
// verdicts. The wall-clock reads feed the diary's telemetry columns only.
// C++20.

#pragma once

#include <f4/geo/position.hpp>
#include <f4/simulation/scenario.hpp>

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace f4::simulation {

class Simulation;   // forward — the .cpp includes simulation.hpp (the
                    // M4/M5a header's include-surface discipline)

/// Harness tuning. The scenario carries the strike's SHAPE (the striker,
/// its route + delivery waypoint action, the airfield, combat config,
/// sim_dt); these carry the RUN + the composed target. Mirrors
/// WvrMergeHarnessOptions (the M5a shape) with the strike-target block
/// the scenario schema does not carry yet (the M5b composition contract —
/// see the header comment).
struct GroundStrikeHarnessOptions {
    /// The scenario JSON to run. combat.enabled MUST be true (execute()
    /// refuses non-combat scenarios — the M4 refusal contract, same
    /// stable abort prefix for the QC's exit 2; the static bomb clock +
    /// the spent-bomb sweep are gated on it).
    std::filesystem::path scenario_json;

    /// The asset directory for the scenario (defaults to the scenario's
    /// parent — @asset: refs resolve against this).
    std::filesystem::path asset_dir;

    /// Sim-seconds of run. The delivery pass resolves in well under two
    /// minutes (spawn-to-release ~45 s at the shipped geometry — 30 kft
    /// of transit at 675 fps + the release solution's stand-off); the
    /// acceptance horizon is 300 (5 min at 60 Hz) so a re-attack and the
    /// stick's fall time fit. Smoke runs use 120 s.
    std::int64_t horizon_sec = 300;

    /// Diary + check cadence (sim-seconds per sample).
    double sample_sec = 30.0;

    /// Determinism passes. 2 (default) = the M4 contract — the second
    /// pass re-derives the recorder bytes. 1 skips the proof (the
    /// verdict is vacuously true; the summary says so via an empty
    /// recorder_md5_run1).
    int runs = 2;

    /// Total wall-clock watchdog across ALL runs (0 = off). Exceeded =
    /// ABORT (a harness error, never a strike verdict).
    double max_wall_sec_total = 0.0;

    // --- the composed strike target -------------------------------------
    /// The objective's center (ENU feet; z = the impact plane MSL — the
    /// bomb's terminal plane and the features' datum).
    f4::geo::WorldPosition target_position{0.0, 30000.0, 0.0};
    /// Feature placements: `target_features` features spread along +x,
    /// centered on the objective (the test_bomb.cpp synthetic-objective
    /// shape — the world loader's real placements come from the .SDK
    /// pipeline, which a scenario-scale run does not exercise).
    int target_features = 5;
    double target_feature_spacing_ft = 500.0;
    /// Per-feature class data (the fixture FCD covers a subset of
    /// classes — synthetic features carry their own, documented).
    int target_feature_hit_points = 100;
    int target_feature_value = 10;
    /// Display name (the CampaignIdentityComponent callsign + trace).
    std::string target_name = "STRIKE TARGET";

    // --- the composed ordnance ------------------------------------------
    /// The WeaponClassTable name of the bomb (the built-in table carries
    /// "MK-82" and "GBU-12"). Resolved through the Simulation's own
    /// weapon table (an unknown name is an abort — never a silent A/A
    /// loadout).
    std::string bomb_name = "MK-82";
    /// Rounds per armed striker. The stick is capped at the doctrine
    /// salvo (4 — the campaign bridge's kDoctrineSalvoMax), not the
    /// loadout.
    int bomb_rounds = 6;
    /// The scenario route waypoint whose target_id binds to the injected
    /// objective. Matched by NAME against the brain's mission plan
    /// (per-aircraft route first, then the shared scenario waypoints —
    /// the same order Simulation::initialize used). Empty = the first
    /// waypoint carrying an A/G delivery action.
    std::string delivery_waypoint;
};

/// One row of the strike diary — the world's state at one sample.
/// Cumulative except the sample_* pulses; wall_sec / ticks_per_sec /
/// rss_kb are TELEMETRY (never a verdict).
struct GroundStrikeSample {
    int sample = 0;                 ///< 1-based
    double sim_time_s = 0.0;

    // --- roster identity (the roster_bounded gate) -------------------
    int initial_entities = 0;       ///< EntityWorld::size() at pass start
    int spawned_entities = 0;       ///< cumulative entities created mid-run
    int retired_entities = 0;       ///< cumulative entities destroyed mid-run
    int live_entities = 0;          ///< EntityWorld::size() now
    int live_bombs = 0;             ///< weapons::count_live_bombs(world)

    // --- A/G chain progress (recorder events, cumulative) --------------
    int bombs_released = 0;         ///< cumulative BombReleased events
    int bombs_impacted = 0;         ///< cumulative BombImpact events
    int impacts_on_target = 0;      ///< impact + end_cause "impact" + object == target
    int damage_events = 0;          ///< on-target impacts that destroyed features
    double features_destroyed_max = 0.0;  ///< max impact damage (features) so far
    double destroyed_pct_max = 0.0;       ///< max impact destroyed_pct so far
    double min_miss_distance_ft = -1.0;   ///< min miss over on-target impacts

    // --- the objective's own ledger (objective_damage_summary) ---------
    int ledger_features_destroyed = 0;    ///< destroyed after this sample
    double ledger_destroyed_pct = 0.0;    ///< value-weighted, 0..100

    // --- this sample's pulse -----------------------------------------
    int sample_releases = 0;
    int sample_impacts = 0;

    // --- performance telemetry (diary only; NEVER a verdict) -----------
    double wall_sec = 0.0;
    double ticks_per_sec = 0.0;
    long rss_kb = 0;
};

/// The five M5b gates, with the first violation of each recorded as a
/// diagnostic string ("" when green).
struct GroundStrikeVerdict {
    // -- the gates proper ------------------------------------------------
    bool deterministic = true;       ///< recorder bytes identical across runs
    bool release_occurred = false;   ///< >= 1 BombReleased by an armed striker
    bool impact_on_target = false;   ///< >= 1 on-target impact (end_cause impact)
    bool damage_applied = false;     ///< on-target impacts destroyed features
    bool roster_bounded = true;      ///< roster identity every sample

    // -- diagnostics (first violation per gate; empty when green) --------
    std::string release_stall;       ///< which rung the arming chain stopped at
    std::string impact_failure;      ///< released-but-never-hit, named
    std::string damage_failure;      ///< hit-but-no-damage, named
    std::string roster_leak;         ///< sample N: the identity broke

    // -- the certificate --------------------------------------------------
    std::string recorder_md5_run0;   ///< 32 lowercase hex ("" on abort)
    std::string recorder_md5_run1;   ///< "" when runs == 1

    // -- the strike window (run 0's combat events) ------------------------
    double first_release_s = -1.0;   ///< earliest BombReleased (-1 = none)
    double first_impact_s = -1.0;    ///< earliest BombImpact (any)
    double first_on_target_s = -1.0; ///< earliest on-target impact
    double first_damage_s = -1.0;    ///< earliest feature destruction
    int bombs_released = 0;          ///< BombReleased events (run 0)
    int bombs_impacted = 0;          ///< BombImpact events (run 0)
    int impacts_on_target = 0;       ///< impact + on the objective (run 0)
    int bomb_misses = 0;             ///< impacts off-target or "expired"
    double min_miss_distance_ft = -1.0;   ///< min miss over on-target impacts
    double features_destroyed_max = 0.0;  ///< max features destroyed by one impact
    double destroyed_pct_max = 0.0;       ///< max value-weighted destroyed %
    double features_destroyed_final = 0.0;///< ledger's destroyed total at run end
    double destroyed_pct_final = 0.0;     ///< ledger's destroyed % at run end

    // -- preconditions the gates key on -----------------------------------
    std::uint64_t target_entity_id = 0;  ///< the injected objective (0 = none)
    int strikers_armed = 0;              ///< blue aircraft armed by the harness
};

/// Everything the host needs after execute(): the verdicts, run 0's
/// diary, run 0's recorder JSON (the ground_strike_result.json bytes),
/// the final headline counters, and the abort state.
struct GroundStrikeReport {
    GroundStrikeVerdict verdict;
    std::vector<GroundStrikeSample> diary;   ///< run 0's samples
    std::string recorder_json;               ///< run 0's (byte-stable)

    // Final cumulative counters (run 0, end of horizon).
    int bombs_released = 0;
    int bombs_impacted = 0;
    int samples = 0;

    // Preconditions the gates key on.
    bool combat_enabled = false;          ///< scenario.combat.enabled
    int aircraft_count = 0;               ///< scenario.aircraft.size()
    int blue_aircraft = 0;
    int red_aircraft = 0;
    int strikers_armed = 0;               ///< blue aircraft the harness armed
    std::uint64_t target_entity_id = 0;   ///< the injected objective
    int target_features = 0;              ///< features on the injected objective

    bool aborted = false;
    std::string abort_reason;
};

/// The air-to-ground strike acceptance harness. create() validates the
/// options' shape; execute() builds fresh Simulations, injects the
/// strike world (identically every pass), runs the horizon, samples the
/// diary, checks the identities, and derives the verdicts. One
/// execute() per harness; a second call re-runs the whole strike from
/// scratch (fresh report).
class GroundStrikeHarness {
public:
    /// Called once per diary sample during run 0 (run 1+ are silent).
    using ProgressFn = std::function<void(const GroundStrikeSample&)>;

    [[nodiscard]] static std::unique_ptr<GroundStrikeHarness>
    create(const GroundStrikeHarnessOptions& opts, std::string* error = nullptr);

    ~GroundStrikeHarness();

    GroundStrikeHarness(const GroundStrikeHarness&) = delete;
    GroundStrikeHarness& operator=(const GroundStrikeHarness&) = delete;

    /// Run the strike (opts.runs passes) and derive the verdicts.
    const GroundStrikeReport& execute(ProgressFn on_sample = nullptr);

    [[nodiscard]] const GroundStrikeReport& report() const noexcept {
        return report_;
    }

private:
    GroundStrikeHarness() = default;

    /// One full pass (run index 0..runs-1).
    void run_pass_(int run, const ProgressFn& on_sample);

    /// The strike-world injection (the objective + the ordnance + the
    /// fire control binding), between initialize() and the first tick.
    /// Identical every pass — the determinism contract. Aborts the pass
    /// (with a named reason) when the scenario cannot host a strike.
    void inject_strike_world_(int run);

    /// The sample walk inside run_pass_.
    void sample_(int run);

    /// The per-sample gates (run 0 only): roster_bounded.
    void check_sample_(const GroundStrikeSample& s);

    /// Final verdict derivation (after the last pass).
    void finalize_();

    /// The vacuum→dragged range scale for the trigger, computed from the
    /// bomb card's own ballistics at THIS run's delivery condition (the
    /// route's altitude over the target, the doctrine reference speed) —
    /// the scenario-scale mirror of campaign_bridge.cpp's
    /// bomb_drag_factor_for (which pins the doctrine 5,000-ft reference).
    [[nodiscard]] double strike_drag_factor_(std::uint32_t bomb_handle,
                                             double dz_ft) const;

    GroundStrikeHarnessOptions opts_;
    GroundStrikeReport report_;

    // --- per-pass state ---------------------------------------------------
    Simulation* sim_ = nullptr;
    double pass_t0_ = 0.0;
    int pass_initial_entities_ = 0;
    int pass_spawned_ = 0;
    int pass_retired_ = 0;
    int pass_samples_ = 0;
    double pass_next_sample_t_ = 0.0;
    std::chrono::steady_clock::time_point pass_sample_wall_{};

    // The injected strike world (this pass).
    std::uint64_t target_entity_id_ = 0;
    std::vector<std::uint64_t> striker_ids_;   ///< the armed blue aircraft
    double delivery_dz_ft_ = 0.0;              ///< route altitude over the target
    double plan_route_z_ = 0.0;                ///< the found delivery waypoint's z

    // Cumulative combat-event counters observed THIS pass.
    int pass_bombs_released_ = 0;
    int pass_bombs_impacted_ = 0;
    int pass_impacts_on_target_ = 0;
    int pass_damage_events_ = 0;
    double pass_features_destroyed_max_ = 0.0;
    double pass_destroyed_pct_max_ = 0.0;
    double pass_min_miss_ft_ = -1.0;

    // The previous sample's cumulative counters (for sample_*_delta).
    GroundStrikeSample pass_prev_{};
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
        double damage;             // BombImpact: features destroyed
        double hit_points_after;   // BombImpact: value-weighted destroyed %
        double miss_distance_ft;   // BombImpact: impact -> aim point
    };
    std::vector<EventRow> run0_events_;
};

} // namespace f4::simulation
