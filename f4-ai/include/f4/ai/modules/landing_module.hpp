// f4-ai/include/f4/ai/modules/landing_module.hpp
//
// LandingModule — straight-in approach, landing, rollout, and taxi-in.
//
// FreeFalcon source: digi_landme.cpp (landing states).
//
// The module takes over from NavigationModule at the approach entry fix
// (the mission route's last waypoint) and flies:
//
//   STRAIGHT-IN (fly_traffic_pattern = false, default):
//     RequestApproach -> ProceedToFix -> InterceptFinal -> OnFinal
//       -> Flare -> Rollout -> TaxiIn -> Parked
//
//   TRAFFIC PATTERN (fly_traffic_pattern = true, standard left-hand):
//     RequestApproach -> ProceedToFix -> PatternDownwind -> PatternBase
//       -> InterceptFinal -> OnFinal -> ... (same landing chain)
//
//     PatternDownwind flies the overhead join at pattern altitude:
//     overfly the field upwind, cross the far-corner plane, turn (in the
//     pattern sense) through crosswind onto downwind, and fly downwind
//     past the threshold plane. Leg captures are plane crossings on the
//     runway along/lateral axes — immune to the turn-radius bulges of a
//     fast jet. PatternBase descends toward the extended-centerline aim
//     point, gear down; the base->final turn is the existing
//     InterceptFinal.
//
//   OnFinal -> GoAround (not cleared below DH, or threshold overflown);
//   GoAround -> ProceedToFix (climbed back to pattern altitude: re-fly
//   the approach, pattern or straight-in per the mode flag).
//
// ATC protocol:
//   on_enter(RequestApproach): publishes LandingRequest
//   LandingClearance         -> approach data (threshold, heading, glide
//                               slope, pattern altitude, DH) + Granted
//   on_enter(OnFinal):        publishes ApproachClearance ("established,
//                               request landing")
//   ClearedToLand             -> cleared_to_land
//   GoAround                  publishes GoAroundMessage with a reason
//
// Geometry (computed per tick from the clearance):
//   along   = (pos - threshold) . course_dir   (negative = before threshold)
//   lateral = (pos - threshold) . course_right (positive = right of course)
//   glide-slope target altitude = threshold_alt + (-along) * tan(gs_angle)
//
// Ground phases (Rollout, TaxiIn) reuse the shared GroundSteering laws;
// final tracking reuses the shared AirSteering cascades.
//
// Dependencies: f4-state-machine, f4-messaging, f4-entities, f4-geo,
// f4-flight-api (IAircraftState), f4-ai (AirSteering, GroundSteering).
// C++20.

#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include <f4/entities/entity.hpp>
#include <f4/messaging/bus.hpp>
#include <f4/fsm/state_machine.hpp>
#include <f4/fsm/trace.hpp>
#include <f4/geo/position.hpp>
#include <f4/flight/api/i_aircraft_state.hpp>

#include "f4/ai/ai_output.hpp"
#include "f4/ai/air_steering.hpp"
#include "f4/ai/atc/messages.hpp"
#include "f4/ai/ground_steering.hpp"

namespace f4::ai::modules {

// ============================================================================
// Landing states and events
// ============================================================================
enum class LandingState {
    RequestApproach,   // publishing LandingRequest, waiting for clearance
    ProceedToFix,      // flying to the approach entry fix at pattern altitude
    PatternDownwind,   // traffic pattern: entry onto + along the downwind leg
    PatternBase,       // traffic pattern: base leg, descending
    InterceptFinal,    // turning onto the final course, gear down
    OnFinal,           // tracking localizer + glide slope to the flare
    Flare,             // idle throttle, flare attitude, closing the last feet
    Rollout,           // touchdown -> braking, nose-wheel centerline hold
    TaxiIn,            // runway exit -> parking along the taxi-in route
    Parked,            // stopped at the parking spot, parking brake set
    GoAround          // safety valve: climb away, no re-attempt in phase 1
};

enum class LandingEvent {
    ApproachGranted,   // LandingClearance received
    FixReached,        // at/abeam the entry fix (straight-in mode)
    PatternEntry,      // at/abeam the entry fix (traffic-pattern mode)
    DownwindComplete,  // downwind leg flown — turn base
    BaseComplete,      // base leg reached the extended centerline — turn final
    Established,       // heading + localizer within tolerance
    Flare,             // below flare height AGL
    Touchdown,         // wheels on
    RunwayVacated,     // slowed to taxi speed
    ParkedComplete,    // taxi-in route finished
    GoAround,          // missed-approach condition
    Reintercept        // climbed away after a go-around: re-enter the pattern
};

// ============================================================================
// LandingModule
// ============================================================================
class LandingModule {
public:
    LandingModule();

