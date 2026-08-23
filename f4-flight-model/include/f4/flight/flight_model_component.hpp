// f4-flight-model/include/f4/flight/flight_model_component.hpp
//
// FlightModelComponent — wraps a FlightModel as a BehavioralComponent.
//
// Phase A.2: this is the first behavioral component that owns real simulation
// state. The brain (BrainComponent in f4-ai) runs in pass 1 (priority 100),
// produces an AIControlOutput, converts it to a PilotInput, and writes it
// to this component's pending_input_ slot via the IPilotInputSink interface.
// This component runs in pass 2 (priority 50), reads pending_input_,
// integrates the FlightModel one step, then clears pending_input_ — so a
// tick where no brain ran produces idle controls (zero stick, zero throttle,
// gear down, brakes on).
//
// Phase 2 (H1/H2): FlightModelComponent implements IAircraftState and
// IPilotInputSink. The AI accesses the flight model through these thin
// interfaces rather than reaching into FlightModelComponent internals.
// IAircraftState exposes the aircraft state subset the AI needs (position,
// airspeed, altitude, heading, ground contact) in the AI's natural ENU
// frame. IPilotInputSink exposes the write side (pending_input).
// This decouples AI modules from the full AircraftState struct and
// eliminates NED↔ENU conversion scattered through AI code.
//
// The component also exposes ground_z_ft / ground_normal slots that the
// host (or a future terrain system) writes each tick. In Phase A the host
// sets these once at startup (flat ground at z=0, normal = straight up)
// and they don't change; a future terrain component will write them per-tick.
//
// Lifetime: the FlightModel is owned by-value inside the component. This
// is acceptable because FlightModel is non-copyable/non-movable but is
// constructible (default ctor) and re-initializable via init(). The
// component is itself non-copyable (inherits FlightModel's restrictions)
// but movable through the BehavioralComponent storage path — actually
// it ISN'T movable either, because the FlightModel is a member, and
// components are stored as unique_ptr<ComponentBase> in EntityRecord,
// so the component never actually moves after construction. Good.
//
// Dependencies: f4-entities (BehavioralComponent), f4-messaging (MessageBus,
// already a dep of f4-flight-model). C++20.

#pragma once

#include "f4/flight/flight_model.hpp"
#include "f4/flight/aircraft_state.hpp"
#include "f4/flight/i_aircraft_state.hpp"
#include "f4/flight/i_pilot_input_sink.hpp"

#include <f4/entities/entity.hpp>
#include <f4/messaging/bus.hpp>

