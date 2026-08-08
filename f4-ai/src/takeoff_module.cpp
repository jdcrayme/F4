// f4-ai/src/takeoff_module.cpp
//
// TakeoffModule implementation — state machine construction, per-state
// control logic, and per-tick transition checks.
//
// DESIGN CHANGES (Phase A cleanup):
//   1. TaxiRequest is published in initialize() via sm_.reset(), which
//      re-fires the RequestTaxi entry action with bus_ set. Previously
//      the entry action fired during construction (bus_ was null) and
//      the request was silently dropped.
//   2. TakeoffRequest moved from on_enter(TakeRunway) to on_enter(HoldShort).
//      The aircraft requests takeoff clearance when it arrives at the
//      hold-short line, not after it's already on the runway.
//   3. Taxi waypoint advancement is done in update() by checking the
//      aircraft's distance to the current waypoint. When the last waypoint
//      is reached, RunwayAssigned fires (Taxi -> HoldShort).
//   4. HoldShort and PrepToTakeRunway no longer auto-transition every tick.
//      HoldShort waits for TakeoffClearance (via subscription).
//      PrepToTakeRunway waits for runway alignment (checked in update()).
//   5. const_cast removed. Control methods are pure (const, no transitions).
//      Transition decisions are made in update() before calling controls.

#include "f4/ai/modules/takeoff_module.hpp"
#include <f4/flight/aircraft_state.hpp>

#include <cmath>