    // --- Mission geometry (set by the host before initialize()) ---
    // `entry_fix` is the approach entry point (the mission route's last
    // waypoint, normally on the runway's extended centerline).
    // `taxi_in_route` is runway-exit -> parking; empty = park on the runway.
    void configure(const geo::WorldPosition& entry_fix,
                   std::vector<geo::WorldPosition> taxi_in_route);

    // --- Initialization (same contract as TakeoffModule) ---
    // Wires the bus, subscribes to clearances, and re-fires the
    // RequestApproach entry action (publishes LandingRequest). The ATC
    // (stub or real) must already be subscribed.
    void initialize(std::uint64_t ownship_id,
                    entities::EntityWorld& world,
                    messaging::MessageBus& bus);

    // --- Per-tick update (same contract as TakeoffModule) ---
    AIControlOutput update(double dt, const flight::IAircraftState* state);

    // --- Accessors ---
    [[nodiscard]] LandingState state() const noexcept { return sm_.current(); }
    [[nodiscard]] bool is_complete() const noexcept {
        return sm_.current() == LandingState::Parked;
    }
    [[nodiscard]] bool going_around() const noexcept {
        return sm_.current() == LandingState::GoAround;
    }
    [[nodiscard]] bool cleared_to_land() const noexcept { return cleared_to_land_; }

    /// Output for the sequencer once the mission is Complete after
    /// landing: parked, brakes + parking brake, gear down.
    [[nodiscard]] AIControlOutput hold_complete() const;

    // --- Configuration (public doubles, f4-ai module convention) ---
    double approach_speed_kts{210.0};   // final approach CAS
    double fix_radius_ft{2500.0};       // "reached" radius for the entry fix
    double fix_abeam_ft{15000.0};       // off-nose capture distance window
    double fix_abeam_bearing_rad{1.4};  // ~80 deg off the nose = "passed it"

    // --- Traffic pattern (visual approach; standard left-hand unless
    // pattern_left_traffic is false). Geometry is derived from the
    // clearance's threshold + runway heading; distances in ft. The join
    // is the classic overhead arrival: overfly the field on the UPWIND
    // heading, pass the far end, then consecutive 90-deg turns through
    // crosswind onto downwind — every turn is in the pattern sense, so
    // the join never needs a >90-deg reversal (a fast jet cannot out-turn
    // its ~13,000 ft radius, and the old far-end join bulged 20k ft off
    // the field). ---
    bool fly_traffic_pattern{false};    // false = straight-in (phase 1)
    bool pattern_left_traffic{true};    // left-hand pattern (standard)
    double pattern_speed_kts{200.0};    // downwind/base CAS — slow enough
                                        // that the ~35-deg bank turns fit
                                        // the pattern (~5,000 ft radius;
                                        // at 250 kts it is ~13,000 ft and
                                        // the legs stop existing)
    double pattern_offset_ft{15000.0};  // lateral distance of the downwind
                                        // leg from the runway centerline
    double upwind_along_ft{14000.0};    // upwind/crosswind corner this far
                                        // past the threshold (runway end
                                        // + ~5,500 ft on a 8400 ft runway)
    double pattern_join_offset_ft{4000.0}; // slight lateral offset while
                                        // overflying on the upwind heading
    double base_turn_along_ft{14000.0}; // begin the base turn this far
                                        // BEFORE the threshold — far
                                        // enough out that the base arc +
                                        // final turn complete ~2 nm before
                                        // the threshold, leaving a real
                                        // final approach (at 8,000 the
                                        // aircraft established 6,000 ft
                                        // out and immediately overflew
                                        // the missed-approach plane)
    double base_aim_along_ft{22000.0};  // base leg aims at the extended
                                        // centerline this far out
    double base_alt_agl_ft{900.0};      // base-leg altitude over the field
                                        // — near the intercept floor, so
                                        // the final turn ends CLOSE to the
                                        // beam (1500 AGL left the aircraft
                                        // 1000+ ft high at establish and
                                        // the dive-to-beam burned the
                                        // whole final approach)
    double base_capture_lateral_ft{11500.0}; // base -> final turn when this
                                        // close to the extended centerline
                                        // — sized to ~one turn radius at
                                        // approach speed PLUS the settle
                                        // distance: the 90-deg turn and
                                        // roll-out then complete ~2-3 nm
                                        // BEFORE the threshold, leaving a
                                        // real final approach. At 8,500 ft
                                        // the aircraft established right
                                        // at the threshold and instantly
                                        // hit the missed-approach plane.
    double intercept_floor_agl_ft{600.0}; // never descend below this AGL
                                        // while still intercepting (the
                                        // beam is meaningless laterally
                                        // far off; chasing it low+slow
                                        // away from the runway = dirt)

