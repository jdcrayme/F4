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
#include <f4/data/signature_data.hpp>
#include <f4/messaging/bus.hpp>
// Tranche 0d: f4-models is no longer linked. VisualModelComponent carries
// vis_type (the identity); the renderer resolves the mesh through its own
// model cache. The Simulation does not own a ModelDatabase.
#include <f4/data/aircraft_config.hpp>
#include <f4/data/brain_data.hpp>       // SimData BRAINDAT.brn archetypes
#include <f4/data/formation_data.hpp>  // SimData FORMDAT.FIL formations
#include <f4/data/sensor_data.hpp>      // SimData SENSDATA/IRST seeker cards
#include <f4/weapons/weapon_class_table.hpp>
#include <f4/world_types/class_table.hpp>  // owned here (see class_table_)
#include <f4/world/theater_tables.hpp>    // CAMP-SCALE-1: converted tables
#include <f4/ai/air_picture.hpp>       // PERF-1: the shared snapshot
#include <f4/ai/modules/strike_module.hpp>   // Tranche D: WP_REFUEL predicate

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "f4/simulation/scenario.hpp"
#include <f4/simulation/weather_system.hpp>  // Task 73: Weather v1
#include <f4/simulation/campaign_bridge.hpp>  // AirbaseAirfieldMap (B.3+)

#include <f4/terrain/terrain_source.hpp>  // TerrainSource (Path B1)

