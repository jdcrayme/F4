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
//
// Phase 2 (H2): cache_aircraft_state() now reads from the IAircraftState
// interface instead of the full AircraftState struct. The interface
// presents position in ENU (the AI's natural frame), so the AI no longer
// needs to do NED→ENU conversion or know about the flight model's
// internal coordinate convention.

#include "f4/ai/modules/takeoff_module.hpp"

#include <algorithm>
#include <cmath>

namespace f4::ai::modules {

// ============================================================================
// State machine construction
// ============================================================================

TakeoffModule::TakeoffModule()
    : sm_(build_sm())
{
    air_steering.max_bank_rad = 0.26;
    air_steering.bank_gain = 1.0;
}

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
            // STAB-E9: latch the event instead of processing it inline.
            // StubATC answers SYNCHRONOUSLY inside publish(); when that
            // publish originates from this module's own entry action
            // (initialize() -> sm_.reset() -> on_enter(RequestTaxi) ->
            // publish(TaxiRequest) -> TaxiClearance -> HERE), calling
            // sm_.process() now re-enters the state machine while it is
            // still inside reset() — undefined behavior (observed segfault
            // in trace_runner at first tick). update() drains the latch
            // on the owning tick instead.
            deferred_event_ = TakeoffEvent::ClearanceGranted;
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
                deferred_event_ = TakeoffEvent::TakeoffCommand;
            }
        }
    });

    // Re-fire the RequestTaxi entry action now that bus_ is set.
    // This publishes the TaxiRequest. The SM constructor already fired
    // the entry action once, but bus_ was null at that point so it was
    // a no-op. reset() fires it again with bus_ valid.
    sm_.reset();

    // STAB-E9: StubATC answers the TaxiRequest synchronously inside the
    // publish chain above; the clearance handler latched the event rather
    // than processing it (re-entrancy safety — an inline sm_.process()
    // there re-entered the SM mid-reset and segfaulted). reset() has now
    // returned; we are outside any SM frame, so drain the latch here to
    // reach Taxi within initialize() (the documented contract).
    if (deferred_event_) {
        const auto ev = *deferred_event_;
        deferred_event_.reset();
        sm_.process(ev);
    }
}

// ============================================================================
// Per-tick update
// ============================================================================

