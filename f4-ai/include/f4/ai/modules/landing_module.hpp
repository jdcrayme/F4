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
#include <optional>
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
    double approach_speed_kts{185.0};   // final approach CAS.
                                        // STAB-E5: raised from 160 (Phase
                                        // C3). Trace-driven correction:
                                        // at landing weight (W/S ~84 lb/ft2,
                                        // fuel 6500 + stores) the aero
                                        // model's stall speed on final is
                                        // ~166 kts (17.16*sqrt(84/0.89) with
                                        // CL=0.89 at alpha 12, TEF 1.0) —
                                        // 160 kts is BELOW stall and the
                                        // aircraft literally cannot hold
                                        // 1 G there (observed: on_glideslope
                                        // spawn at 165 kts fell with
                                        // lift=0, stalled latch on, until
                                        // it accelerated past 171 kts).
                                        // 185 kts restores ~20 kt stall
                                        // margin; the energy-managed flare
                                        // (C4) bleeds the excess over the
                                        // threshold.
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
    double pattern_speed_kts{195.0};    // downwind/base CAS — slow enough
                                        // that the ~35-deg bank turns fit
                                        // the pattern (~5,000 ft radius;
                                        // at 250 kts it is ~13,000 ft and
                                        // the legs stop existing)
    double pattern_offset_ft{12000.0};  // lateral distance of the downwind
                                        // leg from the runway centerline.
                                        // STAB-E49: 15,000 -> 12,000 with
                                        // the 23-deg bank cap — a tighter
                                        // pattern that the slower turns can
                                        // still fly without the legs
                                        // degenerating
    double upwind_along_ft{16000.0};    // upwind/crosswind corner this far
                                        // past the threshold (STAB-E49:
                                        // 14,000 -> 16,000 — the 23-deg
                                        // crosswind turn's ~8,700 ft radius
                                        // needs the extra room)
    double pattern_join_offset_ft{4000.0}; // slight lateral offset while
                                        // overflying on the upwind heading
    double base_turn_along_ft{28000.0}; // begin the base turn this far
                                        // BEFORE the threshold. STAB-E49:
                                        // 24,000 -> 28,000 — the final turn
                                        // at the 23-deg bank cap (~8,700 ft
                                        // radius at 195 kts) spans ~110 deg
                                        // + overshoot + realign and needs
                                        // ~19,000 ft of along-track after the
                                        // capture; 28,000 + the 14,000
                                        // capture lateral leaves the
                                        // establish floor (5,000 ft) intact.
    double base_aim_along_ft{18000.0};  // base leg aims at the extended
                                        // centerline this far out (kept
                                        // close so the base leg does not
                                        // angle away from the field and
                                        // steepen the final turn)
    double base_alt_agl_ft{900.0};      // base-leg altitude over the field
                                        // — near the intercept floor, so
                                        // the final turn ends CLOSE to the
                                        // beam (1500 AGL left the aircraft
                                        // 1000+ ft high at establish and
                                        // the dive-to-beam burned the
                                        // whole final approach)
    double base_capture_lateral_ft{9000.0}; // base -> final turn when this
                                        // close to the extended centerline
                                        // — MUST sit INSIDE the pattern
                                        // offset (E52: an earlier 14,000
                                        // against a 12,000 offset made the
                                        // capture fire at the downwind->base
                                        // handoff itself, the base leg was
                                        // skipped, and the intercept started
                                        // from a 138-deg-off downwind
                                        // heading — a 16,000-ft lateral S
                                        // that never stabilized). 9,000 with
                                        // the 23-deg bank cap's ~8,700 ft
                                        // final-turn radius completes
                                        // outside the establish floor.
    double intercept_floor_agl_ft{700.0}; // never descend below this AGL
                                        // while still intercepting (the
                                        // beam is meaningless laterally
                                        // far off; chasing it low+slow
                                        // away from the runway = dirt).
                                        // STAB-E27: 600 -> 700 for one
                                        // more turn-sink margin

    double beam_aim_offset_ft{1500.0};  // beam zero-point PAST the threshold
                                        // (real ILS aims ~1000-1500 ft in;
                                        // aiming at the threshold itself
                                        // makes the flare land short)
    double establish_hdg_tol_rad{0.26}; // ~15 deg on-course for Established
    double establish_lateral_ft{500.0}; // localizer capture tolerance
    double localizer_gain{0.0005};      // heading correction per ft of xtrack
                                        // STAB-E20/E53: softened from 0.0009
                                        // (and originally 0.0015) — at 200
                                        // kts the 23-deg bank cap turns with
                                        // an ~8,700 ft radius, and commands
                                        // steeper than ~1 deg per 40 ft of
                                        // offset cannot reverse before
                                        // crossing the course: the final
                                        // hunted ±900 ft around the centerline
                                        // all the way down (fix29 t=1197-1213)
    // Phase B1 (FLIGHT_CONTROL_NEXT_STEPS.md §4 Phase B1): raised from 0.5
    // rad (~30 deg) to 0.87 rad (~50 deg). At 0.5 rad the correction
    // saturated at 333 ft cross-track (0.5 / 0.0015), so beyond 333 ft off
    // centerline the aircraft turned at max 30-deg bank — too shallow to
    // close a 1000+ ft intercept offset. At 0.87 rad the saturation point
    // is ~580 ft, allowing the aircraft to point more aggressively at the
    // centerline during intercept. The actual intercept geometry (Phase B2)
    // takes over beyond 1000 ft and aims at a point ahead on the centerline.
    double max_localizer_corr_rad{0.87}; // ~50 deg correction clamp
    /// Phase B2 (FLIGHT_CONTROL_NEXT_STEPS.md §4 Phase B2): beyond this
    /// cross-track (feet), the localizer correction is replaced by a direct
    /// intercept heading aimed at a point `intercept_lead_ft` ahead on the
    /// centerline. Standard ILS intercept geometry — at large offset the
    /// proportional localizer law saturates and can't close the gap fast
    /// enough; aiming at a point ahead closes it geometrically.
    double intercept_offset_ft{600.0};  // STAB-E20/E53: narrowed band edge —
                                        // the proportional law only flies
                                        // the last few hundred feet near the
                                        // course; 0.0005 x 600 = 17 deg,
                                        // continuous with the scaled-lead
                                        // cut at the boundary (18.4 deg)
    double intercept_lead_ft{1500.0};   // floor on the intercept lead
                                        // distance (ft along the course).
                                        // Keeps small offsets from cutting
                                        // near-perpendicular to the course.
    double intercept_lead_ratio{3.0};   // STAB-E20/E53: the lead SCALES with
                                        // the cross-track at this ratio —
                                        // 3.0 bounds the intercept cut at
                                        // atan(1/3) = 18.4 deg for ANY
                                        // offset. 2.0 (26.6 deg cuts) still
                                        // hunted at the 23-deg bank cap's
                                        // turn radius (fix29)
    double establish_beam_tol_ft{400.0};// STAB-E23: Established also
                                        // requires being this close to the
                                        // glide beam — refuses to hand
                                        // OnFinal an approach it cannot
                                        // stabilize (observed: +3,500 ft
                                        // high at handoff, threshold
                                        // overflown at 1,905 ft AGL)
    double establish_floor_ft{4000.0};  // STAB-E21/E50: not established by
                                        // 4,000 ft out = intercept not
                                        // converging; go around cleanly
                                        // instead of dragging a crosser
                                        // through the missed-approach
                                        // plane. 5,000 -> 4,000: the final
                                        // turn at the 23-deg bank cap
                                        // reliably closes to ~1,000 ft
                                        // lateral by ~4,500 ft out but not
                                        // by 5,000 (fix25: gates otherwise
                                        // ALL green at along -5,729 with
                                        // lat -1,052, floor fired first).
    double flare_agl_ft{60.0};          // begin the flare below this AGL
    double flare_pitch_deg{8.0};        // flare target pitch attitude
    double flare_pitch_gain{3.0};       // stick per rad of pitch error
    // Phase C2 (FLIGHT_CONTROL_NEXT_STEPS.md §4 Phase C2): flap settings
    // commanded on OnFinal entry (and held through touchdown). The FM
    // already actuates tefPos/lefPos from PilotInput.tefCmd/lefCmd
    // (flight_model.cpp:453-454); these fields are what gets copied into
    // the AIControlOutput every tick during OnFinal + Flare. Real F-16
    // landing config is TEF full down (1.0) + LEF ~60% (0.6); the exact
    // values may need per-aircraft tuning via the .dat aero tables.
    double landing_tef_cmd{1.0};        // TEF (trailing-edge flap) on final
    double landing_lef_cmd{0.6};        // LEF (leading-edge flap) on final
    double rollout_exit_speed_kts{10.0};// slow to this before taxiing off
    double taxi_speed_kts{15.0};        // taxi-in speed
    double taxi_wp_capture_radius_ft{40.0};
    double dh_goaround_agl_ft{200.0};   // DH: below this uncleared = go around
    // Phase C5 (FLIGHT_CONTROL_NEXT_STEPS.md §4 Phase C5): tightened from
    // 4000 ft to 2500 ft. With the energy-managed flare (Phase C4) the
    // aircraft should touch down within ±500 ft of the aim point, so a
    // 4000 ft past-threshold go-around window was far too generous —
    // it let unstable approaches continue past the point where they
    // could be salvaged. 2500 ft is still well within a typical 5000-8500 ft
    // runway but tight enough to force a go-around when the predicted
    // touchdown is genuinely bad.
    double missed_along_ft{2500.0};     // past-threshold airborne distance -> go
                                        // around (far enough in that a normal
                                        // high crossing can flare inside the
                                        // runway first)
    double flare_overrun_ft{3500.0};    // STAB-E55: EXTRA along-track the
                                        // FLARE state may float past the
                                        // missed plane before its overflight
                                        // valve fires (a flare begun at
                                        // ~+2,300 still has pavement; the
                                        // bare +2,500 plane insta-aborted
                                        // legitimate late flares)

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

    // --- Final-course geometry (from the cached state + clearance) ---
    // Exposed for the FCS trace exporter and for hosts that need to read
    // the active approach's beam geometry.
    [[nodiscard]] double course_along_ft() const;    // <0 before threshold
    [[nodiscard]] double course_lateral_ft() const;  // >0 right of course
    [[nodiscard]] double glide_slope_alt_ft() const; // target MSL on the beam
    [[nodiscard]] double localizer_heading_rad() const; // corrected desired hdg
    /// Runway heading (rad, compass). Exposed for the FCS trace exporter
    /// and for hosts that need to read the cleared final approach course.
    [[nodiscard]] double runway_heading_rad() const noexcept {
        return runway_heading_rad_;
    }

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
    /// STAB-E34: air_input() + the glide beam's own descent rate as the
    /// VS feedforward — used by every beam-parallel pattern state.
    [[nodiscard]] AirSteering::Input beam_input() const noexcept;

    // --- External references (set by initialize) ---
    std::uint64_t ownship_id_{0};
    /// STAB-E9: clearance event latched by a bus subscription handler,
    /// drained at the top of update() (re-entrancy safe — see initialize).
    std::optional<LandingEvent> deferred_event_{};
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
    double flare_timer_{0.0};   ///< seconds in Flare (timeout safety valve, STAB-E3)
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
    double current_roll_rate_radps_{0.0};
    double current_pitch_rate_radps_{0.0};
    double current_vs_fpm_{0.0};
    bool on_ground_{false};

    // State machine (MUST be last — its ctor fires entry actions).
    fsm::StateMachine<LandingState, LandingEvent> sm_;
};

} // namespace f4::ai::modules
