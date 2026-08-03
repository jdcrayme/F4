// test_world_state.cpp — WorldState JSON loader.
//
// Tests against a synthetic JSON snippet (for unit-level field correctness)
// and against the real save1.cam-derived JSON (for end-to-end fidelity).

#include <gtest/gtest.h>
#include <f4/world/f4_world.hpp>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <set>

using namespace f4::world;

namespace {
std::string read_file(const std::string& path) {
    std::ifstream f(path);
    if (!f) return {};
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}
}

TEST(WorldState, LoadsVersionAndCampaignHeader) {
    WorldState ws;
    ws.load_from_string(R"({
        "version": 63,
        "campaign": {
            "current_time": 1000,
            "te_start_time": 500,
            "te_time_limit": 3600,
            "te_victory_points": 42,
            "te_type": 0,
            "te_number_teams": 0,
            "te_team": 0,
            "te_flags": 7,
            "te_number_aircraft": [1,2,3,4,5,6,7,8],
            "te_team_pts": [10,20,30,40,50,60,70,80],
            "teams": [],
            "decoded_bytes": 100,
            "undecoded_bytes": 200
        },
        "raw_subfiles": {}
    })");
    EXPECT_EQ(ws.version, 63);
    EXPECT_EQ(ws.campaign.current_time, 1000);
    EXPECT_EQ(ws.campaign.te_victory_points, 42);
    EXPECT_EQ(ws.campaign.te_flags, 7);
    ASSERT_EQ(ws.campaign.te_number_aircraft.size(), 8u);
    EXPECT_EQ(ws.campaign.te_number_aircraft[3], 4);
    ASSERT_EQ(ws.campaign.te_team_pts.size(), 8u);
    EXPECT_EQ(ws.campaign.te_team_pts[7], 80);
}

TEST(WorldState, LoadsTeamSlots) {
    WorldState ws;
    ws.load_from_string(R"({
        "version": 63,
        "campaign": {
            "current_time": 0,
            "teams": [
                {"slot": 0, "flags": 0, "colour": 0, "name": "XX", "motto": ""},
                {"slot": 1, "flags": 1, "colour": 1, "name": "U.S.", "motto": "E Pluribus"},
                {"slot": 2, "flags": 2, "colour": 2, "name": "ROK", "motto": ""}
            ]
        }
    })");
    ASSERT_EQ(ws.teams.size(), 3u);
    EXPECT_EQ(ws.teams[0].name, "XX");
    EXPECT_EQ(ws.teams[1].name, "U.S.");
    EXPECT_EQ(ws.teams[1].motto, "E Pluribus");
    EXPECT_EQ(ws.teams[2].slot, 2);
    EXPECT_EQ(ws.teams[2].flags, 2);
}

TEST(WorldState, RealCamJsonLoadsAllEightTeamSlots) {
    // Requires cam2json to have been run on the real fixture. The test
    // CMakeLists generates the JSON at build time into this path.
    const std::string path = WORLD_JSON_FIXTURE;
    if (!std::filesystem::exists(path)) GTEST_SKIP() << "fixture JSON not generated yet";
    auto json = read_file(path);
    ASSERT_FALSE(json.empty());
    WorldState ws;
    ws.load_from_string(json);
    EXPECT_EQ(ws.version, 63);
    EXPECT_EQ(ws.teams.size(), 8u);
    // The known Korea-theater team names must be present.
    std::vector<std::string> names;
    for (const auto& t : ws.teams) names.push_back(t.name);
    auto has = [&](const std::string& s) {
        return std::find(names.begin(), names.end(), s) != names.end();
    };
    EXPECT_TRUE(has("ROK"));
    EXPECT_TRUE(has("Japan"));
    EXPECT_TRUE(has("PRC"));
    EXPECT_TRUE(has("DPRK"));
}

// ---------------------------------------------------------------------------
// New tests: theater/terrain_file metadata, objectives, units
// ---------------------------------------------------------------------------
TEST(WorldState, LoadsTheaterAndTerrainFile) {
    WorldState ws;
    ws.load_from_string(R"({
        "version": 63,
        "theater": "korea",
        "terrain_file": "korea.terrain.json",
        "campaign": { "teams": [] }
    })");
    EXPECT_EQ(ws.theater, "korea");
    EXPECT_EQ(ws.terrain_file, "korea.terrain.json");
}

