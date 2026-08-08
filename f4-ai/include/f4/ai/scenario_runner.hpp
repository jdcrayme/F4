// f4-ai/include/f4/ai/scenario_runner.hpp
//
// ScenarioRunner — headless simulation loop with flight recording.
//
// This is the integration point that wires together:
//   - An AI brain (or individual module)
//   - The FlightModel
//   - The FlightRecorder
//   - The MessageBus (for ATC, wingman, etc.)
//
// It runs a fixed-timestep simulation without any rendering, producing
// a JSON recording that can be loaded in the world viewer for replay.
//
// Usage:
//   ScenarioRunner runner;
//   runner.set_flight_model(&fm);
//   runner.set_brain(&brain);
//   runner.set_recorder(&recorder);
//   runner.set_message_bus(&bus);
//   runner.run(duration_s, dt);
//   recorder.write_json("takeoff_kunsan.json");
//
// Dependencies: f4-flight-model, f4-ai, f4-recorder, f4-entities, f4-messaging.
// C++20.

#pragma once

#include <cstdint>
#include <string>

#include <f4/flight/flight_model.hpp>
#include <f4/flight/aircraft_state.hpp>
#include <f4/ai/ai_brain.hpp>
#include <f4/ai/ai_output.hpp>
#include <f4/recorder/flight_recorder.hpp>
#include <f4/entities/entity.hpp>
#include <f4/messaging/bus.hpp>

namespace f4::ai {

// ============================================================================
// ScenarioRunner
// ============================================================================
class ScenarioRunner {
public:
    // --- Configuration ---

    void set_flight_model(flight::FlightModel* fm) { fm_ = fm; }
    void set_brain(ai::IAIBrain* brain) { brain_ = brain; }
    void set_recorder(recorder::FlightRecorder* rec) { recorder_ = rec; }
    void set_message_bus(messaging::MessageBus* bus) { bus_ = bus; }
    void set_entity_world(entities::EntityWorld* world) { world_ = world; }

    void set_entity_id(std::uint64_t id) { entity_id_ = id; }
    void set_callsign(std::string cs) { callsign_ = std::move(cs); }
    void set_scenario_name(std::string name) { scenario_name_ = std::move(name); }

    // Snapshot rate: record every N ticks (1 = every tick, 10 = every 10th).
    void set_snapshot_rate(int every_n_ticks) { snapshot_rate_ = every_n_ticks; }

    // --- Run ---
    // Execute the simulation loop for the given duration at fixed timestep dt.
    // Produces a recording in the FlightRecorder.
    void run(double duration_s, double dt);

    // --- Accessors ---
    [[nodiscard]] std::uint64_t total_ticks() const noexcept { return total_ticks_; }
    [[nodiscard]] double total_sim_time() const noexcept { return total_sim_time_; }

private:
    // Build a FlightSnapshot from the current simulation state.
    recorder::FlightSnapshot build_snapshot(
        std::uint64_t tick, double sim_time_s) const;

    // Map AIControlOutput to PilotInput.
    static flight::PilotInput map_to_pilot_input(const ai::AIControlOutput& ai_out);

    flight::FlightModel* fm_{nullptr};
    ai::IAIBrain* brain_{nullptr};
    recorder::FlightRecorder* recorder_{nullptr};
    messaging::MessageBus* bus_{nullptr};
    entities::EntityWorld* world_{nullptr};

    std::uint64_t entity_id_{0};
    std::string callsign_{"Viper1"};
    std::string scenario_name_;
    int snapshot_rate_{1};  // every tick

    std::uint64_t total_ticks_{0};
    double total_sim_time_{0.0};
};

} // namespace f4::ai
