// f4-world/include/f4/world/world_loader.hpp
//
// Populates an f4-entities EntityWorld from a parsed WorldState. Each team
// becomes an entity tagged with team identity; as objective/unit decoders
// come online, this is where they spawn entities with TransformComponents
// positioned at real theater coordinates.

#pragma once

#include <f4/entities/entity.hpp>
#include <f4/world/world_state.hpp>

namespace f4::world {

/// Populate an EntityWorld with team entities from the WorldState.
/// Each non-empty team slot becomes an entity with:
///   - a CampaignIdentityComponent (team_id, callsign = team name)
///   - tags: role="team", team=<name>, alive=true
/// Returns the created entity IDs.
///
/// This is the structural fix for the injection-harness trap: the entity
/// world now carries REAL team identities parsed from a REAL campaign file,
/// not synthetic test fixtures.
std::vector<f4::entities::EntityId> populate_teams(f4::entities::EntityWorld& world,
                                                    const WorldState& ws);

} // namespace f4::world
