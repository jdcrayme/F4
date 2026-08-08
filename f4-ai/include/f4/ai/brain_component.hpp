// f4-ai/include/f4/ai/brain_component.hpp
//
// BrainComponent — wraps a TakeoffModule as a BehavioralComponent.
//
// Phase A.2: this is the first behavioral brain component. It runs in
// pass 1 (priority 100), reads the parent entity's FlightModelComponent,
// calls TakeoffModule::update() to get an AIControlOutput, converts it
// to a PilotInput, and writes it to the FlightModelComponent's pending_input
// slot. The FlightModelComponent then runs in pass 2 and integrates the
// FlightModel with that input.
//
// The brain resolves the FlightModelComponent pointer LAZILY in update(),
// not in on_attached(). Why? At on_attached() time, the brain is being
// added to the entity — but the FlightModelComponent may not have been
// added yet (component add order is not guaranteed). Resolving lazily
// in update() means the brain tolerates being added before OR after the
// FlightModelComponent, as long as both are present by the first tick.
//
// If no FlightModelComponent is present on the entity, update() is a
// no-op. This allows a brain to be added to an entity that's temporarily
// "brain-only" (e.g. a ghost/replay entity) without crashing.
//
// The AIControlOutput → PilotInput conversion lives here so the brain is
// self-contained — the sim loop (f4-sim, Phase A.3) just calls
// EntityWorld::update_all(dt, bus) and the brain writes the FM slot directly.
//
// Dependencies: f4-entities (BehavioralComponent, EntityHandle),
// f4-messaging (MessageBus), f4-flight-model (FlightModelComponent,
// PilotInput, AircraftState). C++20.

#pragma once

#include <f4/entities/entity.hpp>
#include <f4/messaging/bus.hpp>
#include <f4/flight/flight_model_component.hpp>
#include <f4/flight/aircraft_state.hpp>

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

    // Capture the owning EntityHandle so we can look up the sibling
    // FlightModelComponent on demand. The handle is stable for the
    // lifetime of the entity — EntityHandle is a value type that
    // captures (EntityId, EntityWorld*, cookie) at construction.
    void on_attached(entities::EntityHandle& self) override {
        owner_ = &self;
    }

    void update(double dt, messaging::MessageBus& bus) override {
        if (!owner_) return;  // not attached to any entity — no-op

        // Lazily resolve the FlightModelComponent. We re-resolve every
        // tick (one hashmap lookup) rather than caching the pointer,
        // because the FM component can be removed and re-added by the
        // host (e.g. entity "possessed" by a different brain). At
        // Phase A scale this is invisible; if it ever shows up in
        // profiles, cache the pointer and invalidate on remove.
        auto* fm_comp = owner_->get<flight::FlightModelComponent>();
        if (!fm_comp) return;  // no flight model on this entity — no-op

        // Initialize the TakeoffModule on first update. We defer to
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
        const auto& state = fm_comp->state();
        const auto ai_out = module_.update(dt, &state);

        // Convert AIControlOutput → PilotInput and write to the FM slot.
        fm_comp->pending_input() = map_to_pilot_input(ai_out);
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
