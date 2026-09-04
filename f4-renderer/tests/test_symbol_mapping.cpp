// f4-renderer/tests/test_symbol_mapping.cpp
//
// Unit tests for the symbol-key mapping tables (entity_render.hpp):
// ObjectiveType → "obj_*" key, UnitClass → "frame_*" key, and
// (UnitClass, subtype) → "glyph_*" key. Ported 1:1 from the old
// symbol_for_objective_type()/symbol_for_unit() switches — every entry
// here has a symbols/<key>.svg in the repo (checked by the corpus test
// in test_svg_import.cpp). Pure functions; no GPU context.

#include <f4/renderer/entity_render.hpp>

#include <gtest/gtest.h>

using namespace f4::renderer;
using UC = f4::entities::UnitClass;

// ── Objective type mapping ────────────────────────────────────────────────

TEST(SymbolMapping, ObjectiveType_MappedKeys) {
    EXPECT_STREQ("obj_airbase",       key_for_objective_type(1));
    EXPECT_STREQ("obj_airstrip",      key_for_objective_type(2));
    EXPECT_STREQ("obj_army_base",     key_for_objective_type(3));
    EXPECT_STREQ("obj_beach",         key_for_objective_type(4));
    EXPECT_STREQ("obj_border",        key_for_objective_type(5));
    EXPECT_STREQ("obj_bridge",        key_for_objective_type(6));
    EXPECT_STREQ("obj_chemical",      key_for_objective_type(7));
    EXPECT_STREQ("obj_city",          key_for_objective_type(8));
    EXPECT_STREQ("obj_com_control",   key_for_objective_type(9));
    EXPECT_STREQ("obj_depot",         key_for_objective_type(10));
    EXPECT_STREQ("obj_factory",       key_for_objective_type(11));
    EXPECT_STREQ("obj_ford",          key_for_objective_type(12));
    EXPECT_STREQ("obj_fortification", key_for_objective_type(13));
    EXPECT_STREQ("obj_hill_top",      key_for_objective_type(14));
    EXPECT_STREQ("obj_intersection",  key_for_objective_type(15));
    EXPECT_STREQ("obj_nuclear",       key_for_objective_type(17));
    EXPECT_STREQ("obj_pass",          key_for_objective_type(18));
    EXPECT_STREQ("obj_port",          key_for_objective_type(19));
    EXPECT_STREQ("obj_power_plant",   key_for_objective_type(20));
    EXPECT_STREQ("obj_radar",         key_for_objective_type(21));
    EXPECT_STREQ("obj_radio_tower",   key_for_objective_type(22));
    EXPECT_STREQ("obj_rail_terminal", key_for_objective_type(23));
    EXPECT_STREQ("obj_railroad",      key_for_objective_type(24));
    EXPECT_STREQ("obj_refinery",      key_for_objective_type(25));
    EXPECT_STREQ("obj_road",          key_for_objective_type(26));
    EXPECT_STREQ("obj_sam_site",      key_for_objective_type(27));
    EXPECT_STREQ("obj_town",          key_for_objective_type(28));
    EXPECT_STREQ("obj_village",       key_for_objective_type(29));
    EXPECT_STREQ("obj_harts",         key_for_objective_type(30));
    EXPECT_STREQ("obj_air_terminal",  key_for_objective_type(39));
}

TEST(SymbolMapping, ObjectiveType_UnmappedFallsBackToUnknown) {
    // Parity with the old switch: 16 and 31..38 were never mapped.
    EXPECT_STREQ("obj_unknown", key_for_objective_type(0));
    EXPECT_STREQ("obj_unknown", key_for_objective_type(16));
    EXPECT_STREQ("obj_unknown", key_for_objective_type(31));
    EXPECT_STREQ("obj_unknown", key_for_objective_type(38));
    EXPECT_STREQ("obj_unknown", key_for_objective_type(200));
}

// ── Unit frame mapping ────────────────────────────────────────────────────

