// f4-world-convert/tests/test_theater_data.cpp
//
// Tests for the Falcon4.OCD/.PHD/.PD/.UCD/.VCD/.FED/.FCD parsers.
//
// Approach: since we don't have real Falcon4.OCD/.PHD/.PD/... fixture files
// (these ship with the game, not in the source repo), we build synthetic
// binary buffers matching the on-disk layout, write them to temp files,
// load them via the parsers, and verify the decoded values match what we
// wrote. This validates that:
//   1. Our struct-size constants match the on-disk record size (otherwise
//      the size-assertion in read_entry_count would reject the file).
//   2. Our field offsets are correct (otherwise the decoded values would
//      be wrong).
//   3. Our little-endian decoding is correct.
//   4. The FF-DB Control trailing-short fallback works.
//   5. The case-insensitive file search works.
//
// When the user later supplies real Falcon4.OCD/.PHD/.PD/... files from
// their install, the same parsers will Just Work on those — the on-disk
// format is identical.

#include <f4/world_convert/theater_data.hpp>
#include <f4/world_convert/class_table.hpp>
#include <f4/world_convert/world_json.hpp>
#include <f4/world_convert/cam_archive.hpp>

#include <gtest/gtest.h>

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <vector>

namespace {

// Build a synthetic Falcon4.OCD file with N entries.
// Each entry has a known name and feature count so we can verify the parser.
std::vector<uint8_t> build_synthetic_ocd(int n_entries) {
    std::vector<uint8_t> buf;
    buf.reserve(2 + static_cast<std::size_t>(n_entries) * f4::world_convert::OCD_RECORD_SIZE);

    // Header: short NumEntities (LE)
    buf.push_back(static_cast<uint8_t>(n_entries & 0xFF));
    buf.push_back(static_cast<uint8_t>((n_entries >> 8) & 0xFF));

    for (int i = 0; i < n_entries; ++i) {
        // Index (int16)
        const int16_t idx = static_cast<int16_t>(i);
        buf.push_back(static_cast<uint8_t>(idx & 0xFF));
        buf.push_back(static_cast<uint8_t>((idx >> 8) & 0xFF));
        // Name (20 bytes) — "Obj_XXXX\0..." where XXXX = i
        char name[20] = {0};
        std::snprintf(name, sizeof(name), "Obj_%d", i);
        for (int j = 0; j < 20; ++j) buf.push_back(static_cast<uint8_t>(name[j]));
        // DataRate (int16)
        buf.push_back(static_cast<uint8_t>(i)); buf.push_back(0);
        // DeagDistance (int16)
        buf.push_back(static_cast<uint8_t>(i * 2)); buf.push_back(0);
        // PtDataIndex (int16) — only set for type 1 (Airbase)
        const int16_t pt_idx = (i == 0) ? 5 : 0;
        buf.push_back(static_cast<uint8_t>(pt_idx & 0xFF));
        buf.push_back(static_cast<uint8_t>((pt_idx >> 8) & 0xFF));
        // Detection[8] — all i
        for (int j = 0; j < 8; ++j) buf.push_back(static_cast<uint8_t>(i + j));
        // DamageMod[11] — all (i+1)
        for (int j = 0; j < 11; ++j) buf.push_back(static_cast<uint8_t>(i + 1));
        // 1 byte of padding to align IconIndex to offset 48 within the entry
        // (DamageMod ends at offset 47; IconIndex needs 2-byte alignment → 48).
        buf.push_back(0);
        // IconIndex (int16)
        buf.push_back(static_cast<uint8_t>(i * 3)); buf.push_back(0);
        // Features (uchar)
        buf.push_back(static_cast<uint8_t>(i + 1));
        // RadarFeature (uchar)
        buf.push_back(static_cast<uint8_t>((i == 21) ? 5 : 0));  // type 21 = RADAR
        // FirstFeature (int16)
        buf.push_back(static_cast<uint8_t>(i * 4)); buf.push_back(0);
    }
    return buf;
}

// Build a synthetic Falcon4.PHD file with N entries.
std::vector<uint8_t> build_synthetic_phd(int n_entries) {
    std::vector<uint8_t> buf;
    buf.reserve(2 + static_cast<std::size_t>(n_entries) * f4::world_convert::PHD_RECORD_SIZE);

    buf.push_back(static_cast<uint8_t>(n_entries & 0xFF));
    buf.push_back(static_cast<uint8_t>((n_entries >> 8) & 0xFF));

    for (int i = 0; i < n_entries; ++i) {
        // objID (int16)
        const int16_t oid = static_cast<int16_t>(i % 10);
        buf.push_back(static_cast<uint8_t>(oid & 0xFF));
        buf.push_back(static_cast<uint8_t>((oid >> 8) & 0xFF));
        // type (uchar) — cycle through RunwayListType=1, ParkListType=11
        buf.push_back(static_cast<uint8_t>((i % 2 == 0) ? 1 : 11));
        // count (uchar)
        buf.push_back(static_cast<uint8_t>(i + 2));
        // features[5] (uchar[5])
        for (int j = 0; j < 5; ++j) buf.push_back(static_cast<uint8_t>(j));
        // data (int16) — heading
        const int16_t heading = static_cast<int16_t>(i * 10);
        buf.push_back(static_cast<uint8_t>(heading & 0xFF));
        buf.push_back(static_cast<uint8_t>((heading >> 8) & 0xFF));
        // sinHeading (float) — 0.0 + i/10.0
        float sin_h = i * 0.1f;
        uint32_t sin_bits;
        std::memcpy(&sin_bits, &sin_h, 4);
        for (int b = 0; b < 4; ++b) buf.push_back(static_cast<uint8_t>((sin_bits >> (b * 8)) & 0xFF));
        // cosHeading (float)
        float cos_h = 1.0f - i * 0.05f;
        uint32_t cos_bits;
        std::memcpy(&cos_bits, &cos_h, 4);
        for (int b = 0; b < 4; ++b) buf.push_back(static_cast<uint8_t>((cos_bits >> (b * 8)) & 0xFF));
        // first (int16)
        const int16_t first = static_cast<int16_t>(i * 3);
        buf.push_back(static_cast<uint8_t>(first & 0xFF));
        buf.push_back(static_cast<uint8_t>((first >> 8) & 0xFF));
        // texIdx (int16)
        buf.push_back(static_cast<uint8_t>(i)); buf.push_back(0);
        // runwayNum (char) — -1 if not a runway
        buf.push_back(static_cast<uint8_t>((i % 2 == 0) ? 0 : -1));
        // ltrt (char)
        buf.push_back(static_cast<uint8_t>(i % 2));
        // nextHeader (int16) — chain: entry i → entry i+1, last → 0
        const int16_t next = (i + 1 < n_entries) ? static_cast<int16_t>(i + 1) : 0;
        buf.push_back(static_cast<uint8_t>(next & 0xFF));
        buf.push_back(static_cast<uint8_t>((next >> 8) & 0xFF));
        // 1 byte of trailing padding to align struct size to 4 (largest
        // member is float, so struct size must be a multiple of 4).
        buf.push_back(0);
    }
    return buf;
}

// Build a synthetic Falcon4.PD file with N entries.
std::vector<uint8_t> build_synthetic_pd(int n_entries) {
    std::vector<uint8_t> buf;
    buf.reserve(2 + static_cast<std::size_t>(n_entries) * f4::world_convert::PD_RECORD_SIZE);

    buf.push_back(static_cast<uint8_t>(n_entries & 0xFF));
    buf.push_back(static_cast<uint8_t>((n_entries >> 8) & 0xFF));

    for (int i = 0; i < n_entries; ++i) {
        // xOffset (float)
        float x = i * 100.0f;
        uint32_t x_bits;
        std::memcpy(&x_bits, &x, 4);
        for (int b = 0; b < 4; ++b) buf.push_back(static_cast<uint8_t>((x_bits >> (b * 8)) & 0xFF));
        // yOffset (float)
        float y = i * 50.0f;
        uint32_t y_bits;
        std::memcpy(&y_bits, &y, 4);
        for (int b = 0; b < 4; ++b) buf.push_back(static_cast<uint8_t>((y_bits >> (b * 8)) & 0xFF));
        // type (uchar) — cycle through RunwayPt=1, TaxiPt=3, SmallParkPt=11
        const uint8_t types[] = {1, 3, 11};
        buf.push_back(types[i % 3]);
        // flags (uchar) — PT_FIRST=0x01 if i==0, PT_LAST=0x02 if i==n-1
        uint8_t flags = 0;
        if (i == 0) flags |= 0x01;
        if (i == n_entries - 1) flags |= 0x02;
        buf.push_back(flags);
        // 2 bytes of trailing padding (struct size = 12, fields = 10)
        buf.push_back(0);
        buf.push_back(0);
    }
    return buf;
}

// Write a buffer to a temp file with the given extension. Returns the path.
std::filesystem::path write_temp_file(const std::string& stem,
                                       const std::string& ext,
                                       const std::vector<uint8_t>& data) {
    const auto dir = std::filesystem::temp_directory_path();
    auto path = dir / (stem + "." + ext);
    std::ofstream f(path, std::ios::binary);
    f.write(reinterpret_cast<const char*>(data.data()),
            static_cast<std::streamsize>(data.size()));
    f.close();
    return path;
}

// RAII helper to delete a temp file when it goes out of scope.
struct TempFile {
    std::filesystem::path path;
    explicit TempFile(std::filesystem::path p) : path(std::move(p)) {}
    ~TempFile() { std::filesystem::remove(path); }
};

} // namespace