TEST(WorldState, RealCamJsonLoadsObjectives) {
    const std::string path = WORLD_JSON_FIXTURE;
    if (!std::filesystem::exists(path)) GTEST_SKIP() << "fixture JSON not generated yet";
    auto json = read_file(path);
    WorldState ws;
    ws.load_from_string(json);
    // save1.cam has 2,659 objectives (verified by f4-world-convert tests).
    EXPECT_EQ(ws.objectives.size(), 2659u);
    // Every objective must have plausible grid coordinates (0..1024).
    for (const auto& o : ws.objectives) {
        EXPECT_GE(o.x, 0);
        EXPECT_LE(o.x, 1024);
        EXPECT_GE(o.y, 0);
        EXPECT_LE(o.y, 1024);
    }
    // Multiple owners must be represented (PRC, ROK, ...).
    std::set<int> owners;
    for (const auto& o : ws.objectives) owners.insert(o.owner);
    EXPECT_GT(owners.size(), 1u);
}

TEST(WorldState, RealCamJsonLoadsUnits) {
    const std::string path = WORLD_JSON_FIXTURE;
    if (!std::filesystem::exists(path)) GTEST_SKIP() << "fixture JSON not generated yet";
    auto json = read_file(path);
    WorldState ws;
    ws.load_from_string(json);
    // After the v63 full-decode port, all 683 units are decoded.
    EXPECT_EQ(ws.units.size(), 683u);
    const auto& u = ws.units[0];
    EXPECT_GE(u.x, 0);
    EXPECT_LE(u.x, 1024);
    EXPECT_GE(u.y, 0);
    EXPECT_LE(u.y, 1024);
    EXPECT_LE(u.owner, 7);
    // The new fields should be populated:
    EXPECT_NE(u.unit_class, UnitClass::Unknown)
        << "every unit should have a recognized class";
}

TEST(WorldState, RealCamJsonUnitClassDistribution) {
    // The full-decode port should produce the known Korea distribution:
    // 524 battalions, 85 brigades, 72 squadrons, 2 taskforces.
    const std::string path = WORLD_JSON_FIXTURE;
    if (!std::filesystem::exists(path)) GTEST_SKIP() << "fixture JSON not generated yet";
    auto json = read_file(path);
    WorldState ws;
    ws.load_from_string(json);
    int counts[7] = {0};
    for (const auto& u : ws.units) {
        counts[static_cast<int>(u.unit_class)]++;
    }
    EXPECT_EQ(counts[static_cast<int>(UnitClass::Battalion)],  524);
    EXPECT_EQ(counts[static_cast<int>(UnitClass::Brigade)],    85);
    EXPECT_EQ(counts[static_cast<int>(UnitClass::Squadron)],   72);
    EXPECT_EQ(counts[static_cast<int>(UnitClass::TaskForce)],  2);
}

TEST(WorldState, RealCamJsonBattalionSupplyIsPopulated) {
    const std::string path = WORLD_JSON_FIXTURE;
    if (!std::filesystem::exists(path)) GTEST_SKIP() << "fixture JSON not generated yet";
    auto json = read_file(path);
    WorldState ws;
    ws.load_from_string(json);
    int with_supply = 0;
    for (const auto& u : ws.units) {
        if (u.unit_class == UnitClass::Battalion && u.supply > 0) {
            ++with_supply;
        }
    }
    EXPECT_GT(with_supply, 0) << "battalions should have non-zero supply";
}

TEST(WorldState, LoadTerrainResolvesRelativePath) {
    // Generate a tiny terrain JSON in a temp dir, write a world JSON that
    // references it by basename, and verify load_terrain() finds it.
    const std::string tmp_dir = "/tmp/f4_world_state_test";
    std::filesystem::create_directories(tmp_dir);
    const std::string terrain_path = std::string(tmp_dir) + "/korea.terrain.json";
    {
        std::ofstream f(terrain_path);
        f << R"({"theater":"korea","width":2,"height":2,"tile_types":[0,1,2,3]})";
    }
    const std::string world_path = std::string(tmp_dir) + "/test.world.json";
    {
        std::ofstream f(world_path);
        f << R"({"version":63,"theater":"korea","terrain_file":"korea.terrain.json","campaign":{"teams":[]}})";
    }

    WorldState ws;
    ws.load(world_path);
    EXPECT_FALSE(ws.terrain_loaded);
    ws.load_terrain();  // uses world_json_dir_ automatically
    EXPECT_TRUE(ws.terrain_loaded);
    EXPECT_EQ(ws.terrain.header.width, 2u);
    EXPECT_EQ(ws.terrain.header.height, 2u);
    EXPECT_EQ(ws.terrain.tile_types.size(), 4u);
}

