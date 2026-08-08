// f4-ai/include/f4/ai/modules/takeoff_module.hpp
//
// TakeoffModule — 9-state takeoff state machine producing AIControlOutput.
//
// FreeFalcon source: digi_landme.cpp (takeoff states), dlogic.cpp (TakeoffMode).
//
// The module drives the aircraft from parking through taxi, runway lineup,
// takeoff roll, rotation at Vr, initial climb, and departure to waypoint
// mode. It interacts with ATC via the MessageBus for clearances.
//
// State machine:
//   RequestTaxi -> Taxi -> HoldShort -> PrepToTakeRunway
//     -> TakeRunway -> Takeoff -> FlyOut -> Done
//   HoldShort -> Wait (holding for clearance)
//   Any -> EmergencyStop
//
// Dependencies: f4-state-machine, f4-messaging, f4-entities, f4-geo, f4-data.
// C++20.

#pragma once

#include <cstdint>
#include <string>

#include <f4/entities/entity.hpp>
#include <f4/messaging/bus.hpp>
#include <f4/fsm/state_machine.hpp>
#include <f4/fsm/trace.hpp>
#include <f4/geo/position.hpp>

#include "f4/ai/ai_output.hpp"
#include "f4/ai/atc/messages.hpp"

// Forward declaration — the full definition is in <f4/flight/aircraft_state.hpp>.
// The .cpp includes the full header; the header only needs the pointer type.
namespace f4::flight { struct AircraftState; }

namespace f4::ai::modules {

// ============================================================================
// Takeoff states and events
// ============================================================================
enum class TakeoffState {
    RequestTaxi,
    Taxi,
    HoldShort,
    Wait,               // holding for clearance
    PrepToTakeRunway,
    TakeRunway,
    Takeoff,            // rolling / rotating
    FlyOut,             // climbing to departure altitude
    EmergencyStop,
    Done
};

enum class TakeoffEvent {
    RequestTaxi,
    ClearanceGranted,
    RunwayAssigned,
    TakeoffCommand,
    Liftoff,
    EmergencyStop,
    FlyOutComplete
};

// ============================================================================
// TakeoffModule
// ============================================================================
class TakeoffModule {
public:
    // --- Construction ---
    TakeoffModule();

    // --- Initialization ---
    void initialize(
        std::uint64_t ownship_id,
        entities::EntityWorld& world,
        messaging::MessageBus& bus);

    // --- Per-tick update ---
    // Produces control outputs for the current state.
    // The caller (DigitalBrain or ScenarioRunner) passes in the current
    // aircraft state and receives the AI's stick/throttle commands.
    AIControlOutput update(double dt, const flight::AircraftState* state);

    // --- Accessors ---
    [[nodiscard]] TakeoffState state() const noexcept { return sm_.current(); }
    [[nodiscard]] bool is_complete() const noexcept {
        return sm_.current() == TakeoffState::Done;
    }

    // --- Configuration ---
    // These come from AircraftConfig in production, but can be set directly
    // for testing.
    double rotate_speed_kts{140.0};       // Vr (aircraft-dependent)
    double climb_rate_fpm{500.0};         // initial climb rate
    double gear_up_alt_ft{200.0};         // retract gear above this AGL
    double departure_alt_ft{2500.0};      // climb to this MSL before Done
    double taxi_speed_kts{15.0};          // max taxi speed
    double takeoff_throttle{1.0};         // throttle during takeoff roll (1.0 = MIL)
    double rotate_pitch_cmd{0.5};         // pitch stick input at Vr

    // --- Trace ---
    void set_trace(fsm::Trace<TakeoffState, TakeoffEvent>* t) noexcept {
        sm_.set_trace(t);
    }
    [[nodiscard]] const fsm::Trace<TakeoffState, TakeoffEvent>* trace() const noexcept {
        return sm_.trace();
    }

    // --- Human-readable names ---
    [[nodiscard]] std::string state_name() const;
    [[nodiscard]] std::string mode_name() const { return "TakeoffMode"; }

private:
    // Build the state machine transition table.
    // Non-static: entry actions capture `this` to publish ATC messages.
    fsm::StateMachine<TakeoffState, TakeoffEvent> build_sm();

    // Per-state control logic.
    AIControlOutput taxi_controls(double dt) const;
    AIControlOutput takeoff_roll_controls(double dt) const;
    AIControlOutput flyout_controls(double dt) const;

    // --- Data members (ordered so sm_ is LAST) ---
    // C++ initializes members in declaration order. The StateMachine
    // constructor fires the initial state's entry action, which accesses
    // other members via the `this` pointer captured in the on_enter lambda.
    // If sm_ were declared first, those members would contain garbage
    // (uninitialized) at the point the entry action runs, causing an
    // access violation. By declaring sm_ last, all other members are
    // initialized to their defaults before sm_ is constructed.

    // External references (set by initialize).
    std::uint64_t ownship_id_{0};
    entities::EntityWorld* world_{nullptr};
    messaging::MessageBus* bus_{nullptr};

    // Taxi route from ATC clearance.
    std::vector<geo::WorldPosition> taxi_route_;
    std::size_t taxi_wp_index_{0};

    // Runway data from clearance.
    int runway_id_{0};
    double runway_heading_rad_{0.0};
    geo::WorldPosition threshold_position_;
    geo::WorldPosition runway_end_position_;

    // Cached state for control logic.
    double current_vcas_kts_{0.0};
    double current_alt_agl_ft{0.0};
    double current_alt_msl_ft{0.0};
    double current_heading_rad{0.0};
    bool on_ground_{true};

    // State machine (MUST be last — its ctor fires entry actions that
    // read other members via captured `this`).
    fsm::StateMachine<TakeoffState, TakeoffEvent> sm_;
};

} // namespace f4::ai::modules
