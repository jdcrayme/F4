// f4-convert/tests/test_formation_parser.cpp
//
// Tests for the FORMDAT.FIL parser. Anchored against the SHIPPED game
// fixture (fixtures/simdata/FORMDAT.FIL — the nine formations
// ACFormationData loads: spread, wedge, trail, ladder, stack, rescell,
// box, arrowhead, fluid).

#include "f4/convert/formation_parser.hpp"

#include <gtest/gtest.h>

#include <string>

using namespace f4::convert;
using namespace f4::data;

namespace {

FormationParseResult loadShippedFixture() {
    return loadFormFile(F4_CONVERT_TEST_FIXTURES_DIR "/simdata/FORMDAT.FIL");
}

} // namespace

TEST(FormationParser, ShippedFileParsesNineFormations) {
    auto r = loadShippedFixture();
    ASSERT_TRUE(r.ok);
    ASSERT_EQ(r.data.formations.size(), 9u);
}

TEST(FormationParser, NamesAndFormNumsMatchWingManCmdEnum) {
    auto r = loadShippedFixture();
    ASSERT_TRUE(r.ok);
    // formNum == the WingManCmd enum value (wingmanmsg.h:20-29).
    const struct { const char* name; int num; } expected[9] = {
        {"spread", 0}, {"wedge", 1}, {"trail", 2}, {"ladder", 3},
        {"stack", 4},  {"rescell", 5}, {"box", 6}, {"arrowhead", 7},
        {"fluid", 8},
    };
    for (const auto& e : expected) {
        const auto* f = r.data.find_by_name(e.name);
        ASSERT_NE(f, nullptr) << e.name;
        EXPECT_EQ(f->form_num, e.num) << e.name;
    }
}

TEST(FormationParser, WedgeSlotsMatchFile) {
    auto r = loadShippedFixture();
    ASSERT_TRUE(r.ok);
    const auto* w = r.data.find_by_name("wedge");
    ASSERT_NE(w, nullptr);
    ASSERT_EQ(w->slots.size(), 3u);
    // wedge slots: 135/0/0.165, -135/0/1.0, -135/0/1.165
    EXPECT_DOUBLE_EQ(w->slots[0].rel_az_deg, 135.0);
    EXPECT_DOUBLE_EQ(w->slots[0].rel_el_deg, 0.0);
    EXPECT_DOUBLE_EQ(w->slots[0].range_nm, 0.165);
    EXPECT_DOUBLE_EQ(w->slots[1].rel_az_deg, -135.0);
    EXPECT_DOUBLE_EQ(w->slots[1].range_nm, 1.0);
    EXPECT_DOUBLE_EQ(w->slots[2].rel_az_deg, -135.0);
    EXPECT_DOUBLE_EQ(w->slots[2].range_nm, 1.165);
    // num2Slots == 0 for wedge: the 2-ship slot inherits slot[0]
    // (formdata.cpp:85-91) and is flagged non-explicit.
    EXPECT_FALSE(w->two_ship_explicit);
    EXPECT_DOUBLE_EQ(w->two_ship.rel_az_deg, 135.0);
    EXPECT_DOUBLE_EQ(w->two_ship.range_nm, 0.165);
}

TEST(FormationParser, TrailHasTheExplicitTwoShipSlot) {
    auto r = loadShippedFixture();
    ASSERT_TRUE(r.ok);
    const auto* t = r.data.find_by_name("trail");
    ASSERT_NE(t, nullptr);
    // "3 1 2 trail": 3 four-ship slots + 1 explicit 2-ship triple.
    ASSERT_EQ(t->slots.size(), 3u);
    EXPECT_TRUE(t->two_ship_explicit);
    EXPECT_DOUBLE_EQ(t->two_ship.rel_az_deg, 180.0);
    EXPECT_DOUBLE_EQ(t->two_ship.range_nm, 2.0);
}

TEST(FormationParser, LadderIsBehindAndAbove) {
    auto r = loadShippedFixture();
    ASSERT_TRUE(r.ok);
    const auto* l = r.data.find_by_name("ladder");
    ASSERT_NE(l, nullptr);
    // ladder slots: 180 deg az at 45 deg el — climbing up behind.
    for (const auto& s : l->slots) {
        EXPECT_DOUBLE_EQ(s.rel_az_deg, 180.0);
        EXPECT_DOUBLE_EQ(s.rel_el_deg, 45.0);
    }
}

TEST(FormationParser, StackIsStraightBelow) {
    auto r = loadShippedFixture();
    ASSERT_TRUE(r.ok);
    const auto* st = r.data.find_by_name("stack");
    ASSERT_NE(st, nullptr);
    for (const auto& s : st->slots) {
        EXPECT_DOUBLE_EQ(s.rel_az_deg, 0.0);
        EXPECT_DOUBLE_EQ(s.rel_el_deg, -90.0);
    }
}

