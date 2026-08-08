// f4-ai/src/takeoff_module.cpp
//
// TakeoffModule implementation — state machine construction and per-state
// control logic.

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
            nullptr, nullptr, "line_up_command")

        .on(TakeoffState::HoldShort, TakeoffState::Wait,
            TakeoffEvent::RequestTaxi,
            nullptr, nullptr, "waiting_for_clearance")

        .on(TakeoffState::PrepToTakeRunway, TakeoffState::TakeRunway,
            TakeoffEvent::ClearanceGranted,
            nullptr, nullptr, "takeoff_clearance")

        .on(TakeoffState::TakeRunway, TakeoffState::Takeoff,
            TakeoffEvent::TakeoffCommand,
            nullptr, nullptr, "begin_takeoff_roll")

        .on(TakeoffState::Takeoff, TakeoffState::FlyOut,
            TakeoffEvent::Liftoff,
            nullptr, nullptr, "airborne")

        .on(TakeoffState::FlyOut, TakeoffState::Done,
            TakeoffEvent::FlyOutComplete,
            nullptr, nullptr, "reached_departure_alt")

        // Emergency from any state
        .on(TakeoffState::Taxi, TakeoffState::EmergencyStop,
            TakeoffEvent::EmergencyStop,
            nullptr, nullptr, "emergency_on_ground")

        .on(TakeoffState::TakeRunway, TakeoffState::EmergencyStop,
            TakeoffEvent::EmergencyStop,
            nullptr, nullptr, "abort_takeoff")

        // Entry actions
        .on_enter(TakeoffState::RequestTaxi, [this](const TakeoffEvent&) {
            // Publish TaxiRequest to ATC
            if (bus_) {
                atc::TaxiRequest req;
                req.aircraft_id = ownship_id_;
                bus_->publish(req);
            }
        })

        .on_enter(TakeoffState::TakeRunway, [this](const TakeoffEvent&) {
            // Request takeoff clearance
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
            // If we're in TakeRunway state, the clearance means we can roll
            if (sm_.current() == TakeoffState::TakeRunway) {
                sm_.process(TakeoffEvent::TakeoffCommand);
            }
        }
    });
}

// ============================================================================
// Per-tick update
// ============================================================================

AIControlOutput TakeoffModule::update(double dt, const flight::AircraftState* state)
{
    AIControlOutput output;

    // Cache current aircraft state
    if (state) {
        current_vcas_kts_ = state->vcas;
        current_alt_agl_ft = -state->kin.z - state->gear.groundZ_ft;
        current_alt_msl_ft = -state->kin.z;
        current_heading_rad = flight::to_radians(state->kin.psi);
        on_ground_ = !state->gear.inAir;
    }

    switch (sm_.current()) {
        case TakeoffState::RequestTaxi:
            // Waiting for taxi clearance (entry action already sent request)
            // Idle controls: brakes on, throttle idle
            output.wheel_brakes = true;
            output.throttle_cmd = 0.0;
            output.gear_handle_down = true;
            break;

        case TakeoffState::Taxi:
            output = taxi_controls(dt);
            break;

        case TakeoffState::HoldShort:
            // Stopped at hold-short line, brakes on
            output.wheel_brakes = true;
            output.throttle_cmd = 0.0;
            output.gear_handle_down = true;
            // In a full impl, we'd wait for ATC "position and hold" or
            // "cleared for takeoff". For the stub, transition immediately.
            sm_.process(TakeoffEvent::TakeoffCommand);
            break;

        case TakeoffState::Wait:
            // Holding for clearance
            output.wheel_brakes = true;
            output.throttle_cmd = 0.0;
            output.gear_handle_down = true;
            break;

        case TakeoffState::PrepToTakeRunway:
            // Line up on runway (taxi onto centerline)
            output.gear_handle_down = true;
            output.throttle_cmd = 0.1;  // slow creep
            // Transition to TakeRunway when on centerline
            sm_.process(TakeoffEvent::ClearanceGranted);
            break;

        case TakeoffState::TakeRunway:
            // Waiting for takeoff clearance (entry action sent request)
            output.wheel_brakes = true;   // holding brakes
            output.throttle_cmd = 0.0;    // idle until clearance
            output.gear_handle_down = true;
            break;

        case TakeoffState::Takeoff:
            output = takeoff_roll_controls(dt);
            break;

        case TakeoffState::FlyOut:
            output = flyout_controls(dt);
            break;

        case TakeoffState::EmergencyStop:
            output.wheel_brakes = true;
            output.parking_brake = true;
            output.throttle_cmd = 0.0;
            break;

        case TakeoffState::Done:
            // No control output — DigitalBrain should transition to
            // WaypointMode when TakeoffModule is complete.
            break;
    }

    return output;
}

// ============================================================================
// Per-state control logic
// ============================================================================

AIControlOutput TakeoffModule::taxi_controls(double dt) const {
    AIControlOutput output;
    output.gear_handle_down = true;

    // Nose-steer toward the next taxi waypoint
    if (!taxi_route_.empty() && taxi_wp_index_ < taxi_route_.size()) {
        // In a full impl, we'd compute heading error to the next waypoint
        // and use nose steering. For now, set a slow taxi throttle.
        output.throttle_cmd = 0.1;  // taxi thrust
    } else {
        // Reached end of taxi route -> HoldShort
        output.throttle_cmd = 0.0;
        output.wheel_brakes = true;
    }

    return output;
}

AIControlOutput TakeoffModule::takeoff_roll_controls(double dt) const {
    AIControlOutput output;
    output.gear_handle_down = true;

    // Full throttle for takeoff
    output.throttle_cmd = takeoff_throttle;

    // Nose steer: maintain runway centerline heading
    // (In a full impl, use heading error -> roll/yaw correction)

    // At Vr, apply back-pressure to rotate
    if (current_vcas_kts_ >= rotate_speed_kts) {
        output.pitch_cmd = rotate_pitch_cmd;  // pull stick back
    }

    // Detect liftoff: when we leave the ground
    if (!on_ground_ && current_alt_agl_ft > 5.0) {
        // Transition to FlyOut
        const_cast<TakeoffModule*>(this)->sm_.process(TakeoffEvent::Liftoff);
    }

    return output;
}

AIControlOutput TakeoffModule::flyout_controls(double dt) const {
    AIControlOutput output;

    // Climb at runway heading, pitch for climb_rate_fpm
    output.throttle_cmd = takeoff_throttle;  // maintain MIL or AB
    output.pitch_cmd = 0.3;  // climb pitch (in a full impl, PID on altitude)

    // Retract gear above gear_up_alt_ft AGL
    if (current_alt_agl_ft > gear_up_alt_ft) {
        output.gear_handle_down = false;
    } else {
        output.gear_handle_down = true;
    }

    // Check if we've reached departure altitude
    if (current_alt_msl_ft >= departure_alt_ft) {
        const_cast<TakeoffModule*>(this)->sm_.process(TakeoffEvent::FlyOutComplete);
    }

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
