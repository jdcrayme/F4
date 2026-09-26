// f4-simulation/src/wvr_merge_harness.cpp
//
// WvrMergeHarness — see wvr_merge_harness.hpp for the design. Headless,
// deterministic, instrumented: the WVR / guns merge as an acceptance run.
//
// The structural template is BvrInterceptHarness (bvr_intercept_harness
// .cpp — itself the mirror of CampaignWarHarness). The M5a deltas are
// the fight_alive contents (detection AND band entry via the new
// WvrEngaged events), the gun event counters, and the WVR attribution
// rule (the killer may be a gun subject — the guns fight's kill rides
// the gun damage path, missile_id == 0).
//
// The MD5 + RSS helpers are copied verbatim from bvr_intercept_harness
// .cpp (which copied them from campaign_war_harness.cpp — the F4
// codebase prefers duplication over premature sharing; a third consumer
// would be the moment to extract, and this is it — but the M4 harness
// is frozen by its certificate baseline, so the copies stand).

#include "harness_shared.hpp"
#include <f4/simulation/wvr_merge_harness.hpp>

#include <f4/simulation/simulation.hpp>
#include <f4/recorder/flight_recorder.hpp>
#include <f4/recorder/combat_event.hpp>
#include <f4/weapons/missile_battery.hpp>
#include <f4/entities/entity.hpp>
#include <f4/json/f4_json.hpp>

#include <algorithm>
#include <chrono>
#include <fstream>

#if defined(__linux__)
#include <unistd.h>
#elif defined(__APPLE__)
#include <mach/mach.h>
#endif

