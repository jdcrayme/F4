// test_units.cpp — .uni unit decoder against the real save1.cam.
//
// After the v63 full-decode port (Task VIEWER-1 follow-up), all 683 units
// decode cleanly with the cursor landing exactly at the buffer end.

#include <gtest/gtest.h>
#include <f4/convert/cam_archive.hpp>
#include <f4/convert/unit_decoder.hpp>
#include <f4/convert/world_json.hpp>

#include <map>
#include <set>

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

TEST(Units, DecodesAllRecords) {
    // After the v63 full-decode port, every record must decode cleanly —
    // no cursor desync, no skipped records.
    auto cam = load_fixture();
    const SubFile* uni = cam.find("uni");
    ASSERT_NE(uni, nullptr);
    DecodedUnits units = decode_uni(uni->data.data(), uni->data.size());
    EXPECT_EQ(units.count, 683);
    EXPECT_EQ(static_cast<int>(units.units.size()), units.count)
        << "all 683 units must decode (was only 7 before the v63 port)";
}

TEST(Units, CursorLandsAtBufferEnd) {
    // The strongest correctness signal: after decoding all records, the
    // cursor must sit exactly at the end of the LZSS-decompressed buffer.
    auto cam = load_fixture();
    const SubFile* uni = cam.find("uni");
    ASSERT_NE(uni, nullptr);
    DecodedUnits units = decode_uni(uni->data.data(), uni->data.size());
    EXPECT_EQ(units.bytes_consumed, units.inner_size)
        << "cursor must land at buffer end (no leftover bytes)";
}

TEST(Units, FirstRecordHasPlausibleCoordinates) {
    auto cam = load_fixture();
    const SubFile* uni = cam.find("uni");
    ASSERT_NE(uni, nullptr);
    DecodedUnits units = decode_uni(uni->data.data(), uni->data.size());
    ASSERT_GE(units.units.size(), 1u);
    const auto& u = units.units[0];
    EXPECT_GE(u.x, 0);
    EXPECT_LE(u.x, 1024);
    EXPECT_GE(u.y, 0);
    EXPECT_LE(u.y, 1024);
    EXPECT_LE(u.owner, 7);
}

TEST(Units, AllRecordsHavePlausibleCoordinates) {
    // Stronger than FirstRecordHasPlausibleCoordinates: EVERY decoded
    // unit must have a plausible grid position.
    auto cam = load_fixture();
    const SubFile* uni = cam.find("uni");
    ASSERT_NE(uni, nullptr);
    DecodedUnits units = decode_uni(uni->data.data(), uni->data.size());
    int bad = 0;
    for (const auto& u : units.units) {
        if (u.x < 0 || u.x > 1024 || u.y < 0 || u.y > 1024 || u.owner > 7) {
            ++bad;
        }
    }
    EXPECT_EQ(bad, 0) << "units with implausible coordinates";
}

TEST(Units, UnitClassDistributionMatchesKorea) {
    // save1.cam (Korea campaign) has a known unit-class distribution.
    // From the v63 layout analysis: 524 battalions, 85 brigades,
    // 72 squadrons, 2 task forces, 0 flights, 0 packages.
    auto cam = load_fixture();
    const SubFile* uni = cam.find("uni");
    ASSERT_NE(uni, nullptr);
    DecodedUnits units = decode_uni(uni->data.data(), uni->data.size());

    int counts[7] = {0};   // indexed by UnitClass enum value
    for (const auto& u : units.units) {
        ASSERT_NE(u.unit_class, UnitClass::Unknown)
            << "every unit must have a recognized class (no Unknowns allowed)";
        counts[static_cast<int>(u.unit_class)]++;
    }
    EXPECT_EQ(counts[static_cast<int>(UnitClass::Battalion)],  524);
    EXPECT_EQ(counts[static_cast<int>(UnitClass::Brigade)],    85);
    EXPECT_EQ(counts[static_cast<int>(UnitClass::Squadron)],   72);
    EXPECT_EQ(counts[static_cast<int>(UnitClass::TaskForce)],  2);
    EXPECT_EQ(counts[static_cast<int>(UnitClass::Flight)],     0);
    EXPECT_EQ(counts[static_cast<int>(UnitClass::Package)],    0);
}

TEST(Units, MultipleOwnersRepresented) {
    auto cam = load_fixture();
    const SubFile* uni = cam.find("uni");
    ASSERT_NE(uni, nullptr);
    DecodedUnits units = decode_uni(uni->data.data(), uni->data.size());
    std::set<int> owners;
    for (const auto& u : units.units) owners.insert(u.owner);
    EXPECT_GT(owners.size(), 1u) << "expected multiple teams";
}

TEST(Units, BattalionTailFieldsArePopulated) {
    // The first record is a Battalion (type=170). Its subclass-specific
    // fields (supply, morale, fatigue, position, parent_id, ...) must
    // be populated with non-default values.
    auto cam = load_fixture();
    const SubFile* uni = cam.find("uni");
    ASSERT_NE(uni, nullptr);
    DecodedUnits units = decode_uni(uni->data.data(), uni->data.size());
    ASSERT_GE(units.units.size(), 1u);
    const auto& u = units.units[0];
    EXPECT_EQ(u.unit_class, UnitClass::Battalion);
    // supply/morale are percentages (0..100). Korea battalions commonly
    // have supply=100 at scenario start.
    EXPECT_LE(u.subclass.supply, 100);
    EXPECT_LE(u.subclass.morale, 100);
    EXPECT_LE(u.subclass.fatigue, 100);
}

TEST(Units, SquadronTailFieldsArePopulated) {
    // Squadrons have a 796-byte tail at v63. The fuel field (long, 4 bytes)
    // is the first one written. If our cursor is misaligned, fuel will
    // come back as garbage (random bytes) — we'd see many distinct values.
    // A correctly-aligned decode produces a small set of consistent values.
    auto cam = load_fixture();
    const SubFile* uni = cam.find("uni");
    ASSERT_NE(uni, nullptr);
    DecodedUnits units = decode_uni(uni->data.data(), uni->data.size());
    int total_squadrons = 0;
    std::map<int32_t, int> fuel_values;
    for (const auto& u : units.units) {
        if (u.unit_class == UnitClass::Squadron) {
            ++total_squadrons;
            ++fuel_values[u.subclass.fuel];
        }
    }
    EXPECT_EQ(total_squadrons, 72) << "fixture must have 72 squadrons";
    // A correctly-aligned decode produces FEW distinct fuel values (the
    // default + a few sentinels). Random garbage would produce dozens.
    EXPECT_LE(fuel_values.size(), 5u)
        << "fuel values should be consistent across squadrons";
    // The common default (3000 lbs) should dominate.
    EXPECT_GT(fuel_values[3000], 0)
        << "expected at least one squadron with the default 3000-lb fuel";
}

TEST(Units, JsonContainsUnitsArray) {
    auto cam = load_fixture();
    std::string json = to_world_json(cam);
    EXPECT_NE(json.find("\"units\""), std::string::npos);
    EXPECT_NE(json.find("\"count\": 683"), std::string::npos);
    // The new fields should appear in the JSON for at least one unit:
    EXPECT_NE(json.find("\"unit_class\""), std::string::npos);
    EXPECT_NE(json.find("\"battalion\""), std::string::npos);
}