// ============================================================================
// OCD (Falcon4.OCD) parser tests
// ============================================================================

TEST(TheaterData, OcdLoadsAllEntries) {
    const auto buf = build_synthetic_ocd(10);
    const auto path = write_temp_file("Falcon4_test_OCD_loadall", "OCD", buf);
    TempFile cleanup(path);

    f4::world_convert::ObjectiveClassTable tbl;
    ASSERT_NO_THROW(f4::world_convert::load_objective_data(path, tbl));
    EXPECT_EQ(tbl.size(), 10);
}

TEST(TheaterData, OcdDecodesNameAndFields) {
    const auto buf = build_synthetic_ocd(5);
    const auto path = write_temp_file("Falcon4_test_OCD_decode", "OCD", buf);
    TempFile cleanup(path);

    f4::world_convert::ObjectiveClassTable tbl;
    ASSERT_NO_THROW(f4::world_convert::load_objective_data(path, tbl));
    ASSERT_EQ(tbl.size(), 5);

    // Entry 0 should be named "Obj_0", have features=1, pt_data_index=5 (airbase)
    const auto* e0 = tbl.at(0);
    ASSERT_NE(e0, nullptr);
    EXPECT_EQ(e0->name, "Obj_0");
    EXPECT_EQ(e0->features, 1);
    EXPECT_EQ(e0->pt_data_index, 5);
    EXPECT_EQ(e0->icon_index, 0);

    // Entry 4 should be named "Obj_4", have features=5
    const auto* e4 = tbl.at(4);
    ASSERT_NE(e4, nullptr);
    EXPECT_EQ(e4->name, "Obj_4");
    EXPECT_EQ(e4->features, 5);
    EXPECT_EQ(e4->deag_distance, 8);  // i*2 = 4*2 = 8
    EXPECT_EQ(e4->icon_index, 12);    // i*3 = 4*3 = 12
    EXPECT_EQ(e4->data_rate, 4);      // i = 4

    // Radar entry (type 21, which is index 21 in the table). We only built 5
    // entries, so let's verify the radar_feature field on entry 4 (i=4 != 21
    // → radar_feature should be 0).
    EXPECT_EQ(e4->radar_feature, 0);
}

