// f4-ai/include/f4/ai/modules/bvr_module.hpp
//
// BVRModule — beyond-visual-range engagement tactics (AI_IMPLEMENTATION_PLAN.md
// §5 Step 8; FreeFalcon bvrengage.cpp / dlogic.cpp BVR decision).
//
// The module owns the OFFENSIVE side of a BVR fight:
//
//   None      — no visible hostile fighter, or one far outside the fight
//               (range > 1.3 * weapon max range): the brain falls back to
//               its mission module (navigation). Output is empty.
//   Entering  — hostile inside the BVR entry ring but outside the
//               employment envelope: close in, radar to STT (lock intent),
//               accelerate.
//   Employing — inside the employment envelope: fire control decides when
//               to shoot (Pk threshold + cooldown + shoot-shoot limit).
//               Immediately after a shot the tactic flips to Crank (offset
//               ~45 deg off the target bearing) for a support window, then
//               reassesses: shoot again, or separate.
//   Separating— bug out: turn cold (180 deg off the target bearing), full
//               throttle, until the range reopens past the entry ring.
//               Reached when the merge gets too close (2 NM), the shot
//               allotment is spent, or the target is gone (dead — the
//               host-side policy stops painting corpses).
//
// Steering is AirSteering (bank-to-turn + gamma-hold + speed PI), the same
// cascades NavigationModule flies — only the target selection differs
// (lead pursuit / crank / cold instead of legs).
//
// ENGINE-AGNOSTIC CONTRACT: the module consumes TargetInfo snapshots (from
// SensorFusion) and IAircraftState. It NEVER touches the world, the bus,
// f4-weapons, or f4-sensors. The two real-world effects it wants —
// an STT radar lock and a missile off the rail — leave as INTENTS:
//
//   wants_lock()       — true while the brain should hold STT (the host
//                        driver feeds RadarSimComponent::command_track).
//   release_pulse()   — true for EXACTLY ONE tick per shot; the host
//                        driver converts it into launch_missile() through
//                        the simulation's weapon table.
//
// FreeFalcon validation hooks (plan §5 Step 8 table) covered by tests:
//   crank offset 30-60 deg from target bearing
//   range-band transitions (BVR/WVR/merge)
//   5-second tactic re-evaluation interval
//   fire-at-MAR with Pk threshold, cooldown, shoot-shoot
//
// Dependencies: f4-state-machine, f4-geo, f4-flight-api (IAircraftState),
// f4-ai (AirSteering, AIControlOutput, TargetInfo, MissileModule). C++20.

#pragma once

#include <cstdint>
#include <string>

#include <f4/fsm/state_machine.hpp>
#include <f4/fsm/trace.hpp>
#include <f4/flight/api/i_aircraft_state.hpp>
#include <f4/geo/position.hpp>

#include "f4/ai/ai_output.hpp"
#include "f4/ai/air_steering.hpp"
#include "f4/ai/target_info.hpp"
#include "f4/ai/modules/missile_module.hpp"

namespace f4::ai::modules {

// ============================================================================
// States, events, tactics
// ============================================================================
enum class BVRState {
    None,        // no fight — output empty, brain flies the mission
    Entering,    // closing to the employment envelope
    Employing,   // in the envelope / supporting a shot
    Separating   // bug out (cold, full throttle) until the range reopens
};

enum class BVREvent {
    TargetDetected,     // hostile fighter inside the BVR entry ring
    InRange,            // inside the employment envelope
    WeaponFired,        // fire control released a shot
    ThreatDetected,     // (reserved for defensive handoff, M3+)
    BugOut,             // merge too close / shots spent / doctrine
    SeparationComplete, // range reopened past the entry ring
    LostTarget          // target dead or no longer visible
};

/// Tactic subset of FreeFalcon's BVRInterceptType actually flown here.
/// (The full 22-value enum from the plan maps onto these five for a
/// single-ship fight; profiles arrive with WingmanModule, which is what
/// makes SSO/Pince/etc. meaningful.)
enum class BVRTactic {
    None,
    FollowWaypoints,   // None state — brain's mission module flies
    Pursuit,           // lead-pursuit heading on the target
    Crank,             // offset off the target bearing post-shot
    Notch,             // (defensive beam — reserved, MissileModule covers it)
    BugOut             // cold 180 deg
};

/// Which range band a target sits in (plan §5 Step 8 constants;
/// dlogic.cpp range checks). Queries are pure functions of range.
enum class BVRRangeBand {
    OutOfEnvelope,  // beyond BVR entry (1.3 * max range) — not our fight yet
    BVR,            // entry ring .. employment envelope
    Employ,         // inside the employment envelope
    WVR,            // inside 3 NM — WVRModule territory (not yet built)
    Merge           // inside 2 NM — bug out
};

// ============================================================================
// BVRModule
// ============================================================================
class BVRModule {
public:
    /// Employment + doctrine parameters. The host configures the envelope
    /// from the weapon class table (bvr().fire().set_envelope_nm(...));
    /// plan defaults match MissileModule::Config.
    struct Config {
        /// BVR entry ring = entry_range_mult * max_pk_range_nm (plan
        /// BVR_ENTRY_RANGE_MULT = 1.3).
        double entry_range_mult{1.3};
        /// WVR handoff band (plan WVR_ENTRY_RANGE_NM).
        double wvr_entry_range_nm{3.0};
        /// Bug-out merge range (plan SEPARATE_RANGE_NM).
        double separate_range_nm{2.0};
        /// Tactic re-evaluation interval (plan: 5 s, digimain.cpp).
        double re_eval_interval_sec{5.0};
        /// Crank offset off the target bearing (plan: 30-60 deg; 45 mid).
        double crank_offset_rad{0.7853981633974483};  // 45 deg
        /// Support window after each shot before reassessing (crank hold).
        double crank_hold_sec{8.0};
        /// Separation is "complete" once the range exceeds this multiple
        /// of the entry ring (hysteresis so a still-flying corpse can't
        /// yo-yo the module between states).
        double separation_complete_mult{1.2};
        /// Speed target while fighting (kts CAS).
        double engage_speed_kts{450.0};
    };