namespace f4::ai::modules {

// ============================================================================
// State machine construction
// ============================================================================

TakeoffModule::TakeoffModule()
    : sm_(build_sm())
{}

fsm::StateMachine<TakeoffState, TakeoffEvent>
TakeoffModule::build_sm()
{
    return typename fsm::StateMachine<TakeoffState, TakeoffEvent>::Builder()
        // Initial state
        .initial(TakeoffState::RequestTaxi)

        // State names (for trace readability)
        .state(TakeoffState::RequestTaxi,      "RequestTaxi")
        .state(TakeoffState::Taxi,             "Taxi")
        .state(TakeoffState::HoldShort,        "HoldShort")
        .state(TakeoffState::Wait,             "Wait")
        .state(TakeoffState::PrepToTakeRunway, "PrepToTakeRunway")
        .state(TakeoffState::TakeRunway,       "TakeRunway")
        .state(TakeoffState::Takeoff,          "Takeoff")
        .state(TakeoffState::FlyOut,           "FlyOut")
        .state(TakeoffState::EmergencyStop,    "EmergencyStop")
        .state(TakeoffState::Done,             "Done")

        // Event names
        .event_name(TakeoffEvent::RequestTaxi,      "RequestTaxi")
        .event_name(TakeoffEvent::ClearanceGranted,  "ClearanceGranted")
        .event_name(TakeoffEvent::RunwayAssigned,    "RunwayAssigned")
        .event_name(TakeoffEvent::TakeoffCommand,    "TakeoffCommand")
        .event_name(TakeoffEvent::Liftoff,           "Liftoff")
        .event_name(TakeoffEvent::EmergencyStop,     "EmergencyStop")
        .event_name(TakeoffEvent::FlyOutComplete,    "FlyOutComplete")

        // Transitions
        .on(TakeoffState::RequestTaxi, TakeoffState::Taxi,
            TakeoffEvent::ClearanceGranted,
            nullptr, nullptr, "taxi_clearance_received")

        .on(TakeoffState::Taxi, TakeoffState::HoldShort,
            TakeoffEvent::RunwayAssigned,
            nullptr, nullptr, "reached_hold_short")

        .on(TakeoffState::HoldShort, TakeoffState::PrepToTakeRunway,
            TakeoffEvent::TakeoffCommand,
            nullptr, nullptr, "takeoff_clearance_received")

        .on(TakeoffState::HoldShort, TakeoffState::Wait,
            TakeoffEvent::RequestTaxi,
            nullptr, nullptr, "waiting_for_clearance")

        .on(TakeoffState::Wait, TakeoffState::HoldShort,
            TakeoffEvent::TakeoffCommand,
            nullptr, nullptr, "clearance_after_wait")

        .on(TakeoffState::PrepToTakeRunway, TakeoffState::TakeRunway,
            TakeoffEvent::ClearanceGranted,
            nullptr, nullptr, "aligned_on_centerline")

        .on(TakeoffState::TakeRunway, TakeoffState::Takeoff,
            TakeoffEvent::TakeoffCommand,
            nullptr, nullptr, "begin_takeoff_roll")

        .on(TakeoffState::Takeoff, TakeoffState::FlyOut,
            TakeoffEvent::Liftoff,
            nullptr, nullptr, "airborne")

        .on(TakeoffState::FlyOut, TakeoffState::Done,
            TakeoffEvent::FlyOutComplete,
            nullptr, nullptr, "reached_departure_alt")

        // Emergency from any ground state
        .on(TakeoffState::Taxi, TakeoffState::EmergencyStop,
            TakeoffEvent::EmergencyStop,
            nullptr, nullptr, "emergency_on_ground")

        .on(TakeoffState::TakeRunway, TakeoffState::EmergencyStop,
            TakeoffEvent::EmergencyStop,
            nullptr, nullptr, "abort_takeoff")

        // Entry actions
        .on_enter(TakeoffState::RequestTaxi, [this](const TakeoffEvent&) {
            // Publish TaxiRequest to ATC.
            // At construction time bus_ is null (initialize() hasn't been
            // called yet), so this is a no-op. initialize() calls sm_.reset()
            // to re-fire this entry action with bus_ set.
            if (bus_) {
                atc::TaxiRequest req;
                req.aircraft_id = ownship_id_;
                bus_->publish(req);
            }
        })

        .on_enter(TakeoffState::HoldShort, [this](const TakeoffEvent&) {
            // Arrived at hold-short line. Request takeoff clearance.
            if (bus_) {
                atc::TakeoffRequest req;
                req.aircraft_id = ownship_id_;
                req.runway_id = runway_id_;
                bus_->publish(req);
            }
        })

        .build();
}

// ============================================================================
// Initialization
// ============================================================================

void TakeoffModule::initialize(
    std::uint64_t ownship_id,
    entities::EntityWorld& world,
    messaging::MessageBus& bus)
{
    ownship_id_ = ownship_id;
    world_ = &world;
    bus_ = &bus;

    // Subscribe to ATC clearances
    bus.subscribe<atc::TaxiClearance>([this](const atc::TaxiClearance& msg) {
        if (msg.aircraft_id == ownship_id_) {
            taxi_route_ = msg.taxi_route;
            runway_id_ = msg.runway_id;
            taxi_wp_index_ = 0;
            sm_.process(TakeoffEvent::ClearanceGranted);
        }
    });

    bus.subscribe<atc::TakeoffClearance>([this](const atc::TakeoffClearance& msg) {
        if (msg.aircraft_id == ownship_id_) {
            runway_heading_rad_ = msg.runway_heading_rad;
            threshold_position_ = msg.threshold_position;
            departure_alt_ft = msg.departure_altitude_ft;
            // TakeoffClearance is the response to TakeoffRequest (published
            // in on_enter(HoldShort)). It means "cleared for takeoff."
            if (sm_.current() == TakeoffState::HoldShort ||
                sm_.current() == TakeoffState::Wait) {
                sm_.process(TakeoffEvent::TakeoffCommand);
            }
        }
    });

    // Re-fire the RequestTaxi entry action now that bus_ is set.
    // This publishes the TaxiRequest. The SM constructor already fired
    // the entry action once, but bus_ was null at that point so it was
    // a no-op. reset() fires it again with bus_ valid.
    sm_.reset();
}

// ============================================================================
// Per-tick update
// ============================================================================

AIControlOutput TakeoffModule::update(double dt, const flight::AircraftState* state)
{
    // Cache aircraft state for control methods.
    cache_aircraft_state(state);

    // Fire any pending transitions based on the current state and aircraft
    // state. We loop up to a small bound because some transitions chain
    // (e.g. Taxi -> HoldShort -> PrepToTakeRunway in one tick when the
    // StubATC auto-grants clearance). Without the loop, each chained
    // transition would take a separate tick, making the sim need 1 tick
    // per state even when the aircraft isn't moving.
    for (int iter = 0; iter < 8; ++iter) {
        const auto before = sm_.current();
        switch (sm_.current()) {
            case TakeoffState::Taxi:
                check_taxi_progress();
                break;
            case TakeoffState::PrepToTakeRunway:
                check_runway_alignment();
                break;
            case TakeoffState::TakeRunway:
                // Already cleared for takeoff (clearance received at HoldShort).
                // With the StubATC, the single TakeoffClearance grants both
                // lineup and takeoff. In a real ATC scenario, this state would
                // wait for a separate "cleared for takeoff" call.
                sm_.process(TakeoffEvent::TakeoffCommand);
                break;
            case TakeoffState::Takeoff:
                check_liftoff();
                break;
            case TakeoffState::FlyOut:
                check_departure_altitude();
                break;
            default:
                break;
        }
        if (sm_.current() == before) break;  // no transition fired, done
    }

    // Produce control outputs for the (possibly updated) current state.
    switch (sm_.current()) {
        case TakeoffState::RequestTaxi:
            return controls_for_request_taxi();
        case TakeoffState::Taxi:
            return controls_for_taxi();
        case TakeoffState::HoldShort:
            return controls_for_hold_short();
        case TakeoffState::Wait:
            return controls_for_wait();
        case TakeoffState::PrepToTakeRunway:
            return controls_for_prep_to_take_runway();
        case TakeoffState::TakeRunway:
            return controls_for_take_runway();
        case TakeoffState::Takeoff:
            return controls_for_takeoff();
        case TakeoffState::FlyOut:
            return controls_for_flyout();
        case TakeoffState::EmergencyStop:
            return controls_for_emergency_stop();
        case TakeoffState::Done:
            return {};  // no control output
    }
    return {};
}

// ============================================================================
// Cache aircraft state
// ============================================================================

void TakeoffModule::cache_aircraft_state(const flight::AircraftState* state)
{
    if (!state) return;

    // NED (z-down) -> ENU (z-up) via the named helper.
    current_position_ = geo::WorldPosition(
        state->kin.x, state->kin.y, flight::ned_to_enu_altitude_ft(state->kin.z));
    current_vcas_kts_ = state->vcas;
    current_alt_agl_ft_ = flight::altitude_agl_ft(*state);
    current_alt_msl_ft_ = flight::altitude_msl_ft(*state);
    current_heading_rad_ = flight::to_radians(state->kin.psi);
    on_ground_ = !state->gear.inAir;
}

// ============================================================================
// Transition checks
// ============================================================================

void TakeoffModule::check_taxi_progress()
{
    if (taxi_route_.empty() || taxi_wp_index_ >= taxi_route_.size()) {
        // No more waypoints — reached hold-short.
        if (sm_.current() == TakeoffState::Taxi) {
            sm_.process(TakeoffEvent::RunwayAssigned);
        }
        return;
    }

    const auto& target = taxi_route_[taxi_wp_index_];
    const double dx = target.x - current_position_.x;
    const double dy = target.y - current_position_.y;
    const double dist = std::sqrt(dx * dx + dy * dy);

    if (dist < taxi_wp_capture_radius_ft) {
        ++taxi_wp_index_;
        // If that was the last waypoint, transition to HoldShort.
        if (taxi_wp_index_ >= taxi_route_.size()) {
            sm_.process(TakeoffEvent::RunwayAssigned);
        }
    }
}

void TakeoffModule::check_runway_alignment()
{
    // Compute lateral distance from the runway centerline.
    // The centerline runs from threshold_position_ in the direction of
    // runway_heading_rad. We project the aircraft position onto the
    // centerline and measure the perpendicular distance.
    const double dx = current_position_.x - threshold_position_.x;
    const double dy = current_position_.y - threshold_position_.y;

    // Runway heading is measured from north. The centerline direction
    // vector is (sin(hdg), cos(hdg)) in the ENU frame (north = +y).
    const double cx = std::sin(runway_heading_rad_);
    const double cy = std::cos(runway_heading_rad_);

    // Cross product (2D) gives the perpendicular distance.
    const double lateral = std::abs(dx * cy - dy * cx);

    if (lateral < centerline_align_tolerance_ft) {
        sm_.process(TakeoffEvent::ClearanceGranted);
    }
}

void TakeoffModule::check_liftoff()
{
    // Detect liftoff: wheels leave the ground and we have positive altitude.
    if (!on_ground_ && current_alt_agl_ft_ > 5.0) {
        sm_.process(TakeoffEvent::Liftoff);
    }
}

void TakeoffModule::check_departure_altitude()
{
    if (current_alt_msl_ft_ >= departure_alt_ft) {
        sm_.process(TakeoffEvent::FlyOutComplete);
    }
}

// ============================================================================
// Per-state control logic (pure — no transitions, no side effects)
// ============================================================================

AIControlOutput TakeoffModule::controls_for_request_taxi() const {
    AIControlOutput output;
    output.wheel_brakes = true;
    output.throttle_cmd = 0.0;
    output.gear_handle_down = true;
    return output;
}

AIControlOutput TakeoffModule::controls_for_taxi() const {
    AIControlOutput output;
    output.gear_handle_down = true;

    if (taxi_route_.empty() || taxi_wp_index_ >= taxi_route_.size()) {
        // Reached end of taxi route — stop.
        output.throttle_cmd = 0.0;
        output.wheel_brakes = true;
    } else {
        // Taxi toward the next waypoint at low throttle.
        // A full implementation would compute heading error and use nose
        // steering. For now, set a slow taxi throttle.
        output.throttle_cmd = 0.1;
    }

    return output;
}

AIControlOutput TakeoffModule::controls_for_hold_short() const {
    AIControlOutput output;
    output.wheel_brakes = true;
    output.throttle_cmd = 0.0;
    output.gear_handle_down = true;
    return output;
}

AIControlOutput TakeoffModule::controls_for_wait() const {
    AIControlOutput output;
    output.wheel_brakes = true;
    output.throttle_cmd = 0.0;
    output.gear_handle_down = true;
    return output;
}

AIControlOutput TakeoffModule::controls_for_prep_to_take_runway() const {
    AIControlOutput output;
    output.gear_handle_down = true;
    // Slow creep forward to taxi onto the centerline.
    output.throttle_cmd = 0.1;
    return output;
}

AIControlOutput TakeoffModule::controls_for_take_runway() const {
    // Holding brakes, waiting for takeoff clearance (already requested
    // in on_enter(HoldShort)).
    AIControlOutput output;
    output.wheel_brakes = true;
    output.throttle_cmd = 0.0;
    output.gear_handle_down = true;
    return output;
}

AIControlOutput TakeoffModule::controls_for_takeoff() const {
    AIControlOutput output;
    output.gear_handle_down = true;

    // Full throttle for takeoff roll.
    output.throttle_cmd = takeoff_throttle;

    // At Vr, apply back-pressure to rotate.
    if (current_vcas_kts_ >= rotate_speed_kts) {
        output.pitch_cmd = rotate_pitch_cmd;
    }

    return output;
}

AIControlOutput TakeoffModule::controls_for_flyout() const {
    AIControlOutput output;

    // Climb at runway heading, pitch for climb.
    output.throttle_cmd = takeoff_throttle;
    output.pitch_cmd = 0.3;

    // Retract gear above gear_up_alt_ft AGL.
    output.gear_handle_down = (current_alt_agl_ft_ <= gear_up_alt_ft);

    return output;
}

AIControlOutput TakeoffModule::controls_for_emergency_stop() const {
    AIControlOutput output;
    output.wheel_brakes = true;
    output.parking_brake = true;
    output.throttle_cmd = 0.0;
    return output;
}

// ============================================================================
// Human-readable state name
// ============================================================================

std::string TakeoffModule::state_name() const {
    auto name = sm_.name_of(sm_.current());
    return name.empty() ? std::to_string(static_cast<int>(sm_.current()))
                        : std::string(name);
}

} // namespace f4::ai::modules
