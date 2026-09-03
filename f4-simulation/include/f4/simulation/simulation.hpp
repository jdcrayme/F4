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
// f4-recorder, f4-json, f4-io, f4-world, f4-terrain, f4-weapons, f4-sensors
// (the last two drive the combat chain — COMBAT_CHAIN_PLAN.md M3). C++20.

#pragma once

#include <f4/entities/entity.hpp>
#include <f4/messaging/bus.hpp>
#include <f4/models/model_database.hpp>
#include <f4/data/aircraft_config.hpp>
#include <f4/data/brain_data.hpp>       // SimData BRAINDAT.brn archetypes
#include <f4/data/formation_data.hpp>  // SimData FORMDAT.FIL formations
#include <f4/weapons/weapon_class_table.hpp>

#include <cstdint>
#include <filesystem>
#include <memory>
#include <vector>

#include "f4/simulation/scenario.hpp"
#include <f4/simulation/campaign_bridge.hpp>  // AirbaseAirfieldMap (B.3+)

#include <f4/terrain/terrain_source.hpp>  // TerrainSource (Path B1)

namespace f4::ai::atc { class StubATC; }
namespace f4::recorder { class FlightRecorder; class FcsTraceWriter; }

namespace f4::simulation {

class BubbleManager;   // forward declaration — defined in bubble_manager.hpp
class RadarBackedDetectionPolicy;  // combat_policies_ storage (combat_bridge.hpp)

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

    /// Write the FCS/AI/EOM CSV trace to disk (if fcs_trace_path was set).
    /// Called by the host at the end of a run, alongside write_recording().
    void write_fcs_trace();

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

    /// Register an aircraft entity a HOST spawned into world() AFTER
    /// initialize() — the campaign-spawner path (a MissionIntent
    /// materializes mid-run through spawn_aircraft_for_intent /
    /// CampaignSimSpawner). Registered entities join the tick loop's
    /// roster — the ground-elevation pre-pass, the combat-intents
    /// active roster, the per-aircraft FM → TransformComponent sync,
    /// and the recorder — exactly like aircraft the initialize() spawn
    /// paths created.
    ///
    /// Without registration, update_all still advances a late-spawned
    /// aircraft's brain + flight model, but its TransformComponent
    /// never moves: the sync loop walks only the roster. That is the
    /// "materialized but not flying" gap (aircraft park forever on the
    /// renderer's screen while their FM state flies on) — this call
    /// closes it for every host.
    ///
    /// Requirements: the entity must already exist in world() and carry
    /// its components (this call only records the id). Idempotent per
    /// entity — a duplicate registration is a no-op. NOT covered for
    /// late registrants: wingman-ref resolution and SimData AI profile
    /// injection (both run once at initialize(); the campaign spawn
    /// path sets no lead_callsign and no brain_profile, so its aircraft
    /// need neither). Returns true when the entity was newly added,
    /// false on duplicate or unknown entity.
    bool register_aircraft(entities::EntityId id);

    /// All spawned airfield-feature entities (Phase 2A). Each carries
    /// TransformComponent + VisualModelComponent (no FM, no brain). The
    /// renderer iterates all VisualModelComponent-bearing entities to draw
    /// both aircraft and features uniformly.
    [[nodiscard]] const std::vector<entities::EntityId>& feature_entities() const noexcept {
        return feature_entities_;
    }

    /// All parked-aircraft entities spawned from Squadron deaggregation.
    /// Each has the same component shape as a Flight-spawned aircraft
    /// (Transform + FM + VMC + Brain), but the brain is dormant (parked,
    /// not taxiing). These are spawned once at initialize() (Squadrons
    /// don't move, so no per-tick re-deaggregation is needed).
    [[nodiscard]] const std::vector<entities::EntityId>& squadron_aircraft_entities() const noexcept {
        return squadron_aircraft_entities_;
    }

    /// The BubbleManager (Mode B). Null when the sim is not in campaign-flights
    /// mode (BubbleManager only makes sense when the EntityWorld contains
    /// campaign units with VehicleCompositionComponent). Hosts can call
    /// force_deaggregate / force_reaggregate on it for scenario overrides.
    [[nodiscard]] BubbleManager* bubble_manager() const noexcept {
        return bubble_manager_.get();
    }

