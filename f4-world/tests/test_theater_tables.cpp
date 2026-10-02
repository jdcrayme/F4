// f4-world/tests/test_theater_tables.cpp
//
// CAMP-SCALE-1 — the runtime reader for the converted theater tables
// (f4.theater.tables/1) and the countermeasure resolver.
//
// The importer side (f4-world-convert's emit_tables_json + cam2json
// --emit-tables) owns the producer tests; this file pins the consumer:
// the exact field vocabulary parses into runtime structs, additive
// exporter fields never break the reader, and the class-table → VCD →
// WCD chain resolves (or declines) per the golden-identity contract.

#include <gtest/gtest.h>
#include <f4/world/theater_tables.hpp>
#include <f4/world_types/class_table.hpp>

#include <filesystem>
#include <fstream>
#include <string>

using namespace f4::world;

namespace {

// A minimal falcon4.ct.json (the ct2json vocabulary) with six entries
// (DENSE: entity_type = 100 + position, the runtime loader's indexing):
//   100 — a UNIT row (DTYPE_UNIT → UCD; the countermeasure resolver
//         declines, the NAME resolver falls back to the unit-class name)
//   101 — a VEHICLE row (DTYPE_VEHICLE → VCD row 1)
//   102 — a VEHICLE row (DTYPE_VEHICLE → VCD row 2, no dispensers)
//   103 — a UNIT row (DTYPE_UNIT → UCD row 2, vehicle_type[0] = 1)
//   104 — a UNIT row (DTYPE_UNIT → UCD row 3, vehicle_type[0] = 5 — the
//         CLASS-TABLE chain pin: CT 105 → VCD row 1 "F-16X", while a bare
//         positional VCD read of 5 falls off the 3-row table)
//   105 — a VEHICLE row (DTYPE_VEHICLE → VCD row 1, reached ONLY through
//         the UCD vehicle_type link — position ≠ entity_type − 100 here)
const char* kClassTableJson =
    "{\n"
    "  \"count\": 6,\n"
    "  \"entries\": [\n"
    "    {\"entity_type\": 100, \"domain\": 2, \"cls\": 4, \"type\": 0,"
    " \"stype\": 3, \"vis_type\": [0, 0, 0, 0, 0, 0, 0],"
    " \"data_type\": 4, \"data_ptr_index\": 0},\n"
    "    {\"entity_type\": 101, \"domain\": 2, \"cls\": 4, \"type\": 0,"
    " \"stype\": 3, \"vis_type\": [0, 0, 0, 0, 0, 0, 0],"
    " \"data_type\": 5, \"data_ptr_index\": 1},\n"
    "    {\"entity_type\": 102, \"domain\": 2, \"cls\": 4, \"type\": 0,"
    " \"stype\": 3, \"vis_type\": [0, 0, 0, 0, 0, 0, 0],"
    " \"data_type\": 5, \"data_ptr_index\": 2},\n"
    "    {\"entity_type\": 103, \"domain\": 2, \"cls\": 4, \"type\": 0,"
    " \"stype\": 3, \"vis_type\": [0, 0, 0, 0, 0, 0, 0],"
    " \"data_type\": 4, \"data_ptr_index\": 2},\n"
    "    {\"entity_type\": 104, \"domain\": 2, \"cls\": 4, \"type\": 0,"
    " \"stype\": 3, \"vis_type\": [0, 0, 0, 0, 0, 0, 0],"
    " \"data_type\": 4, \"data_ptr_index\": 3},\n"
    "    {\"entity_type\": 105, \"domain\": 2, \"cls\": 4, \"type\": 0,"
    " \"stype\": 3, \"vis_type\": [0, 0, 0, 0, 0, 0, 0],"
    " \"data_type\": 5, \"data_ptr_index\": 1}\n"
    "  ]\n"
    "}\n";

// The tables document — mirrors the emitter's exact field vocabulary.
// Vehicles:
//   row 0: filler (keeps data_ptr 0 unused, the index convention)
//   row 1: "F-16X" — hardpoints [7, 8, 20] = Chaff 30 / Flare 15 / gun 0
//          (weapon id 20 carries shots but is not a dispenser)
//   row 2: "Truck" — hardpoints with shots but no chaff/flare rows
// Weapons:
//   row 7 "Chaff" (mixed case exercises the case-insensitive name rule)
//   row 8 "Flare"
//   row 20 "M-61 rounds" (shots, but not a dispenser)
std::string tables_json() {
    std::string s = "{\n";
    s += "  \"format\": \"f4.theater.tables/1\",\n";
    s += "  \"counts\": {\"units\": 4, \"vehicles\": 3, \"weapons\": 3},\n";

    s += "  \"units\": [\n";
    s += "    {\"index\": 332, \"name\": \"Airlift\", \"flags\": 8,"
         " \"movement_type\": 5, \"movement_type_name\": \"Air\","
         " \"movement_speed\": 999, \"max_range\": 400, \"fuel\": 30,"
         " \"rate\": 100, \"pt_data_index\": 7,"
         " \"num_elements\": [1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0],"
         " \"vehicle_type\": [101, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0],"
         " \"scores\": [10, 90, 0, 30, 0, 0, 0, 0, 0, 0, 40, 0, 0, 0, 0, 0],"
         " \"role\": 20,"
         " \"hit_chance\": [0, 0, 0, 0, 30, 25, 0, 0],"
         " \"strength\": [0, 0, 0, 0, 12, 12, 0, 0],"
         " \"range\": [0, 0, 0, 0, 0, 0, 0, 0],"
         " \"detection\": [0, 0, 0, 0, 0, 0, 0, 0],"
         " \"damage_mod\": [0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0],"
         " \"radar_vehicle\": 255, \"special_index\": 3, \"icon_index\": 14},\n";
    s += "    {\"index\": 828, \"name\": \"Patrol\", \"flags\": 0,"
         " \"movement_type\": 6, \"movement_type_name\": \"Naval\","
         " \"movement_speed\": 45, \"max_range\": 100, \"fuel\": 0,"
         " \"rate\": 0, \"pt_data_index\": 0,"
         " \"num_elements\": [1, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0],"
         " \"vehicle_type\": [578, 578, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0],"
         " \"scores\": [0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0],"
         " \"role\": 3,"
         " \"hit_chance\": [0, 0, 0, 0, 0, 0, 0, 0],"
         " \"strength\": [0, 0, 0, 0, 0, 0, 0, 0],"
         " \"range\": [0, 0, 0, 0, 0, 0, 0, 0],"
         " \"detection\": [0, 0, 0, 0, 0, 0, 0, 0],"
         " \"damage_mod\": [0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0],"
         " \"radar_vehicle\": 0, \"special_index\": 0, \"icon_index\": 9},\n";
    s += "    {\"index\": 402, \"name\": \"Fighter sqn\", \"flags\": 0,"
         " \"movement_type\": 5, \"movement_type_name\": \"Air\","
         " \"movement_speed\": 900, \"max_range\": 350, \"fuel\": 40,"
         " \"rate\": 90, \"pt_data_index\": 0,"
         " \"num_elements\": [1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0],"
         " \"vehicle_type\": [1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0],"
         " \"scores\": [0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0],"
         " \"role\": 0,"
         " \"hit_chance\": [0, 0, 0, 0, 0, 0, 0, 0],"
         " \"strength\": [0, 0, 0, 0, 0, 0, 0, 0],"
         " \"range\": [0, 0, 0, 0, 0, 0, 0, 0],"
         " \"detection\": [0, 0, 0, 0, 0, 0, 0, 0],"
         " \"damage_mod\": [0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0],"
         " \"radar_vehicle\": 0, \"special_index\": 0, \"icon_index\": 14},\n";
    // The CLASS-TABLE chain row: vehicle_type[0] = 5 is a CT position
    // (entity_type 105 → VCD row 1 "F-16X"), deliberately PAST the
    // 3-row vehicle table as a bare position — only the CT walk names
    // this squadron's aircraft.
    s += "    {\"index\": 419, \"name\": \"Test sqn\", \"flags\": 0,"
         " \"movement_type\": 5, \"movement_type_name\": \"Air\","
         " \"movement_speed\": 800, \"max_range\": 300, \"fuel\": 35,"
         " \"rate\": 80, \"pt_data_index\": 0,"
         " \"num_elements\": [1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0],"
         " \"vehicle_type\": [5, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0],"
         " \"scores\": [0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0],"
         " \"role\": 0,"
         " \"hit_chance\": [0, 0, 0, 0, 0, 0, 0, 0],"
         " \"strength\": [0, 0, 0, 0, 0, 0, 0, 0],"
         " \"range\": [0, 0, 0, 0, 0, 0, 0, 0],"
         " \"detection\": [0, 0, 0, 0, 0, 0, 0, 0],"
         " \"damage_mod\": [0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0],"
         " \"radar_vehicle\": 0, \"special_index\": 0, \"icon_index\": 14}\n";
    s += "  ],\n";

    s += "  \"vehicles\": [\n";
    s += "    {\"index\": 0, \"name\": \"Filler\", \"nctr\": \"FLL\","
         " \"hit_points\": 10, \"flags\": 0, \"rcs_factor\": 1.0,"
         " \"max_wt\": 0, \"empty_wt\": 0, \"fuel_wt\": 0, \"fuel_econ\": 0,"
         " \"engine_sound\": 0, \"high_alt\": 0, \"low_alt\": 0,"
         " \"cruise_alt\": 0, \"max_speed\": 0, \"radar_type\": 0,"
         " \"number_of_pilots\": 0, \"rack_flags\": 0, \"visible_flags\": 0,"
         " \"callsign_index\": 0, \"callsign_slots\": 0,"
         " \"hit_chance\": [0, 0, 0, 0, 0, 0, 0, 0],"
         " \"strength\": [0, 0, 0, 0, 0, 0, 0, 0],"
         " \"range\": [0, 0, 0, 0, 0, 0, 0, 0],"
         " \"detection\": [0, 0, 0, 0, 0, 0, 0, 0],"
         " \"weapon\": [0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0],"
         " \"weapons\": [0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0],"
         " \"damage_mod\": [0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0]},\n";
    s += "    {\"index\": 273, \"name\": \"F-16X\", \"nctr\": \"F16\","
         " \"hit_points\": 150, \"flags\": 1105, \"rcs_factor\": 0.0,"
         " \"max_wt\": 37500, \"empty_wt\": 18000, \"fuel_wt\": 7000,"
         " \"fuel_econ\": 70, \"engine_sound\": 12, \"high_alt\": 500,"
         " \"low_alt\": 100, \"cruise_alt\": 300, \"max_speed\": 2170,"
         " \"radar_type\": 18, \"number_of_pilots\": 1,"
         " \"rack_flags\": 4, \"visible_flags\": 4,"
         " \"callsign_index\": 2, \"callsign_slots\": 4,"
         " \"hit_chance\": [0, 0, 0, 0, 40, 35, 0, 0],"
         " \"strength\": [0, 0, 0, 0, 1, 1, 0, 0],"
         " \"range\": [0, 0, 0, 0, 40, 40, 0, 0],"
         " \"detection\": [0, 0, 0, 0, 60, 0, 0, 0],"
         " \"weapon\": [7, 8, 20, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0],"
         " \"weapons\": [30, 15, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0],"
         " \"damage_mod\": [0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0]},\n";
    s += "    {\"index\": 274, \"name\": \"Truck\", \"nctr\": \"TRK\","
         " \"hit_points\": 60, \"flags\": 0, \"rcs_factor\": 2.0,"
         " \"max_wt\": 10000, \"empty_wt\": 8000, \"fuel_wt\": 500,"
         " \"fuel_econ\": 10, \"engine_sound\": 3, \"high_alt\": 0,"
         " \"low_alt\": 0, \"cruise_alt\": 0, \"max_speed\": 80,"
         " \"radar_type\": 0, \"number_of_pilots\": 2,"
         " \"rack_flags\": 0, \"visible_flags\": 0,"
         " \"callsign_index\": 0, \"callsign_slots\": 0,"
         " \"hit_chance\": [10, 0, 0, 0, 0, 0, 0, 0],"
         " \"strength\": [0, 0, 0, 0, 0, 0, 0, 0],"
         " \"range\": [0, 0, 0, 0, 0, 0, 0, 0],"
         " \"detection\": [0, 0, 0, 0, 0, 0, 0, 0],"
         " \"weapon\": [20, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0],"
         " \"weapons\": [200, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0],"
         " \"damage_mod\": [0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0]}\n";
    s += "  ],\n";

    // WCD rows are POSITIONAL: hardpoint weapon IDs index the table by
    // row position, so Chaff must sit at row 7, Flare at row 8, and the
    // gun rounds at row 20 — filler rows pad the gaps (the real tables
    // are ~600 rows; a resolved ID past the array end declines).
    auto dummy_weapon = [](int idx) {
        return "    {\"index\": " + std::to_string(idx) +
               ", \"name\": \"W" + std::to_string(idx) + "\","
               " \"strength\": 0, \"damage_type\": 0, \"range_km\": 0,"
               " \"flags\": 0, \"fire_rate\": 0, \"rarity\": 0,"
               " \"guidance_flags\": 0, \"collective\": 0,"
               " \"simweap_index\": 0, \"weight\": 0, \"drag_index\": 0,"
               " \"blast_radius\": 0, \"radar_type\": 0,"
               " \"sim_data_idx\": 0, \"max_alt\": 0,"
               " \"hit_chance\": [0, 0, 0, 0, 0, 0, 0, 0]},\n";
    };
    s += "  \"weapons\": [\n";
    for (int i = 0; i < 21; ++i) {
        if (i == 7) {
            s += "    {\"index\": 7, \"name\": \"Chaff\", \"strength\": 1,"
                 " \"damage_type\": 0, \"range_km\": 0, \"flags\": 0,"
                 " \"fire_rate\": 1, \"rarity\": 100, \"guidance_flags\": 0,"
                 " \"collective\": 0, \"simweap_index\": 0, \"weight\": 0,"
                 " \"drag_index\": 0, \"blast_radius\": 0, \"radar_type\": 0,"
                 " \"sim_data_idx\": 0, \"max_alt\": 0,"
                 " \"hit_chance\": [0, 0, 0, 0, 0, 0, 0, 0]},\n";
        } else if (i == 8) {
            s += "    {\"index\": 8, \"name\": \"Flare\", \"strength\": 1,"
                 " \"damage_type\": 0, \"range_km\": 0, \"flags\": 0,"
                 " \"fire_rate\": 1, \"rarity\": 100, \"guidance_flags\": 0,"
                 " \"collective\": 0, \"simweap_index\": 0, \"weight\": 0,"
                 " \"drag_index\": 0, \"blast_radius\": 0, \"radar_type\": 0,"
                 " \"sim_data_idx\": 0, \"max_alt\": 0,"
                 " \"hit_chance\": [0, 0, 0, 0, 0, 0, 0, 0]},\n";
        } else if (i == 20) {
            s += "    {\"index\": 20, \"name\": \"M-61 rounds\", \"strength\": 4,"
                 " \"damage_type\": 6, \"range_km\": 2, \"flags\": 0,"
                 " \"fire_rate\": 10, \"rarity\": 100, \"guidance_flags\": 0,"
                 " \"collective\": 0, \"simweap_index\": 5, \"weight\": 0,"
                 " \"drag_index\": 0, \"blast_radius\": 0, \"radar_type\": 0,"
                 " \"sim_data_idx\": 5, \"max_alt\": 0,"
                 " \"hit_chance\": [0, 0, 0, 0, 0, 0, 0, 0]}\n";
        } else {
            s += dummy_weapon(i);
        }
    }
    s += "  ]\n";
    s += "}\n";
    return s;
}

} // namespace