// ============================================================================
// REFACTOR-4: Verify objective link data round-trips through the JSON →
// loader pipeline. The link data (road/rail network) is decoded by
// f4-world-convert, emitted to JSON, and parsed by f4-world's loader. This
// test verifies the round-trip preserves link count, neighbor VU_IDs, and
// the road/rail classification.
//
// This is the consumer-side gate: if the loader drops or mis-parses any
// link field, f4-ai (which will use the link network for pathfinding)
// would see a corrupted graph. The test catches that here, before f4-ai
// is built on top.
// ============================================================================
TEST(WorldState, RealCamJsonObjectiveLinksRoundTrip) {
    const std::string path = WORLD_JSON_FIXTURE;
    if (!std::filesystem::exists(path)) GTEST_SKIP() << "fixture JSON not generated yet";
    auto json = read_file(path);
    WorldState ws;
    ws.load_from_string(json);

    ASSERT_EQ(ws.objectives.size(), 2659u);

    // Tally total links across all objectives. The convert-side test
    // verifies the count is > 100; the loader must preserve every one.
    std::size_t total_links = 0;
    std::size_t objectives_with_links = 0;
    for (const auto& o : ws.objectives) {
        if (!o.links.empty()) ++objectives_with_links;
        total_links += o.links.size();
    }
    EXPECT_GT(total_links, 100u)
        << "expected the road/rail network to survive the JSON round-trip";
    EXPECT_GT(objectives_with_links, 100u)
        << "expected many objectives to retain their link lists";

    // At least some links must be classified as roads (the is_road bool
    // must survive the round-trip, not just the neighbor VU_IDs).
    // NOTE: is_rail is NOT checked here because railroads are modeled as
    // objective types (TYPE_RAILROAD=24), not as link movement costs —
    // see the convert-side test RoadLinksArePresent for the full
    // explanation.
    std::size_t road_links = 0;
    for (const auto& o : ws.objectives) {
        for (const auto& link : o.links) {
            if (link.is_road) ++road_links;
        }
    }
    EXPECT_GT(road_links, 100u)
        << "expected road classification to survive the JSON round-trip";

    // Neighbor VU_IDs must survive the round-trip. A zero neighbor_num
    // would indicate the loader dropped the field.
    std::size_t zero_neighbor_nums = 0;
    for (const auto& o : ws.objectives) {
        for (const auto& link : o.links) {
            if (link.neighbor_num == 0) ++zero_neighbor_nums;
        }
    }
    // Allow a small fraction (some links may legitimately have num=0),
    // but the vast majority must be non-zero.
    const double zero_fraction =
        static_cast<double>(zero_neighbor_nums) / static_cast<double>(total_links);
    EXPECT_LT(zero_fraction, 0.01)
        << zero_neighbor_nums << " of " << total_links
        << " links have zero neighbor_num after round-trip";
}

// Synthetic round-trip test: verify the loader correctly parses a known
// link array from JSON. This isolates the loader from the decoder — if
// this test fails but RealCamJsonObjectiveLinksRoundTrip passes, the
// decoder is correct but the loader has a parsing bug.
TEST(WorldState, ParsesSyntheticObjectiveLinks) {
    WorldState ws;
    ws.load_from_string(R"({
        "version": 63,
        "campaign": { "teams": [] },
        "objectives": {
            "count": 1,
            "decoded": 1,
            "items": [
                {
                    "type": 6,
                    "type_name": "Bridge",
                    "x": 500,
                    "y": 500,
                    "owner": 2,
                    "links": [
                        {"n": 12345, "c": 67890, "road": true,  "rail": false},
                        {"n": 99999, "c": 11111, "road": false, "rail": true},
                        {"n": 1,      "c": 2,      "road": true,  "rail": true}
                    ]
                }
            ]
        }
    })");
    ASSERT_EQ(ws.objectives.size(), 1u);
    ASSERT_EQ(ws.objectives[0].links.size(), 3u);

    // First link: road only
    EXPECT_EQ(ws.objectives[0].links[0].neighbor_num, 12345u);
    EXPECT_EQ(ws.objectives[0].links[0].neighbor_creator, 67890u);
    EXPECT_TRUE(ws.objectives[0].links[0].is_road);
    EXPECT_FALSE(ws.objectives[0].links[0].is_rail);

    // Second link: rail only
    EXPECT_EQ(ws.objectives[0].links[1].neighbor_num, 99999u);
    EXPECT_EQ(ws.objectives[0].links[1].neighbor_creator, 11111u);
    EXPECT_FALSE(ws.objectives[0].links[1].is_road);
    EXPECT_TRUE(ws.objectives[0].links[1].is_rail);

    // Third link: both road and rail
    EXPECT_EQ(ws.objectives[0].links[2].neighbor_num, 1u);
    EXPECT_EQ(ws.objectives[0].links[2].neighbor_creator, 2u);
    EXPECT_TRUE(ws.objectives[0].links[2].is_road);
    EXPECT_TRUE(ws.objectives[0].links[2].is_rail);
}
