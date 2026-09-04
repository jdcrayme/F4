// f4-simulation/include/f4/simulation/campaign_bridge.hpp
//
// Campaign Bridge — derive scenario data (airfield + aircraft roster) from
// a real Falcon 4.0 campaign saved on disk.
//
// This closes the gap identified in Docs/SCENARIO_PLAYER_PLAN.md §4.3 and
// Docs/AIRCRAFT_BINDING_DESIGN.md §8: the v0 host used a hand-authored
// scenario JSON with hardcoded parking spots + taxi route. Phase 2 lets
// the host load a real `save1.cam` (already converted to world JSON by
// f4-world-convert's cam2json CLI), find the airbase objective, extract
// its GroundLayoutList (runway/taxiway/parking), and spawn one aircraft
// entity per Flight-class unit in the campaign.
//
// Mode B extension (unit deaggregation):
//   spawn_vehicles_from_unit / spawn_vehicles_from_units
//     → walks VehicleCompositionComponent on Battalion/Brigade/TaskForce
//       entities, spawns one TransformComponent+VisualModelComponent entity
//       per live vehicle in each group. Uses SYNTHETIC formations (wedge/
//       column) until FreeFalcon's SquadFormations/PlatoonFormations/
//       CompanyFormations tables are ported from gndai.cpp:110-282.
//
//   spawn_aircraft_from_squadrons
//     → walks Squadron entities, spawns parked aircraft at the squadron's
//       airbase using the airfield's parking-spot list (PLT_PARK) or the
//       synthesized 8-spot row from derive_airfield_from_objective().
//       Suppresses aircraft whose pilot slot is already covered by an
//       active Flight (avoids duplication when both paths run).
//
// Dependencies: f4-entities, f4-world, f4-world-convert, f4-models, f4-data,
// f4-flight-model, f4-ai, f4-geo, f4-math. C++20.

#pragma once

#include <f4/ai/brain_component.hpp>  // MissionPlan (B.3 route building)
#include <f4/campaign/campaign.hpp>   // MissionIntent (C3 synthetic spawns)
#include <f4/simulation/scenario.hpp>
#include <f4/simulation/visual_model_component.hpp>
#include <f4/weapons/weapon_class_table.hpp>  // campaign weapon map (A-G)
#include <f4/weapons/weapon_store.hpp>       // loadout -> stations (A-G)

#include <f4/entities/entity.hpp>
#include <f4/world/detail/world_state.hpp>
#include <f4/world_convert/class_table.hpp>
#include <f4/models/model_database.hpp>
#include <f4/data/aircraft_config.hpp>

#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace f4::simulation {

/// Airfield data keyed by airbase VU_ID.num — one entry per airbase-class
/// objective in the world. Campaign saves park flights at their squadrons'
/// home bases (TestCamp: ~40 fields); the flight spawner resolves each
/// flight's base through this map so every aircraft taxis/departs on ITS
/// OWN runway instead of the first airbase objective's (the B.3 QC run
/// caught 12 aircraft taxiing at 19 kts toward a runway 200,000 ft away,
/// never to arrive). Built by the host (Simulation::spawn_from_campaign_
/// flights / campaign_qc) from the world's ObjectiveState list; also
/// registered with the StubATC so clearances are answered per-base.
/// (Defined early — both the saved-flight and the C3 synthetic spawn
/// paths consume it.)
using AirbaseAirfieldMap =
    std::unordered_map<std::uint32_t, ScenarioAirfield>;

