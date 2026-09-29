// test_weather_codec.cpp — the .wth codec (WTH-CODEC-1).
//
// Contract:
//   * the two committed fixtures decode to golden values verified
//     against the upstream reader (campupd/weather.cpp:474 CampLoad);
//   * encode_wth(decode_wth(bytes)) == bytes, BYTE-FOR-BYTE, for every
//     layout the repo can produce (the fixtures' Legacy 37+map form,
//     a synthetic TaceditCompat 37-byte form, and both CobraFlat
//     sizes);
//   * campinit's 0-byte .wth decodes to nullopt (absent weather);
//   * garbage inputs throw (never silently decode).

#include <gtest/gtest.h>

#include <f4/world_convert/cam_archive.hpp>
#include <f4/world_convert/weather_decoder.hpp>
#include <f4/world_convert/weather_encoder.hpp>

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

using namespace f4::world_convert;

namespace {

// Load the .wth sub-file bytes from a .cam fixture. Skips the test when
// the archive is absent (shallow-export tolerance, the TestCamp.cam
// convention).
std::vector<uint8_t> load_wth(const char* path) {
    CamArchive cam;
    if (!std::filesystem::exists(std::filesystem::path(path))) return {};
    cam.load(path);
    const SubFile* sf = cam.find("wth");
    if (!sf) return {};
    return sf->data;
}

// Synthetic TaceditCompat buffer — FreeFalcon's v<75 Save form:
// stratus2Z at @21, COVersion marker at @25, no map.
std::vector<uint8_t> make_tacedit_buffer() {
    std::vector<uint8_t> b(37, 0);
    const float wind_heading = 1.25f;       // @0
    const float cumulus_ft = 8000.0f;       // @4
    const int32_t last_check = 7200000;     // @8
    const float stratus_ft = 22000.0f;      // @12
    const uint8_t temp_c = 21;              // @16
    const uint8_t wind_kts = 12;            // @17
    const uint8_t condition = 2;            // @18 (2 = fair)
    const uint8_t con_base = 30;            // @19
    const uint8_t overcast = 6;             // @20
    const float stratus2_ft = 35000.0f;     // @21 (FreeFalcon Save slot)
    const float co_version = 0.0077f;       // @25 (the marker)
    std::memcpy(b.data() + 0, &wind_heading, 4);
    std::memcpy(b.data() + 4, &cumulus_ft, 4);
    std::memcpy(b.data() + 8, &last_check, 4);
    std::memcpy(b.data() + 12, &stratus_ft, 4);
    b[16] = temp_c;
    b[17] = wind_kts;
    b[18] = condition;
    b[19] = con_base;
    b[20] = overcast;
    std::memcpy(b.data() + 21, &stratus2_ft, 4);
    std::memcpy(b.data() + 25, &co_version, 4);
    return b;
}

// Synthetic CobraFlat36 buffer — the v76+ Save form.
std::vector<uint8_t> make_cobra36_buffer() {
    std::vector<uint8_t> b(36, 0);
    const int32_t condition = 3;         // poor
    const int32_t last_check = 43200000; // 12:00
    const float temp = 15.5f;
    const float wind_speed = 14.2f;
    const float wind_heading = 2.1f;
    const float cumulus_z = -9000.0f;
    const float stratus_z = -22000.0f;
    const float contrail_low = 30000.0f;
    const float contrail_high = 95000.0f;
    std::memcpy(b.data() + 0, &condition, 4);
    std::memcpy(b.data() + 4, &last_check, 4);
    std::memcpy(b.data() + 8, &temp, 4);
    std::memcpy(b.data() + 12, &wind_speed, 4);
    std::memcpy(b.data() + 16, &wind_heading, 4);
    std::memcpy(b.data() + 20, &cumulus_z, 4);
    std::memcpy(b.data() + 24, &stratus_z, 4);
    std::memcpy(b.data() + 28, &contrail_low, 4);
    std::memcpy(b.data() + 32, &contrail_high, 4);
    return b;
}

} // namespace

