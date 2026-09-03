// f4-convert/tests/test_veh_parser.cpp
//
// Tests for the Vehicle.lst / .veh parser. Anchored against the SHIPPED
// game fixture (fixtures/simdata/VehDef/ — Vehicle.lst's 86 rows: 23
// aircraft, 3 ground, 2 helo, 53 weapon, 1 unused, 4 sea + the typo'd
// sea row that never opens its file).

#include "f4/convert/veh_parser.hpp"

#include <gtest/gtest.h>

#include <string>

using namespace f4::convert;
using namespace f4::data;

namespace {

VehParseResult loadShippedFixture() {
    return loadVehicleLstFile(
        F4_CONVERT_TEST_FIXTURES_DIR "/simdata/VehDef/Vehicle.lst");
}

} // namespace

TEST(VehParser, ShippedListParsesAllRows) {
    auto r = loadShippedFixture();
    ASSERT_TRUE(r.ok) << (r.errors.empty() ? "" : r.errors[0]);
    ASSERT_EQ(r.library.entries.size(), 86u);
}

TEST(VehParser, RowCountsByType) {
    auto r = loadShippedFixture();
    ASSERT_TRUE(r.ok);
    // Counted from the shipped Vehicle.lst (rows 1-86).
    EXPECT_EQ(r.library.count_of_type(MoverType::Aircraft), 23u);
    EXPECT_EQ(r.library.count_of_type(MoverType::Ground), 3u);
    EXPECT_EQ(r.library.count_of_type(MoverType::Helicopter), 2u);
    EXPECT_EQ(r.library.count_of_type(MoverType::Weapon), 53u);
    EXPECT_EQ(r.library.count_of_type(MoverType::Sea), 4u);
    EXPECT_EQ(r.library.count_of_type(MoverType::Unused), 1u);
    EXPECT_EQ(r.library.entries.size(),
              23u + 3u + 2u + 53u + 4u + 1u);
}

TEST(VehParser, F16AircraftDefinitionMatchesFile) {
    auto r = loadShippedFixture();
    ASSERT_TRUE(r.ok);
    const auto* f16 = r.library.find("f16");
    ASSERT_NE(f16, nullptr);
    ASSERT_NE(f16->aircraft(), nullptr);

    // f16.veh: combat class 4 (CombatClass F16 — acdef.h:23-34, the
    // 5th entry), airframe index 4 (ACTYPES.LST order F4 F5 F14 F15 F16).
    EXPECT_EQ(f16->aircraft()->combat_class, 4);
    EXPECT_STREQ(kCombatClassNames[f16->aircraft()->combat_class], "F16");
    EXPECT_EQ(f16->aircraft()->airframe_index, 4);

    // Player loadout (f16.veh "Number of Sensors if Player AC" = 4):
    // Visual(3), Radar(1), IRST(0), RWR(2).
    ASSERT_EQ(f16->aircraft()->player_sensors.size(), 4u);
    EXPECT_EQ(f16->aircraft()->player_sensors[0],
              (SensorSlot{3, 0}));
    EXPECT_EQ(f16->aircraft()->player_sensors[1],
              (SensorSlot{1, 0}));
    EXPECT_EQ(f16->aircraft()->player_sensors[2],
              (SensorSlot{0, 0}));
    EXPECT_EQ(f16->aircraft()->player_sensors[3],
              (SensorSlot{2, 0}));

    // AI loadout ("Number of Sensors if not Player AC" = 3): Visual,
    // Radar, RWR — no IRST for the AI F-16, exactly the reference's
    // vehdef.cpp reads.
    ASSERT_EQ(f16->aircraft()->ai_sensors.size(), 3u);
    EXPECT_EQ(f16->aircraft()->ai_sensors[0], (SensorSlot{3, 0}));
    EXPECT_EQ(f16->aircraft()->ai_sensors[1], (SensorSlot{1, 0}));
    EXPECT_EQ(f16->aircraft()->ai_sensors[2], (SensorSlot{2, 0}));
}

TEST(VehParser, HeloAndGroundRowsParse) {
    auto r = loadShippedFixture();
    ASSERT_TRUE(r.ok);
    // "2 sim\vehdef\ah64.veh" — a generic-helo card: airframe index 0
    // + one Visual sensor (the file's own header says "This file is for
    // a generic helo").
    const auto* ah64 = r.library.find("ah64");
    ASSERT_NE(ah64, nullptr);
    ASSERT_NE(ah64->helo(), nullptr);
    EXPECT_EQ(ah64->helo()->airframe_index, 0);
    ASSERT_EQ(ah64->helo()->sensors.size(), 1u);
    EXPECT_EQ(ah64->helo()->sensors[0], (SensorSlot{3, 0}));

    // "1 Sim\VehDef\tank.veh" — ground: Visual + Radar.
    const auto* tank = r.library.find("tank");
    ASSERT_NE(tank, nullptr);
    ASSERT_NE(tank->ground(), nullptr);
    ASSERT_EQ(tank->ground()->sensors.size(), 2u);
    EXPECT_EQ(tank->ground()->sensors[0], (SensorSlot{3, 0}));
    EXPECT_EQ(tank->ground()->sensors[1], (SensorSlot{1, 0}));
}

