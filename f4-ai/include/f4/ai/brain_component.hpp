// f4-ai/include/f4/ai/brain_component.hpp
//
// BrainComponent — the mission sequencer, as a BehavioralComponent.
//
// Phase DIGI-1: the brain sequences the mission's flight phases through
// their modules:
//
//   Ground   TakeoffModule       (taxi -> lineup -> takeoff -> departure)
//      |  is_complete() and route non-empty
//   Enroute  NavigationModule    (fly the scenario route; last wp = entry fix)
//      |  is_complete()
//   Approach LandingModule       (approach -> land -> rollout -> taxi-in -> park)
//      |  is_complete()
//   Complete (hold brakes)
//
// With an empty route the mission ends after takeoff (Phase A behavior).
// This is the sequential-handoff stepping stone toward the documented
// DigitalBrain (AI_IMPLEMENTATION_PLAN.md §5): later, a LayeredStateMachine
// DigiMode ladder arbitrates the modules instead of a fixed sequence.
//
// The brain runs in pass 1 (priority 100), reads the parent entity's
// aircraft state through the IAircraftState interface, calls the active
// module's update() to get an AIControlOutput, converts it to a PilotInput,
// and writes it to the IPilotInputSink interface. The flight model
// component then runs in pass 2 and integrates the FlightModel with that
// input.
//
// The brain resolves the interfaces LAZILY in update() and stores the
// owning EntityHandle BY VALUE (EntityHandle is a cheap value type: id +
// world pointer + cookie — see the regression note in entity.hpp's
// on_attached contract).
//
// If no IAircraftState or IPilotInputSink is present on the entity,
// update() is a no-op.
//
// Dependencies: f4-entities, f4-messaging, f4-flight-api. NOT
// f4-flight-model. C++20.

#pragma once

#include <f4/entities/entity.hpp>
#include <f4/messaging/bus.hpp>
#include <f4/flight/api/i_aircraft_state.hpp>
#include <f4/flight/api/i_pilot_input_sink.hpp>
#include <f4/flight/api/pilot_input.hpp>

#include <f4/geo/position.hpp>

#include "f4/ai/ai_output.hpp"
#include "f4/ai/modules/landing_module.hpp"
#include "f4/ai/modules/navigation_module.hpp"
#include "f4/ai/modules/takeoff_module.hpp"

#include <vector>

namespace f4::ai {

// ============================================================================
// MissionPlan — what the host (Simulation) injects into the brain at spawn.
// ============================================================================
struct MissionPlan {
    /// Air-phase route. Empty = no enroute/landing phases (takeoff-only
    /// mission). The LAST waypoint is the approach entry fix handed to
    /// LandingModule when NavigationModule completes.
    std::vector<modules::NavigationModule::Waypoint> route;

    /// Taxi-in route after landing rollout (runway exit -> parking).
    /// Empty = the aircraft parks on the runway.
    std::vector<geo::WorldPosition> taxi_in_route;

    /// Approach style at the end of the route: false = straight-in final
    /// (default), true = full traffic pattern (downwind/base/final).
    bool fly_traffic_pattern{false};
};

// ============================================================================
// BrainComponent
// ============================================================================
class BrainComponent : public entities::BehavioralComponent<BrainComponent> {
public:
    /// Mission phase — which module currently flies the aircraft.
    enum class Phase {
        Ground,     // TakeoffModule
        Enroute,    // NavigationModule
        Approach,   // LandingModule
        Complete    // parked / no further control
    };

    BrainComponent() = default;

    // --- BehavioralComponent overrides ---
    int priority() const noexcept override {
        return entities::update_phase::BRAIN_PRIORITY;  // 100, pass 1
    }

    // Capture the owning EntityHandle so we can look up sibling
    // components on demand. EntityHandle is a lightweight VALUE type
    // (id + world pointer + cookie) — we store it BY VALUE. Storing a
    // pointer to the `self` reference would dangle: `self` aliases the
    // caller's handle (usually a stack local in spawn code) that dies as
    // soon as the spawning function returns.
    void on_attached(entities::EntityHandle& self) override {
        owner_ = self;
    }

