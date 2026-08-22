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

#include <f4/simulation/scenario.hpp>
#include <f4/simulation/visual_model_component.hpp>

#include <f4/entities/entity.hpp>
#include <f4/world/detail/world_state.hpp>
#include <f4/world_convert/class_table.hpp>
#include <f4/models/model_database.hpp>
#include <f4/data/aircraft_config.hpp>

#include <optional>
#include <vector>

namespace f4::simulation {

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

/// Spawn one aircraft entity per Flight-class unit in the EntityWorld.
///
/// Walks every entity in `world` that has a FlightPlanComponent (already
/// populated by f4-world::populate_units from campaign data). For each
/// flight:
///
///   1. Resolve the flight's squadron via FlightPlanComponent::squadron
///      (an EntityId). The squadron's SquadronComponent::airbase gives
///      the airbase objective's EntityId. The airbase's TransformComponent
///      gives the world position. (We don't yet pick a parking spot from
///      the airbase's GroundLayoutList — Phase 2 uses the airbase center
///      plus a small per-flight offset so multiple aircraft don't overlap.)
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
///        - BrainComponent       (TakeoffModule with taxi/takeoff thresholds)
///
///   4. Push the new EntityId into the returned vector.
///
/// \param world       The EntityWorld (already populated by f4-world::populate_world).
/// \param ct          The ClassTable (loaded from Falcon4.CT) — for entity_type → vis_type.
/// \param db          The ModelDatabase (loaded from KoreaObj.HDR/.LOD) — for vis_type → ModelRecord.
/// \param cfg         The AircraftConfig (loaded from f16.json) — shared across all spawned aircraft.
/// \param airfield    The active ScenarioAirfield (used for runway heading + departure alt).
/// \param scenario_aircraft  Template for callsign + aircraft_config_path + fuel. The
///                    callsign is suffixed with the flight's index (EAGLE1, EAGLE2, ...).
/// \returns The vector of spawned aircraft EntityIds. Empty if no flights were found.
[[nodiscard]] std::vector<f4::entities::EntityId>
spawn_aircraft_from_flights(f4::entities::EntityWorld& world,
                             const f4::world_convert::ClassTable& ct,
                             const f4::models::ModelDatabase& db,
                             const f4::data::AircraftConfig& cfg,
                             const ScenarioAirfield& airfield,
                             const ScenarioAircraft& scenario_aircraft);

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
