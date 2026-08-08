#include "f4/convert/dat_parser.hpp"

#include <gtest/gtest.h>

#include <string>

using namespace f4::convert;
using namespace f4::data;

// ============================================================================
// Test fixture: a minimal synthetic .dat file exercising every parser branch.
// Lives in tests/fixtures/test_synthetic.dat. Tests verify each parsed field
// against the known values in that file.
// ============================================================================

namespace {

// Load the synthetic fixture from disk.
ParseResult loadSyntheticFixture() {
    const std::string path = F4_CONVERT_TEST_FIXTURES_DIR "/test_synthetic.dat";
    return loadFile(path);
}

} // namespace

// ============================================================================
// Top-level parse success
// ============================================================================

TEST(DatParserTest, SyntheticFixtureParsesWithoutErrors) {
    auto r = loadSyntheticFixture();
    ASSERT_TRUE(r.ok) << "Parse errors: ";
    for (auto const& e : r.errors) std::cerr << "  " << e << "\n";
}

TEST(DatParserTest, SyntheticFixtureHasNoWarnings) {
    auto r = loadSyntheticFixture();
    ASSERT_TRUE(r.ok);
    EXPECT_EQ(r.warnings.size(), 0u)
        << "Unexpected warnings:";
    for (auto const& w : r.warnings) std::cerr << "  " << w << "\n";
}

TEST(DatParserTest, SourceMetadataExtractedFromHeaderComments) {
    auto r = loadSyntheticFixture();
    ASSERT_TRUE(r.ok);
    EXPECT_EQ(r.config.sourceTitle,    "Synthetic Test Aircraft");
    EXPECT_EQ(r.config.sourceAuthor,   "f4-convert test suite");
    EXPECT_EQ(r.config.sourceRevision, "1.0");
    EXPECT_EQ(r.config.sourceFile,     "test_synthetic.dat");
    EXPECT_EQ(r.config.name,           "Synthetic Test Aircraft");
}

// ============================================================================
// Geometry / input data
// ============================================================================

TEST(DatParserTest, GeometryFieldsParsedCorrectly) {
    auto r = loadSyntheticFixture();
    ASSERT_TRUE(r.ok);
    EXPECT_NEAR(r.config.geometry.emptyWeight.value(),  19900.0, 1e-9);
    EXPECT_NEAR(r.config.geometry.area.value(),            300.0, 1e-9);
    EXPECT_NEAR(r.config.geometry.internalFuel.value(),   7162.0, 1e-9);
    // maxFuel is set to internalFuel by the parser (FF format has
    // only one fuel value in the .dat file; maxFuel is derived).
    EXPECT_NEAR(r.config.geometry.maxFuel.value(),        r.config.geometry.internalFuel.value(), 1e-9);
    EXPECT_NEAR(r.config.geometry.aoaMax.to<f4::Degrees>().value(),           40.0, 1e-9);
    EXPECT_NEAR(r.config.geometry.aoaMin.to<f4::Degrees>().value(),          -8.0,  1e-9);
    EXPECT_NEAR(r.config.geometry.betaMax.to<f4::Degrees>().value(),         30.0,  1e-9);
    EXPECT_NEAR(r.config.geometry.betaMin.to<f4::Degrees>().value(),        -30.0,  1e-9);
    EXPECT_NEAR(r.config.geometry.maxGs,                9.0,  1e-9);
    EXPECT_NEAR(r.config.geometry.maxRoll.to<f4::Degrees>().value(),        190.0,  1e-9);
    EXPECT_NEAR(r.config.geometry.minVcas.value(),        250.0,  1e-9);
    EXPECT_NEAR(r.config.geometry.maxVcas.value(),        850.0,  1e-9);
    EXPECT_NEAR(r.config.geometry.cornerVcas.value(),     420.0,  1e-9);
    // thetaMax is 35.0 degrees in the .dat, stored as radians.
    EXPECT_NEAR(r.config.geometry.thetaMax.value(), 35.0 * 0.017453292519943295, 1e-9);
    EXPECT_NEAR(r.config.geometry.cgLoc.value(),           27.0, 1e-9);
    EXPECT_NEAR(r.config.geometry.length.value(),          47.0, 1e-9);
    EXPECT_NEAR(r.config.geometry.span.value(),            32.0, 1e-9);
    EXPECT_NEAR(r.config.geometry.fusRadius.value(),        2.5, 1e-9);
    EXPECT_NEAR(r.config.geometry.tailHt.value(),           4.5, 1e-9);
}