TEST(TheaterData, OcdRejectsUndersizedFile) {
    // Build a file that's too small for the claimed entry count.
    std::vector<uint8_t> buf;
    buf.push_back(10); buf.push_back(0);  // 10 entries claimed
    // ...but only 1 entry's worth of data.
    buf.resize(2 + f4::world_convert::OCD_RECORD_SIZE, 0);

    const auto path = write_temp_file("Falcon4_test_OCD_short", "OCD", buf);
    TempFile cleanup(path);

    f4::world_convert::ObjectiveClassTable tbl;
    EXPECT_THROW(f4::world_convert::load_objective_data(path, tbl), std::runtime_error);
}

TEST(TheaterData, OcdHandlesFfDbControlFormat) {
    // FF-DB Control: header says 0, real count is in the last 2 bytes.
    // File layout: [short 0] [entries × record_size] [short real_count]
    const int n = 3;
    auto buf = build_synthetic_ocd(n);
    // Overwrite the header to say 0
    buf[0] = 0; buf[1] = 0;
    // Append the real count at the end (LE)
    buf.push_back(static_cast<uint8_t>(n & 0xFF));
    buf.push_back(static_cast<uint8_t>((n >> 8) & 0xFF));

    const auto path = write_temp_file("Falcon4_test_OCD_ffdb", "OCD", buf);
    TempFile cleanup(path);

    f4::world_convert::ObjectiveClassTable tbl;
    ASSERT_NO_THROW(f4::world_convert::load_objective_data(path, tbl));
    EXPECT_EQ(tbl.size(), 3);
}