TEST(TheaterTables, ParsesFullFieldVocabulary) {
    const auto t = TheaterTables::parse(tables_json());
    ASSERT_EQ(t.units.size(), 4u);
    ASSERT_EQ(t.vehicles.size(), 3u);
    ASSERT_EQ(t.weapons.size(), 21u);  // positional: rows 7/8/20 named

    // UCD row: every field lands (the pinned real-record values).
    const auto& u = t.units[0];
    EXPECT_EQ(u.index, 332);
    EXPECT_EQ(u.name, "Airlift");
    EXPECT_EQ(u.flags, 8u);
    EXPECT_EQ(u.movement_type, 5);
    EXPECT_EQ(u.movement_speed, 999);
    EXPECT_EQ(u.max_range, 400);
    EXPECT_EQ(u.fuel, 30);
    EXPECT_EQ(u.rate, 100);
    EXPECT_EQ(u.pt_data_index, 7);
    ASSERT_EQ(u.num_elements.size(), 16u);
    EXPECT_EQ(u.num_elements[0], 1);
    ASSERT_EQ(u.vehicle_type.size(), 16u);
    EXPECT_EQ(u.vehicle_type[0], 101);
    ASSERT_EQ(u.scores.size(), 16u);
    EXPECT_EQ(u.scores[1], 90);   // the ARO_CA column the ATM reads
    EXPECT_EQ(u.scores[10], 40);  // ARO_REC
    EXPECT_EQ(u.role, 20);
    ASSERT_EQ(u.hit_chance.size(), 8u);
    EXPECT_EQ(u.hit_chance[4], 30);
    ASSERT_EQ(u.range.size(), 8u);
    ASSERT_EQ(u.detection.size(), 8u);
    ASSERT_EQ(u.damage_mod.size(), 11u);
    EXPECT_EQ(u.radar_vehicle, 255);
    EXPECT_EQ(u.special_index, 3);
    EXPECT_EQ(u.icon_index, 14);

    // VCD row: identity + the hardpoint arrays the resolver walks.
    const auto& v = t.vehicles[1];
    EXPECT_EQ(v.index, 273);
    EXPECT_EQ(v.name, "F-16X");
    EXPECT_EQ(v.nctr, "F16");
    EXPECT_EQ(v.hit_points, 150);
    EXPECT_EQ(v.flags, 1105u);
    EXPECT_FLOAT_EQ(v.rcs_factor, 0.0f);
    EXPECT_EQ(v.number_of_pilots, 1);
    ASSERT_EQ(v.weapon.size(), 16u);
    EXPECT_EQ(v.weapon[0], 7);
    EXPECT_EQ(v.weapon[1], 8);
    ASSERT_EQ(v.weapons.size(), 16u);
    EXPECT_EQ(v.weapons[0], 30);
    EXPECT_EQ(v.weapons[1], 15);

    // WCD rows: POSITIONAL — the hardpoint weapon IDs index the table by
    // row position; the name is the dispenser identity.
    EXPECT_EQ(t.weapons[7].name, "Chaff");
    EXPECT_EQ(t.weapons[8].name, "Flare");
    EXPECT_EQ(t.weapons[20].name, "M-61 rounds");

    EXPECT_TRUE(t.loaded());
}