TEST(DatParserTest, GearPointsParsedCorrectly) {
    auto r = loadSyntheticFixture();
    ASSERT_TRUE(r.ok);
    ASSERT_EQ(r.config.geometry.gear.size(), 3u);
    EXPECT_NEAR(r.config.geometry.gear[0].x.value(), 16.5,  1e-9);
    EXPECT_NEAR(r.config.geometry.gear[0].y.value(),  0.0,  1e-9);
    EXPECT_NEAR(r.config.geometry.gear[0].z.value(),  5.88, 1e-9);
    EXPECT_NEAR(r.config.geometry.gear[0].range.to<f4::Degrees>().value(), 90.0, 1e-9);
    EXPECT_NEAR(r.config.geometry.gear[1].x.value(), 30.0,  1e-9);
    EXPECT_NEAR(r.config.geometry.gear[1].y.value(), -3.88, 1e-9);
    EXPECT_NEAR(r.config.geometry.gear[2].x.value(), 30.0,  1e-9);
    EXPECT_NEAR(r.config.geometry.gear[2].y.value(),  3.88, 1e-9);
}

// ============================================================================
// Aero tables (CL, CD, CY)
// ============================================================================

TEST(DatParserTest, AeroBreakpointsParsed) {
    auto r = loadSyntheticFixture();
    ASSERT_TRUE(r.ok);
    EXPECT_EQ(r.config.aero.mach.size(),      3u);
    EXPECT_EQ(r.config.aero.alpha_deg.size(), 4u);
    EXPECT_NEAR(r.config.aero.mach[0], 0.0, 1e-9);
    EXPECT_NEAR(r.config.aero.mach[1], 0.5, 1e-9);
    EXPECT_NEAR(r.config.aero.mach[2], 1.0, 1e-9);
    EXPECT_NEAR(r.config.aero.alpha_deg[0],  0.0, 1e-9);
    EXPECT_NEAR(r.config.aero.alpha_deg[1],  5.0, 1e-9);
    EXPECT_NEAR(r.config.aero.alpha_deg[2], 10.0, 1e-9);
    EXPECT_NEAR(r.config.aero.alpha_deg[3], 15.0, 1e-9);
}

TEST(DatParserTest, ClTableParsedCorrectly) {
    auto r = loadSyntheticFixture();
    ASSERT_TRUE(r.ok);
    ASSERT_EQ(r.config.aero.clift.size(), 12u);  // 3 mach * 4 alpha
    // Row-major: [mach=0][alpha=0..3], [mach=1][...], [mach=2][...]
    EXPECT_NEAR(r.config.aero.clift[0],  0.10, 1e-9);
    EXPECT_NEAR(r.config.aero.clift[1],  0.30, 1e-9);
    EXPECT_NEAR(r.config.aero.clift[2],  0.60, 1e-9);
    EXPECT_NEAR(r.config.aero.clift[3],  0.90, 1e-9);
    EXPECT_NEAR(r.config.aero.clift[4],  0.10, 1e-9);
    EXPECT_NEAR(r.config.aero.clift[5],  0.35, 1e-9);
    EXPECT_NEAR(r.config.aero.clift[11], 0.60, 1e-9);
    EXPECT_NEAR(r.config.aero.clFactor, 1.0, 1e-9);
}

