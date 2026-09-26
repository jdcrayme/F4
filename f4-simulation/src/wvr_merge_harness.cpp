// f4-simulation/src/wvr_merge_harness.cpp
//
// WvrMergeHarness — the M5a end-to-end WVR / guns merge acceptance run
// (design: wvr_merge_harness.hpp). Headless, deterministic,
// instrumented. The structural sibling of BvrInterceptHarness (M4).
//
// The run skeleton is single-sourced in chain_engine.hpp; what remains
// here is the WVR harness's own logic: the gun + band-event tallies
// (GunFired, WvrEngaged), the fight-alive pre-engage gate, the headline
// counters, and the verdict derivation (the WVR attribution rule: the
// killer fired a missile OR a gun — the guns fight's kill rides the gun
// damage path, missile_id == 0).

#include <f4/simulation/wvr_merge_harness.hpp>

#include "chain_engine.hpp"

#include <f4/simulation/simulation.hpp>
#include <f4/weapons/missile_battery.hpp>

#include <string>
#include <vector>

namespace f4::simulation {

namespace {

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

/// The WVR band-flip kinds. WVR-only (kinds 11/12 in the combat-event
/// table); not in harness_shared's common block.
constexpr int kWvrEngaged = 11;
constexpr int kWvrDisengaged = 12;

/// The WVR-specific logic the engine calls into.
struct WvrHooks {
    /// The two extra tallies this harness keeps alongside the common eight.
    struct Extras {
        int gun_bursts = 0;
        int wvr_engagements = 0;
    };
    static constexpr const char* recorder_tag = "wvr_merge";

    static void reset_pass_extras(chain_engine::State<Extras>& st) {
        st.extra = Extras{};
    }
    static void reset_sample_extras(chain_engine::State<Extras>& st) {
        st.extra.gun_bursts = 0;
        st.extra.wvr_engagements = 0;
    }
    static void count_event(chain_engine::State<Extras>& st, int kind) {
        switch (kind) {
            case kGunFired:   ++st.extra.gun_bursts;       break;
            case kWvrEngaged: ++st.extra.wvr_engagements;  break;
            default: break;  // bomb + disengage events not gated here
        }
    }

    /// The extra cumulative fields + this sample's pulse.
    static void fill_sample_extras(const chain_engine::State<Extras>& st,
                                   const WvrMergeSample& prev,
                                   WvrMergeSample& s) {
        s.gun_bursts = st.extra.gun_bursts;
        s.wvr_engagements = st.extra.wvr_engagements;
        s.sample_launches = s.missiles_launched - prev.missiles_launched;
        s.sample_detonations =
            s.missiles_detonated - prev.missiles_detonated;
        s.sample_gun_bursts = s.gun_bursts - prev.gun_bursts;
        s.sample_kills = s.kills - prev.kills;
    }

    /// FIGHT ALIVE (pre-engage): by the second sample the radar has had
    /// two scan windows (scan_interval_s = 1.0 s). The engage-side of
    /// the gate (band entry) is finalized in finalize_gates.
    static void check_sample_alive(const chain_engine::State<Extras>&,
                                   WvrMergeReport& report_,
                                   const WvrMergeSample& s) {
        if (s.sample >= 2 && s.tracks_acquired == 0 &&
            report_.verdict.fight_alive) {
            report_.verdict.fight_alive = false;
            report_.verdict.fight_stall =
                "sample " + std::to_string(s.sample) +
                " (sim " + std::to_string(s.sim_time_s) +
                "s): no radar detected anything by the second sample "
                "(scan volume / rng_seed / spawn geometry may be wrong)";
        }
    }

    /// Run 0's headline counters (run 0, end of horizon).
    static void copy_headline(const chain_engine::State<Extras>& st,
                              WvrMergeReport& r) {
        r.tracks_acquired = st.tracks_acquired;
        r.tracks_dropped = st.tracks_dropped;
        r.rwr_locks = st.rwr_locks;
        r.rwr_launches = st.rwr_launches;
        r.missiles_launched = st.missiles_launched;
        r.missiles_detonated = st.missiles_detonated;
        r.damage_events = st.damage_events;
        r.kills = st.kills;
        r.gun_bursts = st.extra.gun_bursts;
        r.wvr_engagements = st.extra.wvr_engagements;
    }

    /// The engagement window + ENGAGEMENT COMPLETED + FIGHT ALIVE
    /// (engage-side: detection AND band entry), the WVR way.
static void finalize_gates(const chain_engine::State<Extras>&,
                           WvrMergeReport& report_,
                           const std::vector<chain_engine::EventRow>& run0_events_) {

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
}

};

using WvrEngineBase =
    chain_engine::Engine<WvrMergeHarnessOptions, WvrMergeSample,
                         WvrMergeVerdict, WvrMergeReport, WvrHooks>;

} // namespace

struct WvrMergeHarness::Engine final : WvrEngineBase {};

std::unique_ptr<WvrMergeHarness>
WvrMergeHarness::create(const WvrMergeHarnessOptions& opts,
                        std::string* error) {
    if (!WvrEngineBase::validate_options(opts, "wvr harness", error)) {
        return nullptr;
    }
    auto harness = std::unique_ptr<WvrMergeHarness>(new WvrMergeHarness());
    harness->engine_ = std::make_unique<WvrMergeHarness::Engine>();
    harness->opts_ = opts;
    return harness;
}

WvrMergeHarness::~WvrMergeHarness() = default;

const WvrMergeReport& WvrMergeHarness::execute(ProgressFn on_sample) {
    return engine_->execute(opts_, std::move(on_sample));
}

const WvrMergeReport& WvrMergeHarness::report() const noexcept {
    return engine_->report();
}

} // namespace f4::simulation
