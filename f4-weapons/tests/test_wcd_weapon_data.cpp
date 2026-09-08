// f4-weapons/tests/test_wcd_weapon_data.cpp
//
// The real-data tier's weapon reader: parses the wcd2json export schema
// and folds the real employment/damage envelope onto the built-in
// WeaponClassTable via overlay_wcd_weapon_data.
//
// Pinned here:
//   - schema parse: every WCD field round-trips; unknown keys skipped
//   - the format tag is load-bearing (a wrong document is rejected)
//   - find_by_name is case-insensitive + trimmed (fixed-width WCD names)
//   - overlay: the four real-data fields (range/weight/strength/blast)
//     replace the built-ins; the flyout/seeker/fuze card is PRESERVED
//     (the invariants test re-runs over the overlaid table)
//   - overlay degradation: unresolved aliases warn and leave the record
//     untouched — a stale alias map can never throw or corrupt
//   - the km->ft conversion is exact

#include <f4/weapons/wcd_weapon_data.hpp>
#include <f4/weapons/weapon_types.hpp>

#include <gtest/gtest.h>

#include <cmath>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

namespace {

const char* kSampleJson = R"JSON({
  "format": "f4-weapon-class-table",
  "version": 1,
  "source": "FALCON4.WCD",
  "source_fingerprint": "ea53afa6531f6422",
  "count": 3,
  "entries": [
    { "index": 12, "strength": 40, "damage_type": 2, "range_km": 48,
      "flags": 512, "name": "AIM-120 AMRAAM",
      "hit_chance": [0, 0, 0, 0, 0, 0, 0, 0], "fire_rate": 1, "rarity": 1,
      "guidance_flags": 0, "collective": 0, "simweap_index": -1,
      "weight": 335, "drag_index": 0, "blast_radius": 50,
      "radar_type": 0, "sim_data_idx": 0, "max_alt": 60 },
    { "index": 68, "strength": 89, "damage_type": 4, "range_km": 9,
      "flags": 4096, "name": "GBU-12",
      "hit_chance": [0, 0, 0, 0, 0, 0, 0, 0], "fire_rate": 1, "rarity": 2,
      "guidance_flags": 0, "collective": 0, "simweap_index": -1,
      "weight": 275, "drag_index": 1, "blast_radius": 65,
      "radar_type": 0, "sim_data_idx": 0, "max_alt": 50 },
    { "index": 555, "strength": 0, "damage_type": 0, "range_km": 2,
      "flags": 0, "name": "M61",
      "hit_chance": [30, 25, 20, 15, 10, 5, 0, 0], "fire_rate": 10,
      "rarity": 0, "guidance_flags": 0, "collective": 0, "simweap_index": 3,
      "weight": 0, "drag_index": 0, "blast_radius": 0,
      "radar_type": 0, "sim_data_idx": 0, "max_alt": 0 },
    { "index": 556, "unknown_future_field": 7,
      "name": "PADDED NAME   " }
  ]
})JSON";

TEST(WcdWeaponData, ParsesEveryFieldOfTheSchema) {
    const auto r = f4::weapons::load_wcd_weapon_json_string(kSampleJson);
    ASSERT_TRUE(r.ok) << (r.errors.empty() ? "" : r.errors.front());
    EXPECT_EQ(r.data.source, "FALCON4.WCD");
    EXPECT_EQ(r.data.source_fingerprint, "ea53afa6531f6422");
    ASSERT_EQ(r.data.entries.size(), 4u);

    const auto& a = r.data.entries[0];
    EXPECT_EQ(a.index, 12);
    EXPECT_EQ(a.strength, 40);
    EXPECT_EQ(a.damage_type, 2);
    EXPECT_EQ(a.range_km, 48);
    EXPECT_EQ(a.flags, 512);
    EXPECT_EQ(a.name, "AIM-120 AMRAAM");
    EXPECT_EQ(a.fire_rate, 1);
    EXPECT_EQ(a.rarity, 1);
    EXPECT_EQ(a.weight, 335);
    EXPECT_EQ(a.blast_radius, 50);
    EXPECT_EQ(a.max_alt, 60);
    EXPECT_EQ(a.simweap_index, -1);

    // hit_chance round-trips per movement type.
    const auto& g = r.data.entries[2];
    EXPECT_EQ(g.hit_chance[0], 30);
    EXPECT_EQ(g.hit_chance[3], 15);
    EXPECT_EQ(g.hit_chance[7], 0);

    // Unknown keys are skipped, not rejected (the schema may grow).
    const auto& pad = r.data.entries[3];
    EXPECT_EQ(pad.name, "PADDED NAME   ");
}

