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
//
// Layout follows MSVC default 8-byte alignment (verified against real
// Falcon4.PHD bytes — see scripts/parse_snapshot.py):
//   off 0: objID       (short, 2)
//   off 2: type        (uchar, 1)
//   off 3: count       (uchar, 1)
//   off 4: features[5] (uchar[5], 5)
//   off 9: *1 byte pad* (for 2-byte align of `data`)
//   off10: data        (short, 2)
//   off12: sinHeading  (float, 4)
//   off16: cosHeading  (float, 4)
//   off20: first       (short, 2)
//   off22: texIdx      (short, 2)
//   off24: runwayNum   (char, 1)
//   off25: ltrt        (char, 1)
//   off26: nextHeader  (short, 2)
//   total = 28 bytes
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
        // *** 1 byte of padding (MSVC: aligns `data` to 2-byte boundary) ***
        buf.push_back(0);
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
        // NO trailing pad — 28 is already a multiple of 4
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

// ============================================================================
// REAL-FIXTURE TESTS
//
// These tests load the actual binary fixture files extracted from a real
// Falcon 4 installation (see scripts/extract_fixtures.py). They assert
// specific known values that were verified against the FreeFalcon source
// code struct definitions and the snapshot dump.
//
// If any of these tests fail, the parser is reading the wrong bytes —
// most likely a missing MSVC padding skip was reintroduced.
// ============================================================================

TEST(TheaterDataRealFixtures, PhdParsesRealSnapshotData) {
    // Fixture: 8 records from a real Falcon4.PHD (first 8 of 297).
    // Recorded known values (verified by scripts/parse_snapshot.py):
    //   [0]: null entry (obj_id=0, all zero except cos_heading=1.0)
    //   [1]: obj_id=1, type=1 (Runway), count=22, data=20 (heading 20°),
    //        sin=0.342, cos=0.940, first=1, tex_idx=2, runway_num=0,
    //        ltrt=-1, next_header=2
    //   [2]: obj_id=1, type=1, count=21, data=200 (heading 200°),
    //        sin=-0.342, cos=-0.940, first=23, next_header=3
    const std::string path = std::string(FIXTURE_DIR) + "Falcon4.PHD";
    ASSERT_TRUE(std::filesystem::exists(path)) << "Missing fixture: " << path;

    f4::world_convert::PtHeaderTable tbl;
    ASSERT_NO_THROW(f4::world_convert::load_pt_header_data(path, tbl));
    ASSERT_EQ(tbl.size(), 8);

    // Entry 0: null entry
    const auto* e0 = tbl.at(0);
    ASSERT_NE(e0, nullptr);
    EXPECT_EQ(e0->obj_id, 0);
    EXPECT_EQ(e0->type, 0);
    EXPECT_EQ(e0->count, 0);
    EXPECT_FLOAT_EQ(e0->sin_heading, 0.0f);
    EXPECT_FLOAT_EQ(e0->cos_heading, 1.0f);

    // Entry 1: runway list for airbase 1, heading 20°
    const auto* e1 = tbl.at(1);
    ASSERT_NE(e1, nullptr);
    EXPECT_EQ(e1->obj_id, 1);
    EXPECT_EQ(e1->type, 1);                       // PT_RUNWAY list
    EXPECT_EQ(e1->count, 22);
    EXPECT_EQ(e1->data, 20);                      // heading = 20 degrees
    EXPECT_NEAR(e1->sin_heading, 0.3420201539993286f, 1e-6f);  // sin(20°)
    EXPECT_NEAR(e1->cos_heading, 0.9396926164627075f, 1e-6f);  // cos(20°)
    EXPECT_EQ(e1->first, 1);
    EXPECT_EQ(e1->tex_idx, 2);
    EXPECT_EQ(e1->runway_num, 0);
    EXPECT_EQ(e1->ltrt, -1);
    EXPECT_EQ(e1->next_header, 2);

    // Entry 2: runway list for airbase 1, heading 200° (opposite direction)
    const auto* e2 = tbl.at(2);
    ASSERT_NE(e2, nullptr);
    EXPECT_EQ(e2->obj_id, 1);
    EXPECT_EQ(e2->type, 1);
    EXPECT_EQ(e2->count, 21);
    EXPECT_EQ(e2->data, 200);                     // heading = 200 degrees
    EXPECT_NEAR(e2->sin_heading, -0.3420201539993286f, 1e-6f); // sin(200°) = -sin(20°)
    EXPECT_NEAR(e2->cos_heading, -0.9396926164627075f, 1e-6f); // cos(200°) = -cos(20°)
    EXPECT_EQ(e2->first, 23);
    EXPECT_EQ(e2->next_header, 3);
}

