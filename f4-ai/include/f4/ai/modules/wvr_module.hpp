// f4-ai/include/f4/ai/modules/wvr_module.hpp
//
// WVRModule — within-visual-range merge tactics (AI_IMPLEMENTATION_PLAN.md
// §5 Step 9; FreeFalcon wvrengage.cpp / merge.cpp / gunsjink.cpp).
//
// The module owns the fight INSIDE the WVR entry band (BVRModule hands
// off at wvr_entry_range_nm, default 3 NM — the plan's "Range band
// transitions (BVR->WVR at 3NM)" validation). It is the last rung of
// the merge: two aircraft that survive the BVR exchange and keep closing
// sort it out here.
//
// States (plan §5 Step 9 WVRState, FreeFalcon wvrengage.cpp):
//
//   None       — nothing to fight (no visible hostile, or the range
//                reopened past the WVR exit ring — that is BVRModule's
//                fight again). Output is empty; the brain flies its
//                mission module.
//   Merge      — hostile inside the band, geometry not yet sorted
//                (both nose-on, or both beaming). Fly lead pursuit on
//                the merge point, hold the weapons picture, wait for
//                an angle to develop.
//   Offensive  — WE have the angle: the target is in our forward cone
//                (ata_from small) and pointed away from us (ata large —
//                we are behind it). Lead pursuit, hold it in the IR
//                seeker cone, employ when the envelope allows.
//   Defensive  — the TARGET has the angle: it is behind us and pointed
//                at us (the guns/IR threat zone). Break turns with a
//                reversing jink (FreeFalcon gunsjink), full throttle,
//                make ourselves a hard zero.
//   BugOut     — disengage attempt: shots spent + the geometry stayed
//                defensive long enough (doctrine — the plan's
//                "Bugout at 2NM separation" behavior, generalized).
//                Cold heading, full throttle; if the range reopens
//                past the exit ring the module goes None and the brain
//                hands the (re-opened) fight back to BVRModule.
//
// Tactics are the plan's 11-value WVRTactic set (§3.6). The subset
// actually flown in this cut: RandP/Straight (Merge), OverB/RandP
// (Offensive, overshoot control), GunJink (Defensive), BugOut. The
// rest (Roop, Avoid, Beam, BeamReturn, RunAway) stay enum values the
// WingmanModule/skill layers will select — one-circle vs two-circle
// geometry needs the formation picture first.
//
// ENGINE-AGNOSTIC CONTRACT (same as BVRModule): the module consumes
// TargetInfo snapshots (from SensorFusion) and IAircraftState. It NEVER
// touches the world, the bus, f4-weapons, or f4-sensors. Its real-world
// effects leave as INTENTS: wants_lock() (the host driver feeds
// RadarSimComponent::command_track — the STT keeps the weapons-grade
// picture the IR fire control gates on) and release_pulse() (true for
// EXACTLY ONE tick per shot; the host driver converts it into
// launch_missile() through the simulation's weapon table, preferring
// the IR-guided stations).
//
// FreeFalcon validation hooks (plan §5 Step 9 table) covered by tests:
//   BVR->WVR transition at 3 NM (BVRModule::band_for — the band tests
//   live in test_bvr_module.cpp; this module's entry guard is the exit
//   ring's mirror)
//   WVR tactic selection by geometry (advantage/threat/neutral)
//   merge / overshoot / jink behavior
//   IR employment inside the close-in envelope
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
enum class WVRState {
    None,        // no fight — output empty, brain flies the mission
    Merge,       // inside the band, geometry not sorted
    Offensive,   // we have the angle (target in our nose, pointed away)
    Defensive,   // target has the angle (behind us, pointed at us)
    BugOut       // separate: cold, full throttle, exit ring watch
};

enum class WVREvent {
    TargetNear,          // hostile visible inside the WVR band
    Advantage,           // geometry: target in our nose + pointed away
    Threat,              // geometry: target behind us + pointed at us
    Neutralized,         // geometry back to neutral (no angle either way)
    Separate,            // doctrine: shots spent + sustained defense
    SeparationComplete,  // range reopened past the exit ring
    LostTarget           // target dead / no longer visible / left the band
};