TEST(TheaterTables, AdditiveExporterFieldsNeverBreakTheReader) {
    // The tables are a conversion target, not a wire protocol: unknown
    // keys (a future exporter field) skip; an unknown format version
    // throws (the one structural contract).
    {
        std::string s = tables_json();
        s.insert(s.find("\"counts\""),
                 "\"future_field\": {\"a\": [1, 2, 3]},\n  ");
        EXPECT_NO_THROW((void)TheaterTables::parse(s));
    }
    {
        std::string s = tables_json();
        s.replace(s.find("f4.theater.tables/1"),
                  std::string("f4.theater.tables/1").size(),
                  "f4.theater.tables/2");
        EXPECT_THROW((void)TheaterTables::parse(s), std::runtime_error);
    }
}

TEST(TheaterTables, EmptyDocumentLoadsAsEmpty) {
    const auto t = TheaterTables::parse("{\n  \"format\":"
                                        " \"f4.theater.tables/1\",\n"
                                        "  \"units\": [],\n"
                                        "  \"vehicles\": [],\n"
                                        "  \"weapons\": []\n}\n");
    EXPECT_FALSE(t.loaded());
    EXPECT_EQ(t.unit_at(0), nullptr);
    EXPECT_EQ(t.vehicle_at(0), nullptr);
    EXPECT_EQ(t.weapon_at(0), nullptr);
}