// ============================================================================
// PHD (Falcon4.PHD) parser tests
// ============================================================================

TEST(TheaterData, PhdLoadsAllEntries) {
    const auto buf = build_synthetic_phd(8);
    const auto path = write_temp_file("Falcon4_test_PHD_loadall", "PHD", buf);
    TempFile cleanup(path);

    f4::world_convert::PtHeaderTable tbl;
    ASSERT_NO_THROW(f4::world_convert::load_pt_header_data(path, tbl));
    EXPECT_EQ(tbl.size(), 8);
}

TEST(TheaterData, PhdDecodesFields) {
    const auto buf = build_synthetic_phd(5);
    const auto path = write_temp_file("Falcon4_test_PHD_decode", "PHD", buf);
    TempFile cleanup(path);

    f4::world_convert::PtHeaderTable tbl;
    ASSERT_NO_THROW(f4::world_convert::load_pt_header_data(path, tbl));
    ASSERT_EQ(tbl.size(), 5);

    const auto* e0 = tbl.at(0);
    ASSERT_NE(e0, nullptr);
    EXPECT_EQ(e0->obj_id, 0);
    EXPECT_EQ(e0->type, 1);          // RunwayListType
    EXPECT_EQ(e0->count, 2);         // i + 2 = 0 + 2 = 2
    EXPECT_EQ(e0->data, 0);          // heading = i*10 = 0
    EXPECT_FLOAT_EQ(e0->sin_heading, 0.0f);
    EXPECT_FLOAT_EQ(e0->cos_heading, 1.0f);
    EXPECT_EQ(e0->first, 0);
    EXPECT_EQ(e0->runway_num, 0);    // not -1, so it IS a runway
    EXPECT_EQ(e0->next_header, 1);   // chain → entry 1

    const auto* e1 = tbl.at(1);
    ASSERT_NE(e1, nullptr);
    EXPECT_EQ(e1->type, 11);          // ParkListType
    EXPECT_EQ(e1->count, 3);          // i + 2 = 1 + 2 = 3
    EXPECT_EQ(e1->data, 10);          // heading = 1 * 10
    EXPECT_FLOAT_EQ(e1->sin_heading, 0.1f);
    EXPECT_EQ(e1->runway_num, -1);    // -1, not a runway
    EXPECT_EQ(e1->ltrt, 1);
    EXPECT_EQ(e1->next_header, 2);

    const auto* e4 = tbl.at(4);
    ASSERT_NE(e4, nullptr);
    EXPECT_EQ(e4->next_header, 0);   // last in chain → 0
}

// ============================================================================
// PD (Falcon4.PD) parser tests
// ============================================================================

TEST(TheaterData, PdLoadsAllEntries) {
    const auto buf = build_synthetic_pd(20);
    const auto path = write_temp_file("Falcon4_test_PD_loadall", "PD", buf);
    TempFile cleanup(path);

    f4::world_convert::PtDataTable tbl;
    ASSERT_NO_THROW(f4::world_convert::load_pt_data(path, tbl));
    EXPECT_EQ(tbl.size(), 20);
}

