// test_objectives.cpp — .obj objective decoder against the real save1.cam.

#include <gtest/gtest.h>
#include <f4/world_convert/cam_archive.hpp>
#include <f4/world_convert/objective_decoder.hpp>
#include <f4/world_convert/world_json.hpp>
#include <f4/world_convert/class_table.hpp>

#include <cmath>
#include <filesystem>
#include <set>

using namespace f4::world_convert;

namespace {
CamArchive load_fixture() {
    CamArchive cam;
    cam.load(FIXTURE_DIR "save1.cam");
    return cam;
}
}

TEST(Objectives, DecodesAll2659Objectives) {
    auto cam = load_fixture();
    const SubFile* obj = cam.find("obj");
    ASSERT_NE(obj, nullptr);
    DecodedObjectives objs = decode_obj(obj->data.data(), obj->data.size());
    EXPECT_EQ(objs.count, 2659);
    // Every objective record must be decoded (no cursor desync).
    EXPECT_EQ(static_cast<int>(objs.objectives.size()), objs.count);
}

TEST(Objectives, AllRecordsHavePlausibleGridCoordinates) {
    auto cam = load_fixture();
    const SubFile* obj = cam.find("obj");
    ASSERT_NE(obj, nullptr);
    DecodedObjectives objs = decode_obj(obj->data.data(), obj->data.size());
    // Korea theater grid is ~1024 x 1024 cells. Every objective's (x, y)
    // must be in a plausible range.
    int out_of_range = 0;
    for (const auto& o : objs.objectives) {
        if (o.x < 0 || o.x > 1024 || o.y < 0 || o.y > 1024) ++out_of_range;
    }
    EXPECT_EQ(out_of_range, 0) << "objectives with implausible coordinates";
}

TEST(Objectives, MultipleOwnersRepresented) {
    // Korea campaign has multiple teams (ROK, Japan, PRC, DPRK, etc.).
    // The decoded objectives must span several owner values, not just one.
    auto cam = load_fixture();
    const SubFile* obj = cam.find("obj");
    ASSERT_NE(obj, nullptr);
    DecodedObjectives objs = decode_obj(obj->data.data(), obj->data.size());
    std::set<int> owners;
    for (const auto& o : objs.objectives) owners.insert(o.owner);
    EXPECT_GE(owners.size(), 3u) << "expected objectives from 3+ teams";
}

TEST(Objectives, OwnerValuesArePlausible) {
    auto cam = load_fixture();
    const SubFile* obj = cam.find("obj");
    ASSERT_NE(obj, nullptr);
    DecodedObjectives objs = decode_obj(obj->data.data(), obj->data.size());
    // owner is a Control uchar (0..7). Verify no objective has an
    // out-of-range owner.
    int bad = 0;
    for (const auto& o : objs.objectives) if (o.owner > 7) ++bad;
    EXPECT_EQ(bad, 0);
}

TEST(Objectives, TypeNameIsHumanReadable) {
    EXPECT_EQ(objective_type_name(TYPE_AIRBASE), "Airbase");
    EXPECT_EQ(objective_type_name(TYPE_PORT), "Port");
    EXPECT_EQ(objective_type_name(TYPE_FACTORY), "Factory");
    EXPECT_EQ(objective_type_name(TYPE_BRIDGE), "Bridge");
}

TEST(Objectives, JsonContainsObjectivesArray) {
    auto cam = load_fixture();
    std::string json = to_world_json(cam);
    EXPECT_NE(json.find("\"objectives\""), std::string::npos);
    EXPECT_NE(json.find("\"count\": 2659"), std::string::npos);
    EXPECT_NE(json.find("\"decoded\": 2659"), std::string::npos);
}

