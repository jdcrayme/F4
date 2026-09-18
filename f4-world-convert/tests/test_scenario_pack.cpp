// test_scenario_pack.cpp — the CAMP-INIT-1 pack parser's contract.
//
// The pack is a strict format: unknown keys are named errors (the wire
// protocol's own discipline), every cross-reference is validated at the
// parse, and the vocabulary words map onto the wire's own enums. These
// tests pin that surface — a typo'd pack fails loudly, a valid pack
// parses to the exact typed fields.

#include <gtest/gtest.h>
#include <f4/world_convert/scenario_pack.hpp>

using namespace f4::world_convert;

namespace {

const char* kMinimal = R"({
  "pack": 1,
  "name": "t",
  "seed": 7,
  "teams": [ { "slot": 1, "name": "BLUE" }, { "slot": 6, "name": "RED" } ],
  "objectives": [ { "name": "Alpha", "kind": "airbase", "x": 10, "y": 10, "owner": 1 } ]
})";

} // namespace

// ── the happy path ──────────────────────────────────────────────────────

TEST(ScenarioPack, MinimalPackParsesWithDocumentedDefaults) {
    const ScenarioPack p = ScenarioPack::parse(kMinimal);
    EXPECT_EQ(p.pack_version, 1);
    EXPECT_EQ(p.name, "t");
    EXPECT_EQ(p.camp_version, 71);          // the default layout
    EXPECT_EQ(p.seed, 7u);
    EXPECT_EQ(p.theater.size_x, 256);       // the default grid extent
    EXPECT_EQ(p.theater.size_y, 256);
    EXPECT_EQ(p.theater.id, "korea");
    EXPECT_EQ(p.date.current_time, 32400000);
    EXPECT_EQ(p.weather.condition, "clear");
    EXPECT_EQ(p.tempo, 255);                // the stock saves' own value
    ASSERT_EQ(p.teams.size(), 2u);
    EXPECT_EQ(p.teams[0].slot, 1);
    EXPECT_EQ(p.teams[0].name, "BLUE");
    EXPECT_EQ(p.teams[0].air_experience, 80);
    EXPECT_EQ(p.teams[0].replacements, 12); // the default replacement stock
    EXPECT_EQ(p.teams[0].reserve_aircraft, 0);
    ASSERT_EQ(p.objectives.size(), 1u);
    EXPECT_EQ(p.objectives[0].kind, "airbase");
    EXPECT_EQ(p.objectives[0].priority, 30);
    EXPECT_TRUE(p.squadrons.empty());
    EXPECT_TRUE(p.battalions.empty());
    EXPECT_TRUE(p.links.empty());
}

TEST(ScenarioPack, VocabularyWordsMapOntoTheWireEnums) {
    EXPECT_EQ(relation_from_word("war"), PackRelation::War);
    EXPECT_EQ(relation_from_word("allied"), PackRelation::Allied);
    EXPECT_EQ(relation_from_word("friendly"), PackRelation::Friendly);
    EXPECT_EQ(relation_from_word("neutral"), PackRelation::Neutral);
    EXPECT_EQ(relation_from_word("hostile"), PackRelation::Hostile);
    EXPECT_EQ(relation_from_word("none"), PackRelation::NoRelations);

    EXPECT_EQ(objective_type_from_kind("airbase"), 1);   // TYPE_AIRBASE
    EXPECT_EQ(objective_type_from_kind("airstrip"), 2);
    EXPECT_EQ(objective_type_from_kind("armybase"), 3);
    EXPECT_EQ(objective_type_from_kind("city"), 8);
    EXPECT_EQ(objective_type_from_kind("depot"), 10);
    EXPECT_EQ(objective_type_from_kind("port"), 19);

    EXPECT_EQ(battalion_subtype_from_kind("armor"), 3);  // STYPE_LAND_ARMOR
    EXPECT_EQ(battalion_subtype_from_kind("infantry"), 7);

    EXPECT_EQ(squadron_specialty_from_word("none"), 0);
    EXPECT_EQ(squadron_specialty_from_word("AA"), 1);
    EXPECT_EQ(squadron_specialty_from_word("AG"), 2);
}