TEST(VehParser, Sa6WeaponDefinitionMatchesFile) {
    auto r = loadShippedFixture();
    ASSERT_TRUE(r.ok);
    const auto* sa6 = r.library.find("sa6");
    ASSERT_NE(sa6, nullptr);
    ASSERT_NE(sa6->weapon(), nullptr);

    // sa6.veh: flags 0, cd 0.02, weight 225.0, area 0.4, ejection
    // 0/0/0, mnemonic SA6, class 8 (wcSamWpn), domain 1 (wdAir),
    // type 8 (wtSAM), data idx 6.
    EXPECT_EQ(sa6->weapon()->flags, 0);
    EXPECT_DOUBLE_EQ(sa6->weapon()->cd, 0.02);
    EXPECT_DOUBLE_EQ(sa6->weapon()->weight, 225.0);
    EXPECT_DOUBLE_EQ(sa6->weapon()->area, 0.4);
    EXPECT_DOUBLE_EQ(sa6->weapon()->x_ejection, 0.0);
    EXPECT_EQ(sa6->weapon()->mnemonic, "SA6");
    EXPECT_EQ(sa6->weapon()->weapon_class, 8);
    EXPECT_STREQ(kWeaponClassNames[8], "wcSamWpn");
    EXPECT_EQ(sa6->weapon()->domain, wdAir);
    EXPECT_EQ(sa6->weapon()->weapon_type, 8);
    EXPECT_STREQ(kWeaponTypeName[8], "wtSAM");
    EXPECT_EQ(sa6->weapon()->data_idx, 6);
}

TEST(VehParser, SeaRowsNeverOpenTheirFiles) {
    auto r = loadShippedFixture();
    ASSERT_TRUE(r.ok);
    // The shipped list's Sea rows reference ship.veh / sub.veh /
    // torpedo.veh / "dpthchrg,veh" — none exist in the zip and none are
    // opened (vehdef.cpp:74-78 reads the name and discards it). The
    // typo'd comma row must parse as a row, not an error.
    EXPECT_EQ(r.library.count_of_type(MoverType::Sea), 4u);
    bool sawTypo = false;
    for (const auto& e : r.library.entries) {
        if (e.type == MoverType::Sea && e.file.find("dpthchrg") != std::string::npos) {
            sawTypo = true;
            EXPECT_FALSE(e.has_definition());
        }
    }
    EXPECT_TRUE(sawTypo) << "the shipped 'dpthchrg,veh' typo row is missing";
}

TEST(VehParser, DuplicateStemWarnsAndFindReturnsFirst) {
    auto r = loadShippedFixture();
    ASSERT_TRUE(r.ok);
    // f16.veh is listed twice (entries 2 and 41 — both aircraft rows).
    std::size_t f16rows = 0;
    for (const auto& e : r.library.entries) {
        if (e.name == "f16") ++f16rows;
    }
    EXPECT_EQ(f16rows, 2u);
    bool warned = false;
    for (const auto& w : r.warnings) {
        if (w.find("duplicate stem 'f16'") != std::string::npos) warned = true;
    }
    EXPECT_TRUE(warned);
}

TEST(VehParser, CaseInsensitiveAndBackslashPathsResolve) {
    auto r = loadShippedFixture();
    ASSERT_TRUE(r.ok);
    // The list mixes "Sim\VehDef\f16.veh", "sim\vehdef\tu16.veh" and
    // "0 sim\vehdef\an2.veh" — every definition row resolved against
    // the fixture dir's actual (lowercase) files.
    for (const char* name : {"tu16", "an2", "kc10", "Mig29", "F15"}) {
        const auto* e = r.library.find(name);
        ASSERT_NE(e, nullptr) << name;
        EXPECT_TRUE(e->has_definition()) << name;
    }
}

TEST(VehParser, WeaponMnemonicSurvives) {
    auto r = loadShippedFixture();
    ASSERT_TRUE(r.ok);
    // Aim120.veh's mnemonic is "A120" (not "Aim120").
    const auto* a120 = r.library.find("aim120");
    ASSERT_NE(a120, nullptr);
    ASSERT_NE(a120->weapon(), nullptr);
    EXPECT_EQ(a120->weapon()->mnemonic, "A120");
    EXPECT_EQ(a120->weapon()->weapon_class, 0);
    EXPECT_STREQ(kWeaponClassNames[0], "wcAimWpn");
}