    /// Set the terrain elevation source. The sim queries it each tick
    /// for each aircraft to set the flight model's ground plane. When
    /// null (the default), the sim uses a FlatTerrainSource at the
    /// parking spot's altitude (pre-terrain behavior — ground is flat).
    ///
    /// The host typically wraps f4::terrain::TerrainData in a
    /// TerrainDataAdapter and registers it here after initialize().
    /// Must be called BEFORE the first tick() for the terrain to take
    /// effect from the start. The sim does NOT take ownership of the
    /// raw pointer — the host must keep the source alive for the sim's
    /// lifetime (or until set_terrain_source is called again).
    void set_terrain_source(f4::terrain::TerrainSource* source) noexcept {
        terrain_source_ = source;
    }

    [[nodiscard]] f4::terrain::TerrainSource* terrain_source() const noexcept {
        return terrain_source_;
    }

    [[nodiscard]] const f4::models::ModelDatabase& model_db() const noexcept { return *model_db_; }
    [[nodiscard]] const Scenario& scenario() const noexcept { return scenario_; }

    // --- SimData AI data (diagnostics; see apply_simdata_ai_profiles) ---
    /// True when the brain archetype data actually loaded at initialize()
    /// (i.e. some aircraft referenced brain_profile). False = the lazy
    /// contract held: nothing referenced it, nothing loaded, no behavior
    /// changed. The injected pointers themselves live on the brains
    /// (BrainComponent::brain_archetype) — these flags only witness the
    /// load side of the wiring.
    [[nodiscard]] bool brain_data_loaded() const noexcept {
        return brain_data_loaded_;
    }
    /// True when the FORMDAT formation library actually loaded at
    /// initialize() (some wingman referenced formation). Same lazy
    /// contract as brain_data_loaded().
    [[nodiscard]] bool formation_library_loaded() const noexcept {
        return formation_library_loaded_;
    }

    [[nodiscard]] double sim_time_s() const noexcept { return sim_time_s_; }
    [[nodiscard]] std::uint64_t tick_count() const noexcept { return tick_; }
    [[nodiscard]] bool paused() const noexcept { return paused_; }
    void set_paused(bool p) noexcept { paused_ = p; }

    /// The flight recorder (null when the scenario disabled recording).
    /// M4: hosts may query the live recording (snapshots + combat events)
    /// during a run, and the combat event bridge subscribes through this
    /// accessor. The recorder is written by tick() — read-only consumers
    /// between ticks see a consistent per-tick view.
    [[nodiscard]] f4::recorder::FlightRecorder* recorder() noexcept {
        return recorder_.get();
    }
    [[nodiscard]] const f4::recorder::FlightRecorder* recorder() const noexcept {
        return recorder_.get();
    }

    // --- Combat chain (M3 integration; see combat_bridge.hpp) -----------------
    /// The weapon class data the sim runs with (built-in placeholder set —
    /// the FALCON4.WST import replaces the card contents later without
    /// touching call sites). Hosts launch through THIS table:
    ///   weapons::launch_missile(sim.world(), sim.bus(), shooter_handle,
    ///                           target_id, sim.weapon_table(), handle, t);
    [[nodiscard]] const f4::weapons::WeaponClassTable& weapon_table() const noexcept {
        return weapon_table_;
    }
    /// Trace metadata ONLY — does NOT affect tick(). tick(dt) is
    /// authoritative: hosts pace the sim by calling tick() with a FIXED
    /// dt (the scenario's sim_dt) once per unit of owed sim time (the
    /// scenario player drains a wall-clock accumulator in whole sim_dt
    /// ticks). This value is merely recorded in the FCS CSV trace's
    /// time_scale column so baseline runs (e.g. 1x vs 10x wall-clock
    /// pacing) stay identifiable after the fact. Replaces the old
    /// behavioral set_time_scale()/time_scale_ scaling, which silently
    /// moved the FM's minor step off its tuned 1/360 s and forced the
    /// player's 4x slider cap (FLIGHT_CONTROL_STABILITY_PLAN.md §4.2
    /// RC-2).
    void set_trace_time_scale(double s) noexcept { trace_time_scale_ = s; }

private:
    void load_models();           // KoreaObj.HDR/.LOD/.TEX -> ModelDatabase
    void load_aircraft_config();  // f16.json -> AircraftConfig
    /// NAV-D2: rotate runway-frame waypoints into ENU about the threshold.
    /// Runs from spawn_aircraft() (idempotent) so synthetic-airfield
    /// scenarios get the same normalization the real-airbase path had.
    void normalize_waypoint_frame();