TEST(ScenarioPack, FullPackParsesEveryBlock) {
    const ScenarioPack p = ScenarioPack::parse(R"({
      "pack": 1, "name": "full", "camp_version": 63, "seed": 4294967295,
      "theater": { "name": "TW", "ui_name": "TW UI", "scenario": "TW S",
                   "save_file": "TWSAVE", "id": "korea", "size_x": 64, "size_y": 64,
                   "terrain_file": "t.terrain.json" },
      "date": { "current_time": 1000, "day": 3 },
      "weather": { "seed": 9, "condition": "inclement" },
      "bullseye": { "x": 30, "y": 32, "name": 2 },
      "player_team": 6,
      "tempo": 128,
      "teams": [
        { "slot": 1, "name": "BLUE", "motto": "m1", "colour": 2,
          "air_experience": 90, "reserve_aircraft": 4, "replacements": 8,
          "supply": 500, "fuel": 600 },
        { "slot": 6, "name": "RED", "ground_experience": 40 }
      ],
      "relations": [ { "a": 1, "b": 6, "stance": "war" } ],
      "objectives": [
        { "name": "A", "kind": "airbase", "x": 10, "y": 12, "owner": 1,
          "priority": 70, "supply": 90, "fuel": 80, "entity_type": 0 },
        { "name": "B", "kind": "city", "x": 20, "y": 22, "owner": 6 }
      ],
      "links": [ [0, 1] ],
      "squadrons": [ { "team": 1, "base": "A", "name": "1st", "size": 16,
                       "skill": 6, "specialty": "AG" } ],
      "battalions": [ { "team": 6, "at": "B", "kind": "armor", "groups": 5 } ]
    })");
    EXPECT_EQ(p.camp_version, 63);
    EXPECT_EQ(p.seed, 4294967295u);
    EXPECT_EQ(p.theater.save_file, "TWSAVE");
    EXPECT_EQ(p.theater.size_x, 64);
    EXPECT_EQ(p.date.day, 3);
    EXPECT_EQ(p.weather.seed, 9u);
    EXPECT_EQ(p.weather.condition, "inclement");
    EXPECT_EQ(p.bullseye_name, 2);
    EXPECT_EQ(p.player_team, 6);
    EXPECT_EQ(p.tempo, 128);
    ASSERT_EQ(p.teams.size(), 2u);
    EXPECT_EQ(p.teams[0].reserve_aircraft, 4);
    EXPECT_EQ(p.teams[0].supply, 500);
    EXPECT_EQ(p.teams[1].ground_experience, 40);
    ASSERT_EQ(p.relations.size(), 1u);
    EXPECT_EQ(p.relations[0].stance, PackRelation::War);
    ASSERT_EQ(p.objectives.size(), 2u);
    EXPECT_EQ(p.objectives[0].priority, 70);
    EXPECT_EQ(p.objectives[0].supply, 90);
    ASSERT_EQ(p.links.size(), 1u);
    EXPECT_EQ(p.links[0].a, 0);
    EXPECT_EQ(p.links[0].b, 1);
    ASSERT_EQ(p.squadrons.size(), 1u);
    EXPECT_EQ(p.squadrons[0].size, 16);
    EXPECT_EQ(p.squadrons[0].specialty, "AG");
    ASSERT_EQ(p.battalions.size(), 1u);
    EXPECT_EQ(p.battalions[0].groups, 5);
}

// ── the strict surface ──────────────────────────────────────────────────

TEST(ScenarioPack, UnknownKeysAreNamedErrors) {
    try {
        ScenarioPack::parse("{ \"pack\": 1, \"nmae\": \"t\" }");
        FAIL() << "expected the typo'd key to throw";
    } catch (const std::exception& e) {
        EXPECT_NE(std::string(e.what()).find("nmae"), std::string::npos)
            << "the unknown key must be named: " << e.what();
    }
    try {
        ScenarioPack::parse(R"({ "pack": 1, "name": "t", "seed": 1,
            "teams": [ { "slot": 1, "name": "B", "xp": 5 },
                       { "slot": 6, "name": "R" } ],
            "objectives": [ { "name": "A", "kind": "city", "x": 1, "y": 1 } ] })");
        FAIL() << "expected the nested typo to throw";
    } catch (const std::exception& e) {
        EXPECT_NE(std::string(e.what()).find("xp"), std::string::npos)
            << "nested unknown keys are named too: " << e.what();
    }
}

