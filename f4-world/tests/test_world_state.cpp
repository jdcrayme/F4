// test_world_state.cpp — WorldState JSON loader.
//
// Tests against a synthetic JSON snippet (for unit-level field correctness)
// and against the real save1.cam-derived JSON (for end-to-end fidelity).

#include <gtest/gtest.h>
#include <f4/world/f4_world.hpp>

#include <algorithm>
#include <filesystem>
#include <fstream>

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
