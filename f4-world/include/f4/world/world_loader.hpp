// f4-world/include/f4/world/world_loader.hpp
//
// Populates an f4-entities EntityWorld from data sources. Each team,
// objective, and unit becomes an entity with domain components; format-derived
// concepts (VU_ID, nameid, obj_flags) are resolved or discarded here.
//
// Phase 1: populate_teams adds TeamComponent + narrowed CampaignIdentityComponent.
// Phase 3: populate_objectives, populate_units, populate_campaign, populate_world
//   bridge all entity types via WorldState.
// Phase 4: Interface-based bridge overloads accept ICampaignSource, ITeamSource,
//   IObjectiveSource, IUnitSource.
//
// WORLDSTATE IS NOW AN IMPLEMENTATION DETAIL (header-leak fix):
//   The four *Adapter structs (CampaignAdapter, TeamAdapter, ObjectiveAdapter,
//   UnitAdapter) and the WorldStateAdapters bundle previously lived in this
//   public header. They dereference `const WorldState*` in their method bodies,
//   so their definitions required the full WorldState layout — which meant
//   every consumer of <f4/world/f4_world.hpp> transitively pulled in
//   <f4/world/detail/world_state.hpp> and all its format-derived fields.
//
//   The Adapter structs are now defined in src/world_loader.cpp (the only
//   translation unit that needs them). This header forward-declares WorldState
//   so the convenience overloads (populate_world(EntityWorld&, const WorldState&))
//   can be declared without exposing the WorldState struct. Consumers that need
//   to construct a WorldState (e.g. to call load() with a pre-built state, or
//   for testing) include <f4/world/detail/world_state.hpp> explicitly.

#pragma once

#include <filesystem>
#include <unordered_map>

#include <f4/entities/entity.hpp>
#include <f4/world/data_source.hpp>

namespace f4::world {

// Forward declaration — WorldState is an implementation detail defined in
// <f4/world/detail/world_state.hpp>. Include that header explicitly if you
// need to construct or inspect a WorldState directly.
struct WorldState;

// ============================================================================
// PopulatedWorld — result of populate_world().
//
// Phase B slim-down: the four per-kind EntityId vectors (campaign, teams,
// objectives, units) have been removed. They were tag-derivable from
// EntityWorld via with_tag(tags::ROLE, ...) at any time, so storing them
// in a parallel struct was redundant cache that could drift from the
// EntityWorld's actual contents. Consumers now derive the lists they
// need directly:
//
//   • The world viewer snapshots them once at load time into per-kind
//     caches (see ViewerApp::Impl::{campaign,teams,objectives,units}_cache
//     in viewer_state.hpp), so the per-frame render loops don't pay the
//     O(N) with_tag() cost.
//   • Tests call with_tag() directly when they need to count or iterate
//     entities of a specific kind, which is both more honest (the test
//     is exercising the tag system) and removes a parallel source of
//     truth that could mask bridge bugs.
//
// What remains here are the two VU_ID.num → EntityId maps, which are NOT
// tag-derivable (VU_IDs are external identifiers from the binary .cam
// format, not ECS tags). They're kept in PopulatedWorld because:
//   1. The bridge needs them internally during populate_units' second
//      pass to resolve unit→unit and unit→objective cross-references.
//   2. Downstream consumers (the world viewer's inspector) need them to
//      resolve raw VU_IDs that survive in format-residue fields like
//      ObjectivePriorityComponent::parent_id (objective→objective
//      hierarchy isn't modeled in the bridge, so the raw ID leaks
//      through and must be looked up here).
// ============================================================================
struct PopulatedWorld {
    /// VU_ID.num → EntityId maps for cross-reference resolution.
    /// Kept in PopulatedWorld (not just internal to the bridge) because
    /// downstream consumers (e.g. the world viewer's inspector) need them
    /// to resolve raw VU_IDs that appear in format-residue fields like
    /// ObjectivePriorityComponent::parent_id (which has no resolved
    /// EntityId counterpart because objective→objective hierarchy isn't
    /// modeled in the bridge).
    std::unordered_map<uint32_t, f4::entities::EntityId> objective_id_map;
    std::unordered_map<uint32_t, f4::entities::EntityId> unit_id_map;
};

// ============================================================================
// Interface-based bridge functions (Phase 4 — primary API)
// ============================================================================

f4::entities::EntityId populate_campaign(f4::entities::EntityWorld& world,
                                          const ICampaignSource& src);
std::vector<f4::entities::EntityId> populate_teams(f4::entities::EntityWorld& world,
                                                    const ITeamSource& src);
std::vector<f4::entities::EntityId> populate_objectives(
    f4::entities::EntityWorld& world,
    const IObjectiveSource& src,
    std::unordered_map<uint32_t, f4::entities::EntityId>& obj_id_map);
std::vector<f4::entities::EntityId> populate_units(
    f4::entities::EntityWorld& world,
    const IUnitCoreSource& src,
    const std::unordered_map<uint32_t, f4::entities::EntityId>& obj_id_map,
    std::unordered_map<uint32_t, f4::entities::EntityId>& unit_id_map);
PopulatedWorld populate_world(f4::entities::EntityWorld& world,
                               const ICampaignSource& camp_src,
                               const ITeamSource& team_src,
                               const IObjectiveSource& obj_src,
                               const IUnitCoreSource& unit_src);

// ============================================================================
// WorldState-based convenience overloads (backward compatible)
//
// These take `const WorldState&` — a forward declaration is sufficient for
// the declaration. The definitions live in world_loader.cpp and pull in the
// full WorldState header there.
// ============================================================================
f4::entities::EntityId populate_campaign(f4::entities::EntityWorld& world,
                                          const WorldState& ws);
std::vector<f4::entities::EntityId> populate_teams(f4::entities::EntityWorld& world,
                                                    const WorldState& ws);
std::vector<f4::entities::EntityId> populate_objectives(
    f4::entities::EntityWorld& world,
    const WorldState& ws,
    std::unordered_map<uint32_t, f4::entities::EntityId>& obj_id_map);
std::vector<f4::entities::EntityId> populate_units(
    f4::entities::EntityWorld& world,
    const WorldState& ws,
    const std::unordered_map<uint32_t, f4::entities::EntityId>& obj_id_map,
    std::unordered_map<uint32_t, f4::entities::EntityId>& unit_id_map);
PopulatedWorld populate_world(f4::entities::EntityWorld& world,
                               const WorldState& ws);

// ============================================================================
// Convenience API — hides WorldState entirely
// ============================================================================
PopulatedWorld load(const std::filesystem::path& json_path,
                     f4::entities::EntityWorld& world);
PopulatedWorld load_from_string(const std::string& json,
                                 f4::entities::EntityWorld& world);

} // namespace f4::world