TEST(DatParserTest, CdTableMultipliedBy15OnRead) {
    // Legacy FF behaviour: readin.cpp multiplies CD by 1.5 on read.
    // We preserve this so the JSON exposes the post-scale value.
    auto r = loadSyntheticFixture();
    ASSERT_TRUE(r.ok);
    ASSERT_EQ(r.config.aero.cdrag.size(), 12u);
    EXPECT_NEAR(r.config.aero.cdrag[0], 0.02 * 1.5, 1e-9);
    EXPECT_NEAR(r.config.aero.cdrag[1], 0.04 * 1.5, 1e-9);
    EXPECT_NEAR(r.config.aero.cdrag[11], 0.80 * 1.5, 1e-9);
}

TEST(DatParserTest, CyTableParsedWithoutScaling) {
    auto r = loadSyntheticFixture();
    ASSERT_TRUE(r.ok);
    ASSERT_EQ(r.config.aero.cy.size(), 12u);
    EXPECT_NEAR(r.config.aero.cy[0],  0.00, 1e-9);
    EXPECT_NEAR(r.config.aero.cy[1], -0.01, 1e-9);
    EXPECT_NEAR(r.config.aero.cy[11], -0.15, 1e-9);
}

// ============================================================================
// Engine
// ============================================================================

TEST(DatParserTest, EngineOptionsCaptured) {
    auto r = loadSyntheticFixture();
    ASSERT_TRUE(r.ok);
    ASSERT_EQ(r.config.engineOptions.size(), 1u);
    EXPECT_EQ(r.config.engineOptions[0], "fuelflow");
}

TEST(DatParserTest, EngineBreakpointsParsed) {
    auto r = loadSyntheticFixture();
    ASSERT_TRUE(r.ok);
    EXPECT_EQ(r.config.engine.mach.size(),   5u);
    EXPECT_EQ(r.config.engine.alt_ft.size(), 4u);
    EXPECT_NEAR(r.config.engine.mach[0],   0.0, 1e-9);
    EXPECT_NEAR(r.config.engine.mach[4],   2.0, 1e-9);
    EXPECT_NEAR(r.config.engine.alt_ft[0],     0.0, 1e-9);
    EXPECT_NEAR(r.config.engine.alt_ft[3], 50000.0, 1e-9);
}

TEST(DatParserTest, ThrustTablesParsedInAltMajorOrder) {
    auto r = loadSyntheticFixture();
    ASSERT_TRUE(r.ok);
    ASSERT_EQ(r.config.engine.thrust_idle.size(), 20u);  // 4 alt * 5 mach
    ASSERT_EQ(r.config.engine.thrust_mil.size(),  20u);
    ASSERT_EQ(r.config.engine.thrust_ab.size(),   20u);
    // [alt=0][mach=0] = 2000 (idle), 18000 (mil), 28000 (ab)
    EXPECT_NEAR(r.config.engine.thrust_idle[0], 2000.0, 1e-9);
    EXPECT_NEAR(r.config.engine.thrust_mil[0],  18000.0, 1e-9);
    EXPECT_NEAR(r.config.engine.thrust_ab[0],   28000.0, 1e-9);
    // [alt=0][mach=4] = 300 (idle), 2500 (mil), 4500 (ab) — last col of first row
    EXPECT_NEAR(r.config.engine.thrust_idle[4], 300.0, 1e-9);
    EXPECT_NEAR(r.config.engine.thrust_mil[4],  2500.0, 1e-9);
    EXPECT_NEAR(r.config.engine.thrust_ab[4],   4500.0, 1e-9);
}

TEST(DatParserTest, FuelFlowTablesParsedWhenEngoptFuelFlowPresent) {
    auto r = loadSyntheticFixture();
    ASSERT_TRUE(r.ok);
    EXPECT_TRUE(r.config.engine.hasFuelFlow());
    ASSERT_EQ(r.config.engine.fuelflow_idle.size(), 20u);
    ASSERT_EQ(r.config.engine.fuelflow_mil.size(),  20u);
    ASSERT_EQ(r.config.engine.fuelflow_ab.size(),   20u);
    // [alt=0][mach=0]
    EXPECT_NEAR(r.config.engine.fuelflow_idle[0], 100.0, 1e-9);
    EXPECT_NEAR(r.config.engine.fuelflow_mil[0],  900.0, 1e-9);
    EXPECT_NEAR(r.config.engine.fuelflow_ab[0],  1400.0, 1e-9);
    // [alt=0][mach=4] = 15, 125, 200
    EXPECT_NEAR(r.config.engine.fuelflow_idle[4], 15.0, 1e-9);
    EXPECT_NEAR(r.config.engine.fuelflow_mil[4],  125.0, 1e-9);
    EXPECT_NEAR(r.config.engine.fuelflow_ab[4],   200.0, 1e-9);
}