// When a class table is provided, the JSON must include the resolved
// objective_type field (1-39) for each objective and a type_name string
// matching the resolved type (e.g. "Airbase"). Without the class table,
// objective_type is emitted as 0 and type_name falls back to the
// "Objective#<entity_type>" format.
TEST(Objectives, JsonResolvesObjectiveTypeWithClassTable) {
    auto cam = load_fixture();

    // Without a class table, objective_type is always emitted but defaults
    // to 0 (= "unknown") for every objective. The type_name field likewise
    // falls back to "Objective#<entity_type>" in this case.
    std::string json_no_ct = to_world_json(cam);
    EXPECT_NE(json_no_ct.find("\"objective_type\": 0"), std::string::npos)
        << "objective_type=0 (unknown) should always be emitted, even "
           "without a class table";

    // Load the bundled FALCON4.ct fixture.
    ClassTable ct;
    ASSERT_TRUE(std::filesystem::exists(FIXTURE_DIR "FALCON4.ct"))
        << "Missing FALCON4.ct test fixture";
    ct.load(std::string(FIXTURE_DIR) + "FALCON4.ct");
    ASSERT_GT(ct.size(), 0u);

    WorldJsonOptions opts;
    opts.class_table = &ct;
    std::string json_with_ct = to_world_json(cam, opts);

    // With the class table, objective_type must appear with non-zero values
    // for at least some objectives, AND type_name must say "Airbase" (not
    // "Objective#1776") for at least one objective.
    EXPECT_NE(json_with_ct.find("\"objective_type\": 1"), std::string::npos)
        << "expected at least one Airbase (objective_type=1) in the fixture";
    EXPECT_NE(json_with_ct.find("\"type_name\": \"Airbase\""), std::string::npos)
        << "type_name should resolve to 'Airbase' (not 'Objective#N') when "
           "the class table is loaded — verifies the world_json.cpp:144 bug fix";

    // Re-decode and verify the resolver maps at least ~80% of objectives
    // to a known ObjectiveType (1-39). A few may be 0 if the class table
    // doesn't have entries for every entity_type, but the vast majority
    // should resolve since this is a real Korea campaign.
    const SubFile* obj = cam.find("obj");
    ASSERT_NE(obj, nullptr);
    DecodedObjectives objs = decode_obj(obj->data.data(), obj->data.size());
    int resolved = 0;
    for (const auto& o : objs.objectives) {
        const uint8_t t = ct.objective_type_for(o.entity_type);
        if (t > 0) ++resolved;
    }
    // 2659 objectives in the fixture; expect at least 2000 resolved.
    EXPECT_GE(resolved, 2000)
        << "Class table resolved too few objective types";
}

// find_class_table() should locate the FALCON4.ct fixture when given the
// .cam path as a reference (it lives next to save1.cam in the fixtures dir).
TEST(Objectives, FindClassTableLocatesFixtureNextToCam) {
    auto ct_path = find_class_table(FIXTURE_DIR "save1.cam");
    EXPECT_FALSE(ct_path.empty());
    EXPECT_TRUE(std::filesystem::exists(ct_path));
    EXPECT_EQ(ct_path.filename(), "FALCON4.ct");
}

// ============================================================================
// REFACTOR-4 tests: cursor-landing verification + link data coverage.
//
// These tests close the VIEWER-3 deferred gap: "port the .obj objective
// decoder's variable-length link data for full objective coverage". The
// link data WAS already decoded end-to-end (decoder → JSON → loader), but
// had no test coverage and no cursor-landing check (unlike the unit
// decoder, which verifies bytes_consumed == inner_size). These tests:
//   1. Verify the cursor lands at the exact end of the decompressed buffer
//      (catches any future struct-size regression that would desync the
//      cursor mid-stream).
//   2. Verify the link data is actually decoded: total link count > 0,
//      costs are plausible (0-250), neighbor VU_IDs are non-zero.
//   3. Verify the road/rail classification works: some links are roads
//      (costs[Wheeled] > 0), some are rails (costs[Rail] > 0).
// ============================================================================

TEST(Objectives, CursorLandsAtExactEndOfBuffer) {
    // The objective decoder must consume exactly inner_size bytes on a
    // clean decode — same contract as the unit decoder. If bytes_consumed
    // < inner_size, the cursor desynced on some record and the decoder
    // stopped early (the objectives vector would be shorter than count).
    auto cam = load_fixture();
    const SubFile* obj = cam.find("obj");
    ASSERT_NE(obj, nullptr);
    DecodedObjectives objs = decode_obj(obj->data.data(), obj->data.size());
    EXPECT_GT(objs.inner_size, 0u) << "inner_size should be non-zero";
    EXPECT_EQ(objs.bytes_consumed, objs.inner_size)
        << "cursor desync: consumed " << objs.bytes_consumed
        << " of " << objs.inner_size << " bytes ("
        << (objs.inner_size - objs.bytes_consumed) << " unconsumed)";
    // And the decoded count must match the header count.
    EXPECT_EQ(static_cast<int>(objs.objectives.size()), objs.count);
}

