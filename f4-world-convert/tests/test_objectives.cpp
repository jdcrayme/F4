// test_objectives.cpp — .obj objective decoder against the real save1.cam.

#include <gtest/gtest.h>
#include <f4/world_convert/cam_archive.hpp>
#include <f4/world_convert/objective_decoder.hpp>
#include <f4/world_convert/world_json.hpp>
#include <f4/world_convert/class_table.hpp>

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
// objective_type field (1-39) for each objective. Without the class table,
// the field is omitted entirely — which is what causes the viewer to fall
// back to drawing every objective as a generic colored circle.
TEST(Objectives, JsonResolvesObjectiveTypeWithClassTable) {
    auto cam = load_fixture();

    // Sanity: without a class table, objective_type must NOT appear.
    std::string json_no_ct = to_world_json(cam);
    EXPECT_EQ(json_no_ct.find("\"objective_type\""), std::string::npos);

    // Load the bundled FALCON4.ct fixture.
    ClassTable ct;
    ASSERT_TRUE(std::filesystem::exists(FIXTURE_DIR "FALCON4.ct"))
        << "Missing FALCON4.ct test fixture";
    ct.load(std::string(FIXTURE_DIR) + "FALCON4.ct");
    ASSERT_GT(ct.size(), 0u);

    WorldJsonOptions opts;
    opts.class_table = &ct;
    std::string json_with_ct = to_world_json(cam, opts);

    // With the class table, objective_type must appear and at least one
    // objective must resolve to a non-zero type (i.e. icon-mappable).
    EXPECT_NE(json_with_ct.find("\"objective_type\""), std::string::npos);

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