    void update(double dt, messaging::MessageBus& bus) override {
        if (!owner_.valid()) return;  // not attached to any entity — no-op

        // Lazily resolve the flight model interfaces (see entity.hpp for
        // why resolution is per-tick rather than cached).
        auto* state = owner_.get_interface<flight::IAircraftState>();
        auto* sink  = owner_.get_interface<flight::IPilotInputSink>();
        if (!state || !sink) return;  // no flight model on this entity

        // Initialize the TakeoffModule on first update (it needs the bus
        // to publish TaxiRequest / subscribe to clearances).
        if (!takeoff_initialized_) {
            auto* world = owner_.world();
            if (!world) return;
            takeoff_.initialize(owner_.id().value, *world, bus);
            takeoff_initialized_ = true;
        }

        // Sequence the mission phases.
        if (phase_ == Phase::Ground && takeoff_.is_complete()) {
            if (!plan_.route.empty()) {
                nav_.set_route(plan_.route);
                phase_ = Phase::Enroute;
            } else {
                phase_ = Phase::Complete;
            }
        }
        if (phase_ == Phase::Enroute && nav_.is_complete()) {
            auto* world = owner_.world();
            if (world) {
                // The route's last waypoint is the approach entry fix.
                const auto& entry_fix = plan_.route.back().position;
                landing_.configure(entry_fix, plan_.taxi_in_route);
                landing_.fly_traffic_pattern = plan_.fly_traffic_pattern;
                landing_.initialize(owner_.id().value, *world, bus);
                phase_ = Phase::Approach;
            } else {
                phase_ = Phase::Complete;
            }
        }
        if (phase_ == Phase::Approach && landing_.is_complete()) {
            phase_ = Phase::Complete;
        }

        // Run the active module: produce AIControlOutput from the state.
        AIControlOutput ai_out;
        switch (phase_) {
            case Phase::Ground:
                ai_out = takeoff_.update(dt, state);
                break;
            case Phase::Enroute:
                ai_out = nav_.update(dt, state);
                break;
            case Phase::Approach:
                ai_out = landing_.update(dt, state);
                break;
            case Phase::Complete:
                ai_out = landing_.hold_complete();  // brakes on, gear down
                break;
        }

        // Write the AI output to the flight model's pending input slot
        // via the IPilotInputSink interface.
        sink->set_pending_input(map_to_pilot_input(ai_out));
    }

    // --- Mission plan (set by the host at spawn, before first tick) ---
    void set_mission_plan(MissionPlan plan) { plan_ = std::move(plan); }
    [[nodiscard]] const MissionPlan& mission_plan() const noexcept { return plan_; }

    // --- Phase / state reporting (HUD + recorder) ---
    [[nodiscard]] Phase phase() const noexcept { return phase_; }
    [[nodiscard]] const char* phase_name() const noexcept {
        switch (phase_) {
            case Phase::Ground:   return "Ground";
            case Phase::Enroute:  return "Enroute";
            case Phase::Approach: return "Approach";
            case Phase::Complete: return "Complete";
        }
        return "?";
    }
    /// Active module's mode name (e.g. "TakeoffMode"); "Complete" at the end.
    [[nodiscard]] std::string mode_name() const {
        switch (phase_) {
            case Phase::Ground:   return takeoff_.mode_name();
            case Phase::Enroute:  return nav_.mode_name();
            case Phase::Approach: return landing_.mode_name();
            case Phase::Complete: return "MissionComplete";
        }
        return {};
    }
    /// Active module's state name (e.g. "OnFinal"); "Parked" at the end.
    [[nodiscard]] std::string state_name() const {
        switch (phase_) {
            case Phase::Ground:   return takeoff_.state_name();
            case Phase::Enroute:  return nav_.state_name();
            case Phase::Approach: return landing_.state_name();
            case Phase::Complete: return "Parked";
        }
        return {};
    }

    // --- Module access (host configuration + test inspection) ---
    [[nodiscard]] modules::TakeoffModule&       takeoff()       noexcept { return takeoff_; }
    [[nodiscard]] const modules::TakeoffModule& takeoff() const noexcept { return takeoff_; }
    [[nodiscard]] modules::NavigationModule&       navigation()       noexcept { return nav_; }
    [[nodiscard]] const modules::NavigationModule& navigation() const noexcept { return nav_; }
    [[nodiscard]] modules::LandingModule&       landing()       noexcept { return landing_; }
    [[nodiscard]] const modules::LandingModule& landing() const noexcept { return landing_; }

    /// Legacy alias for the Phase A API (tests + hosts configure the
    /// takeoff module through this).
    [[nodiscard]] modules::TakeoffModule&       module()       noexcept { return takeoff_; }
    [[nodiscard]] const modules::TakeoffModule& module() const noexcept { return takeoff_; }

private:
    // Map AIControlOutput to PilotInput. The brain is self-contained —
    // no external runner is needed to translate AI output to pilot input.
    static flight::PilotInput map_to_pilot_input(const AIControlOutput& ai_out) {
        flight::PilotInput pi;
        pi.pstick    = ai_out.pitch_cmd;
        pi.rstick    = ai_out.roll_cmd;
        pi.ypedal    = ai_out.yaw_cmd;
        pi.throttle  = ai_out.throttle_cmd;
        pi.speedBrake = ai_out.speed_brake_cmd;
        pi.gearHandle = ai_out.gear_handle_down ? 1.0 : -1.0;
        pi.wheelBrakes = ai_out.wheel_brakes;
        pi.parkingBrake = ai_out.parking_brake;
        pi.noseSteerOn = true;  // always on for AI
        pi.validate();
        return pi;
    }

    entities::EntityHandle owner_{};

    // Mission plan + sequencing.
    MissionPlan plan_;
    Phase phase_{Phase::Ground};

    // Phase modules. Only the takeoff module initializes up front; the
    // navigation module takes its route at handoff, and the landing module
    // initializes (bus subscriptions) at its handoff.
    modules::TakeoffModule takeoff_;
    bool takeoff_initialized_{false};
    modules::NavigationModule nav_;
    modules::LandingModule landing_;
};

} // namespace f4::ai