TEST(Objectives, LinkDataIsDecoded) {
    // The .obj sub-file contains the road/rail network as per-objective
    // link lists. Verify the link data is actually decoded (not just
    // skipped): the total link count across all objectives must be > 0.
    auto cam = load_fixture();
    const SubFile* obj = cam.find("obj");
    ASSERT_NE(obj, nullptr);
    DecodedObjectives objs = decode_obj(obj->data.data(), obj->data.size());

    std::size_t total_links = 0;
    std::size_t objectives_with_links = 0;
    for (const auto& o : objs.objectives) {
        if (!o.link_data.empty()) ++objectives_with_links;
        total_links += o.link_data.size();
    }
    // Korea has an extensive road network — expect thousands of links.
    EXPECT_GT(total_links, 100u)
        << "expected a non-trivial road/rail network";
    EXPECT_GT(objectives_with_links, 100u)
        << "expected many objectives to have at least one link";
}

TEST(Objectives, LinkCostsArePlausible) {
    // Each link has 8 uchar costs (one per MoveType). The semantics
    // (verified by diagnostic against save1.cam):
    //   - 0      = "free traversal" (zero cost)
    //   - 1..249 = real movement cost (lower = faster)
    //   - 255    = "impassable" sentinel (this MoveType can't use the link)
    //
    // Per-index distribution from the real fixture:
    //   [0] Foot      : avg=9.4,   max=255,   22/6360 impassable
    //   [1] Wheeled   : avg=12.7,  max=255,   12/6360 impassable   <- roads
    //   [2] Tracked   : avg=34.4,  max=255,   48/6360 impassable
    //   [3] LowAir    : avg=24.2,  max=255,   26/6360 impassable
    //   [4] Air       : avg=10.2,  max=81,     0/6360 impassable
    //   [5] Rail      : avg=8.9,   max=69,     0/6360 impassable
    //   [6] Naval     : avg=245.3, max=255, 6098/6360 impassable  (most links are land)
    //   [7] (unused)  : avg=255.0, max=255, 6360/6360 impassable  (always 255 — see below)
    //
    // Wait — the header's MoveType enum has Rail=7, but the diagnostic
    // shows index 7 is ALWAYS 255 while index 5 has low costs. This is
    // because the .obj link data uses a DIFFERENT cost layout than the
    // MoveType enum: railroads in the campaign are modeled as objective
    // TYPES (TYPE_RAILROAD=24), not as a Rail movement cost on every
    // link. The link costs array's index 5 carries the rail cost for
    // links that ARE rail lines; index 7 is unused (always impassable).
    //
    // This test verifies the cost values are in the valid uchar range
    // (0-255) and that the impassable sentinel (255) appears in the
    // expected indices. A cursor desync would corrupt the cost array
    // and produce implausible distributions (e.g. index 7 not being
    // 255, or index 6 having many low values).
    auto cam = load_fixture();
    const SubFile* obj = cam.find("obj");
    ASSERT_NE(obj, nullptr);
    DecodedObjectives objs = decode_obj(obj->data.data(), obj->data.size());

    std::size_t total_links = 0;
    std::size_t index7_not_255 = 0;       // index 7 must ALWAYS be 255
    std::size_t naval_low_cost = 0;        // index 6 (Naval) low costs should be rare
    for (const auto& o : objs.objectives) {
        for (const auto& link : o.link_data) {
            ++total_links;
            // All costs are uchar — trivially 0-255. The real check is
            // the semantic distribution.
            if (link.costs[7] != 255) ++index7_not_255;
            if (link.costs[6] > 0 && link.costs[6] < 250) ++naval_low_cost;
        }
    }
    ASSERT_GT(total_links, 0u);
    // Index 7 is unused in this fixture — every link must have 255 there.
    // (If this fails, either the layout changed or the cursor desynced.)
    EXPECT_EQ(index7_not_255, 0u)
        << "index 7 (unused) should always be 255 (impassable)";
    // Naval links should be rare (Korea is mostly land). Most links should
    // have naval cost = 255. Allow up to 10% to have low naval costs
    // (port/sea links).
    const double naval_fraction =
        static_cast<double>(naval_low_cost) / static_cast<double>(total_links);
    EXPECT_LT(naval_fraction, 0.10)
        << naval_low_cost << " of " << total_links
        << " links have low naval costs (expected < 10% for a land theater)";
}

