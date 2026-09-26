// f4-simulation/src/chain_engine.hpp
//
// ChainEngine — the combat-chain acceptance-run skeleton shared by the
// harnesses. One implementation of the machinery every harness copies:
// the create() validation, the pre-flight combat refusal (M4-FIX order),
// the run loop with the 4-sim-second drain + stall + wall watchdogs, the
// sample cadence walk, the roster-census identity, the recorder-byte
// MD5 certificate, and the abort state.
//
// The engine is a src-private pimpl: each public harness header keeps its
// minimal include surface (only <filesystem> + std + a forward-declared
// Simulation) and holds `std::unique_ptr<Engine>` where Engine is the
// instantiation below. The per-harness differences are supplied through
// the Hooks policy:
//
//   * `recorder_tag`       — the recorder's to_json tag (the certificate
//                            artifact's format name)
//   * reset_pass_extras    — zero the harness's own per-pass tallies
//   * count_event          — observe one combat event (harness-specific
//                            kind tallying)
//   * fill_sample_extras   — write the harness's extra Sample fields and
//                            per-sample pulses (pass_prev_ is the state's
//                            previous sample)
//   * check_sample_alive   — the pre-engage side of the alive gate
//                            (optional; default no-op)
//   * copy_headline        — run 0: copy final tallies into the report
//   * finalize_gates       — post-run verdict derivation (the harness's
//                            own gates and engagement window)
//
// Determinism contract: unchanged from the per-harness skeletons this
// replaces — no RNG, no host clocks in verdict paths; wall-clock reads
// feed the diary's telemetry columns only.

#pragma once

#include "harness_shared.hpp"

#include <f4/simulation/simulation.hpp>
#include <f4/simulation/scenario.hpp>
#include <f4/recorder/flight_recorder.hpp>
#include <f4/entities/entity.hpp>
#include <f4/weapons/missile_battery.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <memory>
#include <string>
#include <vector>

namespace f4::simulation::chain_engine {

/// The combat-event kinds as plain ints, aliased for the engine body
/// (the canonical table lives in harness_shared.hpp).
constexpr int kTrackAcquired    = harness_shared::kTrackAcquired;
constexpr int kTrackDropped     = harness_shared::kTrackDropped;
constexpr int kRwrLock          = harness_shared::kRwrLock;
constexpr int kRwrLaunch        = harness_shared::kRwrLaunch;
constexpr int kMissileLaunched  = harness_shared::kMissileLaunched;
constexpr int kMissileDetonated = harness_shared::kMissileDetonated;
constexpr int kDamageApplied    = harness_shared::kDamageApplied;
constexpr int kEntityKilled     = harness_shared::kEntityKilled;
constexpr int kGunFired         = harness_shared::kGunFired;
constexpr int kBombReleased     = harness_shared::kBombReleased;
constexpr int kBombImpact       = harness_shared::kBombImpact;

/// For harnesses with no extra tallies beyond the eight common counters.
struct EmptyExtras {};

/// One combat event of run 0, stashed from the recorder at pass end for
/// the harness's finalize gates (raw fields — the engine keeps
/// f4-recorder out of the public headers' include surface).
struct EventRow {
    std::uint64_t tick;
    double sim_time_s;
    int kind;             // recorder::CombatEventKind as int
    std::uint64_t subject_id;
    std::uint64_t object_id;
    std::uint64_t missile_id;
    std::string end_cause;
    std::string weapon_name;
};

/// The per-harness tally state the engine keeps alongside its own. The
/// eight common counters are the M4 chain's cumulative event counts;
/// extras (gun bursts, bombs, …) live in `extra` (a Hooks-owned struct).
template <class Extras>
struct State {
    // --- census (the roster_bounded gate) ------------------------------
    double pass_t0 = 0.0;              ///< sim time at pass start
    int pass_initial_entities = 0;     ///< world size at pass start
    int pass_samples = 0;              ///< samples collected this pass
    double pass_next_sample_t = 0.0;   ///< next sample's sim time
    bool pass_first_sample = true;
    std::chrono::steady_clock::time_point pass_sample_wall{};

    // --- the eight common chain counters (cumulative, this pass) --------
    int tracks_acquired = 0;
    int tracks_dropped = 0;
    int rwr_locks = 0;
    int rwr_launches = 0;
    int missiles_launched = 0;
    int missiles_detonated = 0;
    int damage_events = 0;
    int kills = 0;

    Extras extra{};                    ///< harness-owned tallies/state
};

/// The engine. Template parameters are the public harness types (they
/// duck-type: the engine touches only the fields all three share).
template <class Options, class Sample, class Verdict, class Report,
          class Hooks>
class Engine {
public:
    using ProgressFn = std::function<void(const Sample&)>;