/// Derive a ScenarioAirfield from a real airbase objective's ground layout.
///
/// Picks the first runway-class GroundLayoutList as the active runway,
/// finds the runway's two endpoint points (threshold + far end), and
/// builds a taxi route by chaining taxiway + parking points from the
/// objective's other ground-layout lists.
///
/// The objective's (x, y, z) grid coordinates are converted to ENU feet
/// using the same 1024 ft/grid-unit convention as f4-world::populate_world.
/// The GroundLayoutPoint (x, y) offsets (already in feet relative to the
/// objective center) are added on top.
///
/// Returns std::nullopt if:
///   - The objective has no ground_layout at all (not an airbase).
///   - No runway-class list is present in the ground_layout.
///   - The runway list has fewer than 2 points (degenerate).
///
/// \param obj         The campaign objective (must be ObjectiveType::TYPE_AIRBASE
///                    or have a non-empty ground_layout vector).
/// \param active_runway_id  Runway number to populate ScenarioAirfield with
///                    (cosmetic — the actual runway is whichever list the
///                    ground_layout carries; we don't yet support choosing
///                    between multiple runways at the same field).
[[nodiscard]] std::optional<ScenarioAirfield>
derive_airfield_from_objective(const f4::world::ObjectiveState& obj,
                                int active_runway_id = 36);

// ============================================================================
// B.3 tranche — campaign-flight spawn filter, route building, team strings
// ============================================================================
//
// The B.3 campaign→sim loop needs three additions to the bridge:
//
//   1. FlightSpawnFilter — a real save (TestCamp.cam) carries 449 flights;
//      a QC run wants a bounded, meaningful subset (one team, one mission
//      type, a cap). The filter is applied by spawn_aircraft_from_flights
//      AND by the scenario JSON's campaign_flight_filter block (same
//      fields), so the scenario player and the QC tool share one filter
//      vocabulary.
//
//   2. build_mission_plan_from_flight() — converts a Flight unit's
//      WaypointPlanComponent (grid coords, campaign times, WP_ACTION
//      bytes) into the MissionPlan the digi brain consumes (ENU feet
//      route). This is the route-planning slice of B.3: campaign flights
//      fly their SAVED routes instead of idling at the ramp.
//
//   3. owner_team_string() — maps a campaign owner slot to the blue/red/
//      green TEAM-tag vocabulary the sim's hostility model reads.

/// Which campaign flights spawn_aircraft_from_flights() should materialize.
/// Default-constructed = no filtering (every flight — the original
/// behavior). The JSON block "campaign_flight_filter" parses into this.
struct FlightSpawnFilter {
    /// Restrict to one owning team slot (-1 = any).
    int team{-1};
    /// Restrict to one mission byte (-1 = any). Mission names
    /// ("AMIS_BARCAP") resolve through f4-campaign's mission_type_byte.
    int mission{-1};
    /// Hard cap on spawned aircraft (0 = unlimited). Large saves
    /// (TestCamp: 449 flights) need this to keep QC runs bounded.
    int max_flights{0};

    /// True when nothing is filtered (every flight passes).
    [[nodiscard]] bool is_noop() const noexcept {
        return team < 0 && mission < 0 && max_flights <= 0;
    }
};

/// Build the digi MissionPlan for one campaign Flight entity.
///
/// Route source: the flight entity's WaypointPlanComponent, converted
/// grid→ENU feet (1024 ft/grid). Leading WP_TAKEOFF waypoints are dropped
/// (the TakeoffModule owns departure from the airbase); the LAST waypoint
/// becomes the approach entry fix the NavigationModule hands to the
/// LandingModule — for round-trip routes that is the saved WP_LAND at the
/// home airbase.
///
/// A-G tranche: each route waypoint carries its wire WP_ACTION, and
/// delivery-action waypoints (WP_STRIKE/BOMB/GNDSTRIKE/NAVSTRIKE/SEAD)
/// carry their target's EntityId::value — resolved through
/// `objective_id_map` (WaypointState::target_num → entity). G2: when
/// the objective map misses, `unit_id_map` resolves UNIT targets (an
/// aggregate battalion — the CAS family's delivery target; the world
/// loader's own objectives-first-then-units order). Waypoints whose
/// target_num resolves no entity fall back to the FLIGHT's resolved
/// target (FlightPlanComponent::target). This is what arms the brain's
/// StrikeModule at the delivery point.
///
/// Altitude policy (documented approximation): WaypointState.z is MSL
/// feet; waypoints below 500 ft (ramp/taxi legs store 0) are floored to
/// 500 ft so the NavigationModule never commands terrain level. Speed:
/// uniform 400 kts (per-action speeds arrive with the M4.5 route tranche).
///
/// Returns std::nullopt when the flight has no (usable) waypoint plan —
/// callers then spawn takeoff-only aircraft, exactly as before B.3.
[[nodiscard]] std::optional<f4::ai::MissionPlan>
build_mission_plan_from_flight(
    const f4::entities::EntityWorld& world,
    f4::entities::EntityId flight_entity,
    const std::unordered_map<std::uint32_t, f4::entities::EntityId>*
        objective_id_map = nullptr,
    const std::unordered_map<std::uint32_t, f4::entities::EntityId>*
        unit_id_map = nullptr);