TEST(SymbolMapping, FramePerUnitClass) {
    EXPECT_STREQ("frame_battalion", frame_key_for_unit_class(UC::Battalion));
    EXPECT_STREQ("frame_brigade",   frame_key_for_unit_class(UC::Brigade));
    EXPECT_STREQ("frame_squadron",  frame_key_for_unit_class(UC::Squadron));
    EXPECT_STREQ("frame_task_force", frame_key_for_unit_class(UC::TaskForce));
    EXPECT_STREQ("frame_flight",    frame_key_for_unit_class(UC::Flight));
    EXPECT_STREQ("frame_package",   frame_key_for_unit_class(UC::Package));
}

// ── Unit glyph mapping ────────────────────────────────────────────────────

TEST(SymbolMapping, GroundGlyphs_SharedByBattalionAndBrigade) {
    for (const UC cls : {UC::Battalion, UC::Brigade}) {
        EXPECT_STREQ("glyph_air_defense", glyph_key_for_unit(cls, 1));
        EXPECT_STREQ("glyph_airmobile",   glyph_key_for_unit(cls, 2));
        EXPECT_STREQ("glyph_armor",       glyph_key_for_unit(cls, 3));
        EXPECT_STREQ("glyph_armored_cav", glyph_key_for_unit(cls, 4));
        EXPECT_STREQ("glyph_engineer",    glyph_key_for_unit(cls, 5));
        EXPECT_STREQ("glyph_hq",          glyph_key_for_unit(cls, 6));
        EXPECT_STREQ("glyph_infantry",    glyph_key_for_unit(cls, 7));
        EXPECT_STREQ("glyph_marine",      glyph_key_for_unit(cls, 8));
        EXPECT_STREQ("glyph_mechanized",  glyph_key_for_unit(cls, 9));
        EXPECT_STREQ("glyph_rocket",      glyph_key_for_unit(cls, 10));
        EXPECT_STREQ("glyph_artillery",   glyph_key_for_unit(cls, 11));  // SP
        EXPECT_STREQ("glyph_sa_missile",  glyph_key_for_unit(cls, 12));
        EXPECT_STREQ("glyph_supply",      glyph_key_for_unit(cls, 13));
        EXPECT_STREQ("glyph_artillery",   glyph_key_for_unit(cls, 14));  // towed
        // Unmapped ground subtype -> frame only.
        EXPECT_EQ(nullptr, glyph_key_for_unit(cls, 0));
        EXPECT_EQ(nullptr, glyph_key_for_unit(cls, 15));
    }
}

TEST(SymbolMapping, AirGlyphs) {
    const UC cls = UC::Squadron;
    EXPECT_STREQ("glyph_transport",  glyph_key_for_unit(cls, 1));   // transport
    EXPECT_STREQ("glyph_helicopter", glyph_key_for_unit(cls, 4));   // attack helo
    EXPECT_STREQ("glyph_bomber",     glyph_key_for_unit(cls, 6));
    EXPECT_STREQ("glyph_fighter",    glyph_key_for_unit(cls, 8));
    EXPECT_STREQ("glyph_fighter",    glyph_key_for_unit(cls, 9));   // fighter-bomber
    EXPECT_STREQ("glyph_transport",  glyph_key_for_unit(cls, 13));  // tanker
    EXPECT_STREQ("glyph_helicopter", glyph_key_for_unit(cls, 14));  // transport helo
    EXPECT_EQ(nullptr, glyph_key_for_unit(cls, 0));
}

TEST(SymbolMapping, NavalGlyphs) {
    const UC cls = UC::TaskForce;
    EXPECT_STREQ("glyph_carrier", glyph_key_for_unit(cls, 3));
    // Every other naval subtype draws the generic surface-ship glyph —
    // parity with the old table.
    EXPECT_STREQ("glyph_naval_surface", glyph_key_for_unit(cls, 0));
    EXPECT_STREQ("glyph_naval_surface", glyph_key_for_unit(cls, 7));
}

TEST(SymbolMapping, FrameOnlyClasses_HaveNoGlyphs) {
    EXPECT_EQ(nullptr, glyph_key_for_unit(UC::Flight, 8));
    EXPECT_EQ(nullptr, glyph_key_for_unit(UC::Package, 1));
}
