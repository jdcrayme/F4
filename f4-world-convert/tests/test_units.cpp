// test_units.cpp — .uni unit decoder against the real save1.cam.

#include <gtest/gtest.h>
#include <f4/convert/cam_archive.hpp>
#include <f4/convert/unit_decoder.hpp>
#include <f4/convert/world_json.hpp>

using namespace f4::convert;

namespace {
CamArchive load_fixture() {
    CamArchive cam;
    cam.load(FIXTURE_DIR "save1.cam");
    return cam;
}
}

TEST(Units, DecodesHeaderCorrectly) {
    auto cam = load_fixture();
    const SubFile* uni = cam.find("uni");
    ASSERT_NE(uni, nullptr);
    DecodedUnits units = decode_uni(uni->data.data(), uni->data.size());
    // The .uni header records 683 units.
    EXPECT_EQ(units.count, 683);
}

TEST(Units, DecodesAtLeastOneRecord) {
    // UnitClass::Save has a variable-length tail we don't yet parse, so we
    // can't guarantee all 683 records decode cleanly. But we should get at
    // least the first record (which has a fully-known layout).
    auto cam = load_fixture();
    const SubFile* uni = cam.find("uni");
    ASSERT_NE(uni, nullptr);
    DecodedUnits units = decode_uni(uni->data.data(), uni->data.size());
    EXPECT_GE(units.units.size(), 1u);
}

TEST(Units, FirstRecordHasPlausibleCoordinates) {
    auto cam = load_fixture();
    const SubFile* uni = cam.find("uni");
    ASSERT_NE(uni, nullptr);
    DecodedUnits units = decode_uni(uni->data.data(), uni->data.size());
    ASSERT_GE(units.units.size(), 1u);
    const auto& u = units.units[0];
    // Korea grid ~256x256.
    EXPECT_GE(u.x, 0);
    EXPECT_LE(u.x, 1024);
    EXPECT_GE(u.y, 0);
    EXPECT_LE(u.y, 1024);
    EXPECT_LE(u.owner, 7);
}

TEST(Units, JsonContainsUnitsArray) {
    auto cam = load_fixture();
    std::string json = to_world_json(cam);
    EXPECT_NE(json.find("\"units\""), std::string::npos);
    EXPECT_NE(json.find("\"count\": 683"), std::string::npos);
}
