// f4-convert/tests/test_signature_parser.cpp
//
// Tests for the SIGDATA signature grid parser. Anchored against the
// SHIPPED game fixture (fixtures/simdata/SIGDATA/ — SIGDATA.LST's one
// "generic" stem with .RCS/.IR0/.IR1/.IR2/.VIS grids) plus the grid
// interpolation semantics f4-sensors consumes (value_at).

#include "f4/convert/signature_parser.hpp"

#include <gtest/gtest.h>

#include <string>

using namespace f4::convert;
using namespace f4::data;

namespace {

SigParseResult loadShippedDir() {
    return loadSignatureDataDir(
        F4_CONVERT_TEST_FIXTURES_DIR "/simdata/SIGDATA");
}

} // namespace

TEST(SignatureParser, ShippedDirParsesOneSignatureSet) {
    auto r = loadShippedDir();
    ASSERT_TRUE(r.ok) << (r.errors.empty() ? "" : r.errors[0]);
    ASSERT_EQ(r.library.entries.size(), 1u);
    EXPECT_EQ(r.library.entries[0].name, "generic");
}

TEST(SignatureParser, GenericRcsGridMatchesFile) {
    auto r = loadShippedDir();
    ASSERT_TRUE(r.ok);
    const auto& rcs = r.library.entries[0].rcs;
    // -180/0/180 azimuth, -90/0/90 elevation, flat 10.0 m^2.
    ASSERT_EQ(rcs.azimuth_deg.size(), 3u);
    EXPECT_DOUBLE_EQ(rcs.azimuth_deg[0], -180.0);
    EXPECT_DOUBLE_EQ(rcs.azimuth_deg[1], 0.0);
    EXPECT_DOUBLE_EQ(rcs.azimuth_deg[2], 180.0);
    ASSERT_EQ(rcs.elevation_deg.size(), 3u);
    EXPECT_DOUBLE_EQ(rcs.elevation_deg[0], -90.0);
    ASSERT_EQ(rcs.values.size(), 3u);
    for (const auto& row : rcs.values) {
        ASSERT_EQ(row.size(), 3u);
        for (double v : row) EXPECT_DOUBLE_EQ(v, 10.0);
    }
}

TEST(SignatureParser, GenericIrGridsAreRearHot) {
    auto r = loadShippedDir();
    ASSERT_TRUE(r.ok);
    // IR0 (baseline): azimuth 0/90/180, el 0 row = 0.02 / 0.03 / 0.1 —
    // the rear (exhaust) is 5x the nose signature.
    const auto& ir0 = r.library.entries[0].ir0;
    ASSERT_EQ(ir0.azimuth_deg.size(), 3u);
    EXPECT_DOUBLE_EQ(ir0.azimuth_deg[0], 0.0);
    EXPECT_DOUBLE_EQ(ir0.azimuth_deg[2], 180.0);
    ASSERT_EQ(ir0.values.size(), 3u);
    EXPECT_DOUBLE_EQ(ir0.values[1][0], 0.02);
    EXPECT_DOUBLE_EQ(ir0.values[1][1], 0.03);
    EXPECT_DOUBLE_EQ(ir0.values[1][2], 0.1);

    // IR1 (afterburner) el 0 row: 0.2 / 0.5 / 0.1; IR2 (max): 0.8 /
    // 3.5 / 4.0.
    EXPECT_DOUBLE_EQ(r.library.entries[0].ir1.values[1][2], 0.1);
    EXPECT_DOUBLE_EQ(r.library.entries[0].ir2.values[1][0], 0.8);
    EXPECT_DOUBLE_EQ(r.library.entries[0].ir2.values[1][2], 4.0);
}

TEST(SignatureParser, GridInterpolationAtBreakpoints) {
    auto r = loadShippedDir();
    ASSERT_TRUE(r.ok);
    const auto& ir0 = r.library.entries[0].ir0;
    // Exact breakpoints return the file values verbatim.
    EXPECT_DOUBLE_EQ(ir0.value_at(0.0, 0.0), 0.02);
    EXPECT_DOUBLE_EQ(ir0.value_at(90.0, 0.0), 0.03);
    EXPECT_DOUBLE_EQ(ir0.value_at(180.0, 0.0), 0.1);
    EXPECT_DOUBLE_EQ(ir0.value_at(0.0, -90.0), 0.05);
    EXPECT_DOUBLE_EQ(ir0.value_at(180.0, 90.0), 0.05);
}

TEST(SignatureParser, GridInterpolationMidpoints) {
    auto r = loadShippedDir();
    ASSERT_TRUE(r.ok);
    const auto& ir0 = r.library.entries[0].ir0;
    // 45 az / 0 el: halfway between 0.02 and 0.03.
    EXPECT_DOUBLE_EQ(ir0.value_at(45.0, 0.0), 0.025);
    // 90 az / -45 el: halfway between the -90 row (0.05) and the 0 row
    // (0.03) at az 90.
    EXPECT_DOUBLE_EQ(ir0.value_at(90.0, -45.0), 0.04);
    // Full bilinear: 45 az / 45 el -> (0.02, 0.03, 0.05, 0.05) corners.
    EXPECT_DOUBLE_EQ(ir0.value_at(45.0, 45.0), 0.0375);
}