// --- The committed fixtures -------------------------------------------------

TEST(WthCodec, Save1LegacyDecodesToGoldenValues) {
    const auto wth = load_wth(FIXTURE_DIR "save1.cam");
    ASSERT_FALSE(wth.empty());   // the fixture must carry a .wth
    auto w = decode_wth(wth.data(), wth.size(), 63);
    ASSERT_TRUE(w.has_value());
    EXPECT_EQ(w->layout, WthLayout::Legacy);
    EXPECT_FLOAT_EQ(w->wind_heading, 0.0f);
    EXPECT_FLOAT_EQ(w->wind_speed, 10.0f);          // stored KPH
    EXPECT_EQ(w->last_check, 32400000);             // 09:00
    EXPECT_FLOAT_EQ(w->temperature, 20.0f);
    EXPECT_EQ(w->todays_temp, 23);
    EXPECT_EQ(w->todays_wind, 0);
    EXPECT_EQ(w->cloud_base, 0);
    EXPECT_EQ(w->con_layer_start, 30);
    EXPECT_EQ(w->con_layer_end, 36);
    EXPECT_FLOAT_EQ(w->x_off, 0.0f);
    EXPECT_FLOAT_EQ(w->y_off, 0.0f);
    EXPECT_EQ(w->map_w, 128u);
    EXPECT_EQ(w->map_h, 128u);
    ASSERT_EQ(w->map_raw.size(), 128u * 128u * 2u);
    // The pristine 09:00 map: every cell {0xFF level sentinel, 0 cover}.
    for (std::size_t i = 0; i < w->map_raw.size(); i += 2) {
        ASSERT_EQ(w->map_raw[i], 0xFF) << "cell " << i / 2;
        ASSERT_EQ(w->map_raw[i + 1], 0x00) << "cell " << i / 2;
    }
    EXPECT_TRUE(w->trailing_raw.empty());
}

TEST(WthCodec, TestCampLegacyDecodesToGoldenValues) {
    const auto wth = load_wth(REPO_ROOT "TestCamp.cam");
    ASSERT_FALSE(wth.empty());
    auto w = decode_wth(wth.data(), wth.size(), 71);
    ASSERT_TRUE(w.has_value());
    EXPECT_EQ(w->layout, WthLayout::Legacy);
    EXPECT_FLOAT_EQ(w->wind_heading, 1.1252262592315674f);
    EXPECT_FLOAT_EQ(w->wind_speed, 18.652753829956055f);
    EXPECT_EQ(w->last_check, 38574360);
    EXPECT_FLOAT_EQ(w->temperature, 20.656557083129883f);
    EXPECT_EQ(w->todays_temp, 24);
    EXPECT_EQ(w->todays_wind, 22);
    EXPECT_EQ(w->cloud_base, 34);
    EXPECT_EQ(w->con_layer_start, 30);
    EXPECT_EQ(w->con_layer_end, 36);
    EXPECT_FLOAT_EQ(w->x_off, 1.9876466989517212f);
    EXPECT_FLOAT_EQ(w->y_off, 7.558435440063477f);
    EXPECT_EQ(w->map_w, 128u);
    EXPECT_EQ(w->map_h, 128u);
    ASSERT_EQ(w->map_raw.size(), 128u * 128u * 2u);
    // Real cloud map: levels 46..179, covers 0..8 (verified against the
    // committed archive — 672 distinct cells).
    uint8_t level_min = 255, level_max = 0, cover_min = 255, cover_max = 0;
    for (std::size_t i = 0; i < w->map_raw.size(); i += 2) {
        level_min = std::min(level_min, w->map_raw[i]);
        level_max = std::max(level_max, w->map_raw[i]);
        cover_min = std::min(cover_min, w->map_raw[i + 1]);
        cover_max = std::max(cover_max, w->map_raw[i + 1]);
    }
    EXPECT_EQ(level_min, 46);
    EXPECT_EQ(level_max, 179);
    EXPECT_EQ(cover_min, 0);
    EXPECT_EQ(cover_max, 8);
}