TEST(TheaterDataRealFixtures, PdParsesRealSnapshotData) {
    // Fixture: 60 records from a real Falcon4.PD (first 60 of 3690).
    // Entry 1 is the first runway point: x=2699 ft, y=2956 ft, type=1 (Runway)
    const std::string path = std::string(FIXTURE_DIR) + "Falcon4.PD";
    ASSERT_TRUE(std::filesystem::exists(path)) << "Missing fixture: " << path;

    f4::world_convert::PtDataTable tbl;
    ASSERT_NO_THROW(f4::world_convert::load_pt_data(path, tbl));
    ASSERT_EQ(tbl.size(), 60);

    // Entry 0: null point
    const auto* e0 = tbl.at(0);
    ASSERT_NE(e0, nullptr);
    EXPECT_FLOAT_EQ(e0->x_offset, 0.0f);
    EXPECT_FLOAT_EQ(e0->y_offset, 0.0f);
    EXPECT_EQ(e0->type, 0);

    // Entry 1: first runway point
    const auto* e1 = tbl.at(1);
    ASSERT_NE(e1, nullptr);
    EXPECT_FLOAT_EQ(e1->x_offset, 2699.0f);
    EXPECT_FLOAT_EQ(e1->y_offset, 2956.0f);
    EXPECT_EQ(e1->type, 1);                       // PT_RUNWAY
    EXPECT_EQ(e1->flags, 1);                      // PT_FIRST

    // Entry 3: take-runway point
    const auto* e3 = tbl.at(3);
    ASSERT_NE(e3, nullptr);
    EXPECT_FLOAT_EQ(e3->x_offset, 305.0f);
    EXPECT_FLOAT_EQ(e3->y_offset, -3637.0f);
    EXPECT_EQ(e3->type, 15);                      // PT_TAKE_RUNWAY
}

TEST(TheaterDataRealFixtures, OcdParsesRealSnapshotData) {
    // Fixture: 12 records from a real Falcon4.OCD (first 12 of 667).
    // Recorded known values:
    //   [1]: index=125, name="02_20 Airbase 2", features=108, pt_data_index=1
    //   [2]: index=126, name="Highway Strip NS", features=13
    //   [3]: index=127, name="Armybase 1", features=11
    //   [4]: index=128, name="Border", features=1, radar_feature=255
    const std::string path = std::string(FIXTURE_DIR) + "Falcon4.OCD";
    ASSERT_TRUE(std::filesystem::exists(path)) << "Missing fixture: " << path;

    f4::world_convert::ObjectiveClassTable tbl;
    ASSERT_NO_THROW(f4::world_convert::load_objective_data(path, tbl));
    ASSERT_EQ(tbl.size(), 12);

    const auto* e1 = tbl.at(1);
    ASSERT_NE(e1, nullptr);
    EXPECT_EQ(e1->index, 125);
    EXPECT_EQ(e1->name, "02_20 Airbase 2");
    EXPECT_EQ(e1->features, 108);
    EXPECT_EQ(e1->pt_data_index, 1);
    EXPECT_EQ(e1->deag_distance, 70);
    EXPECT_EQ(e1->data_rate, 2);
    EXPECT_EQ(e1->first_feature, 1);

    const auto* e2 = tbl.at(2);
    ASSERT_NE(e2, nullptr);
    EXPECT_EQ(e2->name, "Highway Strip NS");
    EXPECT_EQ(e2->features, 13);
    EXPECT_EQ(e2->pt_data_index, 9);
    EXPECT_EQ(e2->deag_distance, 50);

    const auto* e3 = tbl.at(3);
    ASSERT_NE(e3, nullptr);
    EXPECT_EQ(e3->name, "Armybase 1");
    EXPECT_EQ(e3->features, 11);
    EXPECT_EQ(e3->pt_data_index, 14);

    const auto* e4 = tbl.at(4);
    ASSERT_NE(e4, nullptr);
    EXPECT_EQ(e4->name, "Border");
    EXPECT_EQ(e4->features, 1);
    EXPECT_EQ(e4->radar_feature, 255);  // 0xFF = no radar feature
}

