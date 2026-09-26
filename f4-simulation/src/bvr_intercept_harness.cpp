// f4-simulation/src/bvr_intercept_harness.cpp
//
// BvrInterceptHarness — the M4 end-to-end BVR intercept acceptance run
// (design: bvr_intercept_harness.hpp). Headless, deterministic,
// instrumented.
//
// The run skeleton (create validation, the pre-flight combat refusal,
// the tick-batch loop with the stall/wall watchdogs, the sample cadence,
// the roster-census identity, and the recorder-byte MD5 certificate) is
// single-sourced in chain_engine.hpp; what remains here is the BVR
// harness's own logic: which events it tallies, the fight-alive
// pre-engage gate, the headline counters, and the verdict derivation.

#include <f4/simulation/bvr_intercept_harness.hpp>

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

/// The BVR-specific logic the engine calls into. BVR tallies only the
/// eight common chain kinds (no gun/bomb extras), so most hooks are
/// no-ops; the fight-alive gate and the verdict derivation are the meat.
struct BvrHooks {
    using Extras = chain_engine::EmptyExtras;
    static constexpr const char* recorder_tag = "bvr_intercept";

    static void reset_pass_extras(chain_engine::State<Extras>&) {}
    static void reset_sample_extras(chain_engine::State<Extras>&) {}
    static void count_event(chain_engine::State<Extras>&, int) {}

    /// The per-sample pulse (this sample's new events).
    static void fill_sample_extras(const chain_engine::State<Extras>&,
                                   const InterceptSample& prev,
                                   InterceptSample& s) {
        s.sample_launches = s.missiles_launched - prev.missiles_launched;
        s.sample_detonations =
            s.missiles_detonated - prev.missiles_detonated;
        s.sample_kills = s.kills - prev.kills;
    }

    /// FIGHT ALIVE (pre-engage): by the second sample the radar has had
    /// two scan windows (scan_interval_s = 1.0 s); zero tracks then is a
    /// stalled fight, not a quiet one. The engage-side of the gate is
    /// finalized in finalize_gates.
    static void check_sample_alive(const chain_engine::State<Extras>&,
                                   InterceptReport& report_,
                                   const InterceptSample& s) {
        if (s.sample >= 2 && s.tracks_acquired == 0 &&
            report_.verdict.fight_alive) {
            report_.verdict.fight_alive = false;
            report_.verdict.fight_stall =
                "sample " + std::to_string(s.sample) +
                " (sim " + std::to_string(s.sim_time_s) +
                "s): no RadarTrackAcquired event by the second sample — "
                "the shooter's radar never detected anything (scan "
                "volume / rng_seed / spawn geometry may be wrong)";
        }
    }

    /// Run 0's headline counters (run 0, end of horizon).
    static void copy_headline(const chain_engine::State<Extras>& st,
                              InterceptReport& r) {
        r.tracks_acquired = st.tracks_acquired;
        r.tracks_dropped = st.tracks_dropped;
        r.rwr_locks = st.rwr_locks;
        r.rwr_launches = st.rwr_launches;
        r.missiles_launched = st.missiles_launched;
        r.missiles_detonated = st.missiles_detonated;
        r.damage_events = st.damage_events;
        r.kills = st.kills;
    }