TEST(VehParser, JSONRoundTripPreservesLibrary) {
    auto r = loadShippedFixture();
    ASSERT_TRUE(r.ok);
    const std::string json = writeVehicleDefinitionLibrary(r.library);
    auto back = loadVehicleDefinitionLibraryFromString(json);
    ASSERT_TRUE(back.ok) << (back.errors.empty() ? "" : back.errors[0]);
    ASSERT_EQ(back.library.entries.size(), r.library.entries.size());

    const auto* f16 = back.library.find("f16");
    ASSERT_NE(f16, nullptr);
    ASSERT_NE(f16->aircraft(), nullptr);
    EXPECT_EQ(f16->aircraft()->combat_class, 4);
    EXPECT_EQ(f16->aircraft()->player_sensors.size(), 4u);
    EXPECT_EQ(f16->aircraft()->player_sensors[0], (SensorSlot{3, 0}));

    const auto* sa6 = back.library.find("sa6");
    ASSERT_NE(sa6, nullptr);
    ASSERT_NE(sa6->weapon(), nullptr);
    EXPECT_DOUBLE_EQ(sa6->weapon()->weight, 225.0);
    EXPECT_EQ(sa6->weapon()->mnemonic, "SA6");
}

TEST(VehParser, JSONRejectsWrongKind) {
    auto back = loadVehicleDefinitionLibraryFromString(
        R"({"kind": "f4.formdata", "version": 1})");
    EXPECT_FALSE(back.ok);
    ASSERT_FALSE(back.errors.empty());
}

TEST(VehParser, SyntheticAircraftVehParses) {
    const auto r = loadVehString(
        "# combat class\n2\n"
        "# airframe\n7\n"
        "# player sensors\n1\n3 0\n"
        "# AI sensors\n2\n1 0\n2 0\n",
        MoverType::Aircraft, "synthetic.veh");
    ASSERT_TRUE(r.ok);
    ASSERT_EQ(r.library.entries.size(), 1u);
    const auto* d = r.library.entries[0].aircraft();
    ASSERT_NE(d, nullptr);
    EXPECT_EQ(d->combat_class, 2);
    EXPECT_EQ(d->airframe_index, 7);
    EXPECT_EQ(d->player_sensors.size(), 1u);
    EXPECT_EQ(d->ai_sensors.size(), 2u);
}

TEST(VehParser, MissingTokensIsAnError) {
    // Aircraft .veh truncated after combat class.
    auto r = loadVehString("4\n", MoverType::Aircraft, "bad.veh");
    EXPECT_FALSE(r.ok);
    EXPECT_FALSE(r.errors.empty());
}

TEST(VehParser, NegativeWeaponPhysicalsAreAnError) {
    auto r = loadVehString(
        "0\n-0.5\n150.0\n0.2\n0.0 0.0 0.0\nXX\n0\n1\n2\n1\n",
        MoverType::Weapon, "bad.veh");
    EXPECT_FALSE(r.ok);
}

TEST(VehParser, MissingVehFileIsAnError) {
    auto r = loadVehicleLstString(
        "1\n0 Sim\\VehDef\\nonexistent.veh\n", "", "<string>");
    EXPECT_FALSE(r.ok);
    EXPECT_FALSE(r.errors.empty());
}

TEST(VehParser, TrailingTokensInVehWarn) {
    auto r = loadVehString(
        "0\n0.01\n150.0\n0.2\n0.0 0.0 0.0\nA120\n0\n1\n2\n1\n99\n",
        MoverType::Weapon, "trailing.veh");
    ASSERT_TRUE(r.ok);
    ASSERT_FALSE(r.warnings.empty());
}

TEST(VehParser, ShippedAlqxxxQuirksMatchReferenceAtoi) {
    // The shipped ALQxxx.veh has its "# Data Idx" block LEADING instead
    // of trailing, shifting every field by one. The reference's
    // atoi()/atof() silently read the shift (mnemonic "0.0", class 0
    // from "ALXXX", domain 4, type 0, dataIdx 9); this parser produces
    // the SAME values and warns, instead of failing on shipped data.
    auto r = loadVehFile(
        F4_CONVERT_TEST_FIXTURES_DIR "/simdata/VehDef/ALQxxx.veh",
        MoverType::Weapon);
    ASSERT_TRUE(r.ok) << (r.errors.empty() ? "" : r.errors[0]);
    ASSERT_EQ(r.library.entries.size(), 1u);
    const auto* w = r.library.entries[0].weapon();
    ASSERT_NE(w, nullptr);
    // The reference's exact (garbage) read — fidelity over repair.
    EXPECT_EQ(w->flags, 0);
    EXPECT_DOUBLE_EQ(w->cd, 0.0);
    EXPECT_DOUBLE_EQ(w->weight, 0.02);
    EXPECT_DOUBLE_EQ(w->area, 100.0);
    EXPECT_DOUBLE_EQ(w->x_ejection, 0.4);
    EXPECT_EQ(w->mnemonic, "0.0");
    EXPECT_EQ(w->weapon_class, 0);
    EXPECT_EQ(w->domain, 4);
    EXPECT_EQ(w->weapon_type, 0);
    EXPECT_EQ(w->data_idx, 9);
    // And the shift is loud, not silent.
    bool warned = false;
    for (const auto& warn : r.warnings) {
        if (warn.find("atoi() reads 0") != std::string::npos) warned = true;
    }
    EXPECT_TRUE(warned);
}
