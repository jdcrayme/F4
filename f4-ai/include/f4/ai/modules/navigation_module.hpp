// f4-ai/include/f4/ai/modules/navigation_module.hpp
//
// NavigationModule — waypoint following for the air phase.
//
// FreeFalcon source: digi navigation / WaypointState in digi.h.
//
// The module flies a sequence of waypoints (name, ENU position with target
// altitude, target speed). Each tick it:
//   1. Caches the aircraft state from IAircraftState.
//   2. Advances to the next waypoint when within the turn-anticipation
//      lead of the NEXT leg's course change (or the legacy abeam/capture
//      rules as a fallback).
//   3. Produces AIControlOutput via the shared AirSteering cascades,
//      steering along the CURRENT LEG's course with a cross-track
//      correction — not a bearing to the waypoint (NAV-B).
//
// NAV-B — leg tracking, not homing. The original law aimed at the waypoint
// every tick (pure pursuit). That produces the classic pursuit signatures:
// a bow-shaped intercept that never establishes on a course, the nose
// slewing to keep pointing at the waypoint as it passes abeam, S-turning
// between legs, and capture geometry that depends on closure rate. The
// replacement is standard LNAV: steer the leg FROM the previous waypoint
// TO the active one, with desired heading = leg course + a cross-track
// correction angle (atan2(-xte, xte_gain_ft), clamped to a max intercept
// angle), and sequence waypoints EARLY by the turn radius so the aircraft
// rolls out of the corner established on the next leg.
//
// When the LAST waypoint is captured the module is Done. In the mission
// sequence the last waypoint is the approach entry fix: BrainComponent
// hands off to LandingModule on completion.
//
// State machine (deliberately minimal — the interesting logic is target
// selection + capture):
//   ToWaypoint -> Done   (WaypointCaptured on the last waypoint)
//
// No ATC interaction: enroute flight is unstrolled. The module therefore
// has no bus subscriptions and no initialize() (nothing to wire).
//
// Dependencies: f4-state-machine, f4-geo, f4-flight-api (IAircraftState),
// f4-ai (AirSteering, AIControlOutput). C++20.

#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include <f4/fsm/state_machine.hpp>
#include <f4/fsm/trace.hpp>
#include <f4/geo/position.hpp>
#include <f4/flight/api/i_aircraft_state.hpp>

#include "f4/ai/ai_output.hpp"
#include "f4/ai/air_steering.hpp"

namespace f4::ai::modules {

// ============================================================================
// Navigation states and events
// ============================================================================
enum class NavigationState {
    ToWaypoint,   // flying toward the current waypoint
    Done          // last waypoint captured
};

enum class NavigationEvent {
    WaypointCaptured
};

// ============================================================================
// NavigationModule
// ============================================================================
class NavigationModule {
public:
    /// One flight-plan waypoint (mapped from the scenario by the host).
    struct Waypoint {
        std::string name;
        geo::WorldPosition position;   ///< ENU feet; z = target altitude MSL
        double speed_kts{350.0};       ///< target CAS approaching this waypoint
        /// Wire WP_ACTION of the source waypoint (0 = none). The brain
        /// keys the A-G release trigger off the delivery actions (17
        /// STRIKE, 18 BOMB, 14 GNDSTRIKE, 15 NAVSTRIKE, 19 SEAD) — see
        /// brain_component.hpp's strike wiring. Default 0 keeps every
        /// hand-authored route non-strike.
        std::uint8_t action{0};
        /// The strike target's EntityId::value when `action` is a delivery
        /// action with a resolvable target (the campaign bridge fills this
        /// from the saved waypoint's target VU_ID); 0 otherwise.
        std::uint64_t target_id{0};
    };

    NavigationModule();

    // --- Route ---
    // Set before the first update(). An empty route completes immediately
    // (the module starts and stays Done — a takeoff-only mission).
    void set_route(std::vector<Waypoint> route);

