// f4-convert/tests/test_brain_parser.cpp
//
// Tests for the .brn parser. Anchored against the SHIPPED game fixtures
// (fixtures/simdata/BRAINDAT.brn — 8 named archetypes — and
// GENERIC.BRN — one bare archetype + the Max Gs trailer).

#include "f4/convert/brain_parser.hpp"

#include <gtest/gtest.h>

#include <string>

using namespace f4::convert;
using namespace f4::data;

namespace {

BrainParseResult loadShippedBrainData() {
    return loadBrainFile(F4_CONVERT_TEST_FIXTURES_DIR "/simdata/BRAINDAT.brn");
}

BrainParseResult loadShippedGeneric() {
    return loadBrainFile(F4_CONVERT_TEST_FIXTURES_DIR "/simdata/GENERIC.BRN");
}

} // namespace

// ============================================================================
// BRAINDAT.brn — the 8-archetype file
// ============================================================================

TEST(BrainParser, ShippedFileParsesEightArchetypes) {
    auto r = loadShippedBrainData();
    ASSERT_TRUE(r.ok);
    ASSERT_EQ(r.data.archetypes.size(), 8u);
    EXPECT_EQ(r.data.archetypes[0].name, "Generic");
    EXPECT_EQ(r.data.archetypes[1].name, "SEAD");
    EXPECT_EQ(r.data.archetypes[2].name, "Strike");
    EXPECT_EQ(r.data.archetypes[3].name, "Intercepter");
    EXPECT_EQ(r.data.archetypes[4].name, "Air CAP");
    EXPECT_EQ(r.data.archetypes[5].name, "Air Sweep");
    EXPECT_EQ(r.data.archetypes[6].name, "Escort");
    EXPECT_EQ(r.data.archetypes[7].name, "Waypointer");
}

TEST(BrainParser, GenericArchetypeRowsMatchFile) {
    auto r = loadShippedBrainData();
    ASSERT_TRUE(r.ok);
    const auto& g = r.data.archetypes[0];
    // 25 positional rows in the shipped Generic section.
    ASSERT_EQ(g.modes.size(), 25u);
    // Row 0: GroundAvoidMode / 1 / 0.5 0 0
    EXPECT_EQ(g.modes[0].label, "GroundAvoidMode");
    EXPECT_EQ(g.modes[0].enabled, 1);
    EXPECT_DOUBLE_EQ(g.modes[0].priority, 0.5);
    // The GunsEngage row: 1.0 / 6000 ft / 45 deg.
    const auto* guns = g.find_mode(BrainModeKey::GunsEngage);
    ASSERT_NE(guns, nullptr);
    EXPECT_EQ(guns->enabled, 1);
    EXPECT_DOUBLE_EQ(guns->priority, 1.0);
    EXPECT_DOUBLE_EQ(guns->range_ft, 6000.0);
    EXPECT_DOUBLE_EQ(guns->angle_deg, 45.0);
    // The WVR row: 3.0 / 50000 ft / 180 deg.
    const auto* wvr = g.find_mode(BrainModeKey::WVREngage);
    ASSERT_NE(wvr, nullptr);
    EXPECT_EQ(wvr->enabled, 1);
    EXPECT_DOUBLE_EQ(wvr->priority, 3.0);
    EXPECT_DOUBLE_EQ(wvr->range_ft, 50000.0);
    EXPECT_DOUBLE_EQ(wvr->angle_deg, 180.0);
    // The LastValidMode sentinel row is present but disabled.
    const auto* last = g.find_mode(BrainModeKey::LastValid);
    ASSERT_NE(last, nullptr);
    EXPECT_EQ(last->enabled, 0);
}

TEST(BrainParser, GenericFindModeMatchesBlankLabels) {
    auto r = loadShippedBrainData();
    ASSERT_TRUE(r.ok);
    const auto& g = r.data.archetypes[0];
    // The file carries three blank "# " rows — they parse with empty
    // labels and stay positional.
    std::size_t blank = 0;
    for (const auto& m : g.modes) {
        if (m.label.empty()) ++blank;
    }
    EXPECT_GE(blank, 3u);
}