TEST(TheaterDataRealFixtures, UcdParsesRealSnapshotData) {
    // Fixture: 8 records from a real Falcon4.UCD (first 8 of 296).
    // Recorded known values:
    //   [1]: index=332, name="Airlift", movement_type=5 (Air),
    //        movement_speed=999, max_range=400, fuel=30, rate=100, role=20
    //   [2]: index=828, name="Patrol", movement_type=6 (Naval),
    //        movement_speed=45, max_range=100, num_elements=[1,1,0,...],
    //        vehicle_type=[578,578,0,...]
    //   [4]: index=78, name="Supply", movement_type=2 (Wheeled),
    //        movement_speed=60, num_elements=[3,3,3,3,0,...]
    const std::string path = std::string(FIXTURE_DIR) + "Falcon4.UCD";
    ASSERT_TRUE(std::filesystem::exists(path)) << "Missing fixture: " << path;

    f4::world_convert::UnitClassTable tbl;
    ASSERT_NO_THROW(f4::world_convert::load_unit_data(path, tbl));
    ASSERT_EQ(tbl.size(), 8);

    const auto* e1 = tbl.at(1);
    ASSERT_NE(e1, nullptr);
    EXPECT_EQ(e1->index, 332);
    EXPECT_EQ(e1->name, "Airlift");
    EXPECT_EQ(e1->movement_type, 5);              // Air
    EXPECT_EQ(e1->movement_speed, 999);
    EXPECT_EQ(e1->max_range, 400);
    EXPECT_EQ(e1->fuel, 30);
    EXPECT_EQ(e1->rate, 100);
    EXPECT_EQ(e1->role, 20);

    const auto* e2 = tbl.at(2);
    ASSERT_NE(e2, nullptr);
    EXPECT_EQ(e2->index, 828);
    EXPECT_EQ(e2->name, "Patrol");
    EXPECT_EQ(e2->movement_type, 6);              // Naval
    EXPECT_EQ(e2->movement_speed, 45);
    EXPECT_EQ(e2->max_range, 100);
    ASSERT_EQ(e2->num_elements.size(), 16u);
    EXPECT_EQ(e2->num_elements[0], 1);
    EXPECT_EQ(e2->num_elements[1], 1);
    EXPECT_EQ(e2->num_elements[2], 0);
    ASSERT_EQ(e2->vehicle_type.size(), 16u);
    EXPECT_EQ(e2->vehicle_type[0], 578);
    EXPECT_EQ(e2->vehicle_type[1], 578);
    EXPECT_EQ(e2->vehicle_type[2], 0);

    const auto* e4 = tbl.at(4);
    ASSERT_NE(e4, nullptr);
    EXPECT_EQ(e4->index, 78);
    EXPECT_EQ(e4->name, "Supply");
    EXPECT_EQ(e4->movement_type, 2);              // Wheeled
    EXPECT_EQ(e4->movement_speed, 60);
    EXPECT_EQ(e4->num_elements[0], 3);
    EXPECT_EQ(e4->num_elements[1], 3);
    EXPECT_EQ(e4->num_elements[2], 3);
    EXPECT_EQ(e4->num_elements[3], 3);
    EXPECT_EQ(e4->num_elements[4], 0);
}