    [[nodiscard]] const Report& report() const noexcept {
        return report_;
    }

    static bool validate_options(const Options& opts, const char* who,
                                 std::string* error) {
        const auto fail = [error](const std::string& msg) {
            if (error != nullptr) *error = msg;
            return false;
        };
        if (opts.scenario_json.empty()) {
            return fail(std::string(who) + ": scenario_json is empty");
        }
        if (!std::filesystem::exists(opts.scenario_json)) {
            return fail(std::string(who) + ": scenario_json does not exist: " +
                        opts.scenario_json.string());
        }
        if (opts.horizon_sec <= 0) {
            return fail(std::string(who) + ": horizon_sec must be positive");
        }
        if (opts.sample_sec <= 0.0) {
            return fail(std::string(who) + ": sample_sec must be positive");
        }
        if (opts.runs < 1 || opts.runs > 8) {
            return fail(std::string(who) + ": runs must be 1..8");
        }
        return true;
    }

    static bool options_refuse_scenario(const Options& opts,
                                        std::string* reason) {
        return harness_shared::combat_refusal_reason(opts.scenario_json,
                                                     reason);
    }

    const Report& execute(const Options& opts, ProgressFn on_sample) {
        opts_ = &opts;
        report_ = {};
        std::string why;
        if (options_refuse_scenario(opts, &why)) {
            report_.aborted = true;
            report_.abort_reason = why;
            finalize();
            return report_;
        }

        const auto wall_start = std::chrono::steady_clock::now();
        for (int run = 0; run < opts.runs && !report_.aborted; ++run) {
            run_pass(run, on_sample);
            if (opts.max_wall_sec_total > 0.0) {
                const std::chrono::duration<double> spent =
                    std::chrono::steady_clock::now() - wall_start;
                if (spent.count() > opts.max_wall_sec_total) {
                    report_.aborted = true;
                    report_.abort_reason =
                        "wall-clock watchdog exceeded after run " +
                        std::to_string(run) + " (" +
                        std::to_string(spent.count()) + "s > " +
                        std::to_string(opts.max_wall_sec_total) + "s)";
                    break;
                }
            }
        }
        finalize();
        return report_;
    }

private:
    void run_pass(int run, const ProgressFn& on_sample) {
        // Load the scenario with recording forced on — the recorder is
        // the certificate (its to_json() is the byte-stable MD5 input),
        // and attach_combat_event_recorder is gated on scenario.record.
        // The post-load combat check stays as defense in depth.
        Scenario scenario;
        try {
            scenario = load_scenario(opts_->scenario_json);
        } catch (const std::exception& e) {
            report_.aborted = true;
            report_.abort_reason = "scenario load failed (run " +
                                   std::to_string(run) + "): " + e.what();
            return;
        }
        if (!scenario.combat.enabled) {
            report_.aborted = true;
            report_.abort_reason =
                "scenario combat.enabled is false (run " +
                std::to_string(run) +
                ") — the harness refuses to run a non-combat scenario "
                "(silent success would be the worst failure class)";
            return;
        }
        scenario.record = true;
        scenario.record_every = 1;
        scenario.record_path.clear();

        if (run == 0) {
            report_.combat_enabled = true;
            report_.aircraft_count = static_cast<int>(scenario.aircraft.size());
            for (const auto& ac : scenario.aircraft) {
                if (ac.team == "blue") ++report_.blue_aircraft;
                else if (ac.team == "red") ++report_.red_aircraft;
            }
        }

        const auto asset_dir = opts_->asset_dir.empty()
            ? opts_->scenario_json.parent_path()
            : opts_->asset_dir;

        auto sim = std::make_unique<Simulation>(std::move(scenario), asset_dir);
        try {
            sim->initialize();
        } catch (const std::exception& e) {
            report_.aborted = true;
            report_.abort_reason = "sim initialize failed (run " +
                                   std::to_string(run) + "): " + e.what();
            return;
        }
        sim->set_paused(false);
        sim_ = sim.get();

        // Baseline (before the first tick): the roster identity's left side.
        st_.pass_t0 = sim_->sim_time_s();
        st_.pass_initial_entities = static_cast<int>(sim_->world().size());
        st_.pass_samples = 0;
        st_.pass_next_sample_t = st_.pass_t0 + opts_->sample_sec;
        st_.pass_first_sample = true;
        prev_ = Sample{};
        prev_.sim_time_s = st_.pass_t0;
        st_.pass_sample_wall = std::chrono::steady_clock::now();
        st_.tracks_acquired = 0;
        st_.tracks_dropped = 0;
        st_.rwr_locks = 0;
        st_.rwr_launches = 0;
        st_.missiles_launched = 0;
        st_.missiles_detonated = 0;
        st_.damage_events = 0;
        st_.kills = 0;
        hooks().reset_pass_extras(st_);

        const double sim_dt = sim_->scenario().sim_dt > 0.0
            ? sim_->scenario().sim_dt
            : (1.0 / 60.0);
        const double target =
            st_.pass_t0 + static_cast<double>(opts_->horizon_sec);
        const auto pass_wall_start = std::chrono::steady_clock::now();
        int stalled_batches = 0;

        // The loop: 4-sim-second tick batches (the C5 drain discipline —
        // byte-equivalent to any other split of the same ticks). Keys on
        // the SIM CLOCK, not the debt, so it self-corrects.
        while (sim_->sim_time_s() < target) {
            const double before = sim_->sim_time_s();
            const double batch_target = std::min(before + 4.0, target);
            int steps = 0;
            while (sim_->sim_time_s() < batch_target && steps < 240) {
                sim_->tick(sim_dt);
                ++steps;
            }
            if (sim_->sim_time_s() <= before + 1e-9) {
                ++stalled_batches;
                if (stalled_batches > 64) {
                    report_.aborted = true;
                    report_.abort_reason =
                        "sim clock stopped advancing (sim " +
                        std::to_string(sim_->sim_time_s()) +
                        "s, run " + std::to_string(run) + ")";
                    sim_ = nullptr;
                    return;
                }
            } else {
                stalled_batches = 0;
            }

            while (sim_->sim_time_s() >= st_.pass_next_sample_t &&
                   st_.pass_next_sample_t <= target) {
                collect_sample(run);
                st_.pass_next_sample_t += opts_->sample_sec;
                if (on_sample != nullptr && run == 0 &&
                    !report_.diary.empty()) {
                    on_sample(report_.diary.back());
                }
            }

            if (opts_->max_wall_sec_total > 0.0) {
                const std::chrono::duration<double> spent =
                    std::chrono::steady_clock::now() - pass_wall_start;
                if (spent.count() > opts_->max_wall_sec_total) {
                    report_.aborted = true;
                    report_.abort_reason =
                        "wall-clock watchdog exceeded mid-run (sim " +
                        std::to_string(sim_->sim_time_s()) +
                        "s, run " + std::to_string(run) + ")";
                    sim_ = nullptr;
                    return;
                }
            }
        }

        // The pass's recorder bytes + MD5. Run 0 keeps the document; every
        // run re-derives the digest, and run 1+ compares the BYTES (the
        // strict form of the certificate — a digest collision cannot pass).
        const auto* rec = sim_->recorder();
        const auto json = (rec != nullptr)
            ? rec->to_json(Hooks::recorder_tag)
            : std::string{};
        const auto digest = harness_shared::md5_hex(json);
        if (run == 0) {
            report_.recorder_json = json;
            report_.verdict.recorder_md5_run0 = digest;
            // Stash run 0's combat events for the finalize gates.
            if (rec != nullptr) {
                run0_events_.clear();
                run0_events_.reserve(rec->combat_events().size());
                for (const auto& e : rec->combat_events()) {
                    EventRow r;
                    r.tick = e.tick;
                    r.sim_time_s = e.sim_time_s;
                    r.kind = static_cast<int>(e.kind);
                    r.subject_id = e.subject_id;
                    r.object_id = e.object_id;
                    r.missile_id = e.missile_id;
                    r.end_cause = e.end_cause;
                    r.weapon_name = e.weapon_name;
                    run0_events_.push_back(std::move(r));
                }
            }
            hooks().copy_headline(st_, report_);
            report_.samples = st_.pass_samples;
        } else {
            report_.verdict.recorder_md5_run1 = digest;
            if (json != report_.recorder_json) {
                report_.verdict.deterministic = false;
            }
        }

        sim_ = nullptr;
    }

