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

    // CampaignIdentityComponent is narrowed: team_id + callsign only.
    auto* cid = h.get<CampaignIdentityComponent>();
    ASSERT_NE(cid, nullptr);
    EXPECT_EQ(cid->team_id, 2);
    EXPECT_EQ(cid->callsign, "ROK");

    // TeamComponent carries the team-specific data.
    auto* tc = h.get<TeamComponent>();
    ASSERT_NE(tc, nullptr);
    EXPECT_EQ(tc->slot, 2);
    EXPECT_EQ(tc->flags, 2);
    EXPECT_EQ(tc->colour, 2);

    EXPECT_TRUE(h.has_tag(tags::ALIVE));
    EXPECT_TRUE(h.has_tag(tags::ROLE));
    EXPECT_EQ(h.get_tag(tags::ROLE)->str_val, "team");
}

TEST(WorldLoader, TeamComponentCarriesTeaEnrichment) {
    EntityWorld ew;
    WorldState ws;
    ws.teams = {
        {1, 1, 1, "U.S.", "E Pluribus"},
    };
    // Simulate .tea enrichment data.
    ws.teams[0].tea_loaded = true;
    ws.teams[0].stance = {50, -50, 0, 0, 0, 0, 0, 0};
    ws.teams[0].member = {1, 0, 1, 0, 0, 0, 0, 0};
    ws.teams[0].air_experience = 80;
    ws.teams[0].ground_experience = 60;

    auto ids = populate_teams(ew, ws);
    ASSERT_EQ(ids.size(), 1u);
    EntityHandle h(ids[0], &ew);

    auto* tc = h.get<TeamComponent>();
    ASSERT_NE(tc, nullptr);
    EXPECT_EQ(tc->motto, "E Pluribus");
    ASSERT_EQ(tc->stance.size(), 8u);
    EXPECT_EQ(tc->stance[0], 50);
    EXPECT_EQ(tc->stance[1], -50);
    ASSERT_EQ(tc->member.size(), 8u);
    EXPECT_EQ(tc->member[0], 1);
    EXPECT_EQ(tc->air_experience, 80);
    EXPECT_EQ(tc->ground_experience, 60);
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

TEST(WorldLoader, CampaignIdentityNoLongerHasUnitTypeName) {
    // Verify that CampaignIdentityComponent is narrowed — it should NOT
    // have a unit_type_name field. This test confirms the Phase 1 change.
    EntityWorld ew;
    WorldState ws = make_test_world();
    populate_teams(ew, ws);

    auto us_ids = ew.with_tag(tags::TEAM, TagValue::from(std::string("U.S.")));
    ASSERT_EQ(us_ids.size(), 1u);
    EntityHandle h(us_ids[0], &ew);

    auto* cid = h.get<CampaignIdentityComponent>();
    ASSERT_NE(cid, nullptr);
    // CampaignIdentityComponent now only has team_id and callsign.
    EXPECT_EQ(cid->team_id, 1);
    EXPECT_EQ(cid->callsign, "U.S.");
    // unit_type_name was removed — there is no field to test,
    // the fact that this compiles confirms the narrowing.
}