// ============================================================================
// C3 route tranche — synthetic-intent missions fly their BUILT routes
// ============================================================================
//
// The synthetic ladder's MissionIntents now carry a route (the C3
// campaign-side route builder: airbase → threat-avoiding ingress →
// target → egress → landing). These are the sim-side halves: the
// route → MissionPlan conversion (grid → ENU, the same altitude
// flooring and delivery-target resolution the saved-flight path
// applies), and the aircraft that flies it (the flight-path component
// composition — parking, model from the SQUADRON's entity type, TEAM
// tag, the C1 origin stamp keyed on the intent's own identity, and
// the doctrine ordnance fill for delivery missions).

/// Convert a built route (f4-campaign's RouteWaypoint list) into the
/// MissionPlan the digi brain consumes. Same contract as
/// build_mission_plan_from_flight: leading WP_TAKEOFF dropped (the
/// TakeoffModule owns departure), the LAST waypoint becomes the
/// approach entry fix, delivery-action waypoints carry their target's
/// EntityId::value (waypoint target_num → objective map; G2: unit map
/// second — a CAS route's battalion target; else the intent's own
/// target objective).
[[nodiscard]] std::optional<f4::ai::MissionPlan>
build_mission_plan_from_route(
    const std::vector<f4::campaign::RouteWaypoint>& route,
    std::uint32_t target_objective_vu,
    const std::unordered_map<std::uint32_t, f4::entities::EntityId>*
        objective_id_map = nullptr,
    const std::unordered_map<std::uint32_t, f4::entities::EntityId>*
        unit_id_map = nullptr);

/// Spawn ONE aircraft for ONE synthetic-ladder MissionIntent (the
/// generation-to-spawn leg — the mirror of spawn_aircraft_for_flight
/// for missions that have no live Flight unit in the save). Resolves
/// the intent's squadron through `unit_id_map` for the airbase and
/// model, attaches the MissionPlan built from the intent's route,
/// stamps the C1 origin component (flight_vu = the intent's synthetic
/// flight id — kills write back to the tasked squadron), and arms the
/// doctrine ordnance when the mission is a delivery category.
/// Returns nullopt when the intent carries no route.
[[nodiscard]] std::optional<f4::entities::EntityId>
spawn_aircraft_for_intent(
    f4::entities::EntityWorld& world,
    const f4::campaign::MissionIntent& intent,
    const std::unordered_map<std::uint32_t, f4::entities::EntityId>&
        unit_id_map,
    const f4::world_convert::ClassTable& ct,
    const f4::models::ModelDatabase& db,
    const f4::data::AircraftConfig& cfg,
    const ScenarioAirfield& airfield,
    const ScenarioAircraft& scenario_aircraft,
    int parking_slot,
    const AirbaseAirfieldMap* airbase_airfields = nullptr,
    const std::unordered_map<std::uint32_t, f4::entities::EntityId>*
        objective_id_map = nullptr,
    const weapons::WeaponClassTable* weapon_table = nullptr,
    const std::unordered_map<std::uint32_t, f4::entities::EntityId>*
        target_unit_id_map = nullptr);

