// f4-ai/include/f4/ai/modules/wingman_module.hpp
//
// WingmanModule — formation keeping + engagement discipline for the #2 of a
// flight (AI_IMPLEMENTATION_PLAN.md §5 Step 11; FreeFalcon winglogic.cpp /
// wingactions.cpp / formdata.cpp).
//
// THE ROLE. A wingman is not an independent fighter: it holds a formation
// station on its lead, follows the lead through the fight (the lead cranks,
// the wingman cranks with it), sorts a DIFFERENT bandit than the lead when
// the flight engages (FreeFalcon's wing sort), and rejoins when the fight
// is over. The plan's ladder position:
//
//   Defensive > WVR > BVR > WINGMAN FORMATION > mission module
//
// The formation rung fills the slot the mission module (navigation) would
// otherwise fly: it runs while Enroute whenever no combat rung is active
// and the lead picture is live. When a combat rung takes over, this module
// goes dormant — its steering is preempted, never merged.
//
// THE PICTURE. The module is ENGINE-AGNOSTIC: it never touches the world,
// the bus, or another entity. The host pushes the lead's kinematics each
// tick (LeadPicture — position/velocity/heading/speed/altitude, validity
// flags) BEFORE the brains run, exactly like the detection-policy hook
// pushes sensor truth in. Controls out, picture in — the same contract as
// every other module, mirrored.
//
// THE STEERING. Formation keeping is a two-channel problem:
//   LATERAL — steer to the STATION point (the formation slot in the lead's
//             heading frame), blending to the lead's own heading once
//             close (inside 1.5x tolerance) so the wingman forms up rather
//             than orbiting the slot.
//   LONGITUDINAL — speed control on the ALONG-TRACK error (how far the
//             wingman sits ahead/behind the station, measured along the
//             lead's velocity direction): lead speed +/- a proportional
//             correction, clamped. A pure range-to-station error would
//             speed UP after overshooting the slot; the signed along-track
//             error slows the wingman down instead.
// Altitude rides the station's z (lead altitude + the formation's vertical
// offset) through AirSteering's gamma hold.
//
// STATES:
//   None      — no lead, lead dead, or lead on the ground. Output EMPTY:
//               the brain falls back to its mission module (a wingman
//               without a lead is a single-ship).
//   Following — in (or near) the station, formation keeping.
//   Rejoining — blown out of formation (combat separation, a break, or a
//               spawn offset): converge at rejoin speed, then Following.
// "Engaged"/"Defending" are the BRAIN's combat rungs — while they run the
// wingman module's state freezes (its update() is simply not called) and
// the recorder reports the combat mode instead.
//
// FORMATIONS (2-ship subset of FreeFalcon's 16+; the 4-ship types —
// Finger4, Fluid Four, Box, Vic, ... — need a four-ship roster and land
// with the 4-ship flight model):
//   FightingWing — right side, ~40 deg aft of the beam, 2.5 kft: the
//                  default BVR spread (stays in the lead's radar sweep,
//                  survives one missile).
//   EchelonRight / EchelonLeft — stepped line on one side.
//   Trail        — directly behind (the threat-axis parade formation).
//   LineAbreast  — abeam, wide (the max-mutual-support spread).
//
// FreeFalcon validation hooks (plan §5 Step 11 table) covered by tests:
//   * lead maneuvers, wingman follows within tolerance
//   * formation type changes (command_formation) move the station
//   * wingman engagement discipline (the SORT) lives in SensorFusion::
//     sorted_threat_target + BrainComponent's ladder — see their tests
//   * radio calls on order receipt: DEFERRED to the wing-radio model (the
//     recorder captures the state changes; brevity arrives with the
//     transcript's wingman vocabulary)
//
// Dependencies: f4-state-machine, f4-geo, f4-flight-api, f4-ai
// (AirSteering, AIControlOutput). C++20.

#pragma once

#include <cstdint>
#include <string>

#include <f4/fsm/state_machine.hpp>
#include <f4/fsm/trace.hpp>
#include <f4/flight/api/i_aircraft_state.hpp>
#include <f4/geo/position.hpp>

#include "f4/ai/ai_output.hpp"
#include "f4/ai/air_steering.hpp"