    void spawn_aircraft();        // spawn_mode dispatch (scenario_list | campaign_flights)
    void spawn_from_scenario_list();        // Phase 1: hand-authored aircraft[]
    void spawn_from_campaign_flights();     // Phase 2: campaign-derived roster
    void spawn_airfield_features();         // Phase 2A: static features → VMC entities
    void spawn_squadron_aircraft();         // Mode B: parked aircraft from Squadrons
    /// Step 11: resolve every scenario aircraft's "lead_callsign" to the
    /// lead entity, validate (exists + same team), mark the wingman brains
    /// (set_flight_lead), and record the (wingman, lead) pairs the tick
    /// loop feeds pictures through. Throws on an unresolvable or
    /// cross-team lead — a wingman of a hostile is a scenario-authoring
    /// bug, not a runtime condition.
    void resolve_wingman_refs();
    /// SimData AI wiring (the Data/ side of the f4-convert pipeline):
    /// after all aircraft exist AND wingman refs are resolved, load the
    /// scenario's brain data (brain_data_path, else the build tree's
    /// generated BRAINDAT fixture) and formation library
    /// (formation_library_path, else the generated FORMDAT fixture) —
    /// ONLY when at least one aircraft references a brain_profile or
    /// formation (nothing loads otherwise: the default world is
    /// byte-for-byte the pre-SimData behavior). Then resolve each
    /// aircraft's "brain_profile" name to a BrainArchetype (injected via
    /// BrainComponent::set_brain_archetype — a NON-OWNING pointer into
    /// brain_data_, which is why the storage lives here) and each
    /// wingman's "formation" name to a Formation (injected via
    /// wingman().command_formation_slot — same non-owning rule). Unknown
    /// names and unloadable files fail initialize() loudly.
    void apply_simdata_ai_profiles();
    /// Step 11: push each wingman's lead picture + the lead's engagement
    /// id, every tick BEFORE world update (the wingman module is
    /// engine-agnostic — the host is its eyes). Reads the lead's transform
    /// (one tick old at push time — exactly what every other brain sees)
    /// and the lead brain's combat_engagement_id() for the sort.
    void push_wingman_lead_pictures();
    /// The arbiter's safety rungs (M3-arbiter): every tick BEFORE world
    /// update, push each airborne aircraft brain (a) its TERRAIN picture
    /// — elevation under the jet + the max elevation in the look-ahead
    /// cone along its ground track, sampled from the SAME TerrainSource
    /// the FM's ground plane uses (one source of truth), and (b) its
    /// TRAFFIC picture — every other airborne aircraft within 1 NM
    /// (friendlies included) with velocity + roll rate, plus the own
    /// velocity from the same transform snapshot. The ground-avoid and
    /// collision-avoid modules are engine-agnostic: this push IS their
    /// entire view of terrain and traffic.
    void push_safety_pictures();
    void init_bubble_manager();             // Mode B: BubbleManager for ground/naval units
    void update_bubble();                   // Mode B: per-tick bubble update (in tick())
    void derive_real_airbase();   // airbase_source -> real ground layout
    /// B.3: campaign-flights airfield derivation (BEFORE wire_atc, see
    /// initialize()). Loads the world JSON's objectives, finds the first
    /// airbase, rewrites scenario_.airfield. No-op when the scenario
    /// carries a hand-authored airfield (non-empty taxi route) or when no
    /// airbase objective exists (spawn then fails loudly).
    void derive_campaign_airfield();
    void wire_atc();              // StubATC + AirfieldConfig from scenario
    void record_snapshot();
    void record_fcs_trace_sample();

    // --- Owned state ---
    Scenario scenario_;
    bool has_departure_override{false};  // hand-authored departure alt wins
    std::filesystem::path asset_dir_;