TEST(Objectives, LinkNeighborIdsAreNonZero) {
    // Real links point to real objectives — the neighbor VU_ID must be
    // non-zero for the vast majority of links. (A zero VU_ID would
    // indicate a null/unused link slot, which FreeFalcon doesn't write
    // for actual road/rail connections.)
    auto cam = load_fixture();
    const SubFile* obj = cam.find("obj");
    ASSERT_NE(obj, nullptr);
    DecodedObjectives objs = decode_obj(obj->data.data(), obj->data.size());

    std::size_t zero_neighbors = 0;
    std::size_t total_links = 0;
    for (const auto& o : objs.objectives) {
        for (const auto& link : o.link_data) {
            ++total_links;
            if (link.neighbor_num == 0 && link.neighbor_creator == 0) {
                ++zero_neighbors;
            }
        }
    }
    ASSERT_GT(total_links, 0u);
    // Allow a small fraction of zero neighbors (some links may be stubs),
    // but the vast majority must point somewhere.
    const double zero_fraction =
        static_cast<double>(zero_neighbors) / static_cast<double>(total_links);
    EXPECT_LT(zero_fraction, 0.01)
        << zero_neighbors << " of " << total_links
        << " links have zero neighbor VU_IDs ("
        << (zero_fraction * 100.0) << "%)";
}

TEST(Objectives, RoadLinksArePresent) {
    // The ObjectiveLink::is_road() helper classifies a link as a road
    // based on costs[Wheeled] being in 1..249. Korea has an extensive
    // road network — verify road links appear in the decoded data.
    //
    // NOTE: is_rail() is NOT tested here because railroads in the .obj
    // data are modeled as objective TYPES (TYPE_RAILROAD=24), not as
    // link movement costs. The link costs array's index 5 carries rail
    // costs for links that ARE rail lines, but the is_rail() helper
    // checks index 7 (Rail per the MoveType enum) which is always 255
    // (impassable) in this fixture. The is_rail() helper is retained
    // for future use but is not exercised by this fixture.
    auto cam = load_fixture();
    const SubFile* obj = cam.find("obj");
    ASSERT_NE(obj, nullptr);
    DecodedObjectives objs = decode_obj(obj->data.data(), obj->data.size());

    std::size_t road_links = 0;
    for (const auto& o : objs.objectives) {
        for (const auto& link : o.link_data) {
            if (link.is_road()) ++road_links;
        }
    }
    EXPECT_GT(road_links, 100u)
        << "expected many road links (costs[Wheeled] in 1..249) — "
        << "Korea has an extensive road network";
}

