// test_objectives.cpp — .obj objective decoder against the real save1.cam.

#include <gtest/gtest.h>
#include <f4/convert/cam_archive.hpp>
#include <f4/convert/objective_decoder.hpp>
#include <f4/convert/world_json.hpp>

#include <set>

using namespace f4::convert;

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