/// FreeFalcon's WvrTacticType (plan §3.6, 11 values). The subset flown in
/// this cut is documented in the header comment; the rest are selected by
/// later layers (skill profiles, WingmanModule's formation geometry).
enum class WVRTactic {
    None,
    RandP,       // range-and-pulldown: lead/lag blend at the merge
    OverB,       // overshoot control (don't fly through the target)
    Roop,        // (reserved: vertical roop — needs energy modeling)
    GunJink,     // defensive break turns with roll reversals
    Straight,    // pure closure at the merge, weapons tight
    BugOut,      // cold separation
    Avoid,       // (reserved: positional avoid, WingmanModule)
    Beam,        // (reserved: beam missile defense — MissileModule owns it)
    BeamReturn,  // (reserved: re-commit after a beam)
    RunAway      // (reserved: extend on speed advantage)
};

// ============================================================================
// WVRModule
// ============================================================================
class WVRModule {
public:
    /// Doctrine + maneuver parameters. The entry/exit band constants live
    /// where the range taxonomy lives — BVRModule::Config
    /// (wvr_entry_range_nm) owns ENTRY; this config owns EXIT (the brain
    /// reads both, so there is exactly one source per boundary).
    struct Config {
        /// Fight-over ring: past this range the module goes None and the
        /// brain hands the (reopened) fight back to BVRModule. 1.5x the
        /// 3 NM entry band — hysteresis so the merge cannot ping-pong
        /// the brain between rungs.
        double wvr_exit_range_nm{4.5};
        /// Vertical-game clamps: the merge chases the target's altitude,
        /// floored/ceilinged here (the ground is a real opponent below
        /// the floor; thin air above the ceiling).
        double min_alt_ft{3000.0};
        double max_alt_ft{30000.0};
        /// Speed target while maneuvering (kts CAS).
        double engage_speed_kts{450.0};
        /// Speed target while defensive (running - AB).
        double defensive_speed_kts{480.0};
        /// Minimum dwell in a geometry state before a flip is accepted
        /// (anti-chatter: the angles at a merge swing fast; without a
        /// dwell the FSM would strobe Offensive/Defensive every tick).
        double tactic_dwell_sec{2.0};
        /// Sustained-defense grace before doctrine allows the bug-out
        /// (and only once the IR allotment is spent — no leaving while
        /// there are still heaters).
        double defensive_grace_sec{8.0};
        /// Jink reversal period (s) — the break turn flips side.
        double jink_period_sec{3.0};
        /// Break-turn offset off the THREAT bearing (rad). 60 deg: hard
        /// enough to spoil a guns solution, shallow enough to keep the
        /// bandit in the forward hemisphere for a re-counter.
        double jink_offset_rad{1.0471975511965976};  // 60 deg
        /// Altitude weave amplitude while jinking (ft).
        double jink_alt_swing_ft{800.0};
        /// IR opportunity cone: the seeker tracks what is in OUR
        /// forward cone — a heater at a target on our six has nothing
        /// to see. 75 deg (the same forward-cone bound as the Offensive
        /// geometry class): generous for a 20-deg gimbal because the
        /// launch rail flies the aircraft toward the merge anyway, but
        /// strictly forward-hemisphere.
        double fire_cone_rad{1.3089969389957472};  // 75 deg
        /// Overshoot guard: inside this range with hard closure, offset
        /// the pursuit to control the pass (FreeFalcon OverB).
        double overshoot_range_nm{0.35};
        /// Overshoot offset off the pursuit bearing (rad).
        double overshoot_offset_rad{0.7853981633974483};  // 45 deg
    };

    WVRModule();

    // --- Per-tick update -------------------------------------------------
    // `target` may be nullptr (no visible hostile fighter) or a target
    // the module cannot fight (not visible / not hostile / a missile).
    // Same contract as BVRModule::update: caches state, fires transitions,
    // returns the control output for the current tactic. Output is EMPTY
    // while state == None so the brain can detect "no fight" cheaply.
    AIControlOutput update(double dt, const flight::IAircraftState* state,
                           const TargetInfo* target);

    // --- Intents (read by the host's combat driver each tick) -----------
    /// True while the brain should hold STT on the engagement target
    /// (the weapons-grade picture the fire control gates on).
    [[nodiscard]] bool wants_lock() const noexcept { return wants_lock_; }
    /// EntityId::value of the target to lock (0 when no lock wanted).
    [[nodiscard]] std::uint64_t lock_target_id() const noexcept {
        return engagement_target_id_;
    }
    /// True for EXACTLY ONE update() per shot.
    [[nodiscard]] bool release_pulse() const noexcept { return release_pulse_; }
    /// EntityId::value of the shot's assigned target (0 when no pulse).
    [[nodiscard]] std::uint64_t release_target_id() const noexcept {
        return release_pulse_ ? engagement_target_id_ : 0u;
    }