TEST(TheaterData, PdDecodesFields) {
    const auto buf = build_synthetic_pd(10);
    const auto path = write_temp_file("Falcon4_test_PD_decode", "PD", buf);
    TempFile cleanup(path);

    f4::world_convert::PtDataTable tbl;
    ASSERT_NO_THROW(f4::world_convert::load_pt_data(path, tbl));
    ASSERT_EQ(tbl.size(), 10);

    const auto* e0 = tbl.at(0);
    ASSERT_NE(e0, nullptr);
    EXPECT_FLOAT_EQ(e0->x_offset, 0.0f);
    EXPECT_FLOAT_EQ(e0->y_offset, 0.0f);
    EXPECT_EQ(e0->type, 1);           // RunwayPt (i%3==0 → types[0]=1)
    EXPECT_EQ(e0->flags, 0x01);       // PT_FIRST

    const auto* e1 = tbl.at(1);
    ASSERT_NE(e1, nullptr);
    EXPECT_FLOAT_EQ(e1->x_offset, 100.0f);
    EXPECT_FLOAT_EQ(e1->y_offset, 50.0f);
    EXPECT_EQ(e1->type, 3);           // TaxiPt (i%3==1 → types[1]=3)
    EXPECT_EQ(e1->flags, 0);          // neither first nor last

    const auto* e9 = tbl.at(9);
    ASSERT_NE(e9, nullptr);
    EXPECT_FLOAT_EQ(e9->x_offset, 900.0f);
    EXPECT_FLOAT_EQ(e9->y_offset, 450.0f);
    EXPECT_EQ(e9->type, 1);           // i%3==0 → types[0]=1
    EXPECT_EQ(e9->flags, 0x02);       // PT_LAST
}

// ============================================================================
// Struct-size assertions — guards against accidental layout drift
// ============================================================================

TEST(TheaterData, OcdRecordSizeMatches) {
    // If this fails, the on-disk record size is wrong and the parser will
    // reject every real Falcon4.OCD file. Verify by recompiling and running
    // /home/z/my-project/scripts/size_probe.cpp against the FreeFalcon source.
    EXPECT_EQ(f4::world_convert::OCD_RECORD_SIZE, 54u);
    EXPECT_EQ(f4::world_convert::PHD_RECORD_SIZE, 28u);
    EXPECT_EQ(f4::world_convert::PD_RECORD_SIZE,  12u);
    EXPECT_EQ(f4::world_convert::UCD_RECORD_SIZE, 336u);
    EXPECT_EQ(f4::world_convert::VCD_RECORD_SIZE, 160u);
    EXPECT_EQ(f4::world_convert::FED_RECORD_SIZE, 32u);
    EXPECT_EQ(f4::world_convert::FCD_RECORD_SIZE, 60u);
}

// ============================================================================
// Point type name helpers
// ============================================================================

TEST(TheaterData, PointTypeNamesAreCorrect) {
    EXPECT_STREQ(f4::world_convert::point_type_name(f4::world_convert::PT_RUNWAY),     "Runway");
    EXPECT_STREQ(f4::world_convert::point_type_name(f4::world_convert::PT_TAXI),        "Taxi");
    EXPECT_STREQ(f4::world_convert::point_type_name(f4::world_convert::PT_SMALL_PARK),  "Small Park");
    EXPECT_STREQ(f4::world_convert::point_type_name(f4::world_convert::PT_LARGE_PARK),  "Large Park");
    EXPECT_STREQ(f4::world_convert::point_type_name(f4::world_convert::PT_HELICOPTER),  "Helicopter");
    EXPECT_STREQ(f4::world_convert::point_type_name(255),                               "Unknown");
}

TEST(TheaterData, PointListTypeNamesAreCorrect) {
    EXPECT_STREQ(f4::world_convert::point_list_type_name(f4::world_convert::PLT_RUNWAY),     "Runway");
    EXPECT_STREQ(f4::world_convert::point_list_type_name(f4::world_convert::PLT_PARK),       "Parking");
    EXPECT_STREQ(f4::world_convert::point_list_type_name(f4::world_convert::PLT_HELICOPTER), "Helicopter");
    EXPECT_STREQ(f4::world_convert::point_list_type_name(255),                                "Unknown");
}

TEST(TheaterData, MovementTypeNamesAreCorrect) {
    EXPECT_STREQ(f4::world_convert::movement_type_name(0), "NoMove");
    EXPECT_STREQ(f4::world_convert::movement_type_name(1), "Foot");
    EXPECT_STREQ(f4::world_convert::movement_type_name(2), "Wheeled");
    EXPECT_STREQ(f4::world_convert::movement_type_name(3), "Tracked");
    EXPECT_STREQ(f4::world_convert::movement_type_name(5), "Air");
    EXPECT_STREQ(f4::world_convert::movement_type_name(7), "Rail");
    EXPECT_STREQ(f4::world_convert::movement_type_name(99), "Unknown");
}

// ============================================================================
// Case-insensitive file search
// ============================================================================