AIControlOutput TakeoffModule::update(double dt, const flight::IAircraftState* state)
{
    // Cache aircraft state for control methods.
    cache_aircraft_state(state);

    // STAB-E9: drain any clearance event latched by a subscription handler
    // (see initialize). Processing it here — on the owning tick, outside
    // any sm_.reset()/process() frame — is re-entrancy safe.
    if (deferred_event_) {
        const auto ev = *deferred_event_;
        deferred_event_.reset();
        sm_.process(ev);
    }

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

void TakeoffModule::cache_aircraft_state(const flight::IAircraftState* state)
{
    if (!state) return;

    // IAircraftState presents position in ENU (East-North-Up), which is
    // the AI's natural coordinate frame. No NED→ENU conversion needed —
    // the interface implementation (FlightModelComponent) already did it.
    current_position_ = geo::WorldPosition(
        state->position_east_ft(),
        state->position_north_ft(),
        state->altitude_msl_ft());
    current_vcas_kts_ = state->vcas_kts();
    current_alt_agl_ft_ = state->altitude_agl_ft();
    current_alt_msl_ft_ = state->altitude_msl_ft();
    current_heading_rad_ = state->heading_rad();
    current_pitch_rad_ = state->pitch_angle_rad();
    current_roll_rad_ = state->roll_angle_rad();
    current_roll_rate_radps_ = state->roll_rate_radps();
    current_pitch_rate_radps_ = state->pitch_rate_radps();
    current_vs_fpm_ = state->vertical_speed_fpm();
    on_ground_ = state->on_ground();
}

GroundSteering::Input TakeoffModule::steering_input() const noexcept {
    GroundSteering::Input in;
    in.position = current_position_;
    in.heading_rad = current_heading_rad_;
    in.speed_kts = current_vcas_kts_;
    in.heading_rate_radps = current_yaw_rate_radps_;
    return in;
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

    // The aircraft must also be pointed down the runway before it takes it.
    const double hdg_err = std::abs(GroundSteering::heading_error(
        runway_heading_rad_, current_heading_rad_));

    if (lateral < centerline_align_tolerance_ft &&
        hdg_err < heading_align_tolerance_rad) {
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
        // Reached end of taxi route — hold brakes at the hold-short point.
        return ground_steering.hold();
    }

    // Steer along the taxi route. The last waypoint is the hold-short point:
    // decelerate to a stop there. Intermediate waypoints are fly-through.
    const bool last_wp = (taxi_wp_index_ + 1 == taxi_route_.size());
    return ground_steering.steer_toward(taxi_route_[taxi_wp_index_],
                                        steering_input(),
                                        taxi_speed_kts,
                                        /*stop_at_target=*/last_wp);
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
    // Line up on the runway centerline. Steer toward a point lineup_depth_ft
    // past the threshold while still offset from the centerline; once close
    // to the line, roll forward while aligning heading to the runway heading.
    // check_runway_alignment() takes over (Taxi -> TakeRunway) when both
    // lateral and heading tolerances are met.
    //
    // STAB-E16: the "once close to the line, roll forward while aligning"
    // half of the design was previously NOT implemented — the law always
    // chased lineup_point_far, so the final alignment depended on the phase
    // of the approach orbit around the point. Combined with the A3-tight
    // gate (5 ft / 0.5 deg) the convergence was a coin flip (the
    // BrainComponent.TaxiLineupTakeoff test flipped to failing when the
    // deferred-clearance timing shifted the orbit phase by one tick).
    // Implement the described two-phase law: chase the point while laterally
    // offset; inside lineup_capture_ft roll forward down the runway and
    // align heading directly — deterministic convergence.
    const double fx = std::sin(runway_heading_rad_);
    const double fy = std::cos(runway_heading_rad_);

    const double dx = current_position_.x - threshold_position_.x;
    const double dy = current_position_.y - threshold_position_.y;
    const double lateral = std::abs(dx * fy - dy * fx);

    if (lateral < lineup_capture_ft) {
        // On/near the centerline: aim at a point ON the centerline 800 ft
        // ahead of the current along-track position. As the lateral closes,
        // the bearing converges to the runway heading — this closes BOTH
        // the offset and the heading error (a pure heading-align rolls a
        // parallel track at whatever lateral remained at capture, observed
        // stuck at 65 ft).
        const double along = dx * fx + dy * fy;
        const geo::WorldPosition ahead_on_line(
            threshold_position_.x + fx * (along + 800.0),
            threshold_position_.y + fy * (along + 800.0),
            threshold_position_.z);
        return ground_steering.steer_toward(ahead_on_line, steering_input(),
                                            taxi_speed_kts,
                                            /*stop_at_target=*/false);
    }

    // Laterally offset: steer toward a point lineup_depth_ft (+ lead) past
    // the threshold to cut toward the centerline.
    const geo::WorldPosition lineup_point_far(
        threshold_position_.x + fx * (lineup_depth_ft + 1000.0),
        threshold_position_.y + fy * (lineup_depth_ft + 1000.0),
        threshold_position_.z);
    return ground_steering.steer_toward(lineup_point_far, steering_input(),
                                        taxi_speed_kts, /*stop_at_target=*/false);
}

AIControlOutput TakeoffModule::controls_for_take_runway() const {
    // Holding brakes on the centerline, clearance already in hand
    // (requested in on_enter(HoldShort)).
    return ground_steering.hold();
}

AIControlOutput TakeoffModule::controls_for_takeoff() const {
    AIControlOutput output;
    output.gear_handle_down = true;

    // Full throttle for takeoff roll.
    output.throttle_cmd = takeoff_throttle;

    // Hold the runway centerline heading through the roll. Nose-wheel
    // authority fades with speed inside the EOM (5 deg/s above 150 ft/s),
    // so full pedal authority here is safe.
    const double hdg_err = GroundSteering::heading_error(
        runway_heading_rad_, current_heading_rad_);
    output.yaw_cmd = std::clamp(-ground_steering.heading_gain * hdg_err, -1.0, 1.0);

    // Phase 1c: ZERO roll command during the takeoff roll. Previously this
    // was left at the default (0), but the FCS's `pstab` could pick up
    // transients from any tiny non-zero stick input and the EOM's ground
    // clamp step-released k.p at liftoff — a step input to the roll axis
    // at the moment of liftoff when airspeed was lowest and roll authority
    // most limited. Explicitly zeroing roll_cmd here, plus the rotation
    // law only firing at Vr, ensures pstab is 0 at liftoff.
    output.roll_cmd = 0.0;

    // At Vr, rotate to the target PITCH ATTITUDE (not a fixed stick).
    // A fixed stick commands G; on the ground the G integrator winds up
    // against the EOM's attitude clamp and limit-cycles (15deg -> -2deg)
    // without ever developing a climb.
    if (current_vcas_kts_ >= rotate_speed_kts) {
        const double target = rotate_pitch_deg * (3.14159265358979 / 180.0);
        output.pitch_cmd = std::clamp(rotate_pitch_gain * (target - current_pitch_rad_),
                                      -0.25, 0.5);
    }

    return output;
}

AIControlOutput TakeoffModule::controls_for_flyout() const {
    AIControlOutput output;

    // Phase 1b: FlyOut now uses the AirSteering bank cascade (with roll-rate
    // damping) instead of the bare proportional heading hold that limit-cycled.
    // The cascade commands a target bank from heading error (clamped to
    // max_bank_rad), then commands roll rate from the bank error with explicit
    // roll-rate damping — the same cascade NavigationModule uses. This kills
    // the "roll flutter during turns" symptom.
    //
    // Pitch is still an attitude command (climb_pitch_deg) since FlyOut is
    // a climb phase, not an altitude-capture phase — the air_steering
    // altitude cascade would fight the climb until departure_alt is reached.
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
    output = air_steering.steer(runway_heading_rad_, departure_alt_ft,
                                flyout_speed_kts, in);

    // Override pitch with the climb attitude command (the cascade's altitude
    // capture is for level flight; FlyOut is a sustained climb to departure
    // altitude).
    const double target = climb_pitch_deg * (3.14159265358979 / 180.0);
    output.pitch_cmd = std::clamp(climb_pitch_gain * (target - current_pitch_rad_),
                                  -0.5, 0.5);
    // Throttle stays at MIL for the climb.
    output.throttle_cmd = takeoff_throttle;

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
