// f4-assets/tests/test_asset_id.cpp

#include <f4/assets/asset_id.hpp>

#include <gtest/gtest.h>

#include <stdexcept>
#include <unordered_set>

using namespace f4::assets;

TEST(AssetId, ParsesKoreaobjZeroPadded) {
    AssetId id = parse_asset_id("koreaobj:00042");
    EXPECT_EQ(id.family, AssetFamily::koreaobj);
    EXPECT_EQ(id.local_id, "00042");
    EXPECT_TRUE(id.valid());
    EXPECT_EQ(id.to_string(), "koreaobj:00042");
}

TEST(AssetId, ParsesAllRegisteredFamilies) {
    EXPECT_EQ(parse_asset_id("class:171").family,        AssetFamily::class_);
    EXPECT_EQ(parse_asset_id("theater:korea").family,    AssetFamily::theater);
    EXPECT_EQ(parse_asset_id("campaign:save1").family,  AssetFamily::campaign);
    EXPECT_EQ(parse_asset_id("aircraft:f16").family,    AssetFamily::aircraft);
    EXPECT_EQ(parse_asset_id("tileset:korea.near").family, AssetFamily::tileset);
}

TEST(AssetId, RejectsMissingColon) {
    EXPECT_THROW(parse_asset_id("koreaobj"), std::invalid_argument);
}

TEST(AssetId, RejectsUppercase) {
    EXPECT_THROW(parse_asset_id("Koreaobj:00042"), std::invalid_argument);
    EXPECT_THROW(parse_asset_id("koreaobj:00042A"), std::invalid_argument);
}

TEST(AssetId, RejectsSpaces) {
    EXPECT_THROW(parse_asset_id("koreaobj: 0042"), std::invalid_argument);
    EXPECT_THROW(parse_asset_id(" koreaobj:0042"), std::invalid_argument);
}

TEST(AssetId, RejectsEmptyLocalId) {
    EXPECT_THROW(parse_asset_id("koreaobj:"), std::invalid_argument);
}

TEST(AssetId, AcceptsDotDashUnderscore) {
    EXPECT_NO_THROW(parse_asset_id("tileset:korea.near"));
    EXPECT_NO_THROW(parse_asset_id("tileset:korea-near"));
    EXPECT_NO_THROW(parse_asset_id("tileset:korea_near_v2"));
}

TEST(AssetId, UnknownFamilyRejectedByStrictParser) {
    EXPECT_THROW(parse_asset_id("bmsobj:00042"), std::invalid_argument);
}

TEST(AssetId, UnknownFamilyReturnsInvalidInLenientParser) {
    AssetId id = parse_asset_id_or_invalid("bmsobj:00042");
    EXPECT_FALSE(id.valid());
}

TEST(AssetId, FamilyStringRoundTrip) {
    for (AssetFamily f : {AssetFamily::koreaobj, AssetFamily::class_,
                         AssetFamily::theater, AssetFamily::campaign,
                         AssetFamily::aircraft, AssetFamily::tileset}) {
        std::string s(family_to_string(f));
        EXPECT_EQ(family_from_string(s), f)
            << "round-trip failed for family " << s;
    }
}

TEST(AssetId, ComparisonAndHashing) {
    AssetId a{AssetFamily::koreaobj, "00042"};
    AssetId b{AssetFamily::koreaobj, "00042"};
    AssetId c{AssetFamily::koreaobj, "00043"};
    AssetId d{AssetFamily::class_, "00042"};
    EXPECT_EQ(a, b);
    EXPECT_NE(a, c);
    EXPECT_NE(a, d);
    EXPECT_LT(a, c) << "local-id ordering preserved";
    EXPECT_LT(a, d) << "koreaobj (family=0) < class_ (family=1) in enum order";

    std::unordered_set<AssetId> set{a, b, c, d};
    EXPECT_EQ(set.size(), 3u);  // a==b dedup
}

TEST(AssetId, AssetRefPrefix) {
    EXPECT_TRUE(is_asset_ref("@asset:koreaobj:00042"));
    EXPECT_FALSE(is_asset_ref("koreaobj:00042"));
    EXPECT_FALSE(is_asset_ref(""));
    EXPECT_TRUE(is_asset_ref("@asset:"));
    EXPECT_THROW(parse_asset_ref("@asset:"), std::invalid_argument);
}

TEST(AssetId, ParseAndEmitAssetRef) {
    AssetId id = parse_asset_ref("@asset:theater:korea");
    EXPECT_EQ(id.family, AssetFamily::theater);
    EXPECT_EQ(id.local_id, "korea");
    EXPECT_EQ(to_asset_ref(id), "@asset:theater:korea");
}

TEST(AssetId, ParseAssetRefRejectsMissingPrefix) {
    EXPECT_THROW(parse_asset_ref("theater:korea"), std::invalid_argument);
}

TEST(AssetId, IsValidLocalId) {
    EXPECT_TRUE(is_valid_local_id("00042"));
    EXPECT_TRUE(is_valid_local_id("korea.near"));
    EXPECT_TRUE(is_valid_local_id("save1"));
    EXPECT_FALSE(is_valid_local_id(""));
    EXPECT_FALSE(is_valid_local_id("UPPER"));
    EXPECT_FALSE(is_valid_local_id("has space"));
    EXPECT_FALSE(is_valid_local_id("with/slash"));
}