/// Map a campaign owner slot to the sim's TEAM-tag string vocabulary.
///
/// Resolution: the campaign entity (ROLE "campaign") names the PLAYER team
/// (CampaignStateComponent::te_team). owner == player → "blue"; a team
/// whose stance toward the player is negative → "red"; anything else →
/// "green". When the campaign entity is absent (synthetic test worlds),
/// slot 0 maps "blue" and every other slot "red".
///
/// Documented limitation: the sim's hostility rule is own-relative string
/// comparison (sensor_fusion.cpp — "future: campaign team-stance data
/// replaces the 2-team assumption"), so two allies-of-convenience both
/// mapping "red" read as friendly to each other, and blue/green read as
/// hostile. The stance-accurate pairwise rule lands with the M4.2+ ATM
/// tranche; QC route-flying doesn't consume hostility at all.
[[nodiscard]] std::string
owner_team_string(const f4::entities::EntityWorld& world,
                  std::uint8_t owner);

// ============================================================================
// B.3+ per-airbase airfields
// ============================================================================
// (AirbaseAirfieldMap — typedef above, where both spawn paths see it.)

// ============================================================================
// A-G employment tranche — the campaign weapon map + loadout arming
// ============================================================================
//
// The save's flight LoadoutStruct[] carries WEAPON IDs from the campaign's
// WeaponDataTable (the wire table). The engine's WeaponClassTable is a
// DIFFERENT table (built-in cards; FALCON4.WST import is future work), so
// wire ids must be mapped before a station becomes droppable. Only
// CONFIRMED identities are mapped — every source for the mapping is
// documented in the table below; unmapped ids ride as bookkeeping stations
// labeled "WPN-<id>" (honest placeholders, never invented names).

/// One confirmed wire→engine weapon mapping.
struct CampaignWeaponMapEntry {
    std::uint16_t wire_id;         // campaign WeaponDataTable index
    const char* engine_name;       // WeaponClassTable::find_by_name key
};

/// The confirmed mapping (source: FreeFalcon src/campaign/include/
/// campweap.h — the WEAP_BAI_LOADOUT comment documents "GBU-12 (wid 68)
/// and GBU-22 (wid 310)"). Cross-validated: wire id 68 appears in
/// TestCamp's BAI/SAD flights exactly where the comment says BAI loads
/// GBU-12s.
inline constexpr CampaignWeaponMapEntry kCampaignWeaponMap[] = {
    {68, "GBU-12"},    // GBU-12 Paveway II (the mapped droppable bomb)
    {310, "GBU-12"},   // GBU-22 (v>=73 saves; closest engine card — the
                       // 500-lb-class Paveway shares the Mk-82 warhead)
};

/// Resolve a wire weapon id to an engine weapon handle
/// (kInvalidWeapon when unmapped).
[[nodiscard]] std::uint32_t
campaign_weapon_handle(const weapons::WeaponClassTable& table,
                       std::uint16_t wire_id);

/// Human label for a wire weapon id: the engine name when mapped, else
/// "WPN-<id>" (an honest placeholder — never an invented name).
[[nodiscard]] std::string
campaign_weapon_label(const weapons::WeaponClassTable& table,
                      std::uint16_t wire_id);

/// What arming a flight's ordnance produced — the QC summary + tests
/// read exactly these counters.
struct StrikeArmament {
    /// Stations copied from the decoded wire loadout (droppable mapped
    /// bombs + bookkeeping WPN-<id> stations + pods).
    int wire_stations = 0;
    /// Droppable stations among them (mapped to engine weapon cards).
    int droppable_stations = 0;
    /// Bombs available across the droppable stations.
    int droppable_rounds = 0;
    /// Doctrine MK-82 stations added because the flight's mission is an
    /// A/G delivery category but no wire station mapped to a bomb
    /// (mirrors FreeFalcon's LoadWeapons squadron-stores fallback — the
    /// flight still delivers ordnance, and the QC summary says so).
    int doctrine_stations = 0;
    /// True when the brain's strike fire control is armed (droppable
    /// ordnance exists + the mission is a delivery category).
    bool strike_armed = false;
};

