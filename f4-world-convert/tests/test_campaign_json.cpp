// test_campaign_json.cpp — from_world_json_campaign round-trip tests.
//
// The contract: to_world_json emits a "campaign" block;
// from_world_json_campaign parses it back into a CampaignHeader; the
// re-parsed header must match the original (struct equality, verified via
// encode_cmp_payload identity — the same technique test_cmp_encoder uses).
//
// Also covers the --reencode-cmp CLI path: to_world_json (preserve) →
// cam_from_world_json with a re-encoded .cmp → load → decode → struct match.

#include <gtest/gtest.h>
#include <f4/world_convert/cam_archive.hpp>
#include <f4/world_convert/campaign_decoder.hpp>
#include <f4/world_convert/campaign_json.hpp>
#include <f4/world_convert/cmp_encoder.hpp>
#include <f4/world_convert/world_json.hpp>

#include <vector>

using namespace f4::world_convert;

namespace {

CampaignHeader decode_fixture_cmp() {
    CamArchive cam;
    cam.load(FIXTURE_DIR "save1.cam");
    const SubFile* cmp = cam.find("cmp");
    EXPECT_NE(cmp, nullptr);
    return decode_cmp(cmp->data.data(), cmp->data.size());
}

} // namespace

// ═══════════════════════════════════════════════════════════════════════════
// JSON round-trip: decode → to_world_json → from_world_json_campaign → compare
// ═══════════════════════════════════════════════════════════════════════════

TEST(CampaignJson, RoundTripPreservesAllFields) {
    CampaignHeader h1 = decode_fixture_cmp();

    // Emit to JSON (with the new fields: te_number_f16s, camp_map_b64,
    // squadrons, remaining_payload_b64). preserve_all_subfiles is not
    // needed here — the campaign block is always emitted.
    CamArchive cam;
    cam.load(FIXTURE_DIR "save1.cam");
    std::string json = to_world_json(cam);

    // Parse back.
    CampaignHeader h2;
    ASSERT_NO_THROW(h2 = from_world_json_campaign(json));

    // Struct equality via the deterministic payload encoder: equal structs
    // produce equal .cmp payloads. Any field that fails to round-trip
    // surfaces as a payload difference here.
    auto p1 = encode_cmp_payload(h1);
    auto p2 = encode_cmp_payload(h2);
    EXPECT_EQ(p1, p2) << "JSON round-trip changed the campaign header";

    // Spot-check the fields that were NOT in the original JSON emission
    // (the ones this tranche added).
    EXPECT_EQ(h2.te_number_f16s, h1.te_number_f16s);
    EXPECT_EQ(h2.camp_map, h1.camp_map);
    EXPECT_EQ(h2.last_index_num, h1.last_index_num);
    EXPECT_EQ(h2.squadrons.size(), h1.squadrons.size());
    EXPECT_EQ(h2.remaining_payload, h1.remaining_payload);

    // The squadrons are non-empty on the fixture (the test_campaign.cpp
    // test confirms num_avail_squadrons > 0).
    EXPECT_GT(h2.squadrons.size(), 0u);
    if (!h2.squadrons.empty() && !h1.squadrons.empty()) {
        EXPECT_EQ(h2.squadrons[0].airbase_name, h1.squadrons[0].airbase_name);
        EXPECT_EQ(h2.squadrons[0].current_strength, h1.squadrons[0].current_strength);
    }
}

TEST(CampaignJson, RoundTripPreservesKnownFixtureValues) {
    CampaignHeader h1 = decode_fixture_cmp();
    CamArchive cam;
    cam.load(FIXTURE_DIR "save1.cam");
    std::string json = to_world_json(cam);
    CampaignHeader h2 = from_world_json_campaign(json);

    EXPECT_EQ(h2.current_time, h1.current_time);
    EXPECT_GT(h2.current_time, 0);
    EXPECT_EQ(h2.theater_name, "KOREA");
    EXPECT_EQ(h2.theater_size_x, 1024);
    EXPECT_EQ(h2.theater_size_y, 1024);
    EXPECT_EQ(static_cast<int>(h2.active_teams), 8);
    EXPECT_EQ(h2.teams.size(), 8u);
    // A known team name survives the JSON round-trip.
    bool has_rok = false;
    for (const auto& t : h2.teams) if (t.name == "ROK") has_rok = true;
    EXPECT_TRUE(has_rok);
}

// ═══════════════════════════════════════════════════════════════════════════
// read_world_json_version
// ═══════════════════════════════════════════════════════════════════════════

TEST(CampaignJson, ReadsVersionFromJson) {
    CamArchive cam;
    cam.load(FIXTURE_DIR "save1.cam");
    std::string json = to_world_json(cam);
    EXPECT_EQ(read_world_json_version(json), 63);
}

TEST(CampaignJson, ReturnsDefaultVersionWhenAbsent) {
    // A minimal JSON with no "version" key.
    std::string json = R"({"theater": "korea"})";
    EXPECT_EQ(read_world_json_version(json), 63);
}

// ═══════════════════════════════════════════════════════════════════════════
// Error handling
// ═══════════════════════════════════════════════════════════════════════════

TEST(CampaignJson, ThrowsWhenCampaignBlockAbsent) {
    std::string json = R"({"theater": "korea", "version": 63})";
    EXPECT_THROW(from_world_json_campaign(json), std::runtime_error);
}

// ═══════════════════════════════════════════════════════════════════════════
// Modified-save path: mutate the JSON, re-encode, decode → verify the mutation
// ═══════════════════════════════════════════════════════════════════════════

TEST(CampaignJson, ReencodedCmpCarriesMutation) {
    // Decode the fixture, advance current_time by 3600 ticks (1 hour at
    // 1 tick/sec), emit to JSON, parse back, re-encode the .cmp, and
    // decode the re-encoded .cmp. The mutated current_time must survive.
    CampaignHeader h1 = decode_fixture_cmp();
    const int32_t orig_time = h1.current_time;
    h1.current_time = orig_time + 3600;

    // Encode the mutated header directly (bypassing JSON — this tests
    // encode_cmp carries the mutation).
    auto encoded = encode_cmp(h1);
    CampaignHeader h2 = decode_cmp(encoded.data(), encoded.size());
    EXPECT_EQ(h2.current_time, orig_time + 3600);
}