    entities::EntityWorld world_;
    messaging::MessageBus bus_;
    std::unique_ptr<f4::models::ModelDatabase> model_db_;
    std::unique_ptr<f4::ai::atc::StubATC> atc_;
    std::unique_ptr<f4::recorder::FlightRecorder> recorder_;
    std::unique_ptr<f4::recorder::FcsTraceWriter> fcs_trace_;
    f4::data::AircraftConfig aircraft_cfg_;

    // Combat chain (M3): weapon class data for launch_missile + the
    // component attachment at spawn. Built-in table; WST import later.
    f4::weapons::WeaponClassTable weapon_table_{};

    // M3 tactics: one detection policy per spawned combat aircraft,
    // installed on that aircraft brain's SensorFusion at spawn. The
    // Simulation owns them for the world's lifetime (the policy contract
    // is non-owning — see SensorFusion::set_detection_policy).
    std::vector<std::unique_ptr<RadarBackedDetectionPolicy>> combat_policies_{};

    // Step 11 (wingman/2-ship): resolved (wingman, lead) entity pairs —
    // scenario order matches aircraft_entities_ order for the
    // scenario-list spawn path. Empty when no aircraft declares a
    // lead_callsign (the pre-Step-11 world: no per-tick picture push).
    struct WingmanPair {
        entities::EntityId wingman;
        entities::EntityId lead;
    };
    std::vector<WingmanPair> wingman_pairs_{};

    // SimData AI data (BRAINDAT.brn + FORMDAT.FIL, converted to canonical
    // JSON by f4-convert). OWNED HERE because both consumers take
    // non-owning pointers: BrainComponent::set_brain_archetype and
    // WingmanModule::command_formation_slot reference rows inside these
    // objects for the Simulation's lifetime. Loaded lazily at
    // initialize() (see apply_simdata_ai_profiles) — the flags record
    // which side actually loaded (diagnostics; the pointers handed out
    // are the real contract).
    f4::data::BrainData brain_data_{};
    bool brain_data_loaded_{false};
    f4::data::FormationLibrary formation_library_{};
    bool formation_library_loaded_{false};

    // Phase 2: replaced the single `aircraft_entity_` with a vector. The
    // Phase 1 spawn path (scenario_list) pushes one entry; the Phase 2 path
    // (campaign_flights) pushes one per Flight unit found in the world JSON.
    std::vector<entities::EntityId> aircraft_entities_;

    // Phase 2A: static airfield-feature entities (buildings, runway sections,
    // taxiways, towers, hangars). Each carries TransformComponent +
    // VisualModelComponent. Tracked separately from aircraft so tick() doesn't
    // try to sync them from a flight model (they have none).
    std::vector<entities::EntityId> feature_entities_;

    // Mode B: parked aircraft from Squadron deaggregation. Spawned once at
    // initialize() (Squadrons don't move). Distinct from aircraft_entities_
    // (which holds Flight-spawned aircraft that DO taxi/takeoff).
    std::vector<entities::EntityId> squadron_aircraft_entities_;

    // Mode B: per-tick deagg/reagg manager for ground/naval units. Null
    // when not in campaign-flights mode (no BubbleManager needed for the
    // scenario-list spawn path, which has no campaign units).
    std::unique_ptr<BubbleManager> bubble_manager_;

    // B.3+: per-airbase derived airfields (key: airbase objective VU_ID.num)
    // from the LAST spawn_from_campaign_flights() run. Kept as a member so
    // the ATC registration (step 6) and later host-side queries share one
    // map; rebuilt on every campaign spawn.
    AirbaseAirfieldMap airbase_airfields_{};

    // Terrain elevation source. When null, tick() uses default_terrain_
    // (a FlatTerrainSource at the parking spot's altitude). The host
    // provides a real source via set_terrain_source() — typically a
    // TerrainDataAdapter wrapping f4::terrain::TerrainData.
    f4::terrain::TerrainSource* terrain_source_{nullptr};
    f4::terrain::FlatTerrainSource default_terrain_{0.0};  // updated to parking alt in initialize()

    double sim_time_s_{0.0};
    std::uint64_t tick_{0};
    bool paused_{false};
    // Trace metadata only — see set_trace_time_scale(). Never used to
    // scale tick dt.
    double trace_time_scale_{1.0};
};

} // namespace f4::simulation