TEST(WcdWeaponData, RejectsWrongFormatTag) {
    const auto r = f4::weapons::load_wcd_weapon_json_string(
        R"JSON({"format": "f4-class-table", "entries": []})JSON");
    EXPECT_FALSE(r.ok);
    ASSERT_FALSE(r.errors.empty());
    EXPECT_NE(r.errors.front().find("f4-weapon-class-table"), std::string::npos);
}

TEST(WcdWeaponData, ReportsParseErrorsWithoutThrowing) {
    // Truncated JSON: the reader throws; the loader captures and reports.
    const auto r = f4::weapons::load_wcd_weapon_json_string(
        R"JSON({"format": "f4-weapon-class-table", "entries": [ {"index":)JSON");
    EXPECT_FALSE(r.ok);
    ASSERT_FALSE(r.errors.empty());
    EXPECT_NE(r.errors.front().find("parse failed"), std::string::npos);
}

TEST(WcdWeaponData, FindByNameIsCaseInsensitiveAndTrimmed) {
    const auto r = f4::weapons::load_wcd_weapon_json_string(kSampleJson);
    ASSERT_TRUE(r.ok);
    ASSERT_NE(r.data.find_by_name("aim-120 amraam"), nullptr);
    ASSERT_NE(r.data.find_by_name("  AIM-120 AMRAAM  "), nullptr);
    ASSERT_NE(r.data.find_by_name("PADDED NAME"), nullptr);
    EXPECT_EQ(r.data.find_by_name("AIM-9"), nullptr);
    EXPECT_EQ(r.data.find_by_name(""), nullptr);
}

TEST(WcdWeaponData, FileLoaderReadsTheFixture) {
    namespace fs = std::filesystem;
    fs::path fixture = fs::path(WCD_TEST_FIXTURES_DIR) / "wcd_sample.json";
    const auto r = f4::weapons::load_wcd_weapon_json(fixture);
    ASSERT_TRUE(r.ok) << (r.errors.empty() ? "" : r.errors.front());
    EXPECT_FALSE(r.data.entries.empty());
}

TEST(WcdWeaponData, FileLoaderFailsLoudOnMissingPath) {
    const auto r = f4::weapons::load_wcd_weapon_json(
        "/nonexistent/path/wcd.json");
    EXPECT_FALSE(r.ok);
    ASSERT_FALSE(r.errors.empty());
}

TEST(WcdOverlay, ReplacesRealEnvelopeAndPreservesTheFlyoutCard) {
    auto table = f4::weapons::WeaponClassTable::with_builtins();
    const auto imported = f4::weapons::load_wcd_weapon_json_string(kSampleJson);
    ASSERT_TRUE(imported.ok);

    const auto* before = table.get(table.find_by_name("AIM-120C"));
    ASSERT_NE(before, nullptr);
    const double burnout_before = before->burnout_mass_lb;
    const double thrust_before = before->thrust_lbf;
    const double seeker_before = before->seeker_max_range_ft;
    const double min_range_before = before->min_range_ft;
    const auto guidance_before = before->guidance;
    const auto category_before = before->category;

    std::vector<std::string> warnings;
    const auto n = f4::weapons::overlay_wcd_weapon_data(
        table, imported.data,
        {{"AIM-120C", "AIM-120 AMRAAM"}, {"GBU-12", "GBU-12"}},
        &warnings);
    EXPECT_EQ(n, 2u);
    EXPECT_TRUE(warnings.empty()) << (warnings.empty() ? "" : warnings.front());

    // The real envelope landed (km -> ft exact; lb/ft verbatim).
    const auto* after = table.get(table.find_by_name("AIM-120C"));
    ASSERT_NE(after, nullptr);
    EXPECT_NEAR(after->max_range_ft, 48.0 * 3280.83989501312, 1e-6);
    EXPECT_DOUBLE_EQ(after->launch_mass_lb, 335.0);
    EXPECT_DOUBLE_EQ(after->warhead_power_lb, 40.0);
    EXPECT_DOUBLE_EQ(after->lethal_radius_ft, 50.0);

    // The flyout card is untouched — invariants hold by construction.
    EXPECT_DOUBLE_EQ(after->burnout_mass_lb, burnout_before);
    EXPECT_DOUBLE_EQ(after->thrust_lbf, thrust_before);
    EXPECT_DOUBLE_EQ(after->seeker_max_range_ft, seeker_before);
    EXPECT_DOUBLE_EQ(after->min_range_ft, min_range_before);
    EXPECT_EQ(after->guidance, guidance_before);
    EXPECT_EQ(after->category, category_before);
    EXPECT_GT(after->launch_mass_lb, after->burnout_mass_lb);

    // GBU-12: real blast radius + weight on the bomb card.
    const auto* gbu = table.get(table.find_by_name("GBU-12"));
    ASSERT_NE(gbu, nullptr);
    EXPECT_DOUBLE_EQ(gbu->lethal_radius_ft, 65.0);
    EXPECT_DOUBLE_EQ(gbu->launch_mass_lb, 275.0);
}

