// f4-world/src/world_loader.cpp — populate f4-entities from WorldState.

#include <f4/world/world_loader.hpp>

namespace f4::world {

std::vector<f4::entities::EntityId> populate_teams(f4::entities::EntityWorld& world,
                                                    const WorldState& ws) {
    std::vector<f4::entities::EntityId> ids;
    for (const auto& t : ws.teams) {
        // Slot 0 with an empty or placeholder name is the neutral/unused slot.
        if (t.name.empty()) continue;

        auto h = world.create();
        h.set_tag(f4::entities::tags::ROLE, f4::entities::TagValue::from(std::string("team")));
        h.set_tag(f4::entities::tags::TEAM, f4::entities::TagValue::from(t.name));
        h.set_tag(f4::entities::tags::ALIVE, f4::entities::TagValue::from(true));

        auto& id = h.add<f4::entities::CampaignIdentityComponent>();
        id.team_id = t.slot;
        id.callsign = t.name;
        // unit_type_name is not applicable to team entities; left empty.

        ids.push_back(h.id());
    }
    return ids;
}

} // namespace f4::world