    /// ENGAGEMENT COMPLETED + FIGHT ALIVE (engage-side) + the window.
static void finalize_gates(const chain_engine::State<Extras>&,
                           InterceptReport& report_,
                           const std::vector<chain_engine::EventRow>& run0_events_) {

    // --- ENGAGEMENT COMPLETED -----------------------------------------
    // At least one EntityKilled with attribution. Attribution: the
    // killer (object_id) matches the shooter_id of some
    // MissileLaunched (or GunFired) event in the run. The M4 contract:
    // a fight that runs to a kill, or fails with a named failure class.
    bool kill_attributed = false;
    std::uint64_t killer_id = 0;
    std::uint64_t victim_id = 0;
    double first_kill_s = -1.0;
    double first_detect_s = -1.0;
    double first_launch_s = -1.0;
    int shots_fired = 0;
    int shots_hit = 0;
    int shots_missed = 0;

    // First pass: collect the launch/detect/kill windows + count shots.
    for (const auto& e : run0_events_) {
        switch (e.kind) {
            case kTrackAcquired:
                if (first_detect_s < 0.0) first_detect_s = e.sim_time_s;
                break;
            case kMissileLaunched:
                ++shots_fired;
                if (first_launch_s < 0.0) first_launch_s = e.sim_time_s;
                break;
            case kMissileDetonated:
                if (e.end_cause == kEndCauseTargetHit) ++shots_hit;
                else ++shots_missed;
                break;
            case kEntityKilled:
                if (first_kill_s < 0.0) {
                    first_kill_s = e.sim_time_s;
                    victim_id = e.subject_id;
                    killer_id = e.object_id;
                }
                break;
            default: break;
        }
    }

    // Attribution: the killer must have launched (or fired) at the
    // victim. Scan for a MissileLaunched with shooter == killer. (Gun
    // kills are a separate M4 variant; the BVR harness attributes via
    // missile launches.)
    if (first_kill_s >= 0.0) {
        for (const auto& e : run0_events_) {
            if (e.kind == kMissileLaunched && e.subject_id == killer_id) {
                kill_attributed = true;
                break;
            }
        }
    }
    report_.verdict.engagement_completed = kill_attributed;
    if (!kill_attributed && !run0_events_.empty()) {
        // Name the failure rung.
        if (first_detect_s < 0.0) {
            report_.verdict.engagement_failure =
                "no RadarTrackAcquired event — the chain stopped before "
                "detection (radar scan volume / spawn geometry / rng_seed)";
        } else if (first_launch_s < 0.0) {
            report_.verdict.engagement_failure =
                "no MissileLaunched event — the chain stopped at the BVR "
                "fire-control rung (MAR/Pk gate, cooldown, or hold_fire "
                "blocked the shot; first_detect_s=" +
                std::to_string(first_detect_s) + "s)";
        } else if (first_kill_s < 0.0) {
            report_.verdict.engagement_failure =
                "no EntityKilled event — the chain stopped at the missile "
                "flyout/fuze rung (seeker lost, fuze radius, or tof_limit; "
                "shots_fired=" + std::to_string(shots_fired) +
                ", shots_hit=" + std::to_string(shots_hit) +
                ", shots_missed=" + std::to_string(shots_missed) + ")";
        } else {
            report_.verdict.engagement_failure =
                "kill occurred but attribution failed — the killer_id " +
                std::to_string(killer_id) +
                " matches no MissileLaunched.shooter_id in the run "
                "(a kill with no launch is a harness bug or an unmapped "
                "damage source)";
        }
    }

    // --- FIGHT ALIVE (finalize) ---------------------------------------
    // The finalize side: the brain reached at least BVRState::Entering.
    // Proxy: at least one MissileLaunched (the brain employed) OR at
    // least one RwrLock (the victim's RWR saw the shooter's STT — the
    // shooter locked, which requires the BVR rung). The pre-engage
    // side (tracks_acquired > 0) was checked per-sample.
    if (report_.verdict.fight_alive && first_detect_s >= 0.0) {
        // The brain detected. Confirm it employed by checking for any
        // BVR-relevant event after detection.
        bool brain_engaged = (first_launch_s >= 0.0);  // fired
        if (!brain_engaged) {
            // Fall back to RWR lock — the shooter's STT was commanded
            // (BVRModule::wants_lock → execute_brain_combat_intents →
            // RadarSimComponent::command_track → the victim's RWR sees
            // the lock).
            for (const auto& e : run0_events_) {
                if (e.kind == kRwrLock) { brain_engaged = true; break; }
            }
        }
        if (!brain_engaged) {
            report_.verdict.fight_alive = false;
            report_.verdict.fight_stall =
                "the shooter detected (first_detect_s=" +
                std::to_string(first_detect_s) + "s) but never engaged — "
                "no MissileLaunched and no RwrLock event (the BVR rung "
                "may not have reached Entering, or the STT command failed)";
        }
    }

    // --- ENGAGEMENT WINDOW (for the summary) --------------------------
    report_.verdict.first_detect_s = first_detect_s;
    report_.verdict.first_launch_s = first_launch_s;
    report_.verdict.first_kill_s = first_kill_s;
    report_.verdict.shots_fired = shots_fired;
    report_.verdict.shots_hit = shots_hit;
    report_.verdict.shots_missed = shots_missed;
}

};

using BvrEngineBase =
    chain_engine::Engine<InterceptHarnessOptions, InterceptSample,
                         InterceptVerdict, InterceptReport, BvrHooks>;

} // namespace

struct BvrInterceptHarness::Engine final : BvrEngineBase {};

std::unique_ptr<BvrInterceptHarness>
BvrInterceptHarness::create(const InterceptHarnessOptions& opts,
                             std::string* error) {
    if (!Engine::validate_options(opts, "bvr harness", error)) {
        return nullptr;
    }
    auto harness = std::unique_ptr<BvrInterceptHarness>(
        new BvrInterceptHarness());
    harness->engine_ = std::make_unique<BvrInterceptHarness::Engine>();
    harness->opts_ = opts;
    return harness;
}

BvrInterceptHarness::~BvrInterceptHarness() = default;

const InterceptReport& BvrInterceptHarness::execute(ProgressFn on_sample) {
    return engine_->execute(opts_, std::move(on_sample));
}

const InterceptReport& BvrInterceptHarness::report() const noexcept {
    return engine_->report();
}

} // namespace f4::simulation