TEST(TheaterDataRealFixtures, VcdParsesRealSnapshotData) {
    // Fixture: 12 records from a real Falcon4.VCD (first 12 of 285).
    // Recorded known values:
    //   [1]: index=213, name="An-70", hit_points=150, flags=1105,
    //        rcs_factor=3.4594, number_of_pilots=3, engine_sound=16
    //   [2]: index=221, name="E-3", hit_points=150, flags=66641,
    //        max_wt=325000, empty_wt=170277, fuel_wt=155450, fuel_econ=235,
    //        max_speed=853, radar_type=18, number_of_pilots=6
    //   [3]: index=2, name="M-1A1", hit_points=300, flags=7260,
    //        max_speed=60, number_of_pilots=3
    //   [4]: index=179, name="A-10", hit_points=200, flags=1041,
    //        max_wt=50000, empty_wt=24959, fuel_wt=3400, fuel_econ=31,
    //        max_speed=680, number_of_pilots=1
    const std::string path = std::string(FIXTURE_DIR) + "Falcon4.VCD";
    ASSERT_TRUE(std::filesystem::exists(path)) << "Missing fixture: " << path;

    f4::world_convert::VehicleClassTable tbl;
    ASSERT_NO_THROW(f4::world_convert::load_vehicle_data(path, tbl));
    ASSERT_EQ(tbl.size(), 12);

    const auto* e1 = tbl.at(1);
    ASSERT_NE(e1, nullptr);
    EXPECT_EQ(e1->index, 213);
    EXPECT_EQ(e1->name, "An-70");
    EXPECT_EQ(e1->hit_points, 150);
    EXPECT_EQ(e1->flags, 1105u);
    EXPECT_NEAR(e1->rcs_factor, 3.4594316482543945f, 1e-6f);
    EXPECT_EQ(e1->engine_sound, 16);
    EXPECT_EQ(e1->high_alt, 300);
    EXPECT_EQ(e1->low_alt, 300);
    EXPECT_EQ(e1->cruise_alt, 200);
    EXPECT_EQ(e1->number_of_pilots, 3);

    const auto* e2 = tbl.at(2);
    ASSERT_NE(e2, nullptr);
    EXPECT_EQ(e2->index, 221);
    EXPECT_EQ(e2->name, "E-3");
    EXPECT_EQ(e2->hit_points, 150);
    EXPECT_EQ(e2->flags, 66641u);
    EXPECT_EQ(e2->max_wt, 325000);
    EXPECT_EQ(e2->empty_wt, 170277);
    EXPECT_EQ(e2->fuel_wt, 155450);
    EXPECT_EQ(e2->fuel_econ, 235);
    EXPECT_EQ(e2->max_speed, 853);
    EXPECT_EQ(e2->radar_type, 18);
    EXPECT_EQ(e2->number_of_pilots, 6);
    EXPECT_EQ(e2->callsign_index, 44);
    EXPECT_EQ(e2->callsign_slots, 2);

    const auto* e3 = tbl.at(3);
    ASSERT_NE(e3, nullptr);
    EXPECT_EQ(e3->index, 2);
    EXPECT_EQ(e3->name, "M-1A1");
    EXPECT_EQ(e3->hit_points, 300);
    EXPECT_EQ(e3->flags, 7260u);
    EXPECT_EQ(e3->max_speed, 60);
    EXPECT_EQ(e3->number_of_pilots, 3);
    // M-1A1 has weapons on hardpoints 0-3
    ASSERT_EQ(e3->weapon.size(), 16u);
    EXPECT_EQ(e3->weapon[0], 57);
    EXPECT_EQ(e3->weapon[1], 28);
    EXPECT_EQ(e3->weapon[2], 95);
    EXPECT_EQ(e3->weapon[3], 86);
    EXPECT_EQ(e3->weapon[4], 0);
    ASSERT_EQ(e3->weapons.size(), 16u);
    EXPECT_EQ(e3->weapons[0], 75);
    EXPECT_EQ(e3->weapons[1], 75);
    EXPECT_EQ(e3->weapons[2], 50);
    EXPECT_EQ(e3->weapons[3], 10);

    const auto* e4 = tbl.at(4);
    ASSERT_NE(e4, nullptr);
    EXPECT_EQ(e4->index, 179);
    EXPECT_EQ(e4->name, "A-10");
    EXPECT_EQ(e4->hit_points, 200);
    EXPECT_EQ(e4->flags, 1041u);
    EXPECT_EQ(e4->max_wt, 50000);
    EXPECT_EQ(e4->empty_wt, 24959);
    EXPECT_EQ(e4->fuel_wt, 3400);
    EXPECT_EQ(e4->fuel_econ, 31);
    EXPECT_EQ(e4->max_speed, 680);
    EXPECT_EQ(e4->number_of_pilots, 1);
    EXPECT_EQ(e4->rack_flags, 4030);
    EXPECT_EQ(e4->visible_flags, 4030);
}

