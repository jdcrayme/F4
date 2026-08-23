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
// ATC protocol:
//   on_enter(RequestTaxi): publishes TaxiRequest
//   on_enter(HoldShort):   publishes TakeoffRequest
//   TaxiClearance  -> ClearanceGranted event (RequestTaxi -> Taxi)
//   TakeoffClearance -> TakeoffCommand event (HoldShort -> PrepToTakeRunway)
//
// The module tracks taxi waypoint progress by reading the aircraft position
// from IAircraftState each tick. When the last taxi waypoint is reached,
// RunwayAssigned fires (Taxi -> HoldShort). When the aircraft is aligned
// on the runway centerline, ClearanceGranted fires (PrepToTakeRunway ->
// TakeRunway). When liftoff is detected, Liftoff fires (Takeoff -> FlyOut).
// When departure altitude is reached, FlyOutComplete fires (FlyOut -> Done).
//
// Phase 2 (H2): The module takes const IAircraftState& instead of
// const AircraftState*. This decouples the module from the full
// AircraftState struct (35+ fields) and its NED coordinate convention.
// The IAircraftState interface presents position in ENU (the AI's
// natural frame) and exposes only the 7 fields the AI actually needs.
//
// Dependencies: f4-state-machine, f4-messaging, f4-entities, f4-geo, f4-data,
// f4-flight-model (for IAircraftState interface only — NOT for AircraftState).
// C++20.

#pragma once

#include <cstdint>
#include <string>

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
    // The SM is constructed with the initial state RequestTaxi. The entry
    // action for RequestTaxi is a no-op at construction time (bus_ is null).
    // Call initialize() to wire up the bus and re-fire the entry action,
    // which publishes the TaxiRequest.
    TakeoffModule();

    // --- Initialization ---
    // Wires up external references, subscribes to ATC clearances, and
    // re-fires the RequestTaxi entry action to publish the TaxiRequest.
    // The bus must already have the StubATC (or real ATC) subscribed,
    // otherwise the TaxiRequest will have no handler and the module will
    // stay in RequestTaxi forever.
    void initialize(
        std::uint64_t ownship_id,
        entities::EntityWorld& world,
        messaging::MessageBus& bus);

    // --- Per-tick update ---
    // Produces control outputs for the current state.
    // The caller (BrainComponent or DigitalBrain) passes in the current
    // aircraft state (via the IAircraftState interface) and receives the
    // AI's stick/throttle commands.
    // Transition decisions (waypoint reached, liftoff detected, etc.) are
    // made HERE, not in the per-state control methods — this keeps the
    // control methods pure (const, no side effects) and avoids const_cast.
    //
    // Phase 2: Takes const IAircraftState* instead of const AircraftState*.
    // Null is allowed — update() will produce idle controls (brakes on,
    // zero throttle) with no position tracking. This matches the old
    // behavior where a null AircraftState was treated as a no-op.
    AIControlOutput update(double dt, const flight::IAircraftState* state);

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
    double rotate_pitch_deg{13.0};        // target pitch attitude at Vr
    double rotate_pitch_gain{3.0};        // stick per rad of pitch error (rotation)
    double climb_pitch_deg{10.0};         // target pitch attitude in FlyOut
    double climb_pitch_gain{2.0};         // stick per rad of pitch error (climb)

    // Waypoint capture radius (feet). When the aircraft is within this
    // distance of a taxi waypoint, it advances to the next one.
    double taxi_wp_capture_radius_ft{50.0};

    // Centerline alignment tolerance (feet). When the aircraft in
    // PrepToTakeRunway is within this lateral distance of the runway
    // centerline, it transitions to TakeRunway.
    double centerline_align_tolerance_ft{10.0};

    // Heading alignment tolerance (radians). PrepToTakeRunway additionally
    // requires the heading to be within this of the runway heading before
    // taking the runway, and Takeoff/FlyOut hold the runway heading.
    double heading_align_tolerance_rad{0.15};   // ~8.5 deg

    // How far past the threshold the lineup target sits (feet). PrepToTakeRunway
    // steers toward this point on the runway centerline, then aligns heading.
    double lineup_depth_ft{150.0};

    // Heading-hold gain for FlyOut roll commands (roll units per rad of error).
    // DEPRECATED — kept for back-compat but unused now that FlyOut routes
    // through air_steering's bank cascade. The cascade gives both bank-target
    // limiting AND roll-rate damping, which the bare proportional law lacked
    // (it limit-cycled: any heading error > ~6° commanded full stick, the
    // FCS over-banked, the heading reversed, the stick reversed — the
    // documented "roll flutter during turns" symptom in
    // FLIGHT_CONTROL_STABILITY_PLAN.md §4.1 RC-2).
    double flyout_heading_gain{1.5};

    // Speed target during FlyOut (kts). Default 250 kts is a conservative
    // climb speed for the F-16; hosts can override via AircraftConfig.
    double flyout_speed_kts{250.0};

    // Shared air-steering controller for the FlyOut phase. Public so hosts
    // can tune its gains. Uses the same bank cascade + roll-rate damping
    // as NavigationModule, killing the FlyOut roll limit cycle.
    AirSteering air_steering;

    // Shared ground-steering controller (taxi, lineup, takeoff-roll heading
    // hold). Public so hosts can tune its gains like the fields above.
    GroundSteering ground_steering;

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

    // Per-state control logic. These are pure functions of the cached
    // aircraft state — they do NOT fire transitions. Transition decisions
    // are made in update() before calling the control method.
    AIControlOutput controls_for_request_taxi() const;
    AIControlOutput controls_for_taxi() const;
    AIControlOutput controls_for_hold_short() const;
    AIControlOutput controls_for_wait() const;
    AIControlOutput controls_for_prep_to_take_runway() const;
    AIControlOutput controls_for_take_runway() const;
    AIControlOutput controls_for_takeoff() const;
    AIControlOutput controls_for_flyout() const;
    AIControlOutput controls_for_emergency_stop() const;

    // Transition checks — called from update() before control logic.
    // Each returns true if the corresponding transition should fire.
    void check_taxi_progress();           // Taxi -> HoldShort (RunwayAssigned)
    void check_takeoff_clearance_ack();   // (no-op; handled by subscription)
    void check_runway_alignment();        // PrepToTakeRunway -> TakeRunway
    void check_liftoff();                 // Takeoff -> FlyOut
    void check_departure_altitude();      // FlyOut -> Done

    // Cache the current aircraft state fields for use by control methods.
    // Phase 2: reads from IAircraftState instead of AircraftState.
    void cache_aircraft_state(const flight::IAircraftState* state);

    // Build the GroundSteering input from the cached aircraft state.
    [[nodiscard]] GroundSteering::Input steering_input() const noexcept;

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

    // Cached state for control logic (refreshed each update()).
    geo::WorldPosition current_position_;
    double current_vcas_kts_{0.0};
    double current_alt_agl_ft_{0.0};
    double current_alt_msl_ft_{0.0};
    double current_heading_rad_{0.0};
    double current_pitch_rad_{0.0};
    double current_roll_rad_{0.0};
    double current_roll_rate_radps_{0.0};
    double current_pitch_rate_radps_{0.0};
    double current_vs_fpm_{0.0};
    bool on_ground_{true};

    // State machine (MUST be last — its ctor fires entry actions that
    // read other members via captured `this`).
    fsm::StateMachine<TakeoffState, TakeoffEvent> sm_;
};

} // namespace f4::ai::modules