TEST(FormationParser, FindByFormNumSearch) {
    auto r = loadShippedFixture();
    ASSERT_TRUE(r.ok);
    // FindFormation(msgNum) semantics: formNum lookup.
    EXPECT_NE(r.data.find_by_form_num(0), nullptr);
    EXPECT_EQ(r.data.find_by_form_num(9), nullptr);
    EXPECT_STREQ(r.data.find_by_form_num(2)->name.c_str(), "trail");
}

TEST(FormationParser, UnitAccessorsMatchReferenceConversions) {
    auto r = loadShippedFixture();
    ASSERT_TRUE(r.ok);
    const auto* w = r.data.find_by_name("wedge");
    ASSERT_NE(w, nullptr);
    // formdata.cpp:59-61 conversions: DTR + NM_TO_FT (6076.211).
    EXPECT_NEAR(w->slots[0].az_rad(), 135.0 * 0.017453292519943295, 1e-12);
    EXPECT_NEAR(w->slots[0].range_ft(), 0.165 * 6076.211, 1e-6);
    EXPECT_DOUBLE_EQ(kNmToFt, 6076.211);
}

TEST(FormationParser, JSONRoundTripPreservesFormations) {
    auto r = loadShippedFixture();
    ASSERT_TRUE(r.ok);
    const std::string json = f4::data::writeFormationLibrary(r.data);
    auto back = f4::data::loadFormationLibraryFromString(json);
    ASSERT_TRUE(back.ok) << back.errors.front();
    ASSERT_EQ(back.data.formations.size(), r.data.formations.size());
    for (std::size_t i = 0; i < r.data.formations.size(); ++i) {
        const auto& a = r.data.formations[i];
        const auto& b = back.data.formations[i];
        EXPECT_EQ(b.name, a.name);
        EXPECT_EQ(b.form_num, a.form_num);
        EXPECT_EQ(b.two_ship_explicit, a.two_ship_explicit);
        ASSERT_EQ(b.slots.size(), a.slots.size());
        for (std::size_t s = 0; s < a.slots.size(); ++s) {
            EXPECT_DOUBLE_EQ(b.slots[s].rel_az_deg, a.slots[s].rel_az_deg);
            EXPECT_DOUBLE_EQ(b.slots[s].rel_el_deg, a.slots[s].rel_el_deg);
            EXPECT_DOUBLE_EQ(b.slots[s].range_nm, a.slots[s].range_nm);
        }
        EXPECT_DOUBLE_EQ(b.two_ship.rel_az_deg, a.two_ship.rel_az_deg);
        EXPECT_DOUBLE_EQ(b.two_ship.range_nm, a.two_ship.range_nm);
    }
}

// ============================================================================
// Synthetic edge cases
// ============================================================================

TEST(FormationParser, SyntheticMinimalFile) {
    const std::string src =
        "2\n"
        "2 1 7 mini\n"        // 2 slots + explicit 2-ship
        "10.0 0.0 1.0\n"      // slot 1
        "-10.0 5.0 2.0\n"     // slot 2
        "20.0 -5.0 3.0\n"     // the 2-ship triple
        "1 0 0 solo\n"        // 1 slot, twopos defaults to slot 0
        "90.0 0.0 0.5\n";
    auto r = loadFormString(src);
    ASSERT_TRUE(r.ok) << (r.errors.empty() ? "" : r.errors[0]);
    ASSERT_EQ(r.data.formations.size(), 2u);
    const auto* mini = r.data.find_by_name("mini");
    ASSERT_NE(mini, nullptr);
    EXPECT_TRUE(mini->two_ship_explicit);
    EXPECT_DOUBLE_EQ(mini->two_ship.rel_az_deg, 20.0);
    const auto* solo = r.data.find_by_name("solo");
    ASSERT_NE(solo, nullptr);
    EXPECT_FALSE(solo->two_ship_explicit);
    EXPECT_DOUBLE_EQ(solo->two_ship.rel_az_deg, 90.0);
}

TEST(FormationParser, MissingTokensIsAnError) {
    const std::string src = "1\n2 0 0 x\n10 0\n";  // truncated slot
    auto r = loadFormString(src);
    EXPECT_FALSE(r.ok);
}

TEST(FormationParser, BadTwoShipCountIsAnError) {
    const std::string src = "1\n2 2 0 x\n10 0 1\n-10 0 1\n5 0 1\n5 0 1\n";
    auto r = loadFormString(src);
    EXPECT_FALSE(r.ok);
}

TEST(FormationParser, ImplausibleGeometryIsAnError) {
    const std::string src = "1\n1 0 0 x\n10 95.0 1.0\n";  // el > 90
    auto r = loadFormString(src);
    EXPECT_FALSE(r.ok);
}

TEST(FormationParser, EmptyFileIsAnError) {
    auto r = loadFormString("");
    EXPECT_FALSE(r.ok);
}

TEST(FormationParser, TrailingTokensWarn) {
    const std::string src =
        "1\n1 0 0 x\n90.0 0.0 0.5\nJUNKTOKEN\n";
    auto r = loadFormString(src);
    ASSERT_TRUE(r.ok);
    ASSERT_EQ(r.warnings.size(), 1u);
    EXPECT_NE(r.warnings[0].find("trailing"), std::string::npos);
}