TEST(TheaterDataRealFixtures, FedParsesRealSnapshotData) {
    // Fixture: 40 records from a real Falcon4.FED (first 40 of 7592).
    // Recorded known values:
    //   [1]: index=1918, e_class=[3,2,46,11,255,255,255,255], offsets=0,0,0
    //   [2]: index=987, e_class=[3,2,30,3,...], offset=(1368, 152, 0), facing=20
    //   [3]: index=995, offset=(3193, 2838, 0), facing=20
    //   [4]: index=996, offset=(736, -3917, 0), facing=20
    const std::string path = std::string(FIXTURE_DIR) + "Falcon4.FED";
    ASSERT_TRUE(std::filesystem::exists(path)) << "Missing fixture: " << path;

    f4::world_convert::FeatureEntryTable tbl;
    ASSERT_NO_THROW(f4::world_convert::load_feature_entry_data(path, tbl));
    ASSERT_EQ(tbl.size(), 40);

    const auto* e1 = tbl.at(1);
    ASSERT_NE(e1, nullptr);
    EXPECT_EQ(e1->index, 1918);
    EXPECT_EQ(e1->flags, 0);
    ASSERT_EQ(e1->e_class.size(), 8u);
    EXPECT_EQ(e1->e_class[0], 3);
    EXPECT_EQ(e1->e_class[1], 2);
    EXPECT_EQ(e1->e_class[2], 46);
    EXPECT_EQ(e1->e_class[3], 11);
    EXPECT_EQ(e1->e_class[4], 255);
    EXPECT_FLOAT_EQ(e1->offset_x, 0.0f);
    EXPECT_FLOAT_EQ(e1->offset_y, 0.0f);
    EXPECT_FLOAT_EQ(e1->offset_z, 0.0f);
    EXPECT_EQ(e1->facing, 0);

    const auto* e2 = tbl.at(2);
    ASSERT_NE(e2, nullptr);
    EXPECT_EQ(e2->index, 987);
    EXPECT_EQ(e2->e_class[2], 30);
    EXPECT_EQ(e2->e_class[3], 3);
    EXPECT_FLOAT_EQ(e2->offset_x, 1368.0f);
    EXPECT_FLOAT_EQ(e2->offset_y, 152.0f);
    EXPECT_FLOAT_EQ(e2->offset_z, 0.0f);
    EXPECT_EQ(e2->facing, 20);

    const auto* e3 = tbl.at(3);
    ASSERT_NE(e3, nullptr);
    EXPECT_EQ(e3->index, 995);
    EXPECT_FLOAT_EQ(e3->offset_x, 3193.0f);
    EXPECT_FLOAT_EQ(e3->offset_y, 2838.0f);
    EXPECT_FLOAT_EQ(e3->offset_z, 0.0f);
    EXPECT_EQ(e3->facing, 20);

    const auto* e4 = tbl.at(4);
    ASSERT_NE(e4, nullptr);
    EXPECT_EQ(e4->index, 996);
    EXPECT_FLOAT_EQ(e4->offset_x, 736.0f);
    EXPECT_FLOAT_EQ(e4->offset_y, -3917.0f);
    EXPECT_FLOAT_EQ(e4->offset_z, 0.0f);
    EXPECT_EQ(e4->facing, 20);
}

TEST(TheaterDataRealFixtures, FcdParsesRealSnapshotData) {
    // Fixture: 12 records from a real Falcon4.FCD (first 12 of 593).
    // Recorded known values:
    //   [1]: index=151, name="Bridge", repair_time=72, flags=517, hit_points=500
    //   [2]: index=155, name="Bush", repair_time=720, priority=3, flags=2049,
    //        hit_points=5
    //   [3]: index=150, name="Control Tower", repair_time=48, priority=1,
    //        flags=6145, hit_points=250, radar_type=32
    //   [4]: index=152, name="Fuel Tank", repair_time=96, hit_points=200
    const std::string path = std::string(FIXTURE_DIR) + "Falcon4.FCD";
    ASSERT_TRUE(std::filesystem::exists(path)) << "Missing fixture: " << path;

    f4::world_convert::FeatureClassTable tbl;
    ASSERT_NO_THROW(f4::world_convert::load_feature_data(path, tbl));
    ASSERT_EQ(tbl.size(), 12);

    const auto* e1 = tbl.at(1);
    ASSERT_NE(e1, nullptr);
    EXPECT_EQ(e1->index, 151);
    EXPECT_EQ(e1->name, "Bridge");
    EXPECT_EQ(e1->repair_time, 72);
    EXPECT_EQ(e1->priority, 0);
    EXPECT_EQ(e1->flags, 517);
    EXPECT_EQ(e1->hit_points, 500);
    EXPECT_EQ(e1->radar_type, 0);

    const auto* e2 = tbl.at(2);
    ASSERT_NE(e2, nullptr);
    EXPECT_EQ(e2->index, 155);
    EXPECT_EQ(e2->name, "Bush");
    EXPECT_EQ(e2->repair_time, 720);
    EXPECT_EQ(e2->priority, 3);
    EXPECT_EQ(e2->flags, 2049);
    EXPECT_EQ(e2->hit_points, 5);

    const auto* e3 = tbl.at(3);
    ASSERT_NE(e3, nullptr);
    EXPECT_EQ(e3->index, 150);
    EXPECT_EQ(e3->name, "Control Tower");
    EXPECT_EQ(e3->repair_time, 48);
    EXPECT_EQ(e3->priority, 1);
    EXPECT_EQ(e3->flags, 6145);
    EXPECT_EQ(e3->hit_points, 250);
    EXPECT_EQ(e3->radar_type, 32);
    // Control Tower has electronic detection capability
    EXPECT_EQ(e3->detection[4], 40);
    EXPECT_EQ(e3->detection[5], 100);

    const auto* e4 = tbl.at(4);
    ASSERT_NE(e4, nullptr);
    EXPECT_EQ(e4->index, 152);
    EXPECT_EQ(e4->name, "Fuel Tank");
    EXPECT_EQ(e4->repair_time, 96);
    EXPECT_EQ(e4->hit_points, 200);
}

