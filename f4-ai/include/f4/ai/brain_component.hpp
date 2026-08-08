// f4-ai/include/f4/ai/brain_component.hpp
//
// BrainComponent — wraps a TakeoffModule as a BehavioralComponent.
//
// Phase A.2: this is the first behavioral brain component. It runs in
// pass 1 (priority 100), reads the parent entity's aircraft state
// through the IAircraftState interface, calls TakeoffModule::update()
// to get an AIControlOutput, converts it to a PilotInput, and writes
// it to the IPilotInputSink interface. The flight model component
// then runs in pass 2 and integrates the FlightModel with that input.
//
// Phase 2+ (H1): BrainComponent is FULLY DECOUPLED from
// FlightModelComponent. It resolves the aircraft state and control
// sink via EntityHandle::get_interface<IAircraftState>() and
// get_interface<IPilotInputSink>() — interface-based lookup that
// doesn't require knowing the concrete component type. This means
// f4-ai no longer depends on f4-flight-model at all; it depends only
// on f4-flight-api (the lightweight interface library).
//
// The brain resolves the interfaces LAZILY in update(), not in
// on_attached(). Why? At on_attached() time, the brain is being
// added to the entity — but the flight model component may not have
// been added yet (Gcomponent add order is not guaranteed). Resolving
// lazily in update() means the brain tolerates being added before OR
// after the flight model component, as long as both are present by
// the first tick.
//
// If no IAircraftState or IPilotInputSink is present on the entity,
// update() is a no-op. This allows a brain to be added to an entity
// that's temporarily "brain-only" (e.g. a ghost/replay entity)
// without crashing.
//
// The AIControlOutput → PilotInput conversion lives here so the brain is
// self-contained — the sim loop (f4-sim) just calls
// EntityWorld::update_all(dt, bus) and the brain writes the sink directly.
//
// Dependencies: f4-entities (BehavioralComponent, EntityHandle, get_interface),
// f4-messaging (MessageBus), f4-flight-api (IAircraftState, IPilotInputSink,
// PilotInput). NOT f4-flight-model. C++20.

#pragma once

#include <f4/entities/entity.hpp>
#include <f4/messaging/bus.hpp>
#include <f4/flight/api/i_aircraft_state.hpp>
#include <f4/flight/api/i_pilot_input_sink.hpp>
#include <f4/flight/api/pilot_input.hpp>

#include "f4/ai/ai_output.hpp"
#include "f4/ai/modules/takeoff_module.hpp"

namespace f4::ai {

// ============================================================================
// BrainComponent
// ============================================================================
class BrainComponent : public entities::BehavioralComponent<BrainComponent> {
public:
    BrainComponent() = default;

    // --- BehavioralComponent overrides ---
    int priority() const noexcept override {
        return entities::update_phase::BRAIN_PRIORITY;  // 100, pass 1
    }

    // Capture the owning EntityHandle so we can look up sibling
    // components on demand. The handle is stable for the
    // lifetime of the entity — EntityHandle is a value type that
    // captures (EntityId, EntityWorld*, cookie) at construction.
    void on_attached(entities::EntityHandle& self) override {
        owner_ = &self;
    }

    void update(double dt, messaging::MessageBus& bus) override {
        if (!owner_) return;  // not attached to any entity — no-op

        // Lazily resolve the flight model interfaces. We re-resolve every
        // tick (one dynamic_cast scan per interface) rather than caching
        // the pointers, because the flight model component can be removed
        // and re-added by the host. At Phase A scale (3-5 components per
        // entity) this is invisible; if it ever shows up in profiles,
        // cache the pointers and invalidate on remove.
        auto* state = owner_->get_interface<flight::IAircraftState>();
        auto* sink  = owner_->get_interface<flight::IPilotInputSink>();
        if (!state || !sink) return;  // no flight model on this entity

        // Initialize the TakeoffModule onG first update. We defer to
        // first update() (rather than on_attached()) because the
        // MessageBus reference is passed to update() but not to
        // on_attached(). The module needs the bus to subscribe to ATC
        // clearances and publish requests.
        if (!module_initialized_) {
            auto* world = owner_->world();
            if (!world) return;  // no world bound — can't initialize
            module_.initialize(owner_->id().value, *world, bus);
            module_initialized_ = true;
        }

        // Run the brain: produce AIControlOutput from the current state.
        // The IAircraftState interface presents position in ENU (the AI's
        // natural frame) and exposes only the fields the AI needs.
        const auto ai_out = module_.update(dt, state);

        // Write the AI output to the flight model's pending input slot
        // via the IPilotInputSink interface.
        sink->set_pending_input(map_to_pilot_input(ai_out));
    }

    // --- TakeoffModule access ---
    // Exposed so the host can configure the module (rotate speed, gear-up
    // altitude, etc.) and attach a trace.
    [[nodiscard]] modules::TakeoffModule&       module()       noexcept { return module_; }
    [[nodiscard]] const modules::TakeoffModule& module() const noexcept { return module_; }

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

    entities::EntityHandle* owner_{nullptr};
    modules::TakeoffModule  module_;
    bool                    module_initialized_{false};
};

} // namespace f4::ai