    // --- Per-tick update ---
    // Same contract as TakeoffModule::update: caches state, fires
    // transitions, returns control outputs for the current state.
    AIControlOutput update(double dt, const flight::IAircraftState* state);

    // --- Accessors ---
    [[nodiscard]] NavigationState state() const noexcept { return sm_.current(); }
    [[nodiscard]] bool is_complete() const noexcept {
        return sm_.current() == NavigationState::Done;
    }
    /// Index of the waypoint currently being flown.
    [[nodiscard]] std::size_t current_waypoint_index() const noexcept {
        return wp_index_;
    }
    /// The waypoint currently being flown (nullptr once Done).
    [[nodiscard]] const Waypoint* current_waypoint() const noexcept {
        return wp_index_ < route_.size() ? &route_[wp_index_] : nullptr;
    }
    /// Last cached aircraft heading (rad). Updated each update() call.
    /// Exposed for the FCS trace exporter so it can plot target-vs-actual
    /// heading without re-deriving from position deltas.
    [[nodiscard]] double current_heading_rad() const noexcept {
        return current_heading_rad_;
    }

    /// NAV-B: the LNAV desired heading for the ACTIVE leg — leg course +
    /// cross-track correction (see controls_for_waypoint). Pure function
    /// of the cached state; call after update(). Exposed for unit tests
    /// and the FCS trace exporter.
    [[nodiscard]] double nav_heading_rad() const;

    /// NAV-B: signed cross-track distance from the ACTIVE leg's course
    /// (+ = right of course). Exposed for the establishment gate and tests.
    [[nodiscard]] double cross_track_ft() const;

    // --- Configuration ---
    double capture_radius_ft{3000.0};  ///< waypoint capture radius (last wp).
                                       ///< Tranche 37: also used as a floor for
                                       ///< the speed-proportional capture
                                       ///< (effective_capture = max(this, 10*vcas)).
    double abeam_capture_ft{25000.0};  ///< off-nose capture distance window.
                                       ///< Tranche 37: widened from 15000 — at
                                       ///< 300+ kts the aircraft exits the 15000
                                       ///< ft window in 30 s (before the dwell
                                       ///< timer fires), flying 86 NM past the
                                       ///< waypoint without capturing.
    double abeam_bearing_rad{1.4};     ///< ~80 deg off the nose = "passed it"
    double min_wp_dwell_s{30.0};       ///< min time on a wp before abeam capture
    double turn_speed_kts{250.0};      ///< slow to this for large heading changes
    double turn_slow_hdg_rad{0.8};     ///< ~45 deg heading error = "in a turn"