namespace f4::flight {

// ============================================================================
// FlightModelComponent
// ============================================================================
class FlightModelComponent : public entities::BehavioralComponent<FlightModelComponent>,
                             public IAircraftState,
                             public IPilotInputSink {
public:
    FlightModelComponent() = default;

    // Non-copyable, non-movable — FlightModel is both, and we hold one by value.
    FlightModelComponent(const FlightModelComponent&) = delete;
    FlightModelComponent& operator=(const FlightModelComponent&) = delete;
    FlightModelComponent(FlightModelComponent&&) = delete;
    FlightModelComponent& operator=(FlightModelComponent&&) = delete;

    // --- BehavioralComponent overrides ---
    int priority() const noexcept override {
        return entities::update_phase::PHYSICS_PRIORITY;  // 50, pass 2
    }

    void update(double dt, messaging::MessageBus& bus) override {
        // Skip if the FlightModel hasn't been initialized yet. The host
        // calls init() (or model().init()) once at startup; before that,
        // the FM's subsystems have empty tables and calling update()
        // would crash on null deref or produce garbage. This makes the
        // component safe to add to an entity and tick via update_all
        // before the host has wired up the config — important for the
        // brain to be able to initialize (it runs in pass 1, before the
        // FM in pass 2, and the first brain tick may fire before the
        // host has called init()).
        if (!initialized_) return;

        // Drive the FlightModel one step using the pending input.
        // If no brain wrote to pending_input_ this tick, we use idle
        // controls (the default-constructed PilotInput has throttle=0,
        // brakes=false, gear=down — safe for an aircraft sitting on
        // the ground, but NOT safe for an aircraft in flight).
        //
        // After consuming, clear pending_input_ so next tick starts
        // from idle. A brain that wants to keep controlling must write
        // every tick.
        fm_.update(dt, pending_input_, ground_z_ft_, ground_normal_);
        pending_input_ = PilotInput{};

        // Forward the bus to the FlightModel so it can publish stall
        // events. set_message_bus is idempotent and cheap (one pointer
        // store) so calling it every tick is fine.
        fm_.set_message_bus(&bus);
    }

    // --- Initialization ---
    // Wraps FlightModel::init() and sets the initialized_ flag so update()
    // stops skipping. The host can either call this wrapper OR call
    // model().init() directly followed by mark_initialized().
    void init(const f4::data::AircraftConfig& cfg,
              double initialAltitude_ft,
              double initialVt_ftps,
              double initialHeading_rad,
              bool inAir,
              double initialNorth_ft = 0.0,
              double initialEast_ft = 0.0) {
        fm_.init(cfg, initialAltitude_ft, initialVt_ftps, initialHeading_rad, inAir);
        // World placement: the FM integrates from (0,0) by default; hosts
        // with an absolute ENU spawn (e.g. a campaign airfield at grid
        // (234,655)) must seed the NED position or every AI reads the
        // aircraft at the theater origin.
        fm_.state().kin.x = initialNorth_ft;
        fm_.state().kin.y = initialEast_ft;
        initialized_ = true;
    }

    // If the host calls model().init() directly (bypassing the wrapper),
    // it must call this to tell the component the FM is ready to tick.
    void mark_initialized() noexcept { initialized_ = true; }

    [[nodiscard]] bool is_initialized() const noexcept { return initialized_; }

    // --- FlightModel access ---
    [[nodiscard]] FlightModel&       model()       noexcept { return fm_; }
    [[nodiscard]] const FlightModel& model() const noexcept { return fm_; }

    [[nodiscard]] const AircraftState& state() const noexcept { return fm_.state(); }

    // --- Pending input slot (brain writes here) ---
    // Written by BrainComponent::update() in pass 1, read by this
    // component's update() in pass 2.
    [[nodiscard]] PilotInput&       pending_input()       noexcept { return pending_input_; }
    [[nodiscard]] const PilotInput& pending_input() const noexcept { return pending_input_; }

    // --- Ground state slots (host or terrain system writes here) ---
    void set_ground(double z_ft, const math::Vec3d& normal) noexcept {
        ground_z_ft_ = z_ft;
        ground_normal_ = normal;
    }
    [[nodiscard]] double       ground_z_ft()     const noexcept { return ground_z_ft_; }
    [[nodiscard]] math::Vec3d  ground_normal()   const noexcept { return ground_normal_; }

    // --- IAircraftState implementation ---
    // Presents aircraft state in ENU frame (the AI's natural frame).
    // The flight model stores position internally as NED; these methods
    // perform the conversion so AI consumers never handle NED directly.
    //
    // NED convention (internal): x=north, y=east, z=down
    // ENU convention (interface): x=east, y=north, z=up
    // Crossing: enu_x = ned_y,  enu_y = ned_x,  enu_z = -ned_z

    double position_east_ft() const override {
        return fm_.state().kin.y;   // NED y = east
    }

    double position_north_ft() const override {
        return fm_.state().kin.x;   // NED x = north
    }

    double altitude_msl_ft() const override {
        return -fm_.state().kin.z;  // NED z-down → ENU z-up
    }

    double altitude_agl_ft() const override {
        const auto& s = fm_.state();
        return -s.kin.z - s.gear.groundZ_ft;
    }

    double vcas_kts() const override {
        return fm_.state().vcas;
    }

    double heading_rad() const override {
        return to_radians(fm_.state().kin.psi);
    }

    double pitch_angle_rad() const override {
        return to_radians(fm_.state().kin.theta);
    }

    double roll_angle_rad() const override {
        return to_radians(fm_.state().kin.phi);
    }

    double roll_rate_radps() const override {
        // kin.p is body-axis roll rate (rad/s, + = rolling right).
        return fm_.state().kin.p;
    }

    double pitch_rate_radps() const override {
        // kin.q is body-axis pitch rate (rad/s, + = pitching up).
        return fm_.state().kin.q;
    }

    double vertical_speed_fpm() const override {
        // NED z is down and zdot is in ft/s: climbing = negative zdot.
        return -fm_.state().kin.zdot * 60.0;
    }

    bool on_ground() const override {
        return !fm_.state().gear.inAir;
    }

    // --- IPilotInputSink implementation ---
    void set_pending_input(const PilotInput& input) override {
        pending_input_ = input;
    }

private:
    FlightModel fm_;

    // Set to true by init() or mark_initialized(). When false, update()
    // is a no-op (the FM's tables are empty; calling update() would crash).
    bool initialized_{false};

    // The brain's output for this tick. Default-constructed = idle controls.
    // Written by BrainComponent in pass 1, consumed and cleared in pass 2.
    PilotInput pending_input_{};

    // Ground state. Defaults: flat ground at z=0, normal pointing up
    // (NED frame: -Z is up, so up = (0, 0, -1)).
    double      ground_z_ft_{0.0};
    math::Vec3d ground_normal_{0.0, 0.0, -1.0};
};

} // namespace f4::flight
