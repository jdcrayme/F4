// f4-world/include/f4/world/world_loader.hpp
//
// Populates an f4-entities EntityWorld from a parsed WorldState. Each team,
// objective, and unit becomes an entity with domain components; format-derived
// concepts (VU_ID, nameid, obj_flags) are resolved or discarded here.
//
// Phase 1: populate_teams adds TeamComponent + narrowed CampaignIdentityComponent.
// Phase 3: populate_objectives, populate_units, populate_campaign, populate_world
//   bridge all entity types. After populate_world, no consumer needs to touch
//   WorldState directly — they work through EntityWorld.

#pragma once

#include <unordered_map>

#include <f4/entities/entity.hpp>
#include <f4/world/world_state.hpp>

namespace f4::world {

// ============================================================================
// PopulatedWorld — result of populate_world(), giving the caller access to
// all created entity IDs grouped by kind.
// ============================================================================
struct PopulatedWorld {
    f4::entities::EntityId campaign;
    std::vector<f4::entities::EntityId> teams;
    std::vector<f4::entities::EntityId> objectives;
    std::vector<f4::entities::EntityId> units;

    /// VU_ID.num → EntityId maps for cross-reference resolution.
    /// After populate_world(), these map every objective/unit VU_ID to its
    /// ECS entity, so systems can resolve references without WorldState.
    std::unordered_map<uint32_t, f4::entities::EntityId> objective_id_map;
    std::unordered_map<uint32_t, f4::entities::EntityId> unit_id_map;
};

/// Populate an EntityWorld with campaign state from the WorldState.
/// Creates a single entity with CampaignStateComponent, tagged role="campaign".
/// Returns the created entity ID.
f4::entities::EntityId populate_campaign(f4::entities::EntityWorld& world,
                                          const WorldState& ws);

/// Populate an EntityWorld with team entities from the WorldState.
/// Each non-empty team slot becomes an entity with:
///   - a CampaignIdentityComponent (team_id, callsign = team name)
///   - a TeamComponent (slot, flags, colour, motto, stance, member,
///     experience, pilot slots)
///   - tags: role="team", team=<name>, alive=true
/// Returns the created entity IDs.
///
/// This is the structural fix for the injection-harness trap: the entity
/// world now carries REAL team identities parsed from a REAL campaign file,
/// not synthetic test fixtures.
std::vector<f4::entities::EntityId> populate_teams(f4::entities::EntityWorld& world,
                                                    const WorldState& ws);

/// Populate an EntityWorld with objective entities from the WorldState.
/// Each ObjectiveState becomes an entity with:
///   - TransformComponent (grid→feet position)
///   - ObjectiveTypeComponent, OwnershipComponent, ObjectivePriorityComponent
///   - Conditional: SupplyStateComponent, DamageBitmapComponent, RadarComponent,
///     NetworkLinksComponent, GroundLayoutComponent, FeatureSetComponent
///   - PropertyBag for format residue (vu_id_creator, vu_id_num, entity_type, obj_flags)
///   - tags: role="objective", team=<owner>, alive=true
/// Returns the created entity IDs and populates obj_id_map with VU_ID.num→EntityId.
std::vector<f4::entities::EntityId> populate_objectives(
    f4::entities::EntityWorld& world,
    const WorldState& ws,
    std::unordered_map<uint32_t, f4::entities::EntityId>& obj_id_map);

/// Populate an EntityWorld with unit entities from the WorldState.
/// Each UnitState becomes an entity with:
///   - TransformComponent (grid→feet position)
///   - UnitCoreComponent
///   - Subclass-specific component (GroundTacticalComponent, SquadronComponent,
///     FlightPlanComponent, PackageSupportComponent)
///   - Conditional: WaypointPlanComponent, VehicleCompositionComponent,
///     UnitClassScoreComponent, HierarchyComponent
///   - PropertyBag for format residue
///   - tags: role=<unit_class_name>, team=<owner>, domain=<air|ground|naval>, alive=true
///
/// The obj_id_map (from populate_objectives) is used to resolve Squadron→airbase
/// cross-references. A second pass resolves unit→unit references
/// (battalion→brigade, flight→package/squadron) using unit_id_map.
/// Returns the created entity IDs and populates unit_id_map with VU_ID.num→EntityId.
std::vector<f4::entities::EntityId> populate_units(
    f4::entities::EntityWorld& world,
    const WorldState& ws,
    const std::unordered_map<uint32_t, f4::entities::EntityId>& obj_id_map,
    std::unordered_map<uint32_t, f4::entities::EntityId>& unit_id_map);

/// Populate the entire EntityWorld from a WorldState.
/// Calls populate_campaign, populate_teams, populate_objectives, populate_units
/// and resolves all cross-references. Returns a PopulatedWorld with all IDs
/// and VU_ID maps.
PopulatedWorld populate_world(f4::entities::EntityWorld& world,
                               const WorldState& ws);

} // namespace f4::world