    void collect_sample(int run) {
        Sample s;
        s.sample = st_.pass_samples + 1;
        s.sim_time_s = sim_->sim_time_s();
        s.initial_entities = st_.pass_initial_entities;
        s.live_entities = static_cast<int>(sim_->world().size());
        s.live_missiles = static_cast<int>(
            weapons::count_live_missiles(sim_->world()));

        // Refresh the common cumulative counters from the recorder (the
        // source of truth, wired by attach_combat_event_recorder inside
        // initialize). The hook tallies the harness's own kinds.
        const auto* rec = sim_->recorder();
        st_.tracks_acquired = 0;
        st_.tracks_dropped = 0;
        st_.rwr_locks = 0;
        st_.rwr_launches = 0;
        st_.missiles_launched = 0;
        st_.missiles_detonated = 0;
        st_.damage_events = 0;
        st_.kills = 0;
        hooks().reset_sample_extras(st_);
        if (rec != nullptr) {
            for (const auto& e : rec->combat_events()) {
                switch (static_cast<int>(e.kind)) {
                    case kTrackAcquired:    ++st_.tracks_acquired;    break;
                    case kTrackDropped:     ++st_.tracks_dropped;     break;
                    case kRwrLock:          ++st_.rwr_locks;          break;
                    case kRwrLaunch:        ++st_.rwr_launches;       break;
                    case kMissileLaunched:  ++st_.missiles_launched;  break;
                    case kMissileDetonated: ++st_.missiles_detonated; break;
                    case kDamageApplied:    ++st_.damage_events;      break;
                    case kEntityKilled:     ++st_.kills;              break;
                    default: break;
                }
                hooks().count_event(st_, static_cast<int>(e.kind));
            }
        }
        s.tracks_acquired = st_.tracks_acquired;
        s.tracks_dropped = st_.tracks_dropped;
        s.rwr_locks = st_.rwr_locks;
        s.rwr_launches = st_.rwr_launches;
        s.missiles_launched = st_.missiles_launched;
        s.missiles_detonated = st_.missiles_detonated;
        s.damage_events = st_.damage_events;
        s.kills = st_.kills;
        hooks().fill_sample_extras(st_, prev_, s);

        // Spawned/retired deltas from the previous sample — the roster
        // identity's right side, observed as world-size deltas corrected
        // for the other side.
        if (st_.pass_first_sample) {
            s.spawned_entities = s.live_entities - s.initial_entities;
            s.retired_entities = 0;
        } else {
            const int live_delta = s.live_entities - prev_.live_entities;
            if (live_delta > 0) {
                s.spawned_entities = prev_.spawned_entities + live_delta;
                s.retired_entities = prev_.retired_entities;
            } else if (live_delta < 0) {
                s.spawned_entities = prev_.spawned_entities;
                s.retired_entities = prev_.retired_entities + (-live_delta);
            } else {
                s.spawned_entities = prev_.spawned_entities;
                s.retired_entities = prev_.retired_entities;
            }
        }

        // Telemetry (diary only; never a verdict).
        const auto now = std::chrono::steady_clock::now();
        const std::chrono::duration<double> wall = now - st_.pass_sample_wall;
        s.wall_sec = wall.count();
        s.ticks_per_sec = (s.sim_time_s - prev_.sim_time_s) /
                          (s.wall_sec > 0.0 ? s.wall_sec : 1.0);
        s.rss_kb = harness_shared::current_rss_kb();
        st_.pass_sample_wall = now;

        // Run 0 keeps the diary + runs the gates.
        if (run == 0) {
            report_.diary.push_back(s);
            check_sample(s);
        }

        prev_ = s;
        st_.pass_first_sample = false;
        ++st_.pass_samples;
    }