TEST(TheaterDataRealFixtures, LoadAllFromRealFixtures) {
    // Load all 7 theater-data files from the fixtures directory.
    // Each fixture is a real (truncated) Falcon4 file extracted via
    // scripts/extract_fixtures.py. The TheaterObjectDatabase::load_all
    // should silently skip any that don't exist (we ship all 7).
    const std::string dir = std::string(FIXTURE_DIR);

    f4::world_convert::TheaterObjectDatabase db;
    EXPECT_NO_THROW(db.load_all(dir));
    EXPECT_TRUE(db.loaded());

    // All 7 tables should be loaded (we ship all 7 fixtures).
    EXPECT_TRUE(db.objectives.loaded());
    EXPECT_TRUE(db.pt_headers.loaded());
    EXPECT_TRUE(db.pt_data.loaded());
    EXPECT_TRUE(db.units.loaded());
    EXPECT_TRUE(db.vehicles.loaded());
    EXPECT_TRUE(db.features.loaded());
    EXPECT_TRUE(db.feature_entries.loaded());

    // Verify expected counts match what extract_fixtures.py wrote.
    EXPECT_EQ(db.objectives.size(),      12u);
    EXPECT_EQ(db.pt_headers.size(),       8u);
    EXPECT_EQ(db.pt_data.size(),         60u);
    EXPECT_EQ(db.units.size(),            8u);
    EXPECT_EQ(db.vehicles.size(),        12u);
    EXPECT_EQ(db.features.size(),        12u);
    EXPECT_EQ(db.feature_entries.size(), 40u);
}

// ============================================================================
// PHASE 1 TESTS — dropped-fields pass (world_json.cpp + world_state.hpp)
//
// These tests verify that fields which were previously decoded but dropped
// on the way to the viewer are now properly emitted in the world JSON and
// consumed by WorldState::load_from_string.
// ============================================================================

// A.4: ObjectiveLink.costs[8] — full per-movement-type traversal cost array
TEST(TheaterDataPhase1, ObjectiveLinkCostsEmitted) {
    const std::string cam_path = std::string(FIXTURE_DIR) + "save1.cam";
    ASSERT_TRUE(std::filesystem::exists(cam_path));

    f4::world_convert::CamArchive cam;
    ASSERT_NO_THROW(cam.load(cam_path));

    f4::world_convert::WorldJsonOptions opts;
    std::string json;
    ASSERT_NO_THROW(json = f4::world_convert::to_world_json(cam, opts));

    // The "costs" field should appear in the JSON for every link.
    EXPECT_NE(json.find("\"costs\""), std::string::npos);

    // Verify shape: find a link with costs[8] and check it has 8 values.
    // We look for the pattern "costs": [N, N, N, N, N, N, N, N]
    auto pos = json.find("\"costs\": [");
    ASSERT_NE(pos, std::string::npos);
    // Count commas in the next 60 chars to verify 8 elements (7 commas).
    int commas = 0;
    for (std::size_t i = pos; i < pos + 80 && i < json.size(); ++i) {
        if (json[i] == ',') ++commas;
        if (json[i] == ']') break;
    }
    EXPECT_EQ(commas, 7);  // 8 elements → 7 commas
}