TEST(TheaterTables, ResolveCountermeasuresChain) {
    const auto t = TheaterTables::parse(tables_json());
    f4::world_types::ClassTable ct;
    const auto ct_path = std::filesystem::temp_directory_path() /
                         "f4_theater_tables_ct.json";
    {
        std::ofstream f(ct_path);
        f << kClassTableJson;
    }
    ct.load_json(ct_path);
    std::filesystem::remove(ct_path);

    // The happy path: vehicle 101 → VCD row 1 → hardpoints 7/8/20 →
    // the WCD rows named Chaff/Flare sum to the dispenser supply; the
    // gun rounds (weapon 20, 0 shots) contribute nothing.
    const auto cm = resolve_countermeasures(t, ct, 101);
    ASSERT_TRUE(cm.has_value());
    EXPECT_EQ(cm->chaff_rounds, 30);
    EXPECT_EQ(cm->flare_rounds, 15);

    // A vehicle with shots but no dispenser rows declines — the caller
    // keeps the documented 30/15 defaults (never re-priced by absence).
    EXPECT_FALSE(resolve_countermeasures(t, ct, 102).has_value());

    // A UNIT-class entity type (DTYPE_UNIT → the UCD) declines: the
    // resolver walks vehicles only.
    EXPECT_FALSE(resolve_countermeasures(t, ct, 100).has_value());

    // Out-of-range entity types decline.
    EXPECT_FALSE(resolve_countermeasures(t, ct, 9999).has_value());

    // Empty tables decline (and never throw).
    const auto empty = TheaterTables::parse(
        "{ \"format\": \"f4.theater.tables/1\", \"units\": [],"
        " \"vehicles\": [], \"weapons\": [] }");
    EXPECT_FALSE(resolve_countermeasures(empty, ct, 101).has_value());
}