/// Arm one spawned campaign aircraft with ordnance + the strike fire
/// control. Attaches the WeaponStoreComponent built from the flight's
/// decoded loadout (mapped stations droppable, the rest bookkeeping),
/// applies the doctrine fill for A/G-mission flights with no droppable
/// wire station, and configures the brain's StrikeModule (drag factor
/// from the bomb card's own ballistics, salvo = the droppable rounds,
/// clipped to the doctrine stick).
///
/// Missions in the Strike/SEAD/CAS categories arm; everything else
/// (CAP/escort/support/recon) keeps the store for bookkeeping only and
/// the strike trigger stays disarmed — an air-superiority flight never
/// drops on a target it passes.
[[nodiscard]] StrikeArmament
arm_flight_strike(const weapons::WeaponClassTable& table,
                  f4::entities::EntityHandle& aircraft,
                  const std::vector<f4::entities::LoadoutStationState>&
                      loadout_stations,
                  std::uint8_t mission_byte);

/// Read an airbase entity's VU_ID.num from its PropertyBag (0 when the
/// entity has no id recorded — hand-built test objectives).
[[nodiscard]] std::uint32_t
airbase_vu_id(const f4::entities::EntityWorld& world,
              f4::entities::EntityId airbase_entity);

/// Spawn ONE aircraft for ONE campaign Flight entity (the shared core of
/// the B.3 spawn paths). Composes Transform + FlightModel + VisualModel +
/// Brain (+ MissionPlan from the flight's saved waypoints, when usable) +
/// TEAM tag, at the flight's squadron airbase plus the caller-chosen
/// parking slot offset.
///
/// `parking_slot` is the flight's per-airbase index (0, 1, 2, ...) — the
/// caller owns the per-airbase bookkeeping (the bulk path counts per
/// airbase; CampaignSimSpawner counts the same way).
///
/// `airbase_airfields` (B.3+): when non-null and the flight's home base
/// resolves to an entry, THAT airfield's runway heading / departure
/// altitude drive the spawn pose and the brain, and the brain's
/// TakeoffModule is tagged with the airbase's VU_ID.num (published in the
/// ATC requests so clearances come back per-base). Null = legacy behavior
/// (the single fallback `airfield` — scenario-list and single-base tests).
///
/// Returns the spawned entity, or std::nullopt when `flight_entity` is not
/// a Flight (no FlightPlanComponent).
[[nodiscard]] std::optional<f4::entities::EntityId>
spawn_aircraft_for_flight(f4::entities::EntityWorld& world,
                          f4::entities::EntityId flight_entity,
                          const f4::world_convert::ClassTable& ct,
                          const f4::models::ModelDatabase& db,
                          const f4::data::AircraftConfig& cfg,
                          const ScenarioAirfield& airfield,
                          const ScenarioAircraft& scenario_aircraft,
                          int parking_slot,
                          const AirbaseAirfieldMap* airbase_airfields = nullptr,
                          const std::unordered_map<std::uint32_t,
                              f4::entities::EntityId>* objective_id_map = nullptr,
                          const weapons::WeaponClassTable* weapon_table = nullptr,
                          const std::unordered_map<std::uint32_t,
                              f4::entities::EntityId>* unit_id_map = nullptr);

