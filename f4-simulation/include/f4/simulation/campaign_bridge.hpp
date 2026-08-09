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
// Two functions:
//
//   derive_airfield_from_objective(obj, active_runway_id)
//     → std::optional<ScenarioAirfield>
//
//     Pure conversion: takes a campaign objective (already loaded into a
//     WorldState by f4-world) and produces a ScenarioAirfield. Returns
//     nullopt if the objective isn't an airbase (no runway list).
//
//   spawn_aircraft_from_flights(world, ct, db, cfg, airfield, scenario_aircraft)
//     → std::vector<EntityId>
//
//     Side-effectful: walks the EntityWorld (already populated by
//     f4-world::populate_world), finds every entity with a FlightPlanComponent,
//     and spawns a child aircraft entity for each. Each child carries
//     TransformComponent + FlightModelComponent + VisualModelComponent +
//     BrainComponent (the four-component "aircraft" composition — see
//     AIRCRAFT_BINDING_DESIGN.md §5).
//
// Both functions live in f4-simulation because they depend on f4-world
// (for WorldState / ObjectiveState), f4-world-convert (for ClassTable),
// f4-models (for ModelDatabase), and the simulation library's own
// VisualModelComponent. They are the ONLY new entry points in Phase 2;
// the Simulation class and the renderer are unchanged in shape (Simulation
// just iterates a vector of EntityIds instead of one).
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

} // namespace f4::simulation