    void check_sample(const Sample& s) {
        // ROSTER BOUNDED — the identity is exact at sample boundaries
        // (between ticks the sweep has run). Catches the missile-leak
        // class the per-tick sweep would otherwise mask across samples.
        const int expected = s.initial_entities + s.spawned_entities -
                             s.retired_entities;
        if (s.live_entities != expected && report_.verdict.roster_bounded) {
            report_.verdict.roster_bounded = false;
            report_.verdict.roster_leak =
                "sample " + std::to_string(s.sample) +
                " (sim " + std::to_string(s.sim_time_s) + "s): live=" +
                std::to_string(s.live_entities) + " != initial=" +
                std::to_string(s.initial_entities) + " + spawned=" +
                std::to_string(s.spawned_entities) + " - retired=" +
                std::to_string(s.retired_entities) + " (expected " +
                std::to_string(expected) + ")";
        }
        hooks().check_sample_alive(st_, report_, s);
    }

    void finalize() {
        if (report_.aborted) return;
        hooks().finalize_gates(st_, report_, run0_events_);
    }

    Hooks& hooks() { return hooks_; }

    const Options* opts_ = nullptr;
    Report report_{};
    Hooks hooks_{};
    State<typename Hooks::Extras> st_{};
    Sample prev_{};
    Simulation* sim_ = nullptr;
    std::vector<EventRow> run0_events_;
};

} // namespace f4::simulation::chain_engine