/// Spawn one aircraft entity per Flight-class unit in the EntityWorld.
///
/// Walks every entity in `world` that has a FlightPlanComponent (already
/// populated by f4-world::populate_units from campaign data). For each
/// flight:
///
///   1. Resolve the flight's squadron via FlightPlanComponent::squadron
///      (an EntityId). The squadron's SquadronComponent::airbase gives
///      the airbase objective's EntityId. The airbase's TransformComponent
///      gives the world position. Parking offsets are per-airbase (B.3
///      fix: the pre-B.3 global counter put flight #400 three miles from
///      its field), alternating sides, 80 ft steps.
///
///   2. Look up the squadron's entity_type via UnitCoreComponent::class_table_index,
///      then resolve it through ClassTable::vis_type_for(entity_type, 0)
///      to get the visual model index. That index goes into ModelDatabase::model()
///      to resolve the ModelRecord pointer for VisualModelComponent.
///
///   3. Compose the aircraft entity:
///        - TransformComponent  (position = airbase + per-flight offset,
///                               heading = runway heading)
///        - FlightModelComponent (init from AircraftConfig, on ground)
///        - VisualModelComponent (ModelRecord* + LOD 0 + gear-down switch)
///        - BrainComponent       (TakeoffModule with taxi/takeoff thresholds;
///                               B.3: plus the MissionPlan built from the
///                               flight's saved waypoints, when present)
///        - TEAM tag             (B.3: owner_team_string() of the flight's
///                               owner slot)
///
///   4. Push the new EntityId into the returned vector.
///
/// One aircraft per FLIGHT unit (the flight lead; a 4-ship flight's
/// elements arrive with the formation tranche).
///
/// \param world       The EntityWorld (already populated by f4-world::populate_world).
/// \param ct          The ClassTable (loaded from Falcon4.CT) — for entity_type → vis_type.
/// \param db          The ModelDatabase (loaded from KoreaObj.HDR/.LOD) — for vis_type → ModelRecord.
/// \param cfg         The AircraftConfig (loaded from f16.json) — shared across all spawned aircraft.
/// \param airfield    The active ScenarioAirfield (used for runway heading + departure alt).
/// \param scenario_aircraft  Template for callsign + aircraft_config_path + fuel. The
///                    callsign is suffixed with the flight's index (EAGLE1, EAGLE2, ...).
/// \param filter      B.3: restrict which flights spawn (team/mission/cap).
///                    Default = no filter (backward compatible).
/// \param airbase_airfields  B.3+: per-base airfield data (see
///                    AirbaseAirfieldMap). Each flight's home base resolves
///                    through it; unknown bases fall back to `airfield`.
///                    Default null = legacy single-airfield behavior.
/// \returns The vector of spawned aircraft EntityIds. Empty if no flights were found.
[[nodiscard]] std::vector<f4::entities::EntityId>
spawn_aircraft_from_flights(f4::entities::EntityWorld& world,
                             const f4::world_convert::ClassTable& ct,
                             const f4::models::ModelDatabase& db,
                             const f4::data::AircraftConfig& cfg,
                             const ScenarioAirfield& airfield,
                             const ScenarioAircraft& scenario_aircraft,
                             const FlightSpawnFilter& filter = {},
                             const AirbaseAirfieldMap* airbase_airfields = nullptr,
                             const std::unordered_map<std::uint32_t,
                                 f4::entities::EntityId>* objective_id_map = nullptr,
                             const weapons::WeaponClassTable* weapon_table = nullptr,
                             const std::unordered_map<std::uint32_t,
                                 f4::entities::EntityId>* unit_id_map = nullptr);

// ============================================================================
// Mode B: Unit Deaggregation
// ============================================================================
//
// FreeFalcon's campaign entities (Battalion, Brigade, TaskForce, Squadron)
// carry no 3D model at the unit level — visType[0] is 0 for every UNIT-
// class entity_type in FALCON4.ct. The 3D models live at the VEHICLE-class
// entity_types referenced by the unit's UCD record (VehicleType[16]).
//
// Deaggregation = walk the unit's VehicleCompositionComponent.groups (each
// group already carries the resolved VEHICLE entity_type + live_count from
// the roster bitmap), look up each vehicle's visType[0] → ModelRecord,
// and spawn one TransformComponent + VisualModelComponent entity per live
// vehicle. The result is a formation of individual vehicles around the
// unit's campaign position, ready for draw_entity_meshes() to render.
//
// Two flavors:
//
//   • Ground/naval (Battalion/Brigade/TaskForce): synthetic formation
//     layout (wedge for ≤4 vehicles, grid for larger counts). Clearly
//     marked SYNTHETIC — replace with ported SquadFormations tables from
//     FreeFalcon's gndai.cpp:110-282 when the source is available.
//
//   • Air (Squadron): real parking spots from the airbase's
//     GroundLayoutComponent (PLT_PARK lists), or the synthesized 8-spot
//     row from derive_airfield_from_objective(). No formation tables
//     needed — parking spots ARE the layout.