namespace f4::ai::modules {

// ============================================================================
// States, events, formations
// ============================================================================
enum class WingState {
    None,        // no live lead picture — output empty, brain flies mission
    Following,   // on station, formation keeping
    Rejoining    // converging back to the station after a blowout
};

enum class WingEvent {
    LeadAcquired,  // first valid lead picture
    StationLost,   // station error blew past the rejoin ring
    InStation,     // converged back inside the station band
    LeadLost       // picture invalid (dead lead / landed / despawned)
};

/// 2-ship formations (subset of FreeFalcon's formation table; see header).
enum class FormationType {
    FightingWing,
    EchelonRight,
    EchelonLeft,
    Trail,
    LineAbreast
};

// ============================================================================
// WingmanModule
// ============================================================================
class WingmanModule {
public:
    /// The lead's kinematic picture, pushed by the HOST each tick before
    /// the brains run (the module cannot read the world). `valid` is false
    /// when the lead is dead, not airborne, or not resolvable — the module
    /// then reports None and the brain falls back to its mission module.
    /// Everything is ENU feet / kts, matching the rest of the stack.
    struct LeadPicture {
        std::uint64_t entity_id{0};
        bool valid{false};
        geo::WorldPosition position{};    // ENU feet
        geo::WorldPosition velocity{};    // ENU ft/s
        double heading_rad{0.0};          // compass, CW from north
        double vcas_kts{0.0};
        double alt_msl_ft{0.0};
    };

    /// Doctrine + station-keeping parameters.
    struct Config {
        /// Station scale: the FightingWing slot (lateral, longitudinal)
        /// in the lead's heading frame. Other formations derive from
        /// these two + the formation table in wingman_module.cpp.
        double lateral_spacing_ft{2000.0};
        double longitudinal_spacing_ft{2500.0};
        /// Vertical offset of the station below the lead (keep the lead
        /// above the wingman's nose). 0 = line abreast in the vertical.
        double vertical_offset_ft{0.0};
        /// "In station" tolerance (plan: 500 ft goal; 800 default headroom
        /// for the nav-tuned steering cascade — skill tightens it later).
        double formation_tolerance_ft{800.0};
        /// Station distance beyond which the module calls it a blowout
        /// and rejoins (rejoin ring).
        double rejoin_range_ft{9000.0};
        /// REJOIN CAPTURE: range to the LEAD (not the slot — the slot
        /// sweeps with the lead's heading during a turn, and station-
        /// distance capture kept missing the flyby in the 2v2 E2E)
        /// inside which the module joins the formation: Following takes
        /// over and its gentle law closes the residual. The nominal
        /// station distance is ~3200 ft; the capture ring sits at
        /// ~1.6x that so a flyby always lands inside it.
        double rejoin_capture_ft{5000.0};
        /// Heading law: correction (rad) per ft of LATERAL station error
        /// once in the formation-keeping zone. Clamped at
        /// max_lateral_correction_rad; 3000 ft off beam -> the clamp
        /// (a 20-deg cut toward the slot).
        double lateral_gain_rad_per_ft{0.00012};
        /// Heading-law clamp near the station (rad).
        double max_lateral_correction_rad{0.35};
        /// Speed law: kts of correction per ft of along-track station
        /// error. 0.08 kt/ft: 2500 ft behind the slot = +200 kt, clamped.
        double follow_speed_gain{0.08};
        /// Speed-law DAMPING: kts per ft/s of closure on the station. The
        /// P term alone overshoots (a 150-kt correction with the airframe's
        /// ~3 s speed lag sails 400+ ft THROUGH the slot and phugoids —
        /// the 2v2 E2E measured a 36 kft oscillation before this term);
        /// 0.3 kt per fps of closure makes the join overdamped — the
        /// wingman arrives, it does not bob.
        double follow_damp_kt_per_fps{0.3};
        /// Speed-law clamps around the LEAD's speed (kts).
        double max_lead_speed_delta_kts{150.0};
        /// Absolute speed clamps (kts CAS).
        double min_speed_kts{200.0};
        double max_speed_kts{520.0};
    };

    WingmanModule();

    // --- Picture in (host pushes each tick, before the brains run) ------
    /// Set the lead's picture for this tick. An invalid picture (valid ==
    /// false) drops the module to None — the brain flies its mission.
    void set_lead_picture(const LeadPicture& p);

    // --- Per-tick update ------------------------------------------------
    /// Steering for formation keeping. Returns an EMPTY output while
    /// state == None (no live lead). The brain only calls this when no
    /// combat rung is active — the module never fights, it follows.
    AIControlOutput update(double dt, const flight::IAircraftState* state);