TEST(TheaterTables, ResolveEntityTypeNameChain) {
    const auto t = TheaterTables::parse(tables_json());
    f4::world_types::ClassTable ct;
    const auto ct_path = std::filesystem::temp_directory_path() /
                         "f4_theater_tables_ct_name.json";
    {
        std::ofstream f(ct_path);
        f << kClassTableJson;
    }
    ct.load_json(ct_path);
    std::filesystem::remove(ct_path);

    // A VEHICLE entity type names itself: CT 101 → VCD row 1 "F-16X",
    // CT 102 → VCD row 2 "Truck".
    EXPECT_EQ(resolve_entity_type_name(t, ct, 101), "F-16X");
    EXPECT_EQ(resolve_entity_type_name(t, ct, 102), "Truck");

    // A UNIT entity type names its FIRST vehicle when the group chain
    // resolves: CT 103 → UCD row 2 ("Fighter sqn") → vehicle 1 →
    // "F-16X" — the squadron displays its aircraft, not its role word.
    EXPECT_EQ(resolve_entity_type_name(t, ct, 103), "F-16X");

    // The vehicle_type link is a CLASS-TABLE position, not a VCD index:
    // CT 104 → UCD row 3 ("Test sqn") → vehicle_type[0] = 5 → CT 105 →
    // VCD row 1 "F-16X". A bare positional VCD read of 5 falls off the
    // 3-row table — only the CT walk names this squadron's aircraft
    // (the real Korea tables only resolve through this chain).
    EXPECT_EQ(resolve_entity_type_name(t, ct, 104), "F-16X");

    // When the unit's own first-vehicle link fails (CT 100 → UCD row 0
    // → vehicle 101, past the table) the unit-class name stands in.
    EXPECT_EQ(resolve_entity_type_name(t, ct, 100), "Airlift");

    // Out-of-range entity types render "" (the caller keeps its raw id).
    EXPECT_EQ(resolve_entity_type_name(t, ct, 9999), "");

    // Empty tables never name anything (and never throw).
    const auto empty = TheaterTables::parse(
        "{ \"format\": \"f4.theater.tables/1\", \"units\": [],"
        " \"vehicles\": [], \"weapons\": [] }");
    EXPECT_EQ(resolve_entity_type_name(empty, ct, 101), "");
}