// --- Byte-identity closure --------------------------------------------------

TEST(WthCodec, Save1RoundTripIsByteIdentical) {
    const auto wth = load_wth(FIXTURE_DIR "save1.cam");
    ASSERT_FALSE(wth.empty());
    auto w = decode_wth(wth.data(), wth.size(), 63);
    ASSERT_TRUE(w.has_value());
    const auto bytes = encode_wth(*w);
    ASSERT_EQ(bytes.size(), wth.size());
    EXPECT_EQ(std::memcmp(bytes.data(), wth.data(), wth.size()), 0);
}

TEST(WthCodec, TestCampRoundTripIsByteIdentical) {
    const auto wth = load_wth(REPO_ROOT "TestCamp.cam");
    ASSERT_FALSE(wth.empty());
    auto w = decode_wth(wth.data(), wth.size(), 71);
    ASSERT_TRUE(w.has_value());
    const auto bytes = encode_wth(*w);
    ASSERT_EQ(bytes.size(), wth.size());
    EXPECT_EQ(std::memcmp(bytes.data(), wth.data(), wth.size()), 0);
}

TEST(WthCodec, TaceditCompatRoundTripIsByteIdentical) {
    const auto buf = make_tacedit_buffer();
    auto w = decode_wth(buf.data(), buf.size(), 71);
    ASSERT_TRUE(w.has_value());
    EXPECT_EQ(w->layout, WthLayout::TaceditCompat);
    // The reinterpretation overlay reads the slots' semantics.
    EXPECT_FLOAT_EQ(w->tacedit_cumulus_base_ft, 8000.0f);
    EXPECT_FLOAT_EQ(w->tacedit_stratus_base_ft, 22000.0f);
    EXPECT_FLOAT_EQ(w->tacedit_stratus2_base_ft, 35000.0f);
    EXPECT_EQ(w->cloud_base, 2);   // the condition byte
    const auto bytes = encode_wth(*w);
    ASSERT_EQ(bytes.size(), buf.size());
    EXPECT_EQ(std::memcmp(bytes.data(), buf.data(), buf.size()), 0);
}

TEST(WthCodec, TaceditSwappedMarkerRoundTripIsByteIdentical) {
    // The loader accepts the marker at either slot ("Tacedit reverses
    // XOff and YOff when TE is saved"). Byte-identity must hold for the
    // swapped form too — the @21/@25 capture re-emits to the same slots.
    auto buf = make_tacedit_buffer();
    const float stratus2 = 35000.0f;
    const float co_version = 0.0077f;
    std::memcpy(buf.data() + 21, &co_version, 4);   // marker at 21 now
    std::memcpy(buf.data() + 25, &stratus2, 4);
    auto w = decode_wth(buf.data(), buf.size(), 71);
    ASSERT_TRUE(w.has_value());
    EXPECT_EQ(w->layout, WthLayout::TaceditCompat);
    EXPECT_FLOAT_EQ(w->tacedit_stratus2_base_ft, 35000.0f);
    const auto bytes = encode_wth(*w);
    ASSERT_EQ(bytes.size(), buf.size());
    EXPECT_EQ(std::memcmp(bytes.data(), buf.data(), buf.size()), 0);
}