TEST(BrainParser, SeadArchetypeIsDefensiveOnly) {
    auto r = loadShippedBrainData();
    ASSERT_TRUE(r.ok);
    const auto* sead = r.data.find_archetype("SEAD");
    ASSERT_NE(sead, nullptr);
    // SEAD: never engages (all engage modes disabled), still defends
    // (MissileDefeat on with a 50000 ft / 180 deg gate) and still holds
    // formation (WingyMode on).
    EXPECT_FALSE(sead->mode_enabled(BrainModeKey::GunsEngage));
    EXPECT_FALSE(sead->mode_enabled(BrainModeKey::MissileEngage));
    EXPECT_FALSE(sead->mode_enabled(BrainModeKey::WVREngage));
    EXPECT_FALSE(sead->mode_enabled(BrainModeKey::BVREngage));
    EXPECT_TRUE(sead->mode_enabled(BrainModeKey::MissileDefeat));
    EXPECT_TRUE(sead->mode_enabled(BrainModeKey::Wingy));
    const auto* md = sead->find_mode(BrainModeKey::MissileDefeat);
    ASSERT_NE(md, nullptr);
    EXPECT_DOUBLE_EQ(md->range_ft, 50000.0);
    EXPECT_DOUBLE_EQ(md->angle_deg, 180.0);
}

TEST(BrainParser, SeadCarriesTheDuplicatedGroundMnvrRow) {
    // The shipped file's SEAD section has 26 rows: "# GroundMnvr"
    // appears twice (a 1997 editing quirk). Rows are positional; label
    // lookup returns the FIRST match.
    auto r = loadShippedBrainData();
    ASSERT_TRUE(r.ok);
    const auto* sead = r.data.find_archetype("SEAD");
    ASSERT_NE(sead, nullptr);
    EXPECT_EQ(sead->modes.size(), 26u);
    std::size_t gm = 0;
    for (const auto& m : sead->modes) {
        if (m.label == "GroundMnvr") ++gm;
    }
    EXPECT_EQ(gm, 2u);
    const auto* first = sead->find_mode(BrainModeKey::GroundMnvr);
    ASSERT_NE(first, nullptr);
    EXPECT_EQ(first->row, 23u);
}

TEST(BrainParser, TolerantLabelMatching) {
    auto r = loadShippedBrainData();
    ASSERT_TRUE(r.ok);
    const auto& g = r.data.archetypes[0];
    // The tag row "#Defensive Modes - This is a tag" matches the
    // Defensive key through the canonicalizer.
    const auto* def = g.find_mode(BrainModeKey::Defensive);
    EXPECT_NE(def, nullptr);
    // "GroundMnvr" == "ground mnvr" after canonicalization.
    const auto* gm = g.find_mode(BrainModeKey::GroundMnvr);
    ASSERT_NE(gm, nullptr);
    EXPECT_EQ(gm->label, "GroundMnvr");
    // Missing keys return nullptr.
    EXPECT_EQ(g.find_mode(BrainModeKey::Bugout), nullptr);
    EXPECT_EQ(g.find_mode(BrainModeKey::Overshoot), nullptr);
}

TEST(BrainParser, GenericFallbackAccessor) {
    auto r = loadShippedBrainData();
    ASSERT_TRUE(r.ok);
    EXPECT_NE(r.data.generic(), nullptr);
    EXPECT_STREQ(r.data.generic()->name.c_str(), "Generic");
    // Case-insensitive lookup.
    EXPECT_NE(r.data.find_archetype("sead"), nullptr);
    EXPECT_EQ(r.data.find_archetype("nope"), nullptr);
}

// ============================================================================
// GENERIC.BRN — the bare single-archetype file
// ============================================================================

TEST(BrainParser, GenericBrnParsesAsSingleArchetype) {
    auto r = loadShippedGeneric();
    ASSERT_TRUE(r.ok);
    ASSERT_EQ(r.data.archetypes.size(), 1u);
    EXPECT_EQ(r.data.archetypes[0].name, "Generic");
    // 22 mode rows + no count header.
    EXPECT_EQ(r.data.archetypes[0].modes.size(), 22u);
}

TEST(BrainParser, GenericBrnMaxGsTrailer) {
    auto r = loadShippedGeneric();
    ASSERT_TRUE(r.ok);
    EXPECT_DOUBLE_EQ(r.max_gs, 9.0);
}