TEST(TheaterData, FindTheaterFileIsCaseInsensitive) {
    // Write a file as "FALCON4.OCD" (uppercase) and try to find it via a
    // lowercase base path "falcon4".
    std::vector<uint8_t> data = {0, 0};  // empty file with 0 entries
    const auto dir = std::filesystem::temp_directory_path() / "f4_theater_find_test";
    std::filesystem::create_directories(dir);
    const auto path = dir / "FALCON4.OCD";
    {
        std::ofstream f(path, std::ios::binary);
        f.write(reinterpret_cast<const char*>(data.data()),
                static_cast<std::streamsize>(data.size()));
    }

    // Search with lowercase base name
    auto found = f4::world_convert::find_theater_file(dir / "falcon4", "OCD");
    EXPECT_EQ(found, path);

    // Search with mixed-case extension
    found = f4::world_convert::find_theater_file(dir / "Falcon4", "ocd");
    EXPECT_EQ(found, path);

    std::filesystem::remove_all(dir);
}

// ============================================================================
// TheaterObjectDatabase::load_all — verify it skips missing files silently
// ============================================================================

TEST(TheaterData, LoadAllSkipsMissingFiles) {
    const auto dir = std::filesystem::temp_directory_path() / "f4_theater_loadall_test";
    std::filesystem::create_directories(dir);

    // Write ONLY an OCD file
    auto ocd_buf = build_synthetic_ocd(3);
    {
        std::ofstream f(dir / "Falcon4.OCD", std::ios::binary);
        f.write(reinterpret_cast<const char*>(ocd_buf.data()),
                static_cast<std::streamsize>(ocd_buf.size()));
    }

    f4::world_convert::TheaterObjectDatabase db;
    EXPECT_NO_THROW(db.load_all(dir));
    EXPECT_TRUE(db.loaded());
    EXPECT_EQ(db.objectives.size(), 3);
    EXPECT_EQ(db.pt_headers.size(), 0);   // no PHD file
    EXPECT_EQ(db.pt_data.size(), 0);      // no PD file
    EXPECT_EQ(db.units.size(), 0);        // no UCD file

    std::filesystem::remove_all(dir);
}

// ============================================================================
// World JSON integration — verify the new fields appear when theater_db is set
// ============================================================================

TEST(TheaterData, WorldJsonEmitsClassNameWhenTheaterDbLoaded) {
    // Build a tiny .cam fixture (we'll use the existing save1.cam fixture
    // since we just need objectives to exist).
    const std::string cam_path = std::string(FIXTURE_DIR) + "save1.cam";
    ASSERT_TRUE(std::filesystem::exists(cam_path));

    f4::world_convert::CamArchive cam;
    ASSERT_NO_THROW(cam.load(cam_path));

    // Build a synthetic theater DB with OCD entries
    auto ocd_buf = build_synthetic_ocd(40);  // 40 entries = one per ObjectiveType
    const auto dir = std::filesystem::temp_directory_path() / "f4_world_json_test";
    std::filesystem::create_directories(dir);
    {
        std::ofstream f(dir / "Falcon4.OCD", std::ios::binary);
        f.write(reinterpret_cast<const char*>(ocd_buf.data()),
                static_cast<std::streamsize>(ocd_buf.size()));
    }

    f4::world_convert::TheaterObjectDatabase theater_db;
    theater_db.load_all(dir);
    ASSERT_TRUE(theater_db.objectives.loaded());

    // Load the class table so objective_type is resolved (1-39)
    f4::world_convert::ClassTable class_table;
    const std::string ct_path = std::string(FIXTURE_DIR) + "FALCON4.ct";
    ASSERT_NO_THROW(class_table.load(ct_path));

    f4::world_convert::WorldJsonOptions opts;
    opts.class_table = &class_table;
    opts.theater_db = &theater_db;

    std::string json;
    ASSERT_NO_THROW(json = f4::world_convert::to_world_json(cam, opts));

    // The JSON should contain class_name fields (from our synthetic OCD)
    EXPECT_NE(json.find("\"class_name\""), std::string::npos);
    EXPECT_NE(json.find("\"Obj_"), std::string::npos);  // our synthetic names
    EXPECT_NE(json.find("\"features_count\""), std::string::npos);
    EXPECT_NE(json.find("\"pt_data_index\""), std::string::npos);

    std::filesystem::remove_all(dir);
}