/// Spawn deaggregated vehicle entities for a single unit.
///
/// Walks the unit's VehicleCompositionComponent.groups. For each group,
/// spawns `group.live_count` vehicle entities (one per live vehicle per
/// the roster bitmap, already decoded into live_count by f4-world-convert).
/// Each vehicle gets:
///   - TransformComponent (position = unit center + formation offset,
///     rotated by unit heading; heading from GroundTacticalComponent)
///   - VisualModelComponent (ModelRecord resolved via
///     ClassTable::vis_type_for(group.vehicle_type, 0))
///
/// Formation layout is SYNTHETIC (wedge for ≤4 vehicles, 4-wide grid for
/// larger counts). Marked clearly so the real FreeFalcon formation tables
/// can be dropped in later without touching the spawn contract.
///
/// \param world    The EntityWorld (mutated — new entities created).
/// \param ct       ClassTable for entity_type → vis_type resolution.
/// \param db       ModelDatabase for vis_type → ModelRecord.
/// \param unit_id  The unit entity to deaggregate. Must have
///                 VehicleCompositionComponent + TransformComponent.
///                 GroundTacticalComponent is optional (heading defaults
///                 to 0 — north-facing — when absent, e.g. naval units).
/// \returns The spawned vehicle EntityIds. Empty if the unit has no
///          VehicleCompositionComponent, no TransformComponent, or no
///          live vehicles in any group.
[[nodiscard]] std::vector<f4::entities::EntityId>
spawn_vehicles_from_unit(f4::entities::EntityWorld& world,
                          const f4::world_convert::ClassTable& ct,
                          const f4::models::ModelDatabase& db,
                          f4::entities::EntityId unit_id);

/// Bulk wrapper: deaggregate every unit with a VehicleCompositionComponent.
///
/// Convenience function for "deaggregate everything in the world" — calls
/// spawn_vehicles_from_unit() for each entity returned by
/// `world.with_component<VehicleCompositionComponent>()`. The BubbleManager
/// uses the single-unit overload instead (per-tick, scoped to the bubble),
/// but this bulk form is useful for tests and headless scenarios.
///
/// \returns The combined vector of all spawned vehicle EntityIds.
[[nodiscard]] std::vector<f4::entities::EntityId>
spawn_vehicles_from_units(f4::entities::EntityWorld& world,
                           const f4::world_convert::ClassTable& ct,
                           const f4::models::ModelDatabase& db);

/// Spawn parked aircraft for Squadron units that have no active Flights.
///
/// Walks every entity with a SquadronComponent. For each squadron:
///
///   1. Resolve the home airbase via SquadronComponent::airbase (an
///      EntityId pointing at an ObjectiveTypeComponent entity). The
///      airbase's GroundLayoutComponent (PLT_PARK lists) provides parking
///      spots. When PLT_PARK is absent (Korea theater), fall back to
///      derive_airfield_from_objective()'s synthesized 8-spot row.
///
///   2. Count active Flights whose FlightPlanComponent::squadron points
///      at this Squadron. Spawn `max(0, N_pilots - active_flights)`
///      parked aircraft — the active flights produce their own aircraft
///      via spawn_aircraft_from_flights(), so we don't duplicate them.
///
///   3. Each parked aircraft gets TransformComponent + VisualModelComponent
///      + FlightModelComponent (on ground, gear down) + BrainComponent
///      (TakeoffModule, but idle — they're parked, not taxiing).
///      The visType comes from the squadron's class_table_index (the
///      aircraft type the squadron flies).
///
/// Parked aircraft are static — they don't taxi or take off. Their FM
/// is initialized on the ground with zero velocity. The brain is wired
/// but dormant (no active mission); a future ATC/taxi dispatcher could
/// activate them.
///
/// \returns The spawned parked-aircraft EntityIds. Empty if no Squadrons
///          or all squadrons are fully covered by active Flights.
[[nodiscard]] std::vector<f4::entities::EntityId>
spawn_aircraft_from_squadrons(f4::entities::EntityWorld& world,
                                const f4::world_convert::ClassTable& ct,
                                const f4::models::ModelDatabase& db,
                                const f4::data::AircraftConfig& cfg,
                                const ScenarioAirfield& airfield,
                                const ScenarioAircraft& scenario_aircraft);

} // namespace f4::simulation