TEST(ScenarioPack, UnknownVocabularyWordsAreNamedErrors) {
    EXPECT_THROW(ScenarioPack::parse(R"({ "pack": 1, "name": "t", "seed": 1,
        "teams": [ { "slot": 1, "name": "B" }, { "slot": 6, "name": "R" } ],
        "relations": [ { "a": 1, "b": 6, "stance": "blood feud" } ],
        "objectives": [ { "name": "A", "kind": "city", "x": 1, "y": 1 } ] })"),
                 std::runtime_error);
    EXPECT_THROW(ScenarioPack::parse(R"({ "pack": 1, "name": "t", "seed": 1,
        "teams": [ { "slot": 1, "name": "B" }, { "slot": 6, "name": "R" } ],
        "objectives": [ { "name": "A", "kind": "castle", "x": 1, "y": 1 } ] })"),
                 std::runtime_error);
}

TEST(ScenarioPack, CrossReferenceFaultsAreNamedErrors) {
    // squadron base names no objective
    EXPECT_THROW(ScenarioPack::parse(R"({ "pack": 1, "name": "t", "seed": 1,
        "teams": [ { "slot": 1, "name": "B" }, { "slot": 6, "name": "R" } ],
        "objectives": [ { "name": "A", "kind": "airbase", "x": 1, "y": 1, "owner": 1 } ],
        "squadrons": [ { "team": 1, "base": "Nope", "size": 4 } ] })"),
                 std::runtime_error);
    // objective owner references an unnamed slot
    EXPECT_THROW(ScenarioPack::parse(R"({ "pack": 1, "name": "t", "seed": 1,
        "teams": [ { "slot": 1, "name": "B" }, { "slot": 6, "name": "R" } ],
        "objectives": [ { "name": "A", "kind": "city", "x": 1, "y": 1, "owner": 4 } ] })"),
                 std::runtime_error);
    // link index out of range
    EXPECT_THROW(ScenarioPack::parse(R"({ "pack": 1, "name": "t", "seed": 1,
        "teams": [ { "slot": 1, "name": "B" }, { "slot": 6, "name": "R" } ],
        "objectives": [ { "name": "A", "kind": "city", "x": 1, "y": 1 } ],
        "links": [ [0, 5] ] })"),
                 std::runtime_error);
    // duplicate objective name
    EXPECT_THROW(ScenarioPack::parse(R"({ "pack": 1, "name": "t", "seed": 1,
        "teams": [ { "slot": 1, "name": "B" }, { "slot": 6, "name": "R" } ],
        "objectives": [ { "name": "A", "kind": "city", "x": 1, "y": 1 },
                        { "name": "A", "kind": "town", "x": 2, "y": 2 } ] })"),
                 std::runtime_error);
    // objective outside the theater grid
    EXPECT_THROW(ScenarioPack::parse(R"({ "pack": 1, "name": "t", "seed": 1,
        "theater": { "size_x": 64, "size_y": 64 },
        "teams": [ { "slot": 1, "name": "B" }, { "slot": 6, "name": "R" } ],
        "objectives": [ { "name": "A", "kind": "city", "x": 100, "y": 1 } ] })"),
                 std::runtime_error);
    // a lone team is not a war
    EXPECT_THROW(ScenarioPack::parse(R"({ "pack": 1, "name": "t", "seed": 1,
        "teams": [ { "slot": 1, "name": "B" } ],
        "objectives": [ { "name": "A", "kind": "city", "x": 1, "y": 1 } ] })"),
                 std::runtime_error);
    // camp map overflows the .cmp's i16 CampMapSize
    EXPECT_THROW(ScenarioPack::parse(R"({ "pack": 1, "name": "t", "seed": 1,
        "theater": { "size_x": 1024, "size_y": 1024 },
        "teams": [ { "slot": 1, "name": "B" }, { "slot": 6, "name": "R" } ],
        "objectives": [ { "name": "A", "kind": "city", "x": 1, "y": 1 } ] })"),
                 std::runtime_error);
    // a self-war is nonsense
    EXPECT_THROW(ScenarioPack::parse(R"({ "pack": 1, "name": "t", "seed": 1,
        "teams": [ { "slot": 1, "name": "B" }, { "slot": 6, "name": "R" } ],
        "relations": [ { "a": 1, "b": 1, "stance": "war" } ],
        "objectives": [ { "name": "A", "kind": "city", "x": 1, "y": 1 } ] })"),
                 std::runtime_error);
}