TEST(SignatureParser, GridClampsAndWraps) {
    auto r = loadShippedDir();
    ASSERT_TRUE(r.ok);
    const auto& ir0 = r.library.entries[0].ir0;
    // Elevation clamps to the -90/90 edges.
    EXPECT_DOUBLE_EQ(ir0.value_at(90.0, -180.0), 0.05);
    EXPECT_DOUBLE_EQ(ir0.value_at(90.0, 180.0), 0.05);
    // Azimuth wraps: -30 mirrors to +30 (between 0.02 and 0.03).
    EXPECT_DOUBLE_EQ(ir0.value_at(-30.0, 0.0), ir0.value_at(30.0, 0.0));
    // 330 degrees == 30 degrees after wrap.
    EXPECT_DOUBLE_EQ(ir0.value_at(330.0, 0.0), ir0.value_at(30.0, 0.0));
    // 200 degrees mirrors to 160.
    EXPECT_DOUBLE_EQ(ir0.value_at(200.0, 0.0), ir0.value_at(160.0, 0.0));
}

TEST(SignatureParser, RcsNegativeAzimuthGridFolds) {
    auto r = loadShippedDir();
    ASSERT_TRUE(r.ok);
    // The RCS grid runs -180..180: an aspect of 30 (either sign) reads
    // the same cell (flat grid -> 10.0, but the path is exercised).
    const auto& rcs = r.library.entries[0].rcs;
    EXPECT_DOUBLE_EQ(rcs.value_at(-30.0, 0.0), rcs.value_at(30.0, 0.0));
    EXPECT_DOUBLE_EQ(rcs.value_at(0.0, 0.0), 10.0);
}

TEST(SignatureParser, JSONRoundTripPreservesGrids) {
    auto r = loadShippedDir();
    ASSERT_TRUE(r.ok);
    auto back =
        loadSignatureDataLibraryFromString(writeSignatureDataLibrary(r.library));
    ASSERT_TRUE(back.ok) << (back.errors.empty() ? "" : back.errors[0]);
    ASSERT_EQ(back.library.entries.size(), 1u);
    EXPECT_EQ(back.library.entries[0], r.library.entries[0]);
    EXPECT_DOUBLE_EQ(back.library.entries[0].ir0.value_at(45.0, 0.0),
                      0.025);
}

TEST(SignatureParser, WrongJSONKindRejected) {
    auto back = loadSignatureDataLibraryFromString(
        R"({"kind": "f4.vehdef", "version": 1})");
    EXPECT_FALSE(back.ok);
}

TEST(SignatureParser, SyntheticGridParses) {
    const auto r = loadSigGridString(
        "# Num Azimuth Breakpoints\n2\n"
        "# Num Elevation Breakpoints\n2\n"
        "# Azimuth Breakpoints\n0.0 180.0\n"
        "# Elevation, then the data\n"
        "-90.0 1.0 2.0\n"
        "90.0 3.0 4.0\n",
        "synthetic.rcs");
    ASSERT_TRUE(r.ok) << (r.errors.empty() ? "" : r.errors[0]);
    ASSERT_EQ(r.grid.azimuth_deg.size(), 2u);
    ASSERT_EQ(r.grid.elevation_deg.size(), 2u);
    EXPECT_DOUBLE_EQ(r.grid.value_at(0.0, -90.0), 1.0);
    EXPECT_DOUBLE_EQ(r.grid.value_at(180.0, 90.0), 4.0);
    EXPECT_DOUBLE_EQ(r.grid.value_at(90.0, 0.0), 2.5);
}

TEST(SignatureParser, MissingDataRowsIsAnError) {
    // numEl says 2, only one row follows.
    auto r = loadSigGridString(
        "2\n2\n0.0 180.0\n-90.0 1.0 2.0\n", "short.rcs");
    EXPECT_FALSE(r.ok);
}

TEST(SignatureParser, NonAscendingAzimuthIsAnError) {
    auto r = loadSigGridString(
        "2\n1\n100.0 0.0\n-5.0 1.0\n", "bad.rcs");
    EXPECT_FALSE(r.ok);
}

TEST(SignatureParser, MissingFamilyFileIsAnError) {
    // An .LST naming a stem with no VISUAL/<stem>.VIS in the shipped
    // dir ("generic" exists everywhere, so use a fake stem).
    auto r = loadSignatureDataDir(
        F4_CONVERT_TEST_FIXTURES_DIR "/simdata/SIGDATA_NOPE");
    EXPECT_FALSE(r.ok);
}