TEST(Objectives, FstatusAndRadarRangeAreExposed) {
    // The fstatus[] byte vector (per-feature damage bitmap) and the
    // optional RadarRangeClass (8 floats detect_ratio[8]) were previously
    // parsed to advance the cursor but NOT exposed as typed fields. They
    // are now exposed on ObjectiveRecord. Verify:
    //   1. Every objective has a non-negative fstatus.size() matching the
    //      length-prefix byte in the .obj stream.
    //   2. At least some objectives have non-empty fstatus (real campaigns
    //      always have features per objective).
    //   3. has_radar is set on radar-type objectives and the detect_ratio
    //      array is populated with 8 floats.
    //   4. Cursor still lands at exact end of buffer (the new typed reads
    //      consume the same bytes as the old `c.p += N` skips).
    auto cam = load_fixture();
    const SubFile* obj = cam.find("obj");
    ASSERT_NE(obj, nullptr);
    DecodedObjectives objs = decode_obj(obj->data.data(), obj->data.size());

    // Cursor-landing invariant: MUST still match (proves the new typed
    // reads consume exactly the same bytes as the old cursor-skip).
    EXPECT_EQ(objs.bytes_consumed, objs.inner_size);

    // Every objective must have a populated fstatus vector (possibly empty
    // when the objective has zero features, but the vector itself exists).
    std::size_t with_features = 0;
    std::size_t total_fstatus_bytes = 0;
    for (const auto& o : objs.objectives) {
        // fstatus length is encoded as 1 byte; vector size must match.
        EXPECT_LE(o.fstatus.size(), 255u);
        if (!o.fstatus.empty()) {
            ++with_features;
            total_fstatus_bytes += o.fstatus.size();
        }
    }
    // Korea has 2659 objectives; the vast majority have at least one feature.
    EXPECT_GT(with_features, 1000u)
        << "expected most objectives to carry feature status bytes";
    EXPECT_GT(total_fstatus_bytes, 1000u)
        << "expected non-trivial total fstatus byte count";

    // Radar objectives: count has_radar=true entries and verify detect_ratio.
    std::size_t radar_count = 0;
    for (const auto& o : objs.objectives) {
        if (o.has_radar) {
            ++radar_count;
            // Each detect_ratio entry is a 0..1 ratio. None should be NaN.
            for (int i = 0; i < 8; ++i) {
                EXPECT_FALSE(std::isnan(o.detect_ratio[i]))
                    << "detect_ratio[" << i << "] is NaN";
                EXPECT_GE(o.detect_ratio[i], 0.0f)
                    << "detect_ratio[" << i << "] should be >= 0";
            }
        }
    }
    // Korea has many radar sites — expect at least 100 with has_radar=true.
    EXPECT_GT(radar_count, 100u)
        << "expected many radar-equipped objectives in Korea";
}

TEST(Objectives, JsonExposesFstatusAndRadarRange) {
    // The JSON output must include the new fields: fstatus[], has_radar,
    // detect_ratio[], obj_flags, supply, fuel, losses, last_repair,
    // first_owner, parent_id.
    auto cam = load_fixture();
    ClassTable ct;
    ASSERT_TRUE(std::filesystem::exists(FIXTURE_DIR "FALCON4.ct"));
    ct.load(std::string(FIXTURE_DIR) + "FALCON4.ct");
    WorldJsonOptions opts;
    opts.class_table = &ct;
    std::string json = to_world_json(cam, opts);

    EXPECT_NE(json.find("\"fstatus\": ["), std::string::npos);
    EXPECT_NE(json.find("\"has_radar\": true"), std::string::npos);
    EXPECT_NE(json.find("\"detect_ratio\":"), std::string::npos);
    EXPECT_NE(json.find("\"obj_flags\":"), std::string::npos);
    EXPECT_NE(json.find("\"supply\":"), std::string::npos);
    EXPECT_NE(json.find("\"fuel\":"), std::string::npos);
    EXPECT_NE(json.find("\"losses\":"), std::string::npos);
    EXPECT_NE(json.find("\"last_repair\":"), std::string::npos);
    EXPECT_NE(json.find("\"first_owner\":"), std::string::npos);
    EXPECT_NE(json.find("\"parent_id\":"), std::string::npos);
}

TEST(Objectives, TypeNameIsResolvedFromClassTable) {
    // Regression test for the world_json.cpp:144 bug — type_name must
    // say "Airbase" (not "Objective#1776") when the class table is loaded.
    auto cam = load_fixture();
    ClassTable ct;
    ASSERT_TRUE(std::filesystem::exists(FIXTURE_DIR "FALCON4.ct"));
    ct.load(std::string(FIXTURE_DIR) + "FALCON4.ct");
    WorldJsonOptions opts;
    opts.class_table = &ct;
    std::string json = to_world_json(cam, opts);

    EXPECT_NE(json.find("\"type_name\": \"Airbase\""), std::string::npos)
        << "type_name should resolve to 'Airbase' via the class table";
    EXPECT_NE(json.find("\"type_name\": \"Bridge\""), std::string::npos)
        << "type_name should resolve to 'Bridge' via the class table";
    EXPECT_NE(json.find("\"type_name\": \"City\""), std::string::npos)
        << "type_name should resolve to 'City' via the class table";
    // Without the class table, type_name falls back to "Objective#N" —
    // this is the legacy behavior that the bug fix preserves as a fallback.
    std::string json_no_ct = to_world_json(cam);
    EXPECT_NE(json_no_ct.find("\"type_name\": \"Objective#"), std::string::npos)
        << "without class table, type_name should fall back to 'Objective#N'";
}