TEST(BrainParser, GenericBrnOvershootAndAccelerate) {
    auto r = loadShippedGeneric();
    ASSERT_TRUE(r.ok);
    const auto& g = r.data.archetypes[0];
    // GENERIC.BRN's own spellings: OvershootMode (not OverBMode) and
    // AccelerateMode.
    const auto* ov = g.find_mode(BrainModeKey::Overshoot);
    ASSERT_NE(ov, nullptr);
    EXPECT_EQ(ov->enabled, 0);
    const auto* ac = g.find_mode(BrainModeKey::Accelerate);
    ASSERT_NE(ac, nullptr);
    EXPECT_EQ(ac->enabled, 1);
    EXPECT_DOUBLE_EQ(ac->priority, 2.0);
}

// ============================================================================
// Synthetic edge cases
// ============================================================================

TEST(BrainParser, SyntheticMinimalFile) {
    const std::string src =
        "# Tiny\n"
        "1\n"
        "# Alpha\n"
        "# GroundAvoidMode\n1\n0.5 0 0\n"
        "# GunsEngageMode\n1\n1.0 6000 45\n"
        "# Beta\n"
        "# WVREngageMode\n0\n3.0 50000 180\n";
    auto r = loadBrainString(src);
    ASSERT_TRUE(r.ok) << (r.errors.empty() ? "" : r.errors[0]);
    ASSERT_EQ(r.data.archetypes.size(), 2u);
    EXPECT_EQ(r.data.archetypes[0].name, "Alpha");
    EXPECT_EQ(r.data.archetypes[0].modes.size(), 2u);
    EXPECT_EQ(r.data.archetypes[1].name, "Beta");
    EXPECT_EQ(r.data.archetypes[1].modes.size(), 1u);
    EXPECT_FALSE(r.data.archetypes[1].mode_enabled(BrainModeKey::WVREngage));
}

TEST(BrainParser, BareRowsSynthesizeGenericArchetype) {
    const std::string src =
        "# WingyMode\n1\n10.0 0 0\n";
    auto r = loadBrainString(src);
    ASSERT_TRUE(r.ok);
    ASSERT_EQ(r.data.archetypes.size(), 1u);
    EXPECT_EQ(r.data.archetypes[0].name, "Generic");
    EXPECT_EQ(r.data.archetypes[0].modes.size(), 1u);
}

TEST(BrainParser, MissingTripleIsAnError) {
    const std::string src =
        "1\n"
        "# Alpha\n"
        "# GroundAvoidMode\n1\n";
    auto r = loadBrainString(src);
    EXPECT_FALSE(r.ok);
    EXPECT_FALSE(r.errors.empty());
}

TEST(BrainParser, MalformedTripleIsAnError) {
    const std::string src =
        "1\n"
        "# Alpha\n"
        "# GroundAvoidMode\n1\none two\n";
    auto r = loadBrainString(src);
    EXPECT_FALSE(r.ok);
}

TEST(BrainParser, JSONRoundTripPreservesArchetypes) {
    auto r = loadShippedBrainData();
    ASSERT_TRUE(r.ok);
    const std::string json = f4::data::writeBrainData(r.data);
    auto back = f4::data::loadBrainDataFromString(json);
    ASSERT_TRUE(back.ok) << back.errors.front();
    ASSERT_EQ(back.data.archetypes.size(), r.data.archetypes.size());
    for (std::size_t i = 0; i < r.data.archetypes.size(); ++i) {
        EXPECT_EQ(back.data.archetypes[i].name,
                  r.data.archetypes[i].name);
        ASSERT_EQ(back.data.archetypes[i].modes.size(),
                  r.data.archetypes[i].modes.size());
        for (std::size_t m = 0; m < r.data.archetypes[i].modes.size(); ++m) {
            const auto& a = r.data.archetypes[i].modes[m];
            const auto& b = back.data.archetypes[i].modes[m];
            EXPECT_EQ(b.label, a.label);
            EXPECT_EQ(b.enabled, a.enabled);
            EXPECT_DOUBLE_EQ(b.priority, a.priority);
            EXPECT_DOUBLE_EQ(b.range_ft, a.range_ft);
            EXPECT_DOUBLE_EQ(b.angle_deg, a.angle_deg);
            EXPECT_EQ(b.row, a.row);
        }
    }
}