    // --- NAV-B: leg tracking (cross-track LNAV) ---
    /// Cross-track correction gain (ft). The correction angle is
    /// atan2(-xte, xte_gain_ft): at xte_gain_ft of offset the correction
    /// is 45 deg (before the max-intercept clamp). 4,000 ft converges a
    /// post-corner 600-1,000 ft residual in one smooth cut (a 678 ft
    /// error rides ~9.6 deg, closing in ~8 s) where 8,000 left the E/S
    /// square legs "established-never" — still converging when the next
    /// corner arrived. Smooth far-field: big offsets just clamp to the
    /// 30-deg max intercept anyway.
    double xte_gain_ft{5000.0};
    /// NAV-B2: cross-track RATE damping (rad per rad of closing track
    /// angle). 0.6: a 20-deg closing angle eases the intercept by ~13.6
    /// deg — cancels the heading-loop lag that overshot each zero-crossing
    /// by ~600 ft. Zero effect once settled on course.
    double xte_damp_gain{0.6};
    /// Maximum intercept angle off the leg course (rad). Deliberately
    /// BELOW the 30-deg bank cap: at a 30-deg intercept the heading loop
    /// flies at max bank just to hold the intercept angle, leaving no
    /// authority to shape the convergence — the aircraft snakes through
    /// the course without ever settling (standard_rate_turn E/S legs:
    /// min|xte| reached 0 but |hdg-course| stayed >5 deg at every
    /// crossing). 20 deg keeps the chase inside the bank envelope and
    /// the convergence damped.
    double max_intercept_rad{0.35};   // ~20 deg
    /// Turn anticipation: lead = R*tan(dtheta/2) + turn_lead_lag_s * v,
    /// with R = v^2/(g*tan(max_bank)). The geometric term is the textbook
    /// symmetric-tangent construction (scale 1.0 EXACT — a 1.15 trial put
    /// a systematic ~2,100 ft OUTSIDE overshoot on every 90-deg corner;
    /// scale 1.0 alone UNDERSHOT ~1,100 ft inside because the FCS takes
    /// ~2 s to roll into max bank, during which the aircraft flies
    /// straight through the ideal arc start). The additive lag term
    /// compensates exactly that: v feet per second of roll-in lag.
    /// turn_lead_lag_s = 3.0 s matches the EFFECTIVE roll-in lag
    /// (traces: ~2 s bank build + the FCS roll-rate filter lag; the
    /// standard_rate_turn corners flew ~24 deg of arc behind nominal
    /// with a 2 s allowance).
    double turn_lead_lag_s{3.0};
    /// Clamp on the computed lead distance (ft). The geometric lead for a
    /// 180-deg reversal at 350+ kts exceeds 20k ft; beyond this the module
    /// just sequences at the clamp and lets the cross-track law re-center.
    double turn_lead_max_ft{22000.0};

    // Shared air control laws (heading/altitude/speed cascades). Public so
    // hosts can tune gains like the fields above.
    AirSteering air_steering;

    // --- Trace ---
    void set_trace(fsm::Trace<NavigationState, NavigationEvent>* t) noexcept {
        sm_.set_trace(t);
    }
    [[nodiscard]] const fsm::Trace<NavigationState, NavigationEvent>* trace() const noexcept {
        return sm_.trace();
    }

    // --- Human-readable names ---
    [[nodiscard]] std::string state_name() const;
    [[nodiscard]] std::string mode_name() const { return "NavigationMode"; }

private:
    [[nodiscard]] fsm::StateMachine<NavigationState, NavigationEvent> build_sm();

    void cache_aircraft_state(const flight::IAircraftState* state);
    void check_waypoint_capture();
    [[nodiscard]] AIControlOutput controls_for_waypoint() const;
    [[nodiscard]] AirSteering::Input steering_input() const noexcept;

    /// NAV-B: the leg's anchor — the position the current leg started from
    /// (the previous waypoint, or the aircraft position when the route was
    /// set). The desired course is bearing(leg_from_ -> active wp).
    geo::WorldPosition leg_from_{};

    /// NAV-B: turn anticipation lead distance for the CURRENT waypoint
    /// (ft). R*tan(|dtheta|/2) where dtheta is the course change onto the
    /// NEXT leg; recomputed on each waypoint switch.
    double turn_lead_ft_{0.0};

    /// NAV-B: true once leg_from_ has been anchored to a real position
    /// (set on the first update() after set_route()).
    bool leg_initialized_{false};

    // Route + progress. wp_timer_ tracks seconds on the CURRENT waypoint
    // (guards the abeam capture — see check_waypoint_capture).
    std::vector<Waypoint> route_;
    std::size_t wp_index_{0};
    double wp_timer_{0.0};

    // Cached state for control logic (refreshed each update()).
    geo::WorldPosition current_position_;
    double current_alt_msl_ft_{0.0};
    double current_vcas_kts_{0.0};
    double current_heading_rad_{0.0};
    double current_pitch_rad_{0.0};
    double current_roll_rad_{0.0};
    double current_roll_rate_radps_{0.0};
    double current_pitch_rate_radps_{0.0};
    double current_vs_fpm_{0.0};

    // State machine (MUST be last — its ctor fires entry actions).
    fsm::StateMachine<NavigationState, NavigationEvent> sm_;
};

} // namespace f4::ai::modules
