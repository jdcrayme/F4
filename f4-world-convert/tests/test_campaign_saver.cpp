// test_campaign_saver.cpp — CampaignSaver integration tests.
//
// The contract: save_campaign takes a world JSON (with subfiles_b64) +
// CampaignMutations, and produces a .cam whose .cmp carries the mutations.
// The saved .cam must:
//   - decode to the same subfiles (except .cmp, which is re-encoded)
//   - have a .cmp whose decoded CampaignHeader reflects the mutations
//   - pass through all non-.cmp subfiles byte-identically
//
// This is the integration bridge: it connects the campaign simulation's
// mutated state (via CampaignMutations) to the binary save format.

#include <gtest/gtest.h>
#include <f4/world_convert/cam_archive.hpp>
#include <f4/world_convert/cam_writer.hpp>
#include <f4/world_convert/campaign_decoder.hpp>
#include <f4/world_convert/campaign_json.hpp>
#include <f4/world_convert/campaign_saver.hpp>
#include <f4/world_convert/world_json.hpp>

#include <filesystem>
#include <fstream>
#include <vector>

using namespace f4::world_convert;

namespace fs = std::filesystem;

namespace {

// Load the fixture, emit a preserve-subfiles world JSON, and return it.
std::string make_fixture_world_json() {
    CamArchive cam;
    cam.load(FIXTURE_DIR "save1.cam");
    WorldJsonOptions opts;
    opts.preserve_all_subfiles = true;
    return to_world_json(cam, opts);
}

// Write bytes to a temp file and load as CamArchive.
CamArchive load_from_bytes(const std::vector<uint8_t>& bytes) {
    static int seq = 0;
    fs::path tmp = fs::temp_directory_path() /
                   ("f4_test_saver_" + std::to_string(seq++) + ".cam");
    {
        std::ofstream f(tmp, std::ios::binary);
        f.write(reinterpret_cast<const char*>(bytes.data()),
                static_cast<std::streamsize>(bytes.size()));
    }
    CamArchive cam;
    cam.load(tmp);
    std::error_code ec;
    fs::remove(tmp, ec);
    return cam;
}

} // namespace

// ═══════════════════════════════════════════════════════════════════════════
// Mutation: current_time
// ═══════════════════════════════════════════════════════════════════════════

TEST(CampaignSaver, MutatesCurrentTime) {
    std::string json = make_fixture_world_json();

    // Read the original current_time.
    CampaignHeader orig = from_world_json_campaign(json);
    const int32_t orig_time = orig.current_time;

    // Mutate: advance by 3600 (1 hour).
    CampaignMutations mut;
    mut.current_time = orig_time + 3600;

    auto cam_bytes = build_campaign(json, mut);
    ASSERT_FALSE(cam_bytes.empty());

    // Load and decode the .cmp.
    CamArchive cam = load_from_bytes(cam_bytes);
    const SubFile* cmp = cam.find("cmp");
    ASSERT_NE(cmp, nullptr);
    CampaignHeader saved = decode_cmp(cmp->data.data(), cmp->data.size());

    EXPECT_EQ(saved.current_time, orig_time + 3600);
    EXPECT_NE(saved.current_time, orig_time);
}

// ═══════════════════════════════════════════════════════════════════════════
// Mutation: team pools (te_number_aircraft)
// ═══════════════════════════════════════════════════════════════════════════

TEST(CampaignSaver, MutatesTeamPools) {
    std::string json = make_fixture_world_json();
    CampaignHeader orig = from_world_json_campaign(json);
    ASSERT_EQ(orig.te_number_aircraft.size(), 8u);

    // Mutate: set team 0's pool to 42, team 1's to 99.
    CampaignMutations mut;
    mut.te_number_aircraft = orig.te_number_aircraft;
    mut.te_number_aircraft[0] = 42;
    mut.te_number_aircraft[1] = 99;

    auto cam_bytes = build_campaign(json, mut);
    CamArchive cam = load_from_bytes(cam_bytes);
    const SubFile* cmp = cam.find("cmp");
    CampaignHeader saved = decode_cmp(cmp->data.data(), cmp->data.size());

    EXPECT_EQ(saved.te_number_aircraft[0], 42);
    EXPECT_EQ(saved.te_number_aircraft[1], 99);
    // Unmutated teams keep their original values.
    for (int i = 2; i < 8; ++i) {
        EXPECT_EQ(saved.te_number_aircraft[i], orig.te_number_aircraft[i])
            << "team " << i << " pool changed unexpectedly";
    }
}

// ═══════════════════════════════════════════════════════════════════════════
// Mutation: maintenance timers
// ═══════════════════════════════════════════════════════════════════════════

