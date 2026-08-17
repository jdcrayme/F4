// f4-ai/include/f4/ai/modules/navigation_module.hpp
//
// NavigationModule — waypoint following for the air phase.
//
// FreeFalcon source: digi navigation / WaypointState in digi.h.
//
// The module flies a sequence of waypoints (name, ENU position with target
// altitude, target speed). Each tick it:
//   1. Caches the aircraft state from IAircraftState.
//   2. Advances to the next waypoint when within the capture radius.
//   3. Produces AIControlOutput via the shared AirSteering cascades,
//      steering toward the current waypoint's bearing/altitude/speed.
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

    // --- Configuration ---
    double capture_radius_ft{3000.0};  ///< waypoint capture radius
    double abeam_capture_ft{15000.0};  ///< off-nose capture distance window
    double abeam_bearing_rad{1.4};     ///< ~80 deg off the nose = "passed it"
    double min_wp_dwell_s{30.0};       ///< min time on a wp before abeam capture
    double turn_speed_kts{250.0};      ///< slow to this for large heading changes
    double turn_slow_hdg_rad{0.8};     ///< ~45 deg heading error = "in a turn"

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
    double current_vs_fpm_{0.0};

    // State machine (MUST be last — its ctor fires entry actions).
    fsm::StateMachine<NavigationState, NavigationEvent> sm_;
};

} // namespace f4::ai::modules
