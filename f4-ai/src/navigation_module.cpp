// f4-ai/src/navigation_module.cpp
//
// NavigationModule implementation — waypoint following via AirSteering.

#include "f4/ai/modules/navigation_module.hpp"

#include <cmath>

namespace f4::ai::modules {

// ============================================================================
// Construction
// ============================================================================

NavigationModule::NavigationModule()
    : sm_(build_sm())
{
    // Cruise tune: cooler than the AirSteering defaults. With the default
    // gains the altitude channel phugoids (long-period pitch/speed
    // oscillation) against the FCS G-command lag and never settles on the
    // route altitude — which then hands the approach off far above the
    // beam. Same lesson as the landing tune.
    air_steering.attitude_gain = 1.5;
    air_steering.path_gain = 0.0001;
    air_steering.vs_gain = 5.0;
    air_steering.max_vs_fpm = 3000.0;
    air_steering.roll_gain = 4.0;
}

fsm::StateMachine<NavigationState, NavigationEvent>
NavigationModule::build_sm()
{
    return typename fsm::StateMachine<NavigationState, NavigationEvent>::Builder()
        .initial(NavigationState::ToWaypoint)
        .state(NavigationState::ToWaypoint, "ToWaypoint")
        .state(NavigationState::Done,       "Done")
        .event_name(NavigationEvent::WaypointCaptured, "WaypointCaptured")
        .on(NavigationState::ToWaypoint, NavigationState::Done,
            NavigationEvent::WaypointCaptured,
            nullptr, nullptr, "last_waypoint_reached")
        .build();
}

void NavigationModule::set_route(std::vector<Waypoint> route) {
    route_ = std::move(route);
    wp_index_ = 0;
    wp_timer_ = 0.0;
    // An empty route completes immediately: the aircraft has nowhere to go.
    if (route_.empty()) {
        sm_.process(NavigationEvent::WaypointCaptured);
    }
}

// ============================================================================
// Per-tick update
// ============================================================================

AIControlOutput NavigationModule::update(double dt, const flight::IAircraftState* state)
{
    cache_aircraft_state(state);
    wp_timer_ += dt;

    for (int iter = 0; iter < 8; ++iter) {
        const auto before = sm_.current();
        if (sm_.current() == NavigationState::ToWaypoint) {
            check_waypoint_capture();
        }
        if (sm_.current() == before) break;
    }

    switch (sm_.current()) {
        case NavigationState::ToWaypoint:
            return controls_for_waypoint();
        case NavigationState::Done:
            return {};  // no control output — sequencer takes over
    }
    return {};
}

// ============================================================================
// State caching + transitions
// ============================================================================

void NavigationModule::cache_aircraft_state(const flight::IAircraftState* state)
{
    if (!state) return;
    current_position_ = geo::WorldPosition(
        state->position_east_ft(), state->position_north_ft(), state->altitude_msl_ft());
    current_alt_msl_ft_ = state->altitude_msl_ft();
    current_vcas_kts_ = state->vcas_kts();
    current_heading_rad_ = state->heading_rad();
    current_pitch_rad_ = state->pitch_angle_rad();
    current_roll_rad_ = state->roll_angle_rad();
    current_roll_rate_radps_ = state->roll_rate_radps();
    current_pitch_rate_radps_ = state->pitch_rate_radps();
    current_vs_fpm_ = state->vertical_speed_fpm();
}

void NavigationModule::check_waypoint_capture()
{
    if (wp_index_ >= route_.size()) return;  // nothing left

    const auto& target = route_[wp_index_].position;
    const double dx = target.x - current_position_.x;
    const double dy = target.y - current_position_.y;
    const double dist = std::sqrt(dx * dx + dy * dy);

    // Off-nose (abeam) capture. A fast jet with a bank limit cannot always
    // turn tightly enough to fly directly over a waypoint — pure-pursuit
    // radius capture would orbit it forever (turn radius at 370 kts and
    // 30 deg bank is ~21,000 ft). Once the waypoint is well off the nose
    // AND within the abeam window, sequence to the next one.
    //
    // The dwell timer guards the capture: right after sequencing to
    // waypoint N+1, it can legitimately be >90 deg off the nose and
    // inside the window — without the guard the module would insta-skip
    // it while still heading away. After min_wp_dwell_s the aircraft has
    // turned toward it (30 s at ~3 deg/s covers ~90 deg), so an
    // off-nose >80 deg then genuinely means "passed it". The timer also
    // guarantees no orbit deadlock: by 30 s into a pursuit orbit (which
    // holds the target near 90 deg off the nose) the rule fires.
    const double bearing = AirSteering::bearing_to(current_position_, target);
    const double off_nose = std::abs(AirSteering::heading_error(bearing,
                                                                current_heading_rad_));

    const bool captured =
        dist < capture_radius_ft ||
        (wp_timer_ > min_wp_dwell_s && dist < abeam_capture_ft &&
         off_nose > abeam_bearing_rad);

    if (captured) {
        ++wp_index_;
        wp_timer_ = 0.0;
        if (wp_index_ >= route_.size()) {
            sm_.process(NavigationEvent::WaypointCaptured);
        }
    }
}

// ============================================================================
// Control logic
// ============================================================================

AIControlOutput NavigationModule::controls_for_waypoint() const
{
    if (wp_index_ >= route_.size()) return {};
    const auto& wp = route_[wp_index_];

    const double desired_hdg = AirSteering::bearing_to(current_position_, wp.position);

    // Slow down for big course changes: turn radius scales with V^2, and
    // a slow turn is what lets the aircraft actually converge on the next
    // leg instead of orbiting the waypoint.
    double speed = wp.speed_kts;
    const double hdg_err = std::abs(AirSteering::heading_error(desired_hdg,
                                                               current_heading_rad_));
    if (hdg_err > turn_slow_hdg_rad) {
        speed = std::min(speed, turn_speed_kts);
    }

    return air_steering.steer(desired_hdg, wp.position.z, speed,
                              steering_input());
}

AirSteering::Input NavigationModule::steering_input() const noexcept
{
    AirSteering::Input in;
    in.position = current_position_;
    in.heading_rad = current_heading_rad_;
    in.pitch_rad = current_pitch_rad_;
    in.roll_rad = current_roll_rad_;
    in.roll_rate_radps = current_roll_rate_radps_;
    in.pitch_rate_radps = current_pitch_rate_radps_;
    in.vs_fpm = current_vs_fpm_;
    in.vcas_kts = current_vcas_kts_;
    in.alt_msl_ft = current_alt_msl_ft_;
    return in;
}

// ============================================================================
// Human-readable state name
// ============================================================================

std::string NavigationModule::state_name() const {
    auto name = sm_.name_of(sm_.current());
    return name.empty() ? std::to_string(static_cast<int>(sm_.current()))
                        : std::string(name);
}

} // namespace f4::ai::modules