TEST(CampaignSaver, MutatesTimers) {
    std::string json = make_fixture_world_json();
    CampaignHeader orig = from_world_json_campaign(json);

    CampaignMutations mut;
    mut.last_resupply = orig.last_resupply + 7200;
    mut.last_repair = orig.last_repair + 3600;
    mut.last_reinforcement = orig.last_reinforcement + 14400;

    auto cam_bytes = build_campaign(json, mut);
    CamArchive cam = load_from_bytes(cam_bytes);
    const SubFile* cmp = cam.find("cmp");
    CampaignHeader saved = decode_cmp(cmp->data.data(), cmp->data.size());

    EXPECT_EQ(saved.last_resupply, orig.last_resupply + 7200);
    EXPECT_EQ(saved.last_repair, orig.last_repair + 3600);
    EXPECT_EQ(saved.last_reinforcement, orig.last_reinforcement + 14400);
}

// ═══════════════════════════════════════════════════════════════════════════
// Non-mutated fields are preserved
// ═══════════════════════════════════════════════════════════════════════════

TEST(CampaignSaver, PreservesUnmutatedFields) {
    std::string json = make_fixture_world_json();
    CampaignHeader orig = from_world_json_campaign(json);

    // Mutate only current_time; everything else should stay.
    CampaignMutations mut;
    mut.current_time = orig.current_time + 100;

    auto cam_bytes = build_campaign(json, mut);
    CamArchive cam = load_from_bytes(cam_bytes);
    const SubFile* cmp = cam.find("cmp");
    CampaignHeader saved = decode_cmp(cmp->data.data(), cmp->data.size());

    EXPECT_EQ(saved.theater_name, orig.theater_name);
    EXPECT_EQ(saved.te_type, orig.te_type);
    EXPECT_EQ(saved.te_flags, orig.te_flags);
    EXPECT_EQ(saved.te_number_teams, orig.te_number_teams);
    EXPECT_EQ(saved.teams.size(), orig.teams.size());
    if (saved.teams.size() == orig.teams.size()) {
        EXPECT_EQ(saved.teams[0].name, orig.teams[0].name);
        EXPECT_EQ(saved.teams[0].flags, orig.teams[0].flags);
    }
}

// ═══════════════════════════════════════════════════════════════════════════
// Non-.cmp subfiles pass through byte-identically
// ═══════════════════════════════════════════════════════════════════════════

TEST(CampaignSaver, PassesThroughNonCmpSubfiles) {
    std::string json = make_fixture_world_json();

    // Load the original subfiles (from the JSON) for comparison.
    auto orig_bytes = cam_from_world_json(json);
    CamArchive orig_cam = load_from_bytes(orig_bytes);

    // Save with a mutation.
    CampaignMutations mut;
    mut.current_time = 999999;
    auto saved_bytes = build_campaign(json, mut);
    CamArchive saved_cam = load_from_bytes(saved_bytes);

    // Every non-.cmp subfile must be byte-identical.
    ASSERT_EQ(saved_cam.subfiles().size(), orig_cam.subfiles().size());
    for (std::size_t i = 0; i < orig_cam.subfiles().size(); ++i) {
        const auto& orig_sf = orig_cam.subfiles()[i];
        const auto& saved_sf = saved_cam.subfiles()[i];
        if (orig_sf.ext() == "cmp") continue;   // .cmp was re-encoded
        EXPECT_EQ(saved_sf.name, orig_sf.name)
            << "subfile " << i << " name changed";
        EXPECT_EQ(saved_sf.data, orig_sf.data)
            << "subfile " << i << " (" << orig_sf.name << ") data changed";
    }
}

// ═══════════════════════════════════════════════════════════════════════════
// Default mutations (no overrides) = struct-faithful re-encode
// ═══════════════════════════════════════════════════════════════════════════

TEST(CampaignSaver, DefaultMutationsProduceStructFaithfulReencode) {
    // With no mutations set (all UNSET), the saver re-encodes the .cmp
    // from the parsed campaign block unchanged. The decoded struct must
    // match the original.
    std::string json = make_fixture_world_json();
    CampaignHeader orig = from_world_json_campaign(json);

    CampaignMutations mut;   // all defaults (UNSET)
    auto cam_bytes = build_campaign(json, mut);
    CamArchive cam = load_from_bytes(cam_bytes);
    const SubFile* cmp = cam.find("cmp");
    CampaignHeader saved = decode_cmp(cmp->data.data(), cmp->data.size());

    EXPECT_EQ(saved.current_time, orig.current_time);
    EXPECT_EQ(saved.last_resupply, orig.last_resupply);
    EXPECT_EQ(saved.te_number_aircraft, orig.te_number_aircraft);
    EXPECT_EQ(saved.theater_name, orig.theater_name);
}