    BVRModule();

    // --- Per-tick update -------------------------------------------------
    // `target` may be nullptr (SensorFusion sees no hostile fighter) or may
    // point at a target the module cannot fight (not visible / not hostile /
    // a missile — the brain filters, the module double-checks). Same
    // contract as NavigationModule::update: caches state, fires transitions,
    // returns the control output for the current tactic. Output is EMPTY
    // while state == None so the brain can detect "no fight" cheaply.
    AIControlOutput update(double dt, const flight::IAircraftState* state,
                           const TargetInfo* target);

    // --- Intents (read by the host's combat driver each tick) -----------
    /// True while the brain wants STT radar lock on the engagement target.
    [[nodiscard]] bool wants_lock() const noexcept { return wants_lock_; }
    /// EntityId::value of the target to lock (0 when no lock wanted).
    [[nodiscard]] std::uint64_t lock_target_id() const noexcept {
        return engagement_target_id_;
    }
    /// True for EXACTLY ONE update() per shot (the host driver turns this
    /// into launch_missile(); the module never fires anything itself).
    [[nodiscard]] bool release_pulse() const noexcept { return release_pulse_; }
    /// EntityId::value of the shot's assigned target (0 when no pulse).
    [[nodiscard]] std::uint64_t release_target_id() const noexcept {
        return release_pulse_ ? engagement_target_id_ : 0u;
    }

    // --- State reporting --------------------------------------------------
    [[nodiscard]] BVRState state() const noexcept { return sm_.current(); }
    [[nodiscard]] BVRTactic tactic() const noexcept { return tactic_; }
    [[nodiscard]] int shots_fired() const noexcept {
        return fire_.shots_fired();
    }
    [[nodiscard]] const fsm::Trace<BVRState, BVREvent>* trace() const
        noexcept { return sm_.trace(); }
    [[nodiscard]] std::string state_name() const;
    [[nodiscard]] std::string tactic_name() const;

    // --- Configuration ------------------------------------------------------
    /// The embedded fire control (envelope, Pk threshold, cooldown,
    /// shoot-shoot). The host sets the envelope from the weapon table.
    [[nodiscard]] MissileModule& fire() noexcept { return fire_; }
    [[nodiscard]] const MissileModule& fire() const noexcept { return fire_; }
    [[nodiscard]] Config& config() noexcept { return cfg_; }
    [[nodiscard]] const Config& config() const noexcept { return cfg_; }

    /// The shared AirSteering instance (public fields — combat tune).
    [[nodiscard]] AirSteering& air_steering() noexcept { return air_steering_; }

    // --- Pure queries (unit-test surface) ------------------------------------
    /// Range band for a target at `range_nm` (plan §5 Step 8 validation).
    [[nodiscard]] BVRRangeBand band_for(double range_nm) const;
    /// BVR entry ring in NM (entry_range_mult * max employment range).
    [[nodiscard]] double entry_range_nm() const {
        return cfg_.entry_range_mult * fire_.config().max_pk_range_nm;
    }
    /// Desired heading (rad, CW from north) for the current tactic —
    /// exposed for tests + the FCS trace exporter.
    [[nodiscard]] double desired_heading_rad() const noexcept {
        return desired_heading_rad_;
    }

    /// Hard reset (new engagement / host re-task). Clears the engagement
    /// target, shots, cooldown, and returns the SM to None.
    void reset();

private:
    // --- FSM ---------------------------------------------------------------
    static fsm::StateMachine<BVRState, BVREvent> build_sm();

    // --- Steering helpers -----------------------------------------------------
    /// Lead-pursuit bearing to the target (pure pursuit with a lead point
    /// at the target's velocity * time-to-go estimate).
    [[nodiscard]] double pursuit_heading_rad(
        const flight::IAircraftState& own) const;
    /// Target bearing from ownship (rad, CW from north).
    [[nodiscard]] double target_bearing_rad(
        const flight::IAircraftState& own) const;
    [[nodiscard]] AirSteering::Input steering_input(
        const flight::IAircraftState& s) const noexcept;

    void engage(const TargetInfo& target);
    void clear_engagement();

    // --- FSM + tactic state ---------------------------------------------------
    fsm::StateMachine<BVRState, BVREvent> sm_;
    Config cfg_{};
    BVRTactic tactic_{BVRTactic::None};
    AirSteering air_steering_{};

    MissileModule fire_{};             // offensive fire control only

    std::uint64_t engagement_target_id_{0};
    bool wants_lock_{false};
    bool release_pulse_{false};

    // Timers (seconds, decremented by update's dt).
    double re_eval_timer_{0.0};
    double crank_timer_{0.0};
    double separation_timer_{0.0};

    // Engagement targets for steering (captured at Entering entry).
    double engage_alt_ft_{0.0};

    // Cached ownship/target picture for this tick (steering helpers).
    const TargetInfo* target_{nullptr};
    double current_heading_rad_{0.0};
    double current_pitch_rad_{0.0};
    double current_roll_rad_{0.0};
    double current_roll_rate_radps_{0.0};
    double current_pitch_rate_radps_{0.0};
    double current_vs_fpm_{0.0};
    double current_vcas_kts_{0.0};
    double current_alt_msl_ft_{0.0};
    geo::WorldPosition current_position_{};
    double desired_heading_rad_{0.0};
};

} // namespace f4::ai::modules
