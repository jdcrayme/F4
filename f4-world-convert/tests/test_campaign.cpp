// test_campaign.cpp — .cmp campaign header decode + end-to-end JSON, against
// the real save1.cam fixture. These are the semantic-correctness tests: they
// verify we decode actual data (team names, counts) from the binary, not just
// that the code runs.

#include <gtest/gtest.h>
#include <f4/convert/cam_archive.hpp>
#include <f4/convert/campaign_decoder.hpp>
#include <f4/convert/world_json.hpp>

#include <algorithm>

using namespace f4::convert;

namespace {
CampaignHeader decode_fixture_cmp() {
    CamArchive cam;
    cam.load(FIXTURE_DIR "save1.cam");
    const SubFile* cmp = cam.find("cmp");
    EXPECT_NE(cmp, nullptr);
    return decode_cmp(cmp->data.data(), cmp->data.size());
}
}

TEST(Campaign, DecodesCurrentTime) {
    auto h = decode_fixture_cmp();
    // Korea campaign start — CurrentTime should be a positive tick count.
    EXPECT_GT(h.current_time, 0);
}

TEST(Campaign, DecompressedSizeMatchesHeader) {
    auto h = decode_fixture_cmp();
    EXPECT_EQ(h.decompressed_size, 21259);
}

TEST(Campaign, TeamSlotsContainRealNames) {
    auto h = decode_fixture_cmp();
    // te_number_teams is the Tactical Engagement (TE) team count — 0 for a
    // full campaign save (TE is a separate game mode). The 8 team_name slots
    // are always populated regardless of mode.
    EXPECT_GE(h.te_number_teams, 0);
    EXPECT_LE(h.te_number_teams, 8);
    EXPECT_EQ(h.teams.size(), 8u);
    // Count non-empty team names (slot 0 is typically the neutral/unused slot).
    int non_empty = 0;
    for (const auto& t : h.teams) if (!t.name.empty()) ++non_empty;
    EXPECT_GE(non_empty, 6) << "expected at least 6 populated team slots";
}

TEST(Campaign, DecodesKnownTeamNames) {
    auto h = decode_fixture_cmp();
    // The save1.cam fixture is a Korea-theater campaign. The team_name[20]
    // slots must contain the known team names we saw in the raw strings:
    // "ROK", "Japan", "PRC", and a fourth (DPRK or similar).
    std::vector<std::string> names;
    for (const auto& t : h.teams) names.push_back(t.name);
    // At least ROK, Japan, PRC must appear among the 8 slots.
    auto contains = [&](const std::string& s) {
        return std::find(names.begin(), names.end(), s) != names.end();
    };
    EXPECT_TRUE(contains("ROK"))   << "team ROK not found";
    EXPECT_TRUE(contains("Japan")) << "team Japan not found";
    EXPECT_TRUE(contains("PRC"))   << "team PRC not found";
}

TEST(Campaign, RemainingPayloadPreservedForFutureDecoders) {
    auto h = decode_fixture_cmp();
    // We decode through the team block (~1.8KB of the 21KB payload). The
    // remaining ~19KB (objective/unit references, etc.) is preserved.
    EXPECT_GT(h.remaining_payload.size(), 15000u);
    EXPECT_LT(h.remaining_payload.size(), static_cast<std::size_t>(h.decompressed_size));
}

TEST(WorldJson, EmitsValidJsonWithExpectedFields) {
    CamArchive cam;
    cam.load(FIXTURE_DIR "save1.cam");
    std::string json = to_world_json(cam);

    // Structural: valid JSON object with the top-level keys.
    EXPECT_NE(json.find("\"archive\""),   std::string::npos);
    EXPECT_NE(json.find("\"version\""),   std::string::npos);
    EXPECT_NE(json.find("\"campaign\""),  std::string::npos);
    EXPECT_NE(json.find("\"teams\""),     std::string::npos);
    EXPECT_NE(json.find("\"raw_subfiles\""), std::string::npos);

    // Semantic: the known team names appear in the JSON.
    EXPECT_NE(json.find("ROK"),   std::string::npos);
    EXPECT_NE(json.find("Japan"), std::string::npos);
    EXPECT_NE(json.find("PRC"),   std::string::npos);

    // The version number (63) appears.
    EXPECT_NE(json.find("\"version\": 63"), std::string::npos);
}