TEST(TheaterData, WorldJsonDoesNotEmitClassNameWhenTheaterDbAbsent) {
    // When no theater_db is provided, the JSON should NOT have class_name.
    const std::string cam_path = std::string(FIXTURE_DIR) + "save1.cam";
    ASSERT_TRUE(std::filesystem::exists(cam_path));

    f4::world_convert::CamArchive cam;
    ASSERT_NO_THROW(cam.load(cam_path));

    f4::world_convert::ClassTable class_table;
    const std::string ct_path = std::string(FIXTURE_DIR) + "FALCON4.ct";
    ASSERT_NO_THROW(class_table.load(ct_path));

    f4::world_convert::WorldJsonOptions opts;
    opts.class_table = &class_table;
    // theater_db = nullptr (default)

    std::string json;
    ASSERT_NO_THROW(json = f4::world_convert::to_world_json(cam, opts));

    EXPECT_EQ(json.find("\"class_name\""), std::string::npos);
    EXPECT_EQ(json.find("\"ground_layout\""), std::string::npos);
    EXPECT_EQ(json.find("\"vehicle_groups\""), std::string::npos);
}

TEST(TheaterData, WorldJsonEmitsGroundLayoutForAirbases) {
    // Build an airbase-typed objective's ground layout.
    // We need both OCD (to declare pt_data_index) and PHD/PD (for the layout).
    const std::string cam_path = std::string(FIXTURE_DIR) + "save1.cam";
    ASSERT_TRUE(std::filesystem::exists(cam_path));

    f4::world_convert::CamArchive cam;
    ASSERT_NO_THROW(cam.load(cam_path));

    const auto dir = std::filesystem::temp_directory_path() / "f4_layout_test";
    std::filesystem::create_directories(dir);

    // OCD: 40 entries; entry 0 (Airbase, type 1) has pt_data_index = 5
    auto ocd_buf = build_synthetic_ocd(40);
    // Override entry 0's pt_data_index to be 1 (so the PHD chain starts there).
    // Within entry 0, PtDataIndex is at offset:
    //   Index(2) + Name(20) + DataRate(2) + DeagDistance(2) = 28
    // So absolute file offset = 2 (header) + 0*OCD_RECORD_SIZE + 28 = 28.
    const int16_t pt_idx_override = 1;
    ocd_buf[28] = static_cast<uint8_t>(pt_idx_override & 0xFF);
    ocd_buf[29] = static_cast<uint8_t>((pt_idx_override >> 8) & 0xFF);

    {
        std::ofstream f(dir / "Falcon4.OCD", std::ios::binary);
        f.write(reinterpret_cast<const char*>(ocd_buf.data()),
                static_cast<std::streamsize>(ocd_buf.size()));
    }

    // PHD: 3 entries, chained 1 → 2 → 3 → 0
    auto phd_buf = build_synthetic_phd(3);
    {
        std::ofstream f(dir / "Falcon4.PHD", std::ios::binary);
        f.write(reinterpret_cast<const char*>(phd_buf.data()),
                static_cast<std::streamsize>(phd_buf.size()));
    }

    // PD: 10 entries (enough for the PHD's first+count runs)
    auto pd_buf = build_synthetic_pd(30);
    {
        std::ofstream f(dir / "Falcon4.PD", std::ios::binary);
        f.write(reinterpret_cast<const char*>(pd_buf.data()),
                static_cast<std::streamsize>(pd_buf.size()));
    }

    f4::world_convert::TheaterObjectDatabase theater_db;
    theater_db.load_all(dir);
    ASSERT_TRUE(theater_db.objectives.loaded());
    ASSERT_TRUE(theater_db.pt_headers.loaded());
    ASSERT_TRUE(theater_db.pt_data.loaded());

    f4::world_convert::ClassTable class_table;
    ASSERT_NO_THROW(class_table.load(std::string(FIXTURE_DIR) + "FALCON4.ct"));

    f4::world_convert::WorldJsonOptions opts;
    opts.class_table = &class_table;
    opts.theater_db = &theater_db;

    std::string json;
    ASSERT_NO_THROW(json = f4::world_convert::to_world_json(cam, opts));

    // At least one airbase should have a ground_layout
    EXPECT_NE(json.find("\"ground_layout\""), std::string::npos);
    // The layout should contain point type names
    EXPECT_NE(json.find("\"Runway\""), std::string::npos);
    EXPECT_NE(json.find("\"Parking\""), std::string::npos);

    std::filesystem::remove_all(dir);
}