TEST(DatParserTest, HasABTrueWhenThrustAbDiffersFromMil) {
    auto r = loadSyntheticFixture();
    ASSERT_TRUE(r.ok);
    EXPECT_TRUE(r.config.engine.hasAB());
}

// ============================================================================
// Roll command table
// ============================================================================

TEST(DatParserTest, RollCommandTableParsed) {
    auto r = loadSyntheticFixture();
    ASSERT_TRUE(r.ok);
    EXPECT_EQ(r.config.rollCmd.alpha_deg.size(), 3u);
    EXPECT_EQ(r.config.rollCmd.qbar.size(),      4u);
    EXPECT_EQ(r.config.rollCmd.rollRate.size(),  12u);  // 3 * 4
    EXPECT_NEAR(r.config.rollCmd.alpha_deg[0],  0.0,  1e-9);
    EXPECT_NEAR(r.config.rollCmd.alpha_deg[2], 20.0,  1e-9);
    EXPECT_NEAR(r.config.rollCmd.qbar[0],    0.0,    1e-9);
    EXPECT_NEAR(r.config.rollCmd.qbar[3], 1000.0,   1e-9);
    EXPECT_NEAR(r.config.rollCmd.scale,       1.0,   1e-9);
    // [alpha=0][qbar=0] = 10
    EXPECT_NEAR(r.config.rollCmd.rollRate[0], 10.0, 1e-9);
    // [alpha=2][qbar=3] = 120
    EXPECT_NEAR(r.config.rollCmd.rollRate[11], 120.0, 1e-9);
}

// ============================================================================
// Limiters
// ============================================================================

TEST(DatParserTest, LimitersParsedCorrectly) {
    auto r = loadSyntheticFixture();
    ASSERT_TRUE(r.ok);
    // LimiterKey::NegGLimiter (index 0) — type=Line, x1=-4, y1=-3, x2=0, y2=-1
    EXPECT_EQ(static_cast<int>(r.config.limiter(LimiterKey::NegGLimiter).type),
              static_cast<int>(LimiterType::Line));
    EXPECT_NEAR(r.config.limiter(LimiterKey::NegGLimiter).x1, -4.0, 1e-9);
    EXPECT_NEAR(r.config.limiter(LimiterKey::NegGLimiter).y1, -3.0, 1e-9);
    EXPECT_NEAR(r.config.limiter(LimiterKey::NegGLimiter).x2,  0.0, 1e-9);
    EXPECT_NEAR(r.config.limiter(LimiterKey::NegGLimiter).y2, -1.0, 1e-9);

    // CatIIICommandType (index 5) — type=Value, x1=0
    EXPECT_EQ(static_cast<int>(r.config.limiter(LimiterKey::CatIIICommandType).type),
              static_cast<int>(LimiterType::Value));
    EXPECT_NEAR(r.config.limiter(LimiterKey::CatIIICommandType).x1, 0.0, 1e-9);

    // CatIIIAOALimiter (index 6) — type=Percent, x1=1.0
    EXPECT_EQ(static_cast<int>(r.config.limiter(LimiterKey::CatIIIAOALimiter).type),
              static_cast<int>(LimiterType::Percent));
    EXPECT_NEAR(r.config.limiter(LimiterKey::CatIIIAOALimiter).x1, 1.0, 1e-9);

    // CatIIIRollRateLimiter (index 7) — type=Value, x1=15.0
    EXPECT_EQ(static_cast<int>(r.config.limiter(LimiterKey::CatIIIRollRateLimiter).type),
              static_cast<int>(LimiterType::Value));
    EXPECT_NEAR(r.config.limiter(LimiterKey::CatIIIRollRateLimiter).x1, 15.0, 1e-9);
}

