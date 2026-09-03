// f4-convert/tests/test_mnvr_parser.cpp
//
// Tests for the mnvrdata.dat parser. Anchored against the SHIPPED game
// fixture (fixtures/simdata/mnvrdata.dat, extracted from the install's
// SimData.zip) — the exact values FreeFalcon's ReadManeuverData would
// produce IF its one-byte file-type check accepted the shipped 'A'
// marker (see f4/data/maneuver_data.hpp for the quirk).

#include "f4/convert/mnvr_parser.hpp"

#include <gtest/gtest.h>

#include <string>

using namespace f4::convert;
using namespace f4::data;

namespace {

MnvrParseResult loadShippedFixture() {
    const std::string path =
        F4_CONVERT_TEST_FIXTURES_DIR "/simdata/mnvrdata.dat";
    return loadMnvFile(path);
}

} // namespace

// ============================================================================
// The shipped fixture
// ============================================================================

TEST(MnvrParser, ShippedFixtureParses) {
    auto r = loadShippedFixture();
    ASSERT_TRUE(r.ok);
    for (auto const& e : r.errors) std::cerr << "  " << e << "\n";
}

TEST(MnvrParser, ShippedFileCarriesTheReferenceAMarkerWarning) {
    auto r = loadShippedFixture();
    ASSERT_TRUE(r.ok);
    // The fidelity note: the file starts with 'A', FreeFalcon would warn
    // "Bad Maneuver Data File Format" and skip it. Our parser accepts the
    // marker and records the quirk.
    ASSERT_GE(r.warnings.size(), 1u);
    EXPECT_NE(r.warnings[0].find("file-type marker"), std::string::npos)
        << r.warnings[0];
}

TEST(MnvrParser, NineClassFlagsMatchTheFile) {
    auto r = loadShippedFixture();
    ASSERT_TRUE(r.ok);
    // Verbatim from the file: 0x724 0x737 0x727 0x727 0x737 0x524 0x524
    // 0x337 0x323
    EXPECT_EQ(r.data.classFlags[0], 0x724u);   // F4
    EXPECT_EQ(r.data.classFlags[1], 0x737u);   // F5
    EXPECT_EQ(r.data.classFlags[2], 0x727u);   // F14
    EXPECT_EQ(r.data.classFlags[3], 0x727u);   // F15
    EXPECT_EQ(r.data.classFlags[4], 0x737u);   // F16
    EXPECT_EQ(r.data.classFlags[5], 0x524u);   // Mig25
    EXPECT_EQ(r.data.classFlags[6], 0x524u);   // Mig27
    EXPECT_EQ(r.data.classFlags[7], 0x337u);   // A10
    EXPECT_EQ(r.data.classFlags[8], 0x323u);   // Bomber
}

TEST(MnvrParser, FlagBitDecoding) {
    auto r = loadShippedFixture();
    ASSERT_TRUE(r.ok);
    // F4 = 0x724 = Unloaded|Loaded|Snake jinks + TwoCircle + UseVertical;
    // NO LevelTurn, Slice, or OneCircle.
    EXPECT_TRUE (r.data.classCan(0, MnvrClassFlags::CanJinkUnloaded));
    EXPECT_TRUE (r.data.classCan(0, MnvrClassFlags::CanJinkLoaded));
    EXPECT_TRUE (r.data.classCan(0, MnvrClassFlags::CanJinkSnake));
    EXPECT_TRUE (r.data.classCan(0, MnvrClassFlags::CanTwoCircle));
    EXPECT_TRUE (r.data.classCan(0, MnvrClassFlags::CanUseVertical));
    EXPECT_FALSE(r.data.classCan(0, MnvrClassFlags::CanLevelTurn));
    EXPECT_FALSE(r.data.classCan(0, MnvrClassFlags::CanSlice));
    EXPECT_FALSE(r.data.classCan(0, MnvrClassFlags::CanOneCircle));
    // F5/F16 = 0x737: all eight DEFINED flags (the enum has gaps at
    // 0x8/0x40/0x80 — iterating raw bits would test undefined values).
    const MnvrClassFlags allFlags[] = {
        MnvrClassFlags::CanLevelTurn,   MnvrClassFlags::CanSlice,
        MnvrClassFlags::CanUseVertical, MnvrClassFlags::CanOneCircle,
        MnvrClassFlags::CanTwoCircle,   MnvrClassFlags::CanJinkSnake,
        MnvrClassFlags::CanJinkLoaded,  MnvrClassFlags::CanJinkUnloaded,
    };
    for (const auto f : allFlags) {
        EXPECT_TRUE(r.data.classCan(1, f)) << "flag 0x"
            << std::hex << static_cast<std::uint32_t>(f);
    }
    // Bounds guard.
    EXPECT_FALSE(r.data.classCan(9, MnvrClassFlags::CanTwoCircle));
}