    // --- State reporting --------------------------------------------------
    [[nodiscard]] WVRState state() const noexcept { return sm_.current(); }
    [[nodiscard]] WVRTactic tactic() const noexcept { return tactic_; }
    [[nodiscard]] int shots_fired() const noexcept {
        return fire_.shots_fired();
    }
    [[nodiscard]] const fsm::Trace<WVRState, WVREvent>* trace() const
        noexcept { return sm_.trace(); }
    [[nodiscard]] std::string state_name() const;
    [[nodiscard]] std::string tactic_name() const;

    // --- Configuration ------------------------------------------------------
    /// The embedded IR fire control. The host sets the envelope from the
    /// weapon table's IR-guided A/A class (AIM-9M) at configure time.
    [[nodiscard]] MissileModule& fire() noexcept { return fire_; }
    [[nodiscard]] const MissileModule& fire() const noexcept { return fire_; }
    [[nodiscard]] Config& config() noexcept { return cfg_; }
    [[nodiscard]] const Config& config() const noexcept { return cfg_; }

    /// The shared AirSteering instance (public fields — combat tune).
    [[nodiscard]] AirSteering& air_steering() noexcept { return air_steering_; }

    // --- Pure queries (unit-test surface) ------------------------------------
    /// Geometry class of a target snapshot: do WE hold the angle?
    /// Pure function of ata/ata_from — the FSM's Advantage/Threat guard.
    [[nodiscard]] static bool own_advantage(const TargetInfo& t) noexcept;
    /// Does the TARGET hold the angle on us?
    [[nodiscard]] static bool target_advantage(const TargetInfo& t) noexcept;
    /// Desired heading (rad, CW from north) for the current tactic —
    /// exposed for tests + the FCS trace exporter.
    [[nodiscard]] double desired_heading_rad() const noexcept {
        return desired_heading_rad_;
    }
    /// Desired altitude for the current tactic (ft MSL, clamped).
    [[nodiscard]] double desired_alt_ft() const noexcept {
        return desired_alt_ft_;
    }

    /// Hard reset (fight over / brain hands the band back to BVRModule /
    /// host re-task). Clears the engagement, shots, timers, FSM to None.
    /// The IR cooldown is NOT reset (it is the shooter's rail cadence —
    /// same contract as BVRModule).
    void reset();

private:
    // --- FSM ---------------------------------------------------------------
    static fsm::StateMachine<WVRState, WVREvent> build_sm();

    // --- Steering helpers -----------------------------------------------------
    /// Lead-pursuit bearing to the target (lead point at the target's
    /// velocity * time-to-go, clamped like BVRModule's pursuit).
    [[nodiscard]] double pursuit_heading_rad() const;
    /// Target bearing from ownship (rad, CW from north).
    [[nodiscard]] double target_bearing_rad() const;
    [[nodiscard]] AirSteering::Input steering_input(
        const flight::IAircraftState& s) const noexcept;
    /// Clamp an altitude into the vertical-game window.
    [[nodiscard]] double clamp_alt_ft(double alt_ft) const noexcept;

    void engage(const TargetInfo& target);
    void clear_engagement();

    // --- FSM + tactic state ---------------------------------------------------
    fsm::StateMachine<WVRState, WVREvent> sm_;
    Config cfg_{};
    WVRTactic tactic_{WVRTactic::None};
    AirSteering air_steering_{};

    MissileModule fire_{};             // IR fire control only

    std::uint64_t engagement_target_id_{0};
    bool wants_lock_{false};
    bool release_pulse_{false};

    // Timers (seconds; updated by update's dt).
    double dwell_timer_{0.0};          // time in the current geometry state
    double defensive_timer_{0.0};      // sustained-defense accumulator
    double jink_timer_{0.0};           // break-turn reversal phase
    int    jink_side_{+1};             // which side the current break turns

    // Engagement targets for steering (captured at engage()).
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
    double desired_alt_ft_{0.0};
};

} // namespace f4::ai::modules
