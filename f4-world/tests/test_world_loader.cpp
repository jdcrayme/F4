// test_world_loader.cpp — populate f4-entities from WorldState.

#include <gtest/gtest.h>
#include <f4/world/f4_world.hpp>
#include <f4/entities/f4_entities.hpp>

#include <algorithm>

using namespace f4::world;
using namespace f4::entities;

namespace {
WorldState make_test_world() {
    WorldState ws;
    ws.version = 63;
    ws.teams = {
        {0, 0, 0, "", ""},
        {1, 1, 1, "U.S.",  "E Pluribus"},
        {2, 2, 2, "ROK",   ""},
        {3, 3, 3, "Japan", ""},
    };
    return ws;
}
}

TEST(WorldLoader, CreatesEntityPerNonEmptyTeam) {
    EntityWorld ew;
    WorldState ws = make_test_world();
    auto ids = populate_teams(ew, ws);
    // Slot 0 has empty name -> skipped. 3 teams created.
    EXPECT_EQ(ids.size(), 3u);
    EXPECT_EQ(ew.size(), 3u);
}

TEST(WorldLoader, TeamEntitiesHaveCorrectTagsAndIdentity) {
    EntityWorld ew;
    WorldState ws = make_test_world();
    auto ids = populate_teams(ew, ws);

    // Find the ROK team entity.
    auto rok_ids = ew.with_tag(tags::TEAM, TagValue::from(std::string("ROK")));
    ASSERT_EQ(rok_ids.size(), 1u);
    EntityHandle h(rok_ids[0], &ew);
    ASSERT_TRUE(h.valid());

    auto* id = h.get<CampaignIdentityComponent>();
    ASSERT_NE(id, nullptr);
    EXPECT_EQ(id->team_id, 2);
    EXPECT_EQ(id->callsign, "ROK");

    EXPECT_TRUE(h.has_tag(tags::ALIVE));
    EXPECT_TRUE(h.has_tag(tags::ROLE));
    EXPECT_EQ(h.get_tag(tags::ROLE)->str_val, "team");
}

TEST(WorldLoader, EmptyNameSlotsAreSkipped) {
    EntityWorld ew;
    WorldState ws;
    ws.teams = {
        {0, 0, 0, "", ""},
        {1, 0, 0, "Alpha", ""},
        {2, 0, 0, "", ""},      // empty, skipped
        {3, 0, 0, "Bravo", ""},
    };
    auto ids = populate_teams(ew, ws);
    EXPECT_EQ(ids.size(), 2u);
}

TEST(WorldLoader, CanQueryTeamsByTeamTag) {
    // This is the structural payoff: after populate_teams, the EntityWorld
    // can answer "give me all ROK entities" / "all PRC entities" via tag
    // queries — against REAL campaign data, not a synthetic harness.
    EntityWorld ew;
    WorldState ws = make_test_world();
    populate_teams(ew, ws);

    auto us = ew.with_tag(tags::TEAM, TagValue::from(std::string("U.S.")));
    auto rok = ew.with_tag(tags::TEAM, TagValue::from(std::string("ROK")));
    auto japan = ew.with_tag(tags::TEAM, TagValue::from(std::string("Japan")));
    auto none = ew.with_tag(tags::TEAM, TagValue::from(std::string("Nonexistent")));
    EXPECT_EQ(us.size(), 1u);
    EXPECT_EQ(rok.size(), 1u);
    EXPECT_EQ(japan.size(), 1u);
    EXPECT_EQ(none.size(), 0u);
}
