// f4-world/src/world_loader.cpp — populate f4-entities from WorldState.
//
// Phase 1 update: populate_teams now adds TeamComponent (with .tea
// enrichment data) and a narrowed CampaignIdentityComponent (team_id +
// callsign only). unit_type_name is no longer stuffed into
// CampaignIdentityComponent — it was serving two roles.

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

        // Narrowed CampaignIdentityComponent — only team_id + callsign.
        auto& cid = h.add<f4::entities::CampaignIdentityComponent>();
        cid.team_id = t.slot;
        cid.callsign = t.name;

        // TeamComponent — carries all team-specific data including .tea
        // enrichment (stance, member, experience, pilot slots).
        auto& tc = h.add<f4::entities::TeamComponent>();
        tc.slot = t.slot;
        tc.flags = t.flags;
        tc.colour = t.colour;
        tc.motto = t.motto;
        tc.stance = t.stance;
        tc.member = t.member;
        tc.air_experience = t.air_experience;
        tc.ground_experience = t.ground_experience;
        tc.naval_experience = t.naval_experience;
        tc.air_defense_experience = t.air_defense_experience;
        tc.first_colonel = t.first_colonel;
        tc.first_commander = t.first_commander;
        tc.first_wingman = t.first_wingman;
        tc.last_wingman = t.last_wingman;

        ids.push_back(h.id());
    }
    return ids;
}

} // namespace f4::world