TEST(MnvrParser, F4VF4CellMatchesFile) {
    auto r = loadShippedFixture();
    ASSERT_TRUE(r.ok);
    // F4 v F4: 2 intercepts (1 2), 2 merges (2 3), 2 reacts (2 3) — all
    // 1-based in the file, 0-based after the reference's "- 1".
    const auto* c = r.data.choice(0, 0);
    ASSERT_NE(c, nullptr);
    EXPECT_EQ(c->intercepts, (std::vector<int>{0, 1}));
    EXPECT_EQ(c->merges,     (std::vector<int>{1, 2}));
    EXPECT_EQ(c->spikeReacts, (std::vector<int>{1, 2}));
}

TEST(MnvrParser, F16VBomberAndMig25VF16Cells) {
    auto r = loadShippedFixture();
    ASSERT_TRUE(r.ok);
    // F16 v Bomber: 1 intercept (3), 2 merges (1 2), 2 reacts (1 2).
    const auto* fb = r.data.choice(4, 8);
    ASSERT_NE(fb, nullptr);
    EXPECT_EQ(fb->intercepts, (std::vector<int>{2}));
    EXPECT_EQ(fb->merges,     (std::vector<int>{0, 1}));
    EXPECT_EQ(fb->spikeReacts, (std::vector<int>{0, 1}));
    // Mig25 v F16: 1 intercept (3), 1 merge (1), 2 reacts (1 2).
    const auto* mf = r.data.choice(5, 4);
    ASSERT_NE(mf, nullptr);
    EXPECT_EQ(mf->intercepts, (std::vector<int>{2}));
    EXPECT_EQ(mf->merges,     (std::vector<int>{0}));
    EXPECT_EQ(mf->spikeReacts, (std::vector<int>{0, 1}));
}

TEST(MnvrParser, BomberCellsArePureReactives) {
    auto r = loadShippedFixture();
    ASSERT_TRUE(r.ok);
    // Bomber v anyone: 0 intercepts, 0 merges, 4 reacts (1 2 3 4) — the
    // bomber never merges; it spikes back.
    for (std::size_t j = 0; j < kNumMnvrClasses; ++j) {
        const auto* c = r.data.choice(8, j);
        ASSERT_NE(c, nullptr);
        EXPECT_TRUE(c->intercepts.empty()) << "col " << j;
        EXPECT_TRUE(c->merges.empty()) << "col " << j;
        EXPECT_EQ(c->spikeReacts, (std::vector<int>{0, 1, 2, 3}))
            << "col " << j;
    }
}

TEST(MnvrParser, EveryCellIsPopulated) {
    auto r = loadShippedFixture();
    ASSERT_TRUE(r.ok);
    EXPECT_EQ(r.data.populatedCells(), 81u);
    EXPECT_EQ(r.warnings.size(), 1u);  // only the 'A' marker warning
}

// ============================================================================
// Synthetic edge cases
// ============================================================================