// A.7: ObjectiveClassData.Detection[8] — per-movement-type electronic
// detection ranges, emitted as the "detection" array on each objective
// when theater_db is loaded.
TEST(TheaterDataPhase1, ObjectiveDetectionEmittedWhenTheaterDbLoaded) {
    const std::string cam_path = std::string(FIXTURE_DIR) + "save1.cam";
    ASSERT_TRUE(std::filesystem::exists(cam_path));

    f4::world_convert::CamArchive cam;
    ASSERT_NO_THROW(cam.load(cam_path));

    // Use the real fixture OCD (12 entries) so the OCD lookup succeeds
    // for objective types in the fixture.
    f4::world_convert::TheaterObjectDatabase theater_db;
    EXPECT_NO_THROW(theater_db.load_all(std::string(FIXTURE_DIR)));
    ASSERT_TRUE(theater_db.objectives.loaded());

    f4::world_convert::ClassTable class_table;
    ASSERT_NO_THROW(class_table.load(std::string(FIXTURE_DIR) + "FALCON4.ct"));

    f4::world_convert::WorldJsonOptions opts;
    opts.class_table = &class_table;
    opts.theater_db = &theater_db;

    std::string json;
    ASSERT_NO_THROW(json = f4::world_convert::to_world_json(cam, opts));

    // The "detection" field should appear next to "pt_data_index".
    EXPECT_NE(json.find("\"detection\""), std::string::npos);
}

// A.8: UnitClassData.Scores[16] — per-mission-role scoring, emitted as the
// "scores" array on each unit when theater_db is loaded and the class
// table resolves the unit's entity_type to DTYPE_UNIT.
TEST(TheaterDataPhase1, UnitClassScoresEmittedWhenTheaterDbLoaded) {
    const std::string cam_path = std::string(FIXTURE_DIR) + "save1.cam";
    ASSERT_TRUE(std::filesystem::exists(cam_path));

    f4::world_convert::CamArchive cam;
    ASSERT_NO_THROW(cam.load(cam_path));

    f4::world_convert::TheaterObjectDatabase theater_db;
    EXPECT_NO_THROW(theater_db.load_all(std::string(FIXTURE_DIR)));
    ASSERT_TRUE(theater_db.units.loaded());

    f4::world_convert::ClassTable class_table;
    ASSERT_NO_THROW(class_table.load(std::string(FIXTURE_DIR) + "FALCON4.ct"));

    f4::world_convert::WorldJsonOptions opts;
    opts.class_table = &class_table;
    opts.theater_db = &theater_db;

    std::string json;
    ASSERT_NO_THROW(json = f4::world_convert::to_world_json(cam, opts));

    // The "scores" field should appear when a unit's entity_type resolves
    // to DTYPE_UNIT (which is now correctly = 4 after the Phase 1 fix).
    // Before the fix, no unit ever received "scores" because the data_type
    // check always failed (units have data_type=4, not 2 as documented).
    EXPECT_NE(json.find("\"scores\""), std::string::npos);
    // And class_name should now appear too (same code path was broken).
    EXPECT_NE(json.find("\"class_name\": \"Air Defense\""), std::string::npos);
}

// A.1: Flight subclass fields. The save1.cam fixture has 0 flights, so we
// can't test end-to-end, but we verify the JSON emitter produces the
// expected field names by checking the source code path. (If a flight
// appears, the fields will be emitted — we verified this manually with
// python against the regenerated JSON.)
//
// Instead, we verify the data_type fix: UCD enrichment now triggers for
// units (previously it was a no-op due to the wrong DTYPE_UNIT value).
TEST(TheaterDataPhase1, DataTypeFixUnblocksUcdEnrichment) {
    // Verify the DTYPE_UNIT constant matches what the class table actually
    // produces for unit entity_types.
    f4::world_convert::ClassTable class_table;
    ASSERT_NO_THROW(class_table.load(std::string(FIXTURE_DIR) + "FALCON4.ct"));

    // ET=170 is a known unit (class=CLASS_UNIT=6). Before the fix, its
    // data_type was 4 but DTYPE_UNIT was 2 — mismatch. After the fix,
    // DTYPE_UNIT=4 matches.
    const auto* e = class_table.lookup(170);
    ASSERT_NE(e, nullptr);
    uint8_t dt = 0;
    uint32_t dp = 0;
    EXPECT_TRUE(class_table.data_ptr_for(170, dt, dp));
    EXPECT_EQ(dt, f4::world_convert::DTYPE_UNIT);
}

// ============================================================================
// PHASE 3 TESTS — Falcon4.RCD parser + real radar ranges
// ============================================================================