// ============================================================================
// AuxAeroData — typed struct + verbatim capture
// ============================================================================

TEST(DatParserTest, AuxAeroTypedFieldsPopulatedFromKeyValuePairs) {
    auto r = loadSyntheticFixture();
    ASSERT_TRUE(r.ok);
    EXPECT_NEAR(r.config.aux.normSpoolRate,        3.0,  1e-9);
    EXPECT_NEAR(r.config.aux.abSpoolRate,         10.5,  1e-9);
    EXPECT_NEAR(r.config.aux.jfsSpoolUpRate,      15.0,  1e-9);
    EXPECT_NEAR(r.config.aux.fuelFlowFactorNormal, 0.87, 1e-9);
    EXPECT_NEAR(r.config.aux.fuelFlowFactorAb,     2.0,  1e-9);
    EXPECT_NEAR(r.config.aux.minFuelFlow,        868.0,  1e-9);
    EXPECT_EQ(r.config.aux.hasLef, true);
    EXPECT_EQ(r.config.aux.hasTef, true);
    EXPECT_NEAR(r.config.aux.tefMaxAngle.to<f4::Degrees>().value(),    21.5, 1e-9);
    EXPECT_NEAR(r.config.aux.lefMaxAngle.to<f4::Degrees>().value(),    25.0, 1e-9);
    EXPECT_NEAR(r.config.aux.pitchMomentum,   1.3, 1e-9);
    EXPECT_NEAR(r.config.aux.rollCouple,    -0.05, 1e-9);
    EXPECT_EQ(r.config.aux.elevatorRolls, true);
    EXPECT_EQ(r.config.aux.nEngines, 1);
    EXPECT_EQ(r.config.aux.typeEngine, 5);
}

TEST(DatParserTest, RawAuxAeroDataCapturesEveryKeyValuePairVerbatim) {
    auto r = loadSyntheticFixture();
    ASSERT_TRUE(r.ok);
    // All known keys should be in the verbatim map.
    EXPECT_EQ(r.config.rawAuxAeroData.count("normSpoolRate"), 1u);
    EXPECT_EQ(r.config.rawAuxAeroData.count("abSpoolRate"),   1u);
    EXPECT_EQ(r.config.rawAuxAeroData.count("typeEngine"),    1u);
    // Unknown keys must also be captured verbatim (no data loss).
    EXPECT_EQ(r.config.rawAuxAeroData.count("customUnknownKey"), 1u);
    EXPECT_EQ(r.config.rawAuxAeroData.at("customUnknownKey"),
              "42.0 with trailing tokens");
    // Verbatim value preserves the raw string form, including the decimal.
    EXPECT_EQ(r.config.rawAuxAeroData.at("normSpoolRate"), "3.0");
    EXPECT_EQ(r.config.rawAuxAeroData.at("abSpoolRate"),   "10.5");
}

// ============================================================================
// AFM (BMS Advanced Flight Model) detection
// ============================================================================

TEST(DatParserTest, AfmFileByNameSuffixRejected) {
    // A file named "xyz_afm.dat" should be rejected as AFM regardless of
    // contents.
    std::string contents = "19900 300 7162 7162\n40 -8 30 -30\n";
    auto r = loadString(contents, "test_afm.dat");
    EXPECT_FALSE(r.ok);
    ASSERT_FALSE(r.errors.empty());
    EXPECT_NE(r.errors[0].find("AFM format not supported"), std::string::npos);
}