TEST(WcdOverlay, UnresolvedAliasesWarnAndLeaveTheRecordUntouched) {
    auto table = f4::weapons::WeaponClassTable::with_builtins();
    const auto imported = f4::weapons::load_wcd_weapon_json_string(kSampleJson);
    ASSERT_TRUE(imported.ok);

    const auto* before = table.get(table.find_by_name("AIM-9M"));
    ASSERT_NE(before, nullptr);
    const double range_before = before->max_range_ft;

    std::vector<std::string> warnings;
    const auto n = f4::weapons::overlay_wcd_weapon_data(
        table, imported.data,
        {{"AIM-9M", "AIM-9"},            // engine name exists, WCD misses
         {"NO-SUCH-ENGINE", "M61"},      // WCD name exists, engine misses
         {"M61A1", "M61"}},              // resolves
        &warnings);
    EXPECT_EQ(n, 1u);
    ASSERT_EQ(warnings.size(), 2u);

    const auto* after = table.get(table.find_by_name("AIM-9M"));
    ASSERT_NE(after, nullptr);
    EXPECT_DOUBLE_EQ(after->max_range_ft, range_before);

    // M61A1 got the real card (strength 0 is legitimate WCD data — the
    // overlay writes what the table says, it does not editorialize).
    const auto* gun = table.get(table.find_by_name("M61A1"));
    ASSERT_NE(gun, nullptr);
    EXPECT_DOUBLE_EQ(gun->max_range_ft, 2.0 * 3280.83989501312);
}

TEST(WcdOverlay, OverlaidTableStillSatisfiesTheFlyoutInvariants) {
    auto table = f4::weapons::WeaponClassTable::with_builtins();
    const auto imported = f4::weapons::load_wcd_weapon_json_string(kSampleJson);
    ASSERT_TRUE(imported.ok);
    (void)f4::weapons::overlay_wcd_weapon_data(
        table, imported.data,
        {{"AIM-120C", "AIM-120 AMRAAM"}, {"AIM-9M", "AIM-120 AMRAAM"}});

    for (const auto& rec : table.records()) {
        if (rec.category != f4::weapons::WeaponCategory::AirToAirMissile) {
            continue;
        }
        EXPECT_GT(rec.launch_mass_lb, rec.burnout_mass_lb);
        EXPECT_GT(rec.burnout_mass_lb, 0.0);
        EXPECT_GT(rec.thrust_lbf, 0.0);
        EXPECT_GT(rec.burn_time_s, 0.0);
        EXPECT_GT(rec.ref_area_ft2, 0.0);
        EXPECT_GT(rec.cd, 0.0);
        EXPECT_GT(rec.max_speed_fts, 0.0);
        EXPECT_GT(rec.max_g, 0.0);
        EXPECT_GT(rec.seeker_half_angle_deg, 0.0);
        EXPECT_GT(rec.seeker_max_range_ft, rec.fuze_radius_ft);
        EXPECT_GE(rec.lethal_radius_ft, rec.fuze_radius_ft);
        EXPECT_GT(rec.tof_limit_s, 0.0);
        EXPECT_GT(rec.max_range_ft, rec.min_range_ft);
        EXPECT_GT(rec.warhead_power_lb, 0.0);
    }
}

} // namespace