    double beam_aim_offset_ft{1500.0};  // beam zero-point PAST the threshold
                                        // (real ILS aims ~1000-1500 ft in;
                                        // aiming at the threshold itself
                                        // makes the flare land short)
    double establish_hdg_tol_rad{0.26}; // ~15 deg on-course for Established
    double establish_lateral_ft{500.0}; // localizer capture tolerance
    double localizer_gain{0.0015};      // heading correction per ft of xtrack
    double max_localizer_corr_rad{0.5}; // ~30 deg correction clamp
    double flare_agl_ft{60.0};          // begin the flare below this AGL
    double flare_pitch_deg{8.0};        // flare target pitch attitude
    double flare_pitch_gain{3.0};       // stick per rad of pitch error
    double rollout_exit_speed_kts{10.0};// slow to this before taxiing off
    double taxi_speed_kts{15.0};        // taxi-in speed
    double taxi_wp_capture_radius_ft{40.0};
    double dh_goaround_agl_ft{200.0};   // DH: below this uncleared = go around
    double missed_along_ft{4000.0};     // past-threshold airborne distance -> go
                                        // around (far enough in that a normal
                                        // high crossing can flare inside the
                                        // runway first)

    // Shared control laws. Public so hosts can tune gains.
    // pattern_steering is the same cascade with a steeper bank cap — the
    // pattern corners are real turns, while the final track deliberately
    // stays shallow (25 deg) for a smooth beam.
    AirSteering air_steering;
    AirSteering pattern_steering;
    GroundSteering ground_steering;

    // --- Trace ---
    void set_trace(fsm::Trace<LandingState, LandingEvent>* t) noexcept {
        sm_.set_trace(t);
    }
    [[nodiscard]] const fsm::Trace<LandingState, LandingEvent>* trace() const noexcept {
        return sm_.trace();
    }

    // --- Human-readable names ---
    [[nodiscard]] std::string state_name() const;
    [[nodiscard]] std::string mode_name() const { return "LandingMode"; }

private:
    [[nodiscard]] fsm::StateMachine<LandingState, LandingEvent> build_sm();

    void cache_aircraft_state(const flight::IAircraftState* state);

    // Transition checks (called from update()).
    void check_fix_reached();
    void check_pattern_downwind();
    void check_pattern_base();
    void check_established();
    void check_flare_or_goaround();
    void check_touchdown();
    void check_runway_vacated();
    void check_taxi_in_progress();