TEST(DatParserTest, AfmFileByContentRejected) {
    // A file whose first non-comment token is a small integer (1..4) and
    // whose name doesn't end in _afm.dat should also be detected as AFM.
    std::string contents = "# BMS AFM file\n3\n0.5 0.6 0.7\n";
    auto r = loadString(contents, "not_afm_name.dat");
    EXPECT_FALSE(r.ok);
    ASSERT_FALSE(r.errors.empty());
    EXPECT_NE(r.errors[0].find("AFM format not supported"), std::string::npos);
}

// ============================================================================
// Comment stripping and whitespace tolerance
// ============================================================================

TEST(DatParserTest, LineCommentsAreStripped) {
    // A .dat with comments interleaved into the data should parse identically
    // to one without comments. We use a minimal valid .dat fragment that
    // has enough data for the input section to parse without errors.
    std::string with_comments =
        "# header comment\n"
        "19900 # empty weight\n"
        "300 # wing area\n"
        "7162 # internal fuel\n"
        "40 -8 30 -30 # aoa/beta limits\n"
        "9 190 250 850 420 # g/roll/vcas\n"
        "13 # thetaMax\n"
        "0 # numGear (zero gear points)\n"
        "27 47 32 2.5 4.5 # cg/length/span/radius/tail\n";
    std::string without_comments =
        "19900 300 7162\n"
        "40 -8 30 -30\n"
        "9 190 250 850 420\n"
        "13\n"
        "0\n"
        "27 47 32 2.5 4.5\n";
    auto r1 = loadString(with_comments, "with_comments.dat");
    auto r2 = loadString(without_comments, "without_comments.dat");
    // Both should parse OK (the input-data section is complete; later
    // sections will produce warnings but not errors).
    ASSERT_TRUE(r1.ok) << "with_comments failed: " << (r1.errors.empty() ? "" : r1.errors[0]);
    ASSERT_TRUE(r2.ok) << "without_comments failed: " << (r2.errors.empty() ? "" : r2.errors[0]);
    EXPECT_NEAR(r1.config.geometry.emptyWeight.value(),
                r2.config.geometry.emptyWeight.value(), 1e-9);
    EXPECT_NEAR(r1.config.geometry.span.value(),
                r2.config.geometry.span.value(), 1e-9);
}

TEST(DatParserTest, EmptyFileProducesError) {
    auto r = loadString("", "empty.dat");
    EXPECT_FALSE(r.ok);
}

TEST(DatParserTest, MissingFileProducesError) {
    auto r = loadFile("/nonexistent/path/to/file.dat");
    EXPECT_FALSE(r.ok);
    ASSERT_FALSE(r.errors.empty());
    EXPECT_NE(r.errors[0].find("Could not open file"), std::string::npos);
}

// ============================================================================
// Corrupt / truncated input
// ============================================================================

TEST(DatParserTest, TruncatedMidSection) {
    // A minimal valid .dat header: empty weight, area, internal fuel,
    // then truncate mid-table (after aoa/beta limits line but before
    // the rest of the input section).
    std::string truncated =
        "19900 300 7162\n"
        "40 -8 30 -30\n";
    auto r = loadString(truncated, "truncated.dat");
    EXPECT_FALSE(r.ok);
}

TEST(DatParserTest, GarbageAfterValidHeader) {
    // Valid header followed by random garbage instead of the expected
    // numeric sections. The parser should either report ok=false or
    // produce warnings.
    std::string garbage =
        "19900 300 7162\n"
        "40 -8 30 -30\n"
        "9 190 250 850 420\n"
        "13\n"
        "0\n"
        "27 47 32 2.5 4.5\n"
        "GARBAGE_NOT_A_NUMBER !!!\n";
    auto r = loadString(garbage, "garbage_after_header.dat");
    EXPECT_TRUE(!r.ok || !r.warnings.empty());
}

TEST(DatParserTest, TooFewTokensInRow) {
    // A row that has fewer tokens than expected for its section.
    // The input section expects specific token counts per line;
    // providing fewer should produce an error.
    std::string too_few =
        "19900\n";  // Only one token on the first data line (needs 3)
    auto r = loadString(too_few, "too_few_tokens.dat");
    EXPECT_TRUE(!r.ok || !r.errors.empty());
}