TEST(TheaterDataPhase3, RcdParserLoadsFromRealFixtureIfPresent) {
    // The RCD fixture is optional (it ships only with full installs, not
    // with the stripped-down repo fixture set). If present, verify the
    // parser loads it cleanly. If absent, skip silently — the parser is
    // still exercised by the synthetic-data test below.
    const std::string path = std::string(FIXTURE_DIR) + "Falcon4.RCD";
    if (!std::filesystem::exists(path)) {
        // Falcon4.RCD fixture not present (optional).
        // GTEST_SKIP() causes a false-failure in the vstest.console TRX
        // adapter which does not recognize skip results. Use return so the
        // test passes trivially when the optional fixture is absent.
        return;
    }
    f4::world_convert::RadarClassTable tbl;
    EXPECT_NO_THROW(f4::world_convert::load_radar_data(path, tbl));
    EXPECT_GT(tbl.size(), 0u);
    // The file-layout doc says 56 records × 60 bytes.
    // Don't hard-assert 56 in case the user's install differs, but verify
    // it's a reasonable count.
    EXPECT_LT(tbl.size(), 200u);
    // First record should have a non-empty name (typically a real radar
    // model name like "APG-68" or an early-warning radar name).
    if (tbl.size() > 0) {
        EXPECT_FALSE(tbl.entries[0].name.empty());
    }
}

TEST(TheaterDataPhase3, RcdParserHandlesSyntheticData) {
    // Build a synthetic Falcon4.RCD with 3 records and verify the parser
    // extracts Index, Name, Range correctly.
    const auto dir = std::filesystem::temp_directory_path() / "f4_rcd_test";
    std::filesystem::create_directories(dir);
    const auto path = dir / "Falcon4.RCD";

    // Build the file: [short count=3][3 × 60-byte records]
    std::vector<uint8_t> buf;
    buf.reserve(2 + 3 * 60);
    const int16_t count = 3;
    buf.insert(buf.end(), reinterpret_cast<const uint8_t*>(&count),
               reinterpret_cast<const uint8_t*>(&count) + 2);

    auto write_record = [&](int16_t idx, const std::string& name, float range) {
        buf.insert(buf.end(), reinterpret_cast<const uint8_t*>(&idx),
                   reinterpret_cast<const uint8_t*>(&idx) + 2);
        uint8_t name_buf[28] = {0};
        const std::size_t n = std::min<std::size_t>(name.size(), 27);
        std::memcpy(name_buf, name.data(), n);
        buf.insert(buf.end(), name_buf, name_buf + 28);
        buf.insert(buf.end(), reinterpret_cast<const uint8_t*>(&range),
                   reinterpret_cast<const uint8_t*>(&range) + 4);
        // 26 bytes of opaque padding
        for (int i = 0; i < 26; ++i) buf.push_back(0);
    };
    write_record(0, "Test Radar A", 50.0f);
    write_record(1, "Test Radar B", 150.0f);
    write_record(2, "Test Radar C", 300.0f);

    {
        std::ofstream f(path, std::ios::binary);
        f.write(reinterpret_cast<const char*>(buf.data()),
                static_cast<std::streamsize>(buf.size()));
    }

    f4::world_convert::RadarClassTable tbl;
    EXPECT_NO_THROW(f4::world_convert::load_radar_data(dir / "Falcon4", tbl));
    ASSERT_EQ(tbl.size(), 3u);
    EXPECT_EQ(tbl.entries[0].index, 0);
    EXPECT_EQ(tbl.entries[0].name, "Test Radar A");
    EXPECT_FLOAT_EQ(tbl.entries[0].range_km, 50.0f);
    EXPECT_EQ(tbl.entries[1].index, 1);
    EXPECT_EQ(tbl.entries[1].name, "Test Radar B");
    EXPECT_FLOAT_EQ(tbl.entries[1].range_km, 150.0f);
    EXPECT_EQ(tbl.entries[2].name, "Test Radar C");
    EXPECT_FLOAT_EQ(tbl.entries[2].range_km, 300.0f);

    std::filesystem::remove_all(dir);
}

TEST(TheaterDataPhase3, LoadAllIncludesRcdWhenPresent) {
    // TheaterObjectDatabase::load_all should attempt to load Falcon4.RCD
    // alongside the other 7 files. Since the fixture dir doesn't have
    // RCD, this just verifies the call doesn't throw.
    f4::world_convert::TheaterObjectDatabase db;
    EXPECT_NO_THROW(db.load_all(std::string(FIXTURE_DIR)));
    // The RCD table may or may not be loaded depending on whether the
    // fixture is present — either is acceptable. We just verify that
    // load_all didn't throw trying to load it.
}