namespace f4::simulation {

namespace {
namespace {

// The digest, RSS telemetry, and combat-event kinds are single-sourced in
// harness_shared.hpp (this file's copies were the four-way duplication).
using harness_shared::md5_hex;
using harness_shared::current_rss_kb;
using harness_shared::kTrackAcquired;
using harness_shared::kTrackDropped;
using harness_shared::kRwrLock;
using harness_shared::kRwrLaunch;
using harness_shared::kMissileLaunched;
using harness_shared::kMissileDetonated;
using harness_shared::kDamageApplied;
using harness_shared::kEntityKilled;
using harness_shared::kGunFired;
using harness_shared::kBombReleased;
using harness_shared::kBombImpact;
using harness_shared::kEndCauseTargetHit;
} // namespace

// ===========================================================================
// MD5 (RFC 1321) — copied verbatim from bvr_intercept_harness.cpp.

/// The CombatEventKind values as plain ints (mirror combat_event.hpp —
/// the header keeps f4-recorder out of its include surface).
constexpr int kWvrEngaged        = 11;
constexpr int kWvrDisengaged     = 12;


} // namespace

// ===========================================================================
// create / execute
// ===========================================================================

std::unique_ptr<WvrMergeHarness>
WvrMergeHarness::create(const WvrMergeHarnessOptions& opts,
                        std::string* error) {
    const auto fail = [error](const std::string& msg) {
        if (error != nullptr) *error = msg;
        return nullptr;
    };
    if (opts.scenario_json.empty()) {
        return fail("wvr harness: scenario_json is empty");
    }
    if (!std::filesystem::exists(opts.scenario_json)) {
        return fail("wvr harness: scenario_json does not exist: " +
                    opts.scenario_json.string());
    }
    if (opts.horizon_sec <= 0) {
        return fail("wvr harness: horizon_sec must be positive");
    }
    if (opts.sample_sec <= 0.0) {
        return fail("wvr harness: sample_sec must be positive");
    }
    if (opts.runs < 1 || opts.runs > 8) {
        return fail("wvr harness: runs must be 1..8");
    }
    auto harness =
        std::unique_ptr<WvrMergeHarness>(new WvrMergeHarness());
    harness->opts_ = opts;
    return harness;
}

WvrMergeHarness::~WvrMergeHarness() = default;

const WvrMergeReport& WvrMergeHarness::execute(ProgressFn on_sample) {
    report_ = {};

    // The M4-FIX refusal order: reject a non-combat scenario BEFORE any
    // per-run load validation (a non-combat scenario need not carry a
    // valid aircraft list — load-first would abort with the WRONG
    // failure class). The abort prefix is the QC tool's exit-2 contract;
    // keep "scenario combat.enabled is false" verbatim. The scan is
    // best-effort over the raw JSON; parse problems defer to
    // run_pass_'s authoritative load error, and the post-load check
    // stays as defense in depth.
    if (std::string why;
        harness_shared::combat_refusal_reason(opts_.scenario_json, &why)) {
        report_.aborted = true;
        report_.abort_reason = why;
        finalize_();
        return report_;
    }

    const auto wall_start = std::chrono::steady_clock::now();
    for (int run = 0; run < opts_.runs && !report_.aborted; ++run) {
        run_pass_(run, on_sample);
        if (opts_.max_wall_sec_total > 0.0) {
            const std::chrono::duration<double> spent =
                std::chrono::steady_clock::now() - wall_start;
            if (spent.count() > opts_.max_wall_sec_total) {
                report_.aborted = true;
                report_.abort_reason =
                    "wall-clock watchdog exceeded after run " +
                    std::to_string(run) + " (" +
                    std::to_string(spent.count()) + "s > " +
                    std::to_string(opts_.max_wall_sec_total) + "s)";
                break;
            }
        }
    }
    finalize_();
    return report_;
}

// ===========================================================================
// One full pass
// ===========================================================================

void WvrMergeHarness::run_pass_(int run, const ProgressFn& on_sample) {
    Scenario scenario;
    try {
        scenario = load_scenario(opts_.scenario_json);
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
    // Force recording: the recorder is the certificate AND the band
    // events' source (record_wvr_band_flips writes only when the
    // recorder exists).
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

    const auto asset_dir = opts_.asset_dir.empty()
        ? opts_.scenario_json.parent_path()
        : opts_.asset_dir;

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
    pass_t0_ = sim_->sim_time_s();
    pass_initial_entities_ = static_cast<int>(sim_->world().size());
    pass_spawned_ = 0;
    pass_retired_ = 0;
    pass_samples_ = 0;
    pass_next_sample_t_ = pass_t0_ + opts_.sample_sec;
    pass_first_sample_ = true;
    pass_prev_ = WvrMergeSample{};
    pass_prev_.sim_time_s = pass_t0_;
    pass_sample_wall_ = std::chrono::steady_clock::now();

    pass_tracks_acquired_ = 0;
    pass_tracks_dropped_ = 0;
    pass_rwr_locks_ = 0;
    pass_rwr_launches_ = 0;
    pass_missiles_launched_ = 0;
    pass_missiles_detonated_ = 0;
    pass_damage_events_ = 0;
    pass_kills_ = 0;
    pass_gun_bursts_ = 0;
    pass_wvr_engagements_ = 0;

    const double sim_dt = sim_->scenario().sim_dt > 0.0
        ? sim_->scenario().sim_dt
        : (1.0 / 60.0);
    const double target =
        pass_t0_ + static_cast<double>(opts_.horizon_sec);
    const auto pass_wall_start = std::chrono::steady_clock::now();
    int stalled_advances = 0;

    // The loop: 4-sim-second tick batches (the C5/M4 drain discipline).
    while (sim_->sim_time_s() < target) {
        const double before = sim_->sim_time_s();
        const double batch_target = std::min(before + 4.0, target);
        int steps = 0;
        while (sim_->sim_time_s() < batch_target && steps < 240) {
            sim_->tick(sim_dt);
            ++steps;
        }
        if (sim_->sim_time_s() <= before + 1e-9) {
            ++stalled_advances;
            if (stalled_advances > 64) {
                report_.aborted = true;
                report_.abort_reason =
                    "sim clock stopped advancing (sim " +
                    std::to_string(sim_->sim_time_s()) +
                    "s, run " + std::to_string(run) + ")";
                sim_ = nullptr;
                return;
            }
        } else {
            stalled_advances = 0;
        }

        while (sim_->sim_time_s() >= pass_next_sample_t_ &&
               pass_next_sample_t_ <= target) {
            sample_(run);
            const double next = pass_next_sample_t_ + opts_.sample_sec;
            pass_next_sample_t_ = next;
            if (on_sample != nullptr && run == 0 && !report_.diary.empty()) {
                on_sample(report_.diary.back());
            }
        }

        if (opts_.max_wall_sec_total > 0.0) {
            const std::chrono::duration<double> spent =
                std::chrono::steady_clock::now() - pass_wall_start;
            if (spent.count() > opts_.max_wall_sec_total) {
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
    // run re-derives the digest, and run 1+ compares the BYTES.
    const auto* rec = sim_->recorder();
    const auto json = (rec != nullptr) ? rec->to_json("wvr_merge")
                                       : std::string{};
    const auto md5 = md5_hex(json);
    if (run == 0) {
        report_.recorder_json = json;
        report_.verdict.recorder_md5_run0 = md5;
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
        report_.tracks_acquired = pass_tracks_acquired_;
        report_.tracks_dropped = pass_tracks_dropped_;
        report_.rwr_locks = pass_rwr_locks_;
        report_.rwr_launches = pass_rwr_launches_;
        report_.missiles_launched = pass_missiles_launched_;
        report_.missiles_detonated = pass_missiles_detonated_;
        report_.damage_events = pass_damage_events_;
        report_.kills = pass_kills_;
        report_.gun_bursts = pass_gun_bursts_;
        report_.wvr_engagements = pass_wvr_engagements_;
        report_.samples = pass_samples_;
    } else {
        report_.verdict.recorder_md5_run1 = md5;
        if (json != report_.recorder_json) {
            report_.verdict.deterministic = false;
        }
    }

    sim_ = nullptr;
}

// ===========================================================================
// Sample collection + per-sample gates
// ===========================================================================

void WvrMergeHarness::sample_(int run) {
    WvrMergeSample s;
    s.sample = pass_samples_ + 1;
    s.sim_time_s = sim_->sim_time_s();
    s.initial_entities = pass_initial_entities_;
    s.live_entities = static_cast<int>(sim_->world().size());
    s.live_missiles = static_cast<int>(
        weapons::count_live_missiles(sim_->world()));

    // Refresh cumulative combat-event counters from the recorder.
    const auto* rec = sim_->recorder();
    int tracks_acq = 0, tracks_dr = 0, locks = 0, launches = 0;
    int msl_launched = 0, msl_detonated = 0, dmg = 0, kills = 0;
    int gun_bursts = 0, wvr_eng = 0;
    if (rec != nullptr) {
        for (const auto& e : rec->combat_events()) {
            switch (static_cast<int>(e.kind)) {
                case kTrackAcquired:    ++tracks_acq;    break;
                case kTrackDropped:     ++tracks_dr;     break;
                case kRwrLock:          ++locks;         break;
                case kRwrLaunch:        ++launches;      break;
                case kMissileLaunched:  ++msl_launched;  break;
                case kMissileDetonated: ++msl_detonated; break;
                case kDamageApplied:    ++dmg;           break;
                case kEntityKilled:     ++kills;         break;
                case kGunFired:         ++gun_bursts;    break;
                case kWvrEngaged:       ++wvr_eng;       break;
                default: break;  // bomb + disengage events not gated here
            }
        }
    }
    s.tracks_acquired = tracks_acq;
    s.tracks_dropped = tracks_dr;
    s.rwr_locks = locks;
    s.rwr_launches = launches;
    s.missiles_launched = msl_launched;
    s.missiles_detonated = msl_detonated;
    s.damage_events = dmg;
    s.kills = kills;
    s.gun_bursts = gun_bursts;
    s.wvr_engagements = wvr_eng;

    // Spawned/retired deltas from the previous sample (the M4 identity
    // walk, unchanged).
    if (pass_first_sample_) {
        s.spawned_entities = s.live_entities - s.initial_entities;
        s.retired_entities = 0;
    } else {
        const int live_delta = s.live_entities - pass_prev_.live_entities;
        if (live_delta > 0) {
            s.spawned_entities = pass_prev_.spawned_entities + live_delta;
            s.retired_entities = pass_prev_.retired_entities;
        } else if (live_delta < 0) {
            s.spawned_entities = pass_prev_.spawned_entities;
            s.retired_entities = pass_prev_.retired_entities + (-live_delta);
        } else {
            s.spawned_entities = pass_prev_.spawned_entities;
            s.retired_entities = pass_prev_.retired_entities;
        }
    }

    // Per-sample pulse.
    s.sample_launches = s.missiles_launched - pass_prev_.missiles_launched;
    s.sample_detonations =
        s.missiles_detonated - pass_prev_.missiles_detonated;
    s.sample_gun_bursts = s.gun_bursts - pass_prev_.gun_bursts;
    s.sample_kills = s.kills - pass_prev_.kills;

    // Telemetry (diary only; never a verdict).
    const auto now = std::chrono::steady_clock::now();
    const std::chrono::duration<double> wall = now - pass_sample_wall_;
    s.wall_sec = wall.count();
    s.ticks_per_sec = (s.sim_time_s - pass_prev_.sim_time_s) /
                      (s.wall_sec > 0.0 ? s.wall_sec : 1.0);
    s.rss_kb = current_rss_kb();
    pass_sample_wall_ = now;

    if (run == 0) {
        report_.diary.push_back(s);
        check_sample_(s);
    }

    pass_tracks_acquired_ = tracks_acq;
    pass_tracks_dropped_ = tracks_dr;
    pass_rwr_locks_ = locks;
    pass_rwr_launches_ = launches;
    pass_missiles_launched_ = msl_launched;
    pass_missiles_detonated_ = msl_detonated;
    pass_damage_events_ = dmg;
    pass_kills_ = kills;
    pass_gun_bursts_ = gun_bursts;
    pass_wvr_engagements_ = wvr_eng;

    pass_prev_ = s;
    pass_first_sample_ = false;
    ++pass_samples_;
}

void WvrMergeHarness::check_sample_(const WvrMergeSample& s) {
    // --- ROSTER BOUNDED (the M4 identity) -------------------------------
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

    // --- FIGHT ALIVE (pre-engage) ---------------------------------------
    // By the second sample the radar should have detected (the M4 rule —
    // scan_interval_s = 1.0 s, two samples = two scan windows).
    if (s.sample >= 2 && s.tracks_acquired == 0 &&
        report_.verdict.fight_alive) {
        report_.verdict.fight_alive = false;
        report_.verdict.fight_stall =
            "sample " + std::to_string(s.sample) +
            " (sim " + std::to_string(s.sim_time_s) +
            "s): no RadarTrackAcquired event by the second sample — "
            "no brain's radar ever detected anything (scan volume / "
            "rng_seed / spawn geometry may be wrong)";
    }
}

// ===========================================================================
// Final verdict derivation
// ===========================================================================

void WvrMergeHarness::finalize_() {
    if (report_.aborted) return;

    // --- The engagement window (run 0's events) -------------------------
    bool kill_attributed = false;
    std::uint64_t killer_id = 0;
    std::uint64_t victim_id = 0;
    double first_kill_s = -1.0;
    double first_detect_s = -1.0;
    double first_wvr_engage_s = -1.0;
    double last_wvr_engage_s = -1.0;
    double first_wvr_disengage_s = -1.0;
    double first_launch_s = -1.0;
    double first_gun_s = -1.0;
    int missile_shots = 0;
    int missile_hits = 0;
    int missile_misses = 0;
    int gun_bursts = 0;
    int kills = 0;

    for (const auto& e : run0_events_) {
        switch (e.kind) {
            case kTrackAcquired:
                if (first_detect_s < 0.0) first_detect_s = e.sim_time_s;
                break;
            case kWvrEngaged:
                if (first_wvr_engage_s < 0.0)
                    first_wvr_engage_s = e.sim_time_s;
                last_wvr_engage_s = e.sim_time_s;
                break;
            case kWvrDisengaged:
                if (first_wvr_disengage_s < 0.0)
                    first_wvr_disengage_s = e.sim_time_s;
                break;
            case kMissileLaunched:
                ++missile_shots;
                if (first_launch_s < 0.0) first_launch_s = e.sim_time_s;
                break;
            case kMissileDetonated:
                if (e.end_cause == kEndCauseTargetHit) ++missile_hits;
                else ++missile_misses;
                break;
            case kGunFired:
                ++gun_bursts;
                if (first_gun_s < 0.0) first_gun_s = e.sim_time_s;
                break;
            case kEntityKilled:
                ++kills;
                if (first_kill_s < 0.0) {
                    first_kill_s = e.sim_time_s;
                    victim_id = e.subject_id;
                    killer_id = e.object_id;
                }
                break;
            default: break;
        }
    }

    // Attribution (the WVR rule): the killer fired a missile OR a gun.
    if (first_kill_s >= 0.0) {
        for (const auto& e : run0_events_) {
            if ((e.kind == kMissileLaunched || e.kind == kGunFired) &&
                e.subject_id == killer_id) {
                kill_attributed = true;
                break;
            }
        }
    }
    report_.verdict.engagement_completed = kill_attributed;
    if (!kill_attributed && !run0_events_.empty()) {
        // Name the failure rung (the M4 ladder, WVR's rungs).
        if (first_detect_s < 0.0) {
            report_.verdict.engagement_failure =
                "no RadarTrackAcquired event — the chain stopped before "
                "detection (radar scan volume / spawn geometry / rng_seed)";
        } else if (first_wvr_engage_s < 0.0) {
            report_.verdict.engagement_failure =
                "no WvrEngaged event — the chain stopped at the band "
                "boundary (the brain never handed the fight to the WVR "
                "rung; entry geometry / band constants; "
                "first_detect_s=" + std::to_string(first_detect_s) + "s)";
        } else if (first_launch_s < 0.0 && first_gun_s < 0.0) {
            report_.verdict.engagement_failure =
                "no MissileLaunched and no GunFired event — the chain "
                "stopped at the employment rung (hold_fire, envelope "
                "gates, or the merge never committed; "
                "first_wvr_engage_s=" +
                std::to_string(first_wvr_engage_s) + "s)";
        } else if (first_kill_s < 0.0) {
            report_.verdict.engagement_failure =
                "no EntityKilled event — the chain stopped at the "
                "flyout/fuze/burst rung (seeker lost, fuze radius, or "
                "the bursts missed; missile_shots=" +
                std::to_string(missile_shots) + ", missile_hits=" +
                std::to_string(missile_hits) + ", gun_bursts=" +
                std::to_string(gun_bursts) + ")";
        } else {
            report_.verdict.engagement_failure =
                "kill occurred but attribution failed — the killer_id " +
                std::to_string(killer_id) +
                " matches no MissileLaunched/GunFired subject in the run "
                "(a kill with no shot is a harness bug or an unmapped "
                "damage source)";
        }
    }

    // --- FIGHT ALIVE (finalize side) ------------------------------------
    // The WVR contents: the brain reached the band (>= 1 WvrEngaged
    // event). The pre-engage side (tracks_acquired) ran per-sample.
    if (report_.verdict.fight_alive && first_detect_s >= 0.0 &&
        first_wvr_engage_s < 0.0) {
        report_.verdict.fight_alive = false;
        report_.verdict.fight_stall =
            "the brain(s) detected (first_detect_s=" +
            std::to_string(first_detect_s) + "s) but never entered the "
            "WVR band — no WvrEngaged event (the band handoff geometry, "
            "the archetype's WVREngage gate, or combat_mode never "
            "reached WVR)";
    }

    // --- The window + counters ------------------------------------------
    report_.verdict.first_detect_s = first_detect_s;
    report_.verdict.first_wvr_engage_s = first_wvr_engage_s;
    report_.verdict.last_wvr_engage_s = last_wvr_engage_s;
    report_.verdict.first_wvr_disengage_s = first_wvr_disengage_s;
    report_.verdict.first_launch_s = first_launch_s;
    report_.verdict.first_gun_s = first_gun_s;
    report_.verdict.first_kill_s = first_kill_s;
    report_.verdict.missile_shots = missile_shots;
    report_.verdict.missile_hits = missile_hits;
    report_.verdict.missile_misses = missile_misses;
    report_.verdict.gun_bursts = gun_bursts;
    report_.verdict.kills = kills;

    // --- DETERMINISTIC (set in run_pass_; runs == 1 vacuously true) -----
}

} // namespace f4::simulation