// ── ECM-DATA-1: the per-vehicle ECM fit resolver ──────────────────────────
//
// Upstream "who carries a jammer" is two flag bits: VCD Flags &
// VEH_HAS_JAMMER (0x10000 — vehicle.h:35) and WCD Flags & WEAP_ECM
// (0x04 — campweap.h:34). The fixture: CT 201 → VCD row 1 ("EA-6B",
// the built-in bit), CT 202 → VCD row 2 ("F-16P" with an ALQ-131 pod
// hardpoint), CT 203 → VCD row 3 ("Truck", neither bit).

namespace {

std::string ecm_tables_json() {
    std::string s = "{\n";
    s += "  \"format\": \"f4.theater.tables/1\",\n";
    s += "  \"counts\": {\"units\": 0, \"vehicles\": 4, \"weapons\": 4},\n";
    s += "  \"units\": [],\n";
    s += "  \"vehicles\": [\n";
    auto vehicle_row = [](int idx, const char* name, uint32_t flags,
                          int w0, int shots0) {
        std::string r = "    {\"index\": ";
        r += std::to_string(idx);
        r += ", \"name\": \""; r += name; r += "\", \"nctr\": \"ECM\","
             " \"hit_points\": 100, \"flags\": ";
        r += std::to_string(flags);
        r += ", \"rcs_factor\": 1.0, \"max_wt\": 0, \"empty_wt\": 0,"
             " \"fuel_wt\": 0, \"fuel_econ\": 0, \"engine_sound\": 0,"
             " \"high_alt\": 0, \"low_alt\": 0, \"cruise_alt\": 0,"
             " \"max_speed\": 0, \"radar_type\": 0, \"number_of_pilots\": 1,"
             " \"rack_flags\": 0, \"visible_flags\": 0,"
             " \"callsign_index\": 0, \"callsign_slots\": 0,"
             " \"hit_chance\": [0, 0, 0, 0, 0, 0, 0, 0],"
             " \"strength\": [0, 0, 0, 0, 0, 0, 0, 0],"
             " \"range\": [0, 0, 0, 0, 0, 0, 0, 0],"
             " \"detection\": [0, 0, 0, 0, 0, 0, 0, 0],"
             " \"weapon\": [";
        r += std::to_string(w0);
        r += ", 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0],"
             " \"weapons\": [";
        r += std::to_string(shots0);
        r += ", 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0],"
             " \"damage_mod\": [0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0]}";
        return r;
    };
    s += vehicle_row(0, "Filler", 0, 0, 0) + ",\n";
    s += vehicle_row(1, "EA-6B", 0x10000, 0, 0) + ",\n";   // built-in bit
    s += vehicle_row(2, "F-16P", 0, 3, 1) + ",\n";         // ALQ-131 pod
    s += vehicle_row(3, "Truck", 0, 0, 0) + "\n";
    s += "  ],\n";
    // WCD rows are POSITIONAL: weapon id 3 is row 3 — the ALQ-131, the
    // one Korea WCD row carrying WEAP_ECM (flags 4, range_km 30).
    s += "  \"weapons\": [\n";
    s += "    {\"index\": 0, \"name\": \"Filler0\", \"strength\": 0,"
         " \"damage_type\": 0, \"range_km\": 0, \"flags\": 0,"
         " \"fire_rate\": 0, \"rarity\": 0, \"guidance_flags\": 0,"
         " \"collective\": 0, \"simweap_index\": 0, \"weight\": 0,"
         " \"drag_index\": 0, \"blast_radius\": 0, \"radar_type\": 0,"
         " \"sim_data_idx\": 0, \"max_alt\": 0},\n";
    s += "    {\"index\": 1, \"name\": \"Filler1\", \"strength\": 0,"
         " \"damage_type\": 0, \"range_km\": 0, \"flags\": 0,"
         " \"fire_rate\": 0, \"rarity\": 0, \"guidance_flags\": 0,"
         " \"collective\": 0, \"simweap_index\": 0, \"weight\": 0,"
         " \"drag_index\": 0, \"blast_radius\": 0, \"radar_type\": 0,"
         " \"sim_data_idx\": 0, \"max_alt\": 0},\n";
    s += "    {\"index\": 2, \"name\": \"Filler2\", \"strength\": 0,"
         " \"damage_type\": 0, \"range_km\": 0, \"flags\": 0,"
         " \"fire_rate\": 0, \"rarity\": 0, \"guidance_flags\": 0,"
         " \"collective\": 0, \"simweap_index\": 0, \"weight\": 0,"
         " \"drag_index\": 0, \"blast_radius\": 0, \"radar_type\": 0,"
         " \"sim_data_idx\": 0, \"max_alt\": 0},\n";
    s += "    {\"index\": 3, \"name\": \"ALQ-131\", \"strength\": 0,"
         " \"damage_type\": 0, \"range_km\": 30, \"flags\": 4,"
         " \"fire_rate\": 0, \"rarity\": 0, \"guidance_flags\": 0,"
         " \"collective\": 0, \"simweap_index\": 0, \"weight\": 659,"
         " \"drag_index\": 0, \"blast_radius\": 0, \"radar_type\": 0,"
         " \"sim_data_idx\": 13, \"max_alt\": 0}\n";
    s += "  ]\n";
    s += "}\n";
    return s;
}

// A minimal falcon4.ct.json — entries are POSITIONAL (entity_type =
// 100 + array index): 100 → dummy unit row, 101 → VCD row 1 ("EA-6B",
// the built-in bit), 102 → VCD row 2 ("F-16P", the pod), 103 → VCD
// row 3 ("Truck", neither bit).
std::string ecm_ct_json() {
    std::string s = "{\n  \"count\": 4,\n  \"entries\": [\n";
    s += "    {\"entity_type\": 100, \"domain\": 2, \"cls\": 4, \"type\": 0,"
         " \"stype\": 3, \"vis_type\": [0, 0, 0, 0, 0, 0, 0],"
         " \"data_type\": 4, \"data_ptr_index\": 0},\n";
    for (int i = 0; i < 3; ++i) {
        s += "    {\"entity_type\": " + std::to_string(101 + i) +
             ", \"domain\": 2, \"cls\": 4, \"type\": 0, \"stype\": 3,"
             " \"vis_type\": [0, 0, 0, 0, 0, 0, 0],"
             " \"data_type\": 5, \"data_ptr_index\": " +
             std::to_string(i + 1) + "}" +
             (i < 2 ? ",\n" : "\n");
    }
    s += "  ]\n}\n";
    return s;
}

f4::world_types::ClassTable load_ecm_ct() {
    const auto path = std::filesystem::temp_directory_path() /
                      "f4_theater_tables_ct_ecm.json";
    {
        std::ofstream f(path);
        f << ecm_ct_json();
    }
    f4::world_types::ClassTable ct;
    ct.load_json(path);
    std::filesystem::remove(path);
    return ct;
}

} // namespace

