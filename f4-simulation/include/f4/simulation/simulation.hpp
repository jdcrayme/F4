// f4-simulation/include/f4/simulation/simulation.hpp
//
// Simulation — owns the EntityWorld + MessageBus + ModelDatabase + AircraftConfig
// registry and runs the tick loop. NO rendering — that's the executable's job.
//
// This separation lets us run headless scenarios (CI, trace generation,
// integration tests) without dragging in Raylib. Mirrors the f4-models (lib)
// + f4-models-viewer (exe) split.
//
// Lifecycle:
//   Simulation sim(scenario, asset_dir);
//   sim.initialize();          // loads models, aircraft config, spawns aircraft, wires ATC
//   while (...) {
//       sim.tick(dt);          // world.update_all + flush + sync transforms + record
//   }
//   sim.write_recording();     // writes trace.json if scenario.record is true
//
// The aircraft entity carries four sibling components:
//   - TransformComponent       (where it is)
//   - FlightModelComponent     (how it moves; implements IAircraftState + IPilotInputSink)
//   - VisualModelComponent     (what the renderer draws; the new component)
//   - BrainComponent           (who's flying; runs in pass 1, finds FM via interface lookup)
//
// The brain finds the flight model via interface-based lookup
// (get_interface<IAircraftState>()), not a raw pointer. The entity ID is
// the binding — there is no AircraftClass equivalent. See
// Docs/AIRCRAFT_BINDING_DESIGN.md for the full rationale.
//
// Dependencies: f4-entities, f4-messaging, f4-flight-model, f4-flight-api,
// f4-ai, f4-data, f4-geo, f4-math, f4-units, f4-state-machine, f4-models,
// f4-recorder, f4-json, f4-io. C++20.

#pragma once

#include <f4/entities/entity.hpp>
#include <f4/messaging/bus.hpp>
#include <f4/models/model_database.hpp>
#include <f4/data/aircraft_config.hpp>

#include <cstdint>
#include <filesystem>
#include <memory>

#include "f4/simulation/scenario.hpp"

namespace f4::ai::atc { class StubATC; }
namespace f4::recorder { class FlightRecorder; }

namespace f4::simulation {

/// Simulation owns the EntityWorld + MessageBus + ModelDatabase + AircraftConfig
/// registry and runs the tick loop. NO rendering — that's the executable's job.
class Simulation {
public:
    explicit Simulation(Scenario scenario, std::filesystem::path asset_dir);
    ~Simulation();

    Simulation(const Simulation&) = delete;
    Simulation& operator=(const Simulation&) = delete;
    Simulation(Simulation&&) = delete;
    Simulation& operator=(Simulation&&) = delete;

    /// Load all assets (models, aircraft config), build the EntityWorld,
    /// spawn aircraft, wire ATC. Throws on failure.
    void initialize();

    /// Advance the simulation by one tick. Calls world_.update_all(dt, bus_),
    /// flushes deferred ATC messages, syncs TransformComponent + VisualModelComponent
    /// from the FM state for every aircraft entity, and records a snapshot
    /// per aircraft if recording is enabled.
    void tick(double dt);

    /// Write the flight recording to disk (if recording was enabled).
    void write_recording();

    // --- Accessors for the renderer / host ---
    [[nodiscard]] entities::EntityWorld&       world()       noexcept { return world_; }
    [[nodiscard]] const entities::EntityWorld& world() const noexcept { return world_; }

    /// The message bus. Hosts use this to observe ATC traffic (e.g. the
    /// scenario player's radio transcript overlay) by subscribing after
    /// initialize(). Simulation::tick flushes deferred messages each tick.
    [[nodiscard]] messaging::MessageBus&       bus()       noexcept { return bus_; }
    [[nodiscard]] const messaging::MessageBus& bus() const noexcept { return bus_; }

    /// The primary (first) aircraft entity. Convenience accessor for hosts
    /// that only care about one aircraft (e.g. the camera focus). Returns
    /// a default-constructed EntityId (value=0) if no aircraft were spawned.
    [[nodiscard]] entities::EntityId aircraft_entity() const noexcept {
        return aircraft_entities_.empty() ? entities::EntityId{} : aircraft_entities_.front();
    }

    /// All spawned aircraft entities. Phase 2: the sim tracks N aircraft,
    /// one per Flight in the campaign (or one per ScenarioAircraft entry,
    /// depending on spawn_mode).
    [[nodiscard]] const std::vector<entities::EntityId>& aircraft_entities() const noexcept {
        return aircraft_entities_;
    }

    /// All spawned airfield-feature entities (Phase 2A). Each carries
    /// TransformComponent + VisualModelComponent (no FM, no brain). The
    /// renderer iterates all VisualModelComponent-bearing entities to draw
    /// both aircraft and features uniformly.
    [[nodiscard]] const std::vector<entities::EntityId>& feature_entities() const noexcept {
        return feature_entities_;
    }

    [[nodiscard]] const f4::models::ModelDatabase& model_db() const noexcept { return *model_db_; }
    [[nodiscard]] const Scenario& scenario() const noexcept { return scenario_; }
    [[nodiscard]] double sim_time_s() const noexcept { return sim_time_s_; }
    [[nodiscard]] std::uint64_t tick_count() const noexcept { return tick_; }
    [[nodiscard]] bool paused() const noexcept { return paused_; }
    void set_paused(bool p) noexcept { paused_ = p; }
    void set_time_scale(double s) noexcept { time_scale_ = s; }

private:
    void load_models();           // KoreaObj.HDR/.LOD/.TEX -> ModelDatabase
    void load_aircraft_config();  // f16.json -> AircraftConfig
    void spawn_aircraft();        // spawn_mode dispatch (scenario_list | campaign_flights)
    void spawn_from_scenario_list();        // Phase 1: hand-authored aircraft[]
    void spawn_from_campaign_flights();     // Phase 2: campaign-derived roster
    void spawn_airfield_features();         // Phase 2A: static features → VMC entities
    void wire_atc();              // StubATC + AirfieldConfig from scenario
    void record_snapshot();

    // --- Owned state ---
    Scenario scenario_;
    std::filesystem::path asset_dir_;

    entities::EntityWorld world_;
    messaging::MessageBus bus_;
    std::unique_ptr<f4::models::ModelDatabase> model_db_;
    std::unique_ptr<f4::ai::atc::StubATC> atc_;
    std::unique_ptr<f4::recorder::FlightRecorder> recorder_;
    f4::data::AircraftConfig aircraft_cfg_;

    // Phase 2: replaced the single `aircraft_entity_` with a vector. The
    // Phase 1 spawn path (scenario_list) pushes one entry; the Phase 2 path
    // (campaign_flights) pushes one per Flight unit found in the world JSON.
    std::vector<entities::EntityId> aircraft_entities_;

    // Phase 2A: static airfield-feature entities (buildings, runway sections,
    // taxiways, towers, hangars). Each carries TransformComponent +
    // VisualModelComponent. Tracked separately from aircraft so tick() doesn't
    // try to sync them from a flight model (they have none).
    std::vector<entities::EntityId> feature_entities_;

    double sim_time_s_{0.0};
    std::uint64_t tick_{0};
    bool paused_{false};
    double time_scale_{1.0};
};

} // namespace f4::simulation