namespace f4::ai::atc { class IAirTrafficControl; }
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

    /// Task 73 (Weather v1): the theater environment. Non-null only when
    /// the scenario carries a "weather" or "time" block (the zero-change
    /// rule: absent blocks = no environment system at all, and every
    /// SensorFusion scale stays at its 1.0 default). Hosts (the QC tool,
    /// the campaign session viewers) read the CURRENT state/band/visual
    /// scale for display; the evolution itself runs inside tick().
    [[nodiscard]] const f4::sim::WeatherSystem* environment() const noexcept {
        return weather_.get();
    }

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

    /// The class table (FALCON4.CT) this Simulation loaded at
    /// initialize() — entity_type → vis_type. Empty when the scenario
    /// carries no class_table_path (every consumer degrades
    /// gracefully). Hosts that need their own lookups (renderers,
    /// inspectors) share this instead of re-loading the file.
    [[nodiscard]] const f4::world_types::ClassTable& class_table()
        const noexcept {
        return class_table_;
    }

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

    /// Retire an aircraft: erase it from the flying roster, the
    /// wingman-pair table, and the combat-policy set, then DESTROY the
    /// entity in world(). C5's wreck reaper calls this for aircraft
    /// whose EntityKilledMessage landed `wreck_hold` sim-seconds ago —
    /// the ledger booked the loss at EVENT time (the sink's bus
    /// subscription), so removing the frozen wreck afterwards never
    /// races the books, and the debrief trace is the only other wreck
    /// consumer (long-horizon harnesses run with it off).
    ///
    /// FreeFalcon correspondence: the reference removes dead sim
    /// entities on its own cadence (the sim object dies; the CAMPAIGN
    /// object and its bookkeeping live on — exactly the split this
    /// models: the books survive in the ledger, the corpse does not).
    ///
    /// Without any retire call the lifetime is the pre-C5 behavior —
    /// wrecks freeze in place forever — which every existing golden
    /// pins; retiring is strictly opt-in. Idempotent: returns true only
    /// when the entity was on the flying roster and got removed; a
    /// parked-squadron spawn, a feature, an unknown id, or a double
    /// retire all return false (the parked/feature populations are
    /// never the wreck policy's business — they don't die in this
    /// slice and never join the flying roster).
    bool retire_aircraft(entities::EntityId id);

    /// Aircraft retired via retire_aircraft() so far — the churn
    /// counter long-horizon hosts read (roster == initial + spawned −
    /// retired, the identity the C5 harness pins).
    [[nodiscard]] int retired_aircraft() const noexcept {
        return retired_aircraft_;
    }

    // --- C6: arming the campaign flights (A/A goes live) -----------------
    /// Arm ONE campaign-spawned aircraft for A/A combat — the combat
    /// component set (radar, RWR, signature, damage state, the fighter
    /// gun, the NCTR identity), the fighting brain (envelopes + ROE from
    /// the scenario's combat block), the doctrine by mission ROLE
    /// (CampaignOriginComponent::mission_byte → CAP/Sweep/Intercept/
    /// Escort fight the full ladder; everything else flies defensive-
    /// only through its BRAINDAT archetype), the doctrine A/A loadout
    /// for fighting roles, and a RadarBackedDetectionPolicy owned here
    /// (combat_policies_) and installed on the brain's SensorFusion —
    /// the M2 flip: campaign brains see radar truth, not
    /// GCI-omniscience. See combat_bridge.hpp (arm_campaign_combat) and
    /// CAMPAIGN_LOOP_PLAN.md §5 C6.
    ///
    /// Gated on the scenario's combat.campaign_armed (the C6 opt-in —
    /// default false, every pre-C6 world byte-identical). Call sites:
    /// the bulk campaign spawn path arms inside
    /// spawn_from_campaign_flights(); the session arms every late
    /// spawner materialization in its adopt_new_spawns_() cadence
    /// (right after register_aircraft). Idempotent per entity (an
    /// already-fighting brain is a no-op). Returns true when the
    /// aircraft got armed by THIS call.
    bool arm_campaign_aircraft(entities::EntityId id);

    /// P7 — apply a campaign flight's RoE (the wire roe_check byte
    /// the intent carried) to the entity's brain fire controls, AFTER
    /// arming (the arm's configure_brain_combat sets the scenario's
    /// own holds — the flight's RoE rides on top of it). The wire
    /// vocabulary: 0 = weapons free (no change — the pre-P7 default),
    /// 1 = weapons TIGHT (BVR missile employment suppressed, WVR
    /// heaters + guns still employ), 2 = weapons HOLD (every fire
    /// control tight). Idempotent; a no-op on entities without a
    /// brain.
    void apply_flight_roe(entities::EntityId id, std::uint8_t roe);

    /// CAMP-CMD-1 — the FULL RoE write behind the roe_set command:
    /// recompute every RoE gate from the campaign arm's doctrine
    /// baseline, then impose the level. Unlike apply_flight_roe's
    /// tighten-only ratchet this can LOWER — a roe_set that loosens a
    /// scope must be able to clear holds an earlier command (or a
    /// tighter level) imposed. The baseline mirrors arm_campaign_combat's
    /// configure_brain_combat call for campaign aircraft (hold_fire =
    /// false + the scenario combat's own holds); the envelopes and the
    /// gun rounds budget are deliberately NOT re-written (a re-arm
    /// would resurrect spent gun rounds — RoE touches the gates only).
    /// Same wire vocabulary as apply_flight_roe (0/1/2); idempotent; a
    /// no-op on entities without a brain.
    void set_flight_roe(entities::EntityId id, std::uint8_t roe);

    /// C6 diagnostics: how many campaign aircraft this Simulation armed
    /// (total + per doctrine role). The QC summary + the session stats
    /// read exactly these.
    [[nodiscard]] int campaign_armed_aircraft() const noexcept {
        return campaign_armed_total_;
    }
    [[nodiscard]] int campaign_armed_fighters() const noexcept {
        return campaign_armed_fighters_;
    }
    [[nodiscard]] int campaign_armed_defensive() const noexcept {
        return campaign_armed_defensive_;
    }

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

    /// Tranche D (AAR): the scripted tanker entity (a TransformComponent-
    /// carrying kinematic entity, no flight model). Default-constructed
    /// EntityId (value=0) when the scenario has no tanker. The world viewer
    /// and the recorder use this to draw/track the boom platform.
    [[nodiscard]] entities::EntityId tanker_entity() const noexcept {
        return tanker_entity_;
    }
    [[nodiscard]] bool has_tanker() const noexcept { return tanker_entity_.value != 0; }

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

    // --- V-3DLIVE: the view bubble (camera-driven deaggregation) --------
    /// Point the deaggregation bubble at the VIEWING position instead
    /// of the ownship — the map viewer's camera IS the "player" when
    /// the user is inspecting the map. radius_ft scales with the zoom
    /// (the host computes it — typically a fraction of the visible
    /// extent); the BubbleManager's own radius is overridden while a
    /// view bubble is active. Takes effect on the next update_bubble()
    /// tick — call refresh_bubble() to apply it immediately (a paused
    /// session still deaggregates when the user zooms in).
    void set_view_bubble(double radius_ft,
                         const f4::geo::WorldPosition& center) noexcept {
        view_bubble_active_ = true;
        view_bubble_center_ = center;
        view_bubble_radius_ft_ = radius_ft;
    }

    /// Return to the ownship-driven bubble (the FreeFalcon behavior:
    /// the first aircraft's position, the AII ground radius).
    void clear_view_bubble() noexcept { view_bubble_active_ = false; }

    /// Run one update_bubble() pass NOW — not waiting for the next
    /// tick. The viewer calls this under the session lock when the
    /// camera moved, so a PAUSED session still deaggregates the
    /// battalions the user zooms into (bubbles follow the eye, not
    /// the clock). No-op without a BubbleManager (scenario-list
    /// worlds have no campaign units to deaggregate).
    void refresh_bubble() { update_bubble(); }

    /// FID-1 (Docs/FIDELITY_TIERS_PLAN.md): the AII-parsed AIR deagg
    /// radius in feet (Falcon4.AII [Sim] SIM_BUBBLE_SIZE, 2.5 grid =
    /// 2560 ft when the file is absent). Dead config until the
    /// fidelity-tier session consumed it: the tiered session's air
    /// bubble never shrinks below this radius (the ownship-bubble
    /// semantics the AII documents, camera-driven per V-3DLIVE).
    /// 2560.0 when no BubbleManager exists (scenario-list worlds).
    [[nodiscard]] double air_bubble_radius_ft() const noexcept;

    /// True while a view bubble (camera-driven) overrides the ownship
    /// bubble — diagnostics / tests.
    [[nodiscard]] bool view_bubble_active() const noexcept {
        return view_bubble_active_;
    }

    // --- FID-5 (Docs/FIDELITY_TIERS_PLAN.md §4.5–4.6): the aggregate
    // --- air picture + the commit-window launch veto ---------------------

    /// Publish the Tiered session's AGGREGATE flights as coarse contacts
    /// in the shared air picture (§4.6). Non-owning: `contacts` must
    /// outlive the sim (the session rebuilds its buffer per campaign
    /// second; the sim appends the vector's contents at every picture
    /// build — team strings interned into the picture's own table).
    /// Null (the default) publishes nothing — full-fidelity and the
    /// pre-FID-5 tiered sessions are byte-identical.
    void set_air_picture_aggregates(
        const std::vector<f4::ai::AggregateContact>* contacts) noexcept {
        aggregate_contacts_ = contacts;
    }

    /// Exclude entity ids from the picture's world walk (§4.6). The
    /// session passes its campaign flight entities: their truth lives in
    /// the aggregate engine, so publishing them through the walk would
    /// DOUBLE-count them (the feed above is the single source) — and a
    /// SUSPENDED flight's frozen transform would linger as a stale
    /// phantom contact. Non-owning, null (default) = walk everything.
    void set_air_picture_excluded(
        const std::unordered_set<std::uint64_t>* ids) noexcept {
        picture_excluded_ = ids;
    }

    /// Weapon/gun-release veto against these target ids (§4.5's commit
    /// window): an aggregate contact id resolves to no entity, so a
    /// release against it would fly a phantom missile. The session
    /// passes the published aggregate set; the combat driver skips the
    /// release (and counts it) until the commit trigger deaggregates the
    /// flight — the NEXT campaign second — and the brain re-evaluates
    /// against the real aircraft. Non-owning, null (default) = no veto.
    void set_deferred_launch_ids(
        const std::unordered_set<std::uint64_t>* ids) noexcept {
        deferred_launch_ids_ = ids;
    }

    /// Releases the veto skipped so far (the session surfaces it in the
    /// stats; the certificate's diary reads it as the commit window's
    /// own load number).
    [[nodiscard]] int deferred_releases() const noexcept {
        return deferred_releases_;
    }

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
    /// Warnings from the real-data weapon overlay (unresolved aliases in
    /// the default map, malformed-but-tolerated exports). Empty unless a
    /// CombatConfig::weapon_data_path was configured.
    [[nodiscard]] const std::vector<std::string>&
    weapon_import_warnings() const noexcept {
        return weapon_import_warnings_;
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
    /// Tranche D (AAR): construct the ScriptedTanker from the scenario's
    /// tanker block (initialize(), if present), advance it kinematically
    /// each tick (tick(), BEFORE the brains run), and push its picture to
    /// every armed receiver's BrainComponent. The Simulation is the
    /// tanker's eyes — the RefuelModule is engine-agnostic. Arming: a
    /// receiver is armed when the scenario has a tanker AND the scenario's
    /// waypoint list carries a WP_REFUEL waypoint (the scenario-list path
    /// shares one route across all aircraft; per-aircraft arming waits for
    /// the campaign bridge to emit WP_REFUEL). No tanker block => tanker_
    /// is std::nullopt and this push is a no-op. dt advances the tanker
    /// kinematically (straight-and level; ScriptedTanker holds heading/
    /// alt/speed constant).
    void push_tanker_picture(double dt);
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
    /// PERF-1 (PERFORMANCE_PLAN.md §3): build the SHARED air picture
    /// once per tick — one walk over the transform bucket (clutter
    /// filter, team/role tag reads, team interning) — and hand its
    /// address to every roster brain's SensorFusion. The brains' combat
    /// rebuilds (including the beam-fight every-tick refresh) then
    /// iterate the snapshot's contacts instead of re-walking the entity
    /// database per brain. Output-identical to the fusion's own world
    /// query by construction (same entities, same order, same values);
    /// combat-gated (unarmed worlds never build it), and demand-gated
    /// (the walk happens only on ticks where at least one brain's
    /// fusion will actually rebuild — `dt` is the tick's own dt, the
    /// same value update_all will hand the brains).
    ///
    /// FID-OPT-2: the walk is ALSO cadence-gated — at most one walk per
    /// kPictureCadenceTicks ticks; between walks a demanding tick hands
    /// out the LAST snapshot (bounded <= 100 ms staleness). See the
    /// kPictureCadenceTicks note for the measured rationale.
    void push_air_picture_(double dt);
    /// Task 73: push the weather/day-night visual scale to every roster
    /// brain's SensorFusion (unconditional, O(roster) double writes).
    /// Never called when no environment is configured — the scale stays
    /// at every fusion's 1.0 default (the zero-change rule).
    void push_environment_scale_();
    void init_bubble_manager();             // Mode B: BubbleManager for ground/naval units
    void update_bubble();                   // Mode B: per-tick bubble update (in tick())
    void derive_real_airbase();   // airbase_source -> real ground layout
    /// B.3: campaign-flights airfield derivation (BEFORE wire_atc, see
    /// initialize()). Loads the world JSON's objectives, finds the first
    /// airbase, rewrites scenario_.airfield. No-op when the scenario
    /// carries a hand-authored airfield (non-empty taxi route) or when no
    /// airbase objective exists (spawn then fails loudly).
    void derive_campaign_airfield();
    void wire_atc();              // ATC (stub|tower per scenario.atc) + AirfieldConfig
    void record_snapshot();
    void record_fcs_trace_sample();
    /// Load scenario_.class_table_path into class_table_ ONCE per
    /// initialize() — every long-lived borrower (the spawn paths, the
    /// BubbleManager) references this member. No-op on an empty path
    /// (the table stays empty; every consumer degrades gracefully).
    void load_class_table();
    /// C6: resolve + load the BRAINDAT archetype table for the armed
    /// campaign (called from initialize() when combat.campaign_armed,
    /// BEFORE any aircraft exists — the arm installs non-owning
    /// archetype pointers into brain_data_). Throws loudly when no
    /// data resolves; no-op when already loaded.
    void ensure_campaign_brain_data();

    // --- Owned state ---
    Scenario scenario_;
    bool has_departure_override{false};  // hand-authored departure alt wins
    std::filesystem::path asset_dir_;

    // FID-OPT-1 (member ORDER matters): bus_ is declared BEFORE world_
    // so the reverse destruction order tears the world (and every
    // component, whose dtor unsubscribes its bus subscriptions) down
    // FIRST, while the bus is still alive. The pre-RAII code had this
    // backwards and simply never unsubscribed; with ScopedSubscriptions
    // the components' dtors need a live bus at teardown.
    messaging::MessageBus bus_;
    entities::EntityWorld world_;
    std::unique_ptr<f4::sim::WeatherSystem> weather_;  // Task 73; null = not configured
    std::unique_ptr<f4::ai::atc::IAirTrafficControl> atc_;
    std::unique_ptr<f4::recorder::FlightRecorder> recorder_;
    std::unique_ptr<f4::recorder::FcsTraceWriter> fcs_trace_;
    f4::data::AircraftConfig aircraft_cfg_;

    // The class table (FALCON4.CT) — entity_type → vis_type. OWNED HERE
    // because long-lived borrowers take non-owning references/pointers:
    // the BubbleManager holds `const ClassTable&` for the Simulation's
    // lifetime (per-tick deagg → spawn_vehicles_from_unit), and the
    // campaign-flights/squadron spawn paths read it too. Loaded once by
    // load_class_table() (see initialize).
    //
    // REGRESSION NOTE: init_bubble_manager() used to construct the
    // BubbleManager with a STACK-LOCAL ClassTable that died at function
    // return; the first tick's deagg then read freed stack memory in
    // ClassTable::vis_type_for() — the viewer's "Start Session →
    // access violation" crash (the QC never saw it: its fixture world
    // deaggregates nothing near the bubble center). The member is the
    // fix; the same discipline brain_data_/formation_library_ already
    // follow for their non-owning consumers.
    f4::world_types::ClassTable class_table_{};

    // Combat chain (M3): weapon class data for launch_missile + the
    // component attachment at spawn. Built-in table; WST import later.
    f4::weapons::WeaponClassTable weapon_table_{};
    std::vector<std::string> weapon_import_warnings_;

    // The countermeasure tranche: the IR seeker cards (SimData irstdata)
    // the seduction rolls resolve their flare chances against. Empty
    // library = the kDefaultIrFlareChance identity; owned here exactly
    // like weapon_table_ (loaded once, lent as a const pointer).
    f4::data::IrstSensorData ir_seeker_data_{};

    // Real-data tier (Task 64): the scenario's signature library, owned
    // here (exactly like brain_data_), grids lent to the spawned
    // aircraft's SignatureComponent as non-owning pointers.
    std::unique_ptr<f4::data::SignatureDataLibrary> signature_library_;
    bool signature_library_loaded_ = false;
    void ensure_signature_data();

    // CAMP-SCALE-1: the converted theater tables (combat.
    // theater_tables_path), owned here exactly like the signature
    // library; lent to the spawn paths as a const pointer. Null = the
    // paths run the documented-defaults identity.
    std::unique_ptr<f4::world::TheaterTables> theater_tables_;
    void ensure_theater_tables();

public:
    /// The loaded signature library (null when no signature_data_path
    /// was configured). Grids are pointers INTO this — the Simulation
    /// must outlive every armed entity.
    [[nodiscard]] const f4::data::SignatureDataLibrary*
    signature_library() const noexcept {
        return signature_library_.get();
    }

    /// CAMP-SCALE-1: the loaded converted theater tables (null when no
    /// theater_tables_path was configured). The session's spawn paths
    /// lend the same pointer the sim's own spawn path uses.
    [[nodiscard]] const f4::world::TheaterTables*
    theater_tables() const noexcept {
        return theater_tables_.get();
    }

    /// FID-OPT-2 test/QC accessor: ticks since the last air-picture
    /// walk. Grows without bound through demand-less quiet periods (the
    /// next demand then walks immediately); under CONTINUOUS demand it
    /// cycles 0..kPictureCadenceTicks-1 — a value of 0 = the picture
    /// was (re)built THIS tick.
    [[nodiscard]] int air_picture_age_ticks() const noexcept {
        return ticks_since_picture_walk_;
    }
    /// FID-OPT-2: the shared air picture's refresh cadence in ticks
    /// (6 ticks = 10 Hz at the 60 Hz minor frame). Public: hosts, tests,
    /// and the QC surface reason about the bounded-staleness guarantee.
    static constexpr int kPictureCadenceTicks = 6;

    /// FID-OPT-3 test/QC accessor: ticks since the last RWR sweep. Under
    /// combat it cycles 0..kRwrCadenceTicks-1 — a value of 0 = the RWR
    /// picture was (re)built THIS tick.
    [[nodiscard]] int rwr_sweep_age_ticks() const noexcept {
        return ticks_since_rwr_sweep_;
    }
    /// FID-OPT-3: the RWR sweep's cadence in ticks (6 ticks = 10 Hz at
    /// the 60 Hz minor frame — the SAME bound the design licenses for
    /// the combat refresh and the picture walk). The sweep rebuilds
    /// every live RWR's warning picture per call; the deep-horizon
    /// profile measured it at ~6.6 us/tick (8.6 s per 3-h armed war) —
    /// the third named term after the radar scan and the flight
    /// models. Cadenced host-side (the sweep itself stays a pure
    /// world function — direct callers, including every test, sweep
    /// exactly when they ask). Warnings (Lock/Launch transitions)
    /// publish at the sweep: detection/reaction timing shifts by
    /// <= 100 ms of sim, the licensed bound. Deterministic: an integer
    /// tick counter.
    static constexpr int kRwrCadenceTicks = 6;

private:

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

    // Tranche D (AAR): the scripted tanker, constructed from the
    // scenario's tanker block at initialize(). std::nullopt when the
    // scenario has no tanker (the refuel rung never arms). The tanker is
    // AAR redesign: the tanker is a real aircraft (own flight model +
    // brain, spawned by spawn_from_scenario_list). tanker_entity_ is
    // the EntityId of the tanker (found at initialize by scanning for
    // brain->is_tanker()); 0 when the scenario has no tanker.
    // push_tanker_picture reads the tanker's real FM each tick.
    entities::EntityId tanker_entity_{};
    /// True when the scenario route (shared or per-aircraft) contains a
    /// WP_REFUEL waypoint. Cached at initialize() so the per-tick
    /// push_tanker_picture arming decision is a single bool read.
    bool scenario_has_refuel_waypoint_{false};

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

    // M5a: per-aircraft WVR-band presence (combat_mode == WVR last time
    // the flip recorder looked), for the WvrEngaged/WvrDisengaged combat
    // events. Keyed by EntityId::value; a stale entry after a retirement
    // is harmless (the flip pass walks the CURRENT roster). Only written
    // when recording is on.
    std::unordered_map<std::uint64_t, bool> wvr_band_state_;

    /// M5a: walk the active roster after the intents pass and append a
    /// WvrEngaged/WvrDisengaged CombatEvent wherever a brain's combat
    /// mode crossed the WVR boundary since the previous tick. Recording
    /// only (recorder() == nullptr is a no-op) — the events make the
    /// band transitions first-class replayable evidence for the WVR
    /// harness's fight-alive gate.
    void record_wvr_band_flips(double sim_time_s);

    // C5: aircraft removed via retire_aircraft() (the wreck-reaper
    // counter — roster == initial + registered − retired).
    int retired_aircraft_ = 0;

    // C6: the campaign-combat arm bookkeeping — the seed counter (spawn
    // order is deterministic, so seed_base + index is too) + the
    // diagnostics counters the QC surface reads.
    std::size_t campaign_arm_index_ = 0;
    int campaign_armed_total_ = 0;
    int campaign_armed_fighters_ = 0;
    int campaign_armed_defensive_ = 0;

    // PERF-1: the shared air picture, rebuilt in place on the walk ticks
    // by push_air_picture_() and handed (non-owning) to every roster
    // brain. The members are reused tick over tick so the steady state
    // allocates nothing (contacts/teams clear + repopulate in place).
    f4::ai::AirPicture air_picture_{};

    // FID-OPT-2: the picture's own refresh cadence. The deep-horizon
    // profile (the FID-OPT-2 measurement: the walk is ~1.4 ms — the
    // ~4,400-entity transform scan — and the fusion rebuilds are ~3-6
    // us) showed the WALK, not the per-brain rebuilds, is the
    // concurrent-fight budget: any single demanding brain forced the
    // walk every fight tick. The walk now runs at most every
    // kPictureCadenceTicks ticks (10 Hz at the 60 Hz minor frame) while
    // any brain demands the picture; between walks the LAST snapshot
    // stays valid and is what the fusions' rebuilds consume — bounded
    // staleness (<= 100 ms of sim), the same bound the design licenses
    // for the cadence tier of the fusion refresh. Urgency does not
    // bypass the walk cadence (it buys rebuild rate, not picture
    // freshness). Deterministic: an integer tick counter.
    // Initialized DUE so the first demand walk builds immediately.
    int ticks_since_picture_walk_{kPictureCadenceTicks};

    // FID-OPT-3: the RWR sweep's own cadence — the same shape as the
    // picture's (aged BEFORE the gate: increment then compare, the
    // off-by-one the FID-OPT-2 walk gate caught; initialized DUE so the
    // war's first combat tick sweeps immediately, matching the
    // pre-OPT-3 behavior at war start). Between sweeps every RwrComponent
    // keeps its LAST warning picture — bounded <= 100 ms staleness.
    int ticks_since_rwr_sweep_{kRwrCadenceTicks};

    // FID-5: the aggregate-contact feed + the picture-exclusion and
    // launch-veto sets — all NON-OWNING pointers the fidelity-tier
    // session owns (null = the pre-FID-5 behavior, every default).
    const std::vector<f4::ai::AggregateContact>* aggregate_contacts_ =
        nullptr;
    const std::unordered_set<std::uint64_t>* picture_excluded_ = nullptr;
    const std::unordered_set<std::uint64_t>* deferred_launch_ids_ = nullptr;
    int deferred_releases_ = 0;

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

    // V-3DLIVE: the camera-driven bubble override (see set_view_bubble).
    // When active, update_bubble() centers the deaggregation bubble on
    // view_bubble_center_ (the host's camera position, ENU feet) with
    // view_bubble_radius_ft_ instead of the ownship + the AII default —
    // the map viewer's "zoom into a battalion → its vehicles appear"
    // behavior. The AII default radius is cached so clear_view_bubble()
    // restores it exactly.
    bool view_bubble_active_{false};
    f4::geo::WorldPosition view_bubble_center_{};
    double view_bubble_radius_ft_{1024.0};
    double default_ground_radius_ft_{1024.0};

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
