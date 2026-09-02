// f4-campaign/tests/test_mission_type.cpp
//
// The MissionType wire table: byte ↔ name round-trips, the FreeFalcon
// ordering anchors, range safety, and the tasked/untasked rule.

#include <f4/campaign/mission_type.hpp>

#include <gtest/gtest.h>

using namespace f4::campaign;

TEST(MissionType, TableHas41Entries) {
    EXPECT_EQ(kMissionTypeCount, std::size_t{41});
}

TEST(MissionType, ZeroIsNone) {
    EXPECT_EQ(mission_type_name(0), "AMIS_NONE");
    EXPECT_EQ(mission_type_byte("AMIS_NONE"), std::optional<std::uint8_t>(0));
    EXPECT_FALSE(is_mission_tasked(0));
}

TEST(MissionType, FreeFalconOrderingAnchors) {
    // The ordering the campaign wire format depends on — BARCAP first,
    // the doc-corroborated mid-table names, PATROL at 36.
    EXPECT_EQ(mission_type_name(1), "AMIS_BARCAP");
    EXPECT_EQ(mission_type_name(9), "AMIS_INTERCEPT");
    EXPECT_EQ(mission_type_name(12), "AMIS_OCASTRIKE");
    EXPECT_EQ(mission_type_name(17), "AMIS_SEADSTRIKE");
    EXPECT_EQ(mission_type_name(25), "AMIS_AWACS");
    EXPECT_EQ(mission_type_name(27), "AMIS_TANKER");
    EXPECT_EQ(mission_type_name(35), "AMIS_ASHIP");
    EXPECT_EQ(mission_type_name(36), "AMIS_PATROL");
    // The utility tail.
    EXPECT_EQ(mission_type_name(37), "AMIS_TRAINING");
    EXPECT_EQ(mission_type_name(38), "AMIS_OTHER");
    EXPECT_EQ(mission_type_name(39), "AMIS_TANK");
    EXPECT_EQ(mission_type_name(40), "AMIS_SEARCH");
}

TEST(MissionType, KunsanFixtureByteIsBarcap) {
    // The real-save kunsan fixture's flights carry mission byte 1 —
    // FreeFalcon's BARCAP. (The fixture was authored from save1.cam.)
    EXPECT_EQ(mission_type_name(1), "AMIS_BARCAP");
}

TEST(MissionType, ByteNameRoundTrip) {
    for (std::uint8_t b = 0; b < kMissionTypeCount; ++b) {
        const auto name = mission_type_name(b);
        ASSERT_EQ(mission_type_byte(name), std::optional<std::uint8_t>(b))
            << "byte " << int{b};
    }
}

TEST(MissionType, AllNamesDistinct) {
    for (std::size_t i = 0; i < kMissionTypeCount; ++i) {
        for (std::size_t j = i + 1; j < kMissionTypeCount; ++j) {
            ASSERT_NE(kMissionTypeNames[i], kMissionTypeNames[j])
                << "bytes " << i << " and " << j;
        }
    }
}

TEST(MissionType, OutOfRangeByteRendersSafely) {
    // Corrupt campaign data must render "AMIS_?" instead of UB.
    EXPECT_EQ(mission_type_name(41), "AMIS_?");
    EXPECT_EQ(mission_type_name(200), "AMIS_?");
    EXPECT_EQ(mission_type_byte("AMIS_?"), std::nullopt);
}

TEST(MissionType, UnknownNameIsNullopt) {
    EXPECT_EQ(mission_type_byte(""), std::nullopt);
    EXPECT_EQ(mission_type_byte("AMIS_BARCAPX"), std::nullopt);
    EXPECT_EQ(mission_type_byte("barcap"), std::nullopt); // case-sensitive
    EXPECT_EQ(mission_type_byte("MISSION_BARCAP"), std::nullopt);
}

TEST(MissionType, TaskedRule) {
    EXPECT_FALSE(is_mission_tasked(0));
    for (std::uint8_t b = 1; b < kMissionTypeCount; ++b) {
        EXPECT_TRUE(is_mission_tasked(b)) << "byte " << int{b};
    }
}