    // --- Commands (host / test surface; the plan's wingman action flags)
    /// Change formation. Takes effect next update; station error carries
    /// over so the wingman crosses to the new slot (Rejoining if far).
    void command_formation(FormationType form);
    /// Clear the lead + reset (host re-task / lead changed).
    void reset();

    // --- State reporting --------------------------------------------------
    [[nodiscard]] WingState state() const noexcept { return sm_.current(); }
    [[nodiscard]] FormationType formation() const noexcept { return form_; }
    [[nodiscard]] const fsm::Trace<WingState, WingEvent>* trace() const
        noexcept { return sm_.trace(); }
    [[nodiscard]] std::string state_name() const;
    [[nodiscard]] std::string formation_name() const;
    [[nodiscard]] const LeadPicture& lead_picture() const noexcept {
        return picture_;
    }

    // --- Pure queries (unit-test surface) ---------------------------------
    /// The formation station (world ENU) for the current picture +
    /// formation: the slot's position in the LEAD's heading frame.
    [[nodiscard]] geo::WorldPosition formation_position() const;
    /// Signed along-track station error (ft): + = wingman AHEAD of the
    /// slot (slow down), - = behind it (speed up), measured on the lead's
    /// velocity direction. 0 when no picture.
    [[nodiscard]] double station_error_ft(
        const flight::IAircraftState& own) const;
    /// True when the module holds a live lead picture.
    [[nodiscard]] bool has_live_picture() const noexcept {
        return picture_.valid;
    }
    /// Desired speed (kts CAS) for the current picture + own state —
    /// exposed for tests + the FCS trace exporter. ONE damped PD law for
    /// both states (lead speed +/- P on the along-track error, minus D on
    /// the closure rate, clamped); Rejoining only enforces a 30-kt
    /// closure floor while far so the join always progresses.
    [[nodiscard]] double desired_speed_kts(
        const flight::IAircraftState& own) const;
    /// Signed LATERAL station error (ft): + = wingman RIGHT of the slot
    /// (on the lead's heading axis), - = left. Drives the formation
    /// heading law's correction. 0 when no picture.
    [[nodiscard]] double lateral_error_ft(
        const flight::IAircraftState& own) const;
    /// Desired heading (rad, CW from north) for this tick.
    [[nodiscard]] double desired_heading_rad() const noexcept {
        return desired_heading_rad_;
    }

    // --- Configuration ------------------------------------------------------
    [[nodiscard]] Config& config() noexcept { return cfg_; }
    [[nodiscard]] const Config& config() const noexcept { return cfg_; }
    /// The shared AirSteering instance (public fields — tune per doctrine).
    [[nodiscard]] AirSteering& air_steering() noexcept { return air_steering_; }

private:
    // --- FSM ---------------------------------------------------------------
    static fsm::StateMachine<WingState, WingEvent> build_sm();

    // --- Steering helpers -----------------------------------------------------
    [[nodiscard]] AirSteering::Input steering_input(
        const flight::IAircraftState& s) const noexcept;
    /// Formation geometry table: (lateral, longitudinal, vertical) offsets
    /// in the lead's heading frame, in MULTIPLES of cfg_ spacing.
    [[nodiscard]] std::tuple<double, double, double>
    formation_offsets() const noexcept;

    fsm::StateMachine<WingState, WingEvent> sm_;
    Config cfg_{};
    FormationType form_{FormationType::FightingWing};
    AirSteering air_steering_{};

    LeadPicture picture_{};

    // Along-track rate estimator (the speed law's damping term): finite
    // difference of the station error, EWMA-smoothed (~0.75 s).
    double prev_along_ft_{0.0};
    double along_rate_ftps_{0.0};
    bool along_valid_{false};

    // Range-to-LEAD rate estimator (the REJOIN speed law's damping term
    // — rotation-free, unlike the station frame: during the lead's turn
    // the station error's rate is dominated by frame rotation, not true
    // closure, which is what phugoided the 2v2 rejoin before this).
    double prev_lead_range_ft_{0.0};
    double lead_range_rate_ftps_{0.0};

    // Cached ownship picture for this tick (steering helpers).
    geo::WorldPosition current_position_{};
    double desired_heading_rad_{0.0};
};

} // namespace f4::ai::modules
