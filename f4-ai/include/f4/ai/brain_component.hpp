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

#include <optional>
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

    /// Starting mission phase. Default Ground (taxi -> takeoff -> ...).
    /// Set to Approach to skip directly to the landing module — used by
    /// the isolated `landing_only` diagnostic scenario which spawns the
    /// aircraft already on final (see FLIGHT_CONTROL_NEXT_STEPS.md §3.2).
    /// When set to Approach, the host MUST also set the route's last
    /// waypoint at the approach entry fix — the brain uses that position
    /// to configure the LandingModule.
    enum class StartPhase { Ground, Approach };
    StartPhase start_phase{StartPhase::Ground};
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

        // Phase 0c (isolated scenarios): if the mission plan says to start
        // in Approach, skip the takeoff/navigation phases entirely. The
        // host spawns the aircraft airborne on final, and the brain hands
        // off directly to the LandingModule (configured with the route's
        // last waypoint as the approach entry fix). Used by the
        // `landing_only` diagnostic scenario.
        if (phase_ == Phase::Ground &&
            plan_.start_phase == MissionPlan::StartPhase::Approach &&
            !plan_.route.empty()) {
            auto* world = owner_.world();
            if (world) {
                const auto& entry_fix = plan_.route.back().position;
                landing_.configure(entry_fix, plan_.taxi_in_route);
                landing_.fly_traffic_pattern = plan_.fly_traffic_pattern;
                landing_.initialize(owner_.id().value, *world, bus);
                phase_ = Phase::Approach;
            } else {
                phase_ = Phase::Complete;
            }
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

        // Watchdog (Phase 5a): if the AI produced an empty output (no
        // module set anything), hold the last known good PilotInput
        // rather than letting the FM fly idle for that tick. The default
        // PilotInput{} has throttle=0 and gear=down — safe on the ground,
        // catastrophic in flight (a 1-tick idle transient at low altitude
        // is enough to drop the aircraft into the ground).
        //
        // An output is "empty" if NONE of the meaningful control fields
        // were set. The Complete phase sets brakes+gear explicitly, so
        // it's not affected. Empty outputs happen during phase transitions
        // (e.g. NavigationModule Done before BrainComponent sequences to
        // Approach) or if a module returns {} by mistake.
        const bool empty = (ai_out.pitch_cmd == 0.0 &&
                            ai_out.roll_cmd == 0.0 &&
                            ai_out.yaw_cmd == 0.0 &&
                            ai_out.throttle_cmd == 0.0 &&
                            !ai_out.gear_handle_down &&
                            !ai_out.wheel_brakes &&
                            !ai_out.parking_brake);
        if (empty && last_pilot_input_.has_value() && phase_ != Phase::Complete) {
            sink->set_pending_input(*last_pilot_input_);
            return;
        }

        // Write the AI output to the flight model's pending input slot
        // via the IPilotInputSink interface, and cache it for the watchdog.
        const flight::PilotInput pi = map_to_pilot_input(ai_out);
        last_pilot_input_ = pi;
        sink->set_pending_input(pi);
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
        // Phase C1: forward flap commands to the FM. The FM already actuates
        // tefPos/lefPos from these fields (flight_model.cpp:453-454); before
        // this wiring the AI never set them so the aircraft always flew with
        // flaps retracted — causing 60+ kt excess approach speed and
        // doubling the landing roll distance.
        pi.tefCmd = ai_out.tef_cmd;
        pi.lefCmd = ai_out.lef_cmd;
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

    // Watchdog cache: the last non-empty PilotInput the brain produced.
    // Held for one tick if a module returns an empty AIControlOutput
    // (e.g. during a phase transition) so the FM doesn't see a 1-tick
    // idle transient. See update() for the rationale.
    std::optional<flight::PilotInput> last_pilot_input_;
};

} // namespace f4::ai