TEST(TheaterTables, ResolveEcmBuiltinJammer) {
    const auto t = TheaterTables::parse(ecm_tables_json());
    const auto ct = load_ecm_ct();
    // The built-in leg: VCD Flags & VEH_HAS_JAMMER, no pod, the
    // documented 1.0-strength / 20-NM reference values.
    const auto fit = resolve_vehicle_ecm(t, ct, 101);
    ASSERT_TRUE(fit.has_value());
    EXPECT_TRUE(fit->builtin);
    EXPECT_FALSE(fit->pod);
    EXPECT_TRUE(fit->pod_name.empty());
    EXPECT_FLOAT_EQ(fit->strength, 1.0f);
    EXPECT_FLOAT_EQ(fit->burn_through_range_nm, 20.0f);
    EXPECT_TRUE(fit->carried());
}

TEST(TheaterTables, ResolveEcmPodFromWcdFlags) {
    const auto t = TheaterTables::parse(ecm_tables_json());
    const auto ct = load_ecm_ct();
    // The pod leg: a hardpoint weapon whose WCD Flags carry WEAP_ECM;
    // the pod names itself and seeds the burn-through from its own WCD
    // range_km (30 km × 0.5399568 NM/km ≈ 16.199 NM).
    const auto fit = resolve_vehicle_ecm(t, ct, 102);
    ASSERT_TRUE(fit.has_value());
    EXPECT_FALSE(fit->builtin);
    EXPECT_TRUE(fit->pod);
    EXPECT_EQ(fit->pod_name, "ALQ-131");
    EXPECT_FLOAT_EQ(fit->strength, 1.0f);
    EXPECT_FLOAT_EQ(fit->burn_through_range_nm, 30.0f * 0.5399568f);
}

TEST(TheaterTables, ResolveEcmDeclinesWithoutBits) {
    const auto t = TheaterTables::parse(ecm_tables_json());
    const auto ct = load_ecm_ct();
    // Neither bit → no fit (the data said so; the caller stamps
    // nothing).
    EXPECT_FALSE(resolve_vehicle_ecm(t, ct, 103).has_value());
    // Out-of-range entity types decline.
    EXPECT_FALSE(resolve_vehicle_ecm(t, ct, 9999).has_value());
    // Empty tables decline (and never throw).
    const auto empty = TheaterTables::parse(
        "{ \"format\": \"f4.theater.tables/1\", \"units\": [],"
        " \"vehicles\": [], \"weapons\": [] }");
    EXPECT_FALSE(resolve_vehicle_ecm(empty, ct, 101).has_value());
}