TEST(MnvrParser, SyntheticMinimalTableRoundTrips) {
    // 1x-class-sized 9x9 with only F4 v F4 populated.
    std::string src =
        "# synthetic\n"
        "0x0\n";                    // F4 flags = none
    for (std::size_t j = 0; j < kNumMnvrClasses; ++j) {
        if (j == 0) {
            src += "2 1 1\n1 2\n3\n2\n";   // F4 v F4
        } else {
            src += "0 0 0\n";
        }
    }
    // ...and 8 more empty class rows (flags + 9 empty cells each)
    for (std::size_t i = 1; i < kNumMnvrClasses; ++i) {
        src += "0x0\n";
        for (std::size_t j = 0; j < kNumMnvrClasses; ++j) src += "0 0 0\n";
    }
    auto r = loadMnvString(src);
    ASSERT_TRUE(r.ok);
    EXPECT_EQ(r.data.populatedCells(), 1u);
    const auto* c = r.data.choice(0, 0);
    ASSERT_NE(c, nullptr);
    EXPECT_EQ(c->intercepts, (std::vector<int>{0, 1}));
    EXPECT_EQ(c->merges, (std::vector<int>{2}));
    EXPECT_EQ(c->spikeReacts, (std::vector<int>{1}));
}

TEST(MnvrParser, MissingTokensIsAnError) {
    std::string src = "0x724\n2 2 2\n1 2\n";  // truncated mid-cell
    auto r = loadMnvString(src);
    EXPECT_FALSE(r.ok);
    EXPECT_FALSE(r.errors.empty());
}

TEST(MnvrParser, OutOfRangeIndexIsAnError) {
    // merge index 9 (only 3 merge types exist).
    std::string row = "0 1 0\n9\n";
    std::string src = "0x1\n" + row;
    for (std::size_t j = 1; j < kNumMnvrClasses; ++j) src += "0 0 0\n";
    for (std::size_t i = 1; i < kNumMnvrClasses; ++i) {
        src += "0x0\n";
        for (std::size_t j = 0; j < kNumMnvrClasses; ++j) src += "0 0 0\n";
    }
    auto r = loadMnvString(src);
    EXPECT_FALSE(r.ok);
    EXPECT_FALSE(r.errors.empty());
}

TEST(MnvrParser, NonHexFlagsIsAnError) {
    std::string src = "zzz\n";
    for (std::size_t i = 0; i < kNumMnvrClasses * kNumMnvrClasses; ++i) {
        src += "0 0 0\n";
    }
    auto r = loadMnvString(src);
    EXPECT_FALSE(r.ok);
}

TEST(MnvrParser, PlainPoundPrefixFileParsesWithoutMarkerWarning) {
    // The ORIGINAL-format file (no 'A' marker, first byte '#'): the
    // reference's happy path — no marker warning.
    std::string src = "# header\n";
    for (std::size_t i = 0; i < kNumMnvrClasses; ++i) {
        src += "0x0\n";
        for (std::size_t j = 0; j < kNumMnvrClasses; ++j) src += "0 0 0\n";
    }
    auto r = loadMnvString(src);
    ASSERT_TRUE(r.ok);
    EXPECT_TRUE(r.warnings.empty());
}

TEST(MnvrParser, JSONRoundTripPreservesTable) {
    auto r = loadShippedFixture();
    ASSERT_TRUE(r.ok);
    const std::string json = f4::data::writeManeuverData(r.data);
    auto back = f4::data::loadManeuverDataFromString(json);
    ASSERT_TRUE(back.ok) << back.errors.front();
    for (std::size_t i = 0; i < kNumMnvrClasses; ++i) {
        EXPECT_EQ(back.data.classFlags[i], r.data.classFlags[i]);
        for (std::size_t j = 0; j < kNumMnvrClasses; ++j) {
            EXPECT_EQ(back.data.table[i][j].intercepts,
                      r.data.table[i][j].intercepts);
            EXPECT_EQ(back.data.table[i][j].merges,
                      r.data.table[i][j].merges);
            EXPECT_EQ(back.data.table[i][j].spikeReacts,
                      r.data.table[i][j].spikeReacts);
        }
    }
}

TEST(MnvrParser, JSONRejectsWrongKind) {
    auto r = f4::data::loadManeuverDataFromString("{\"kind\":\"f4.braindata\"}");
    EXPECT_FALSE(r.ok);
}
