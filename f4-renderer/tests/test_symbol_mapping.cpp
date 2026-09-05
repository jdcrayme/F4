// f4-renderer/tests/test_symbol_mapping.cpp
//
// Unit tests for the symbol-key mapping tables (entity_render.hpp):
// ObjectiveType → "obj_*" key and (UnitClass, subtype) → the unit's
// composite icon key ("unit_*", or the bare "frame_*" when the class has
// no subtype glyph). Every entry here has a symbols/<key>.svg in the
// repo (checked by the corpus test in test_svg_import.cpp). Pure
// functions; no GPU context.

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

// ── Composite unit mapping ────────────────────────────────────────────────
//
// Units resolve to ONE icon key: a composite "unit_*" SVG (frame + glyph
// authored together). The frame/glyph split is retired — no glyph is ever
// reused across frames (no bomber battalions, no artillery squadrons).

TEST(SymbolMapping, GroundComposites_SharedByBattalionAndBrigade) {
    for (const UC cls : {UC::Battalion, UC::Brigade}) {
        EXPECT_STREQ("unit_air_defense", unit_symbol_key_for(cls, 1));
        EXPECT_STREQ("unit_airmobile",   unit_symbol_key_for(cls, 2));
        EXPECT_STREQ("unit_armor",       unit_symbol_key_for(cls, 3));
        EXPECT_STREQ("unit_armored_cav", unit_symbol_key_for(cls, 4));
        EXPECT_STREQ("unit_engineer",    unit_symbol_key_for(cls, 5));
        EXPECT_STREQ("unit_hq",          unit_symbol_key_for(cls, 6));
        EXPECT_STREQ("unit_infantry",    unit_symbol_key_for(cls, 7));
        EXPECT_STREQ("unit_marine",      unit_symbol_key_for(cls, 8));
        EXPECT_STREQ("unit_mechanized",  unit_symbol_key_for(cls, 9));
        EXPECT_STREQ("unit_rocket",      unit_symbol_key_for(cls, 10));
        EXPECT_STREQ("unit_artillery",   unit_symbol_key_for(cls, 11));  // SP
        EXPECT_STREQ("unit_sa_missile",  unit_symbol_key_for(cls, 12));
        EXPECT_STREQ("unit_supply",      unit_symbol_key_for(cls, 13));
        EXPECT_STREQ("unit_artillery",   unit_symbol_key_for(cls, 14));  // towed
    }
}

TEST(SymbolMapping, AirComposites) {
    const UC cls = UC::Squadron;
    EXPECT_STREQ("unit_transport",  unit_symbol_key_for(cls, 1));   // transport
    EXPECT_STREQ("unit_helicopter", unit_symbol_key_for(cls, 4));   // attack helo
    EXPECT_STREQ("unit_bomber",     unit_symbol_key_for(cls, 6));
    EXPECT_STREQ("unit_fighter",    unit_symbol_key_for(cls, 8));
    EXPECT_STREQ("unit_fighter",    unit_symbol_key_for(cls, 9));   // fighter-bomber
    EXPECT_STREQ("unit_transport",  unit_symbol_key_for(cls, 13));  // tanker
    EXPECT_STREQ("unit_helicopter", unit_symbol_key_for(cls, 14));  // transport helo
}

TEST(SymbolMapping, NavalComposites) {
    const UC cls = UC::TaskForce;
    EXPECT_STREQ("unit_carrier", unit_symbol_key_for(cls, 3));
    // Every other naval subtype draws the generic surface-ship composite —
    // parity with the old table.
    EXPECT_STREQ("unit_naval_surface", unit_symbol_key_for(cls, 0));
    EXPECT_STREQ("unit_naval_surface", unit_symbol_key_for(cls, 7));
}

// ── Frame fallbacks (classes/subtypes without a glyph) ────────────────────

TEST(SymbolMapping, UnknownSubtypes_FallBackToBareFrame) {
    // Mapped classes with an unmapped subtype render the bare frame —
    // the frame files stay live for exactly this case.
    EXPECT_STREQ("frame_battalion", unit_symbol_key_for(UC::Battalion, 0));
    EXPECT_STREQ("frame_battalion", unit_symbol_key_for(UC::Battalion, 15));
    EXPECT_STREQ("frame_brigade",   unit_symbol_key_for(UC::Brigade, 0));
    EXPECT_STREQ("frame_brigade",   unit_symbol_key_for(UC::Brigade, 20));
    EXPECT_STREQ("frame_squadron",  unit_symbol_key_for(UC::Squadron, 0));
    EXPECT_STREQ("frame_squadron",  unit_symbol_key_for(UC::Squadron, 2));
}

TEST(SymbolMapping, FrameOnlyClasses_AreTheirOwnIcons) {
    // Flight and Package have no subtype glyph — the bare frame IS the
    // composite (for any subtype value).
    EXPECT_STREQ("frame_flight",  unit_symbol_key_for(UC::Flight, 0));
    EXPECT_STREQ("frame_flight",  unit_symbol_key_for(UC::Flight, 8));
    EXPECT_STREQ("frame_package", unit_symbol_key_for(UC::Package, 0));
    EXPECT_STREQ("frame_package", unit_symbol_key_for(UC::Package, 1));
}

TEST(SymbolMapping, UnclassifiableClasses_ReturnNull) {
    EXPECT_EQ(nullptr, unit_symbol_key_for(UC::Unknown, 1));
    EXPECT_EQ(nullptr, unit_symbol_key_for(static_cast<UC>(99), 1));
}