    // Per-state control logic (pure).
    [[nodiscard]] AIControlOutput controls_for_request_approach() const;
    [[nodiscard]] AIControlOutput controls_for_proceed_to_fix() const;
    [[nodiscard]] AIControlOutput controls_for_pattern_downwind() const;
    [[nodiscard]] AIControlOutput controls_for_pattern_base() const;
    [[nodiscard]] AIControlOutput track_final(double target_alt_ft,
                                              double target_speed_kts,
                                              bool pattern_turn = false) const;
    [[nodiscard]] AIControlOutput controls_for_flare() const;
    [[nodiscard]] AIControlOutput controls_for_rollout() const;
    [[nodiscard]] AIControlOutput controls_for_taxi_in() const;
    [[nodiscard]] AIControlOutput controls_for_parked() const;
    [[nodiscard]] AIControlOutput controls_for_go_around() const;

    // Final-course geometry from the cached state + clearance.
    [[nodiscard]] double course_along_ft() const;    // <0 before threshold
    [[nodiscard]] double course_lateral_ft() const;  // >0 right of course
    [[nodiscard]] double glide_slope_alt_ft() const; // target MSL on the beam
    [[nodiscard]] double localizer_heading_rad() const; // corrected desired hdg

    // Traffic-pattern geometry (from the clearance; valid once granted).
    // Lateral convention matches course_lateral_ft(): negative = LEFT of
    // the final course. A left-hand pattern flies its legs on the left
    // side (negative lateral for a positive pattern_offset_ft).
    [[nodiscard]] double pattern_lateral_sign() const noexcept {
        return pattern_left_traffic ? -1.0 : 1.0;
    }
    [[nodiscard]] geo::WorldPosition pattern_point(double along_ft,
                                                   double lateral_ft) const;
    [[nodiscard]] geo::WorldPosition pattern_leg_target() const;
    [[nodiscard]] geo::WorldPosition base_aim_point() const;
    [[nodiscard]] double base_target_alt_ft() const;

    /// Waypoint capture with the radius + timed abeam guard shared by the
    /// entry fix and the pattern legs (see check_fix_reached for why the
    /// dwell timer is load-bearing).
    [[nodiscard]] bool waypoint_captured(const geo::WorldPosition& target,
                                         double dwell_s,
                                         double radius_ft,
                                         double abeam_window_ft) const;

    [[nodiscard]] AirSteering::Input air_input() const noexcept;
    [[nodiscard]] GroundSteering::Input ground_input() const noexcept;

    // --- External references (set by initialize) ---
    std::uint64_t ownship_id_{0};
    entities::EntityWorld* world_{nullptr};
    messaging::MessageBus* bus_{nullptr};

    // Mission geometry.
    geo::WorldPosition entry_fix_;
    std::vector<geo::WorldPosition> taxi_in_route_;
    std::size_t taxi_wp_index_{0};

    // Approach data from LandingClearance.
    int runway_id_{0};
    double runway_heading_rad_{0.0};
    geo::WorldPosition threshold_position_;
    double threshold_alt_ft_{0.0};
    double glide_slope_angle_rad_{3.0 * 3.14159265358979 / 180.0};
    double pattern_altitude_ft_{2500.0};
    bool cleared_to_land_{false};
    double fix_timer_{0.0};     ///< seconds in ProceedToFix (abeam guard)
    double pattern_timer_{0.0}; ///< seconds in the current pattern state
    int pattern_leg_{0};        ///< 0 = upwind overfly -> far corner,
                                ///< 1 = crosswind turn (corner -> offset),
                                ///< 2 = downwind leg to the base turn

    // Cached aircraft state (refreshed each update()).
    geo::WorldPosition current_position_;
    double current_alt_msl_ft_{0.0};
    double current_alt_agl_ft_{0.0};
    double current_vcas_kts_{0.0};
    double current_heading_rad_{0.0};
    double current_pitch_rad_{0.0};
    double current_roll_rad_{0.0};
    double current_vs_fpm_{0.0};
    bool on_ground_{false};

    // State machine (MUST be last — its ctor fires entry actions).
    fsm::StateMachine<LandingState, LandingEvent> sm_;
};

} // namespace f4::ai::modules