TEST(WthCodec, CobraFlat36RoundTripIsByteIdentical) {
    const auto buf = make_cobra36_buffer();
    auto w = decode_wth(buf.data(), buf.size(), 76);
    ASSERT_TRUE(w.has_value());
    EXPECT_EQ(w->layout, WthLayout::CobraFlat36);
    EXPECT_EQ(w->weather_condition, 3);
    EXPECT_EQ(w->last_check, 43200000);
    EXPECT_FLOAT_EQ(w->temperature, 15.5f);
    EXPECT_FLOAT_EQ(w->wind_speed_flat, 14.2f);
    EXPECT_FLOAT_EQ(w->wind_heading_flat, 2.1f);
    EXPECT_FLOAT_EQ(w->cumulus_z, -9000.0f);
    EXPECT_FLOAT_EQ(w->stratus_z, -22000.0f);
    const auto bytes = encode_wth(*w);
    ASSERT_EQ(bytes.size(), buf.size());
    EXPECT_EQ(std::memcmp(bytes.data(), buf.data(), buf.size()), 0);
}

TEST(WthCodec, CobraFlat32RoundTripIsByteIdentical) {
    // The v75 Save form: 8 fields, no cumulusZ.
    auto buf = make_cobra36_buffer();
    buf.resize(32);   // drop cumulusZ+stratusZ+contrailLow+contrailHigh…
    // …then re-fill the last 4 fields so the 32-byte form is coherent.
    const float stratus_z = -22000.0f;
    const float contrail_low = 30000.0f;
    const float contrail_high = 95000.0f;
    std::memcpy(buf.data() + 20, &stratus_z, 4);
    std::memcpy(buf.data() + 24, &contrail_low, 4);
    std::memcpy(buf.data() + 28, &contrail_high, 4);
    auto w = decode_wth(buf.data(), buf.size(), 75);
    ASSERT_TRUE(w.has_value());
    EXPECT_EQ(w->layout, WthLayout::CobraFlat32);
    EXPECT_FLOAT_EQ(w->stratus_z, -22000.0f);
    const auto bytes = encode_wth(*w);
    ASSERT_EQ(bytes.size(), buf.size());
    EXPECT_EQ(std::memcmp(bytes.data(), buf.data(), buf.size()), 0);
}

// --- The campinit empty ride + garbage rejection ----------------------------

TEST(WthCodec, EmptyWthIsAbsentWeather) {
    // campaign_initializer writes stem + ".wth" EMPTY for campinit wars
    // (pinned there by CampaignInitializer.EmptyWeatherRide). The codec
    // reads that as absent — never as a decode failure.
    auto w = decode_wth(nullptr, 0, 71);
    EXPECT_FALSE(w.has_value());
    const uint8_t dummy = 0;
    w = decode_wth(&dummy, 0, 71);
    EXPECT_FALSE(w.has_value());
}

TEST(WthCodec, GarbageSizesThrow) {
    const std::vector<uint8_t> five(5, 0xAB);
    EXPECT_THROW(decode_wth(five.data(), five.size(), 71), std::runtime_error);
    // 37 bytes of zeros: legacy dims are 0x0 — size >= header means the
    // header parses with an EMPTY map (0 cells), a legal-but-degenerate
    // form; the byte-identity closure still holds on it.
    const std::vector<uint8_t> zeros(37, 0);
    auto w = decode_wth(zeros.data(), zeros.size(), 71);
    ASSERT_TRUE(w.has_value());
    EXPECT_EQ(w->layout, WthLayout::Legacy);
    EXPECT_EQ(w->map_w, 0u);
    EXPECT_EQ(w->map_h, 0u);
    EXPECT_TRUE(w->map_raw.empty());
    const auto bytes = encode_wth(*w);
    EXPECT_EQ(bytes, zeros);
}

TEST(WthCodec, TruncatedMapThrows) {
    // Claim 128x128 dims but carry only 10 bytes of map.
    std::vector<uint8_t> b(37, 0);
    const uint32_t w = 128, h = 128;
    std::memcpy(b.data() + 29, &w, 4);
    std::memcpy(b.data() + 33, &h, 4);
    b.resize(37 + 10);
    EXPECT_THROW(decode_wth(b.data(), b.size(), 71), std::runtime_error);
}
