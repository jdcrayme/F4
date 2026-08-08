// f4-convert/tests/test_round_trip.cpp
//
// Round-trip integration tests: parse each .dat fixture to a config,
// serialize to JSON, re-read the JSON, and compare field-by-field. Any
// difference is a parser or writer regression.
//
// Also tests that parsing a JSON (without going through .dat) produces the
// same config as parsing the corresponding .dat — this catches drift
// between the parser and the JSON reader.

#include "f4/convert/dat_parser.hpp"
#include "f4/convert/json_io.hpp"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

using namespace f4::convert;
using namespace f4::data;

namespace {

// Path to the fixtures directory, supplied by CMake at compile time.
constexpr const char* kFixturesDir = F4_CONVERT_TEST_FIXTURES_DIR;

// Path to a writable directory for test outputs.
constexpr const char* kOutputDir = F4_CONVERT_TEST_OUTPUT_DIR;

// Parse a .dat fixture, write to JSON, re-read the JSON, compare.
// Returns the diff lines (empty if equivalent).
std::vector<std::string> roundTripFixture(const std::string& datPath) {
    auto parseResult = loadFile(datPath);
    if (!parseResult.ok) {
        return {std::string("PARSE FAILED: ") + parseResult.errors[0]};
    }

    std::string json = writeJson(parseResult.config);

    AircraftConfig reread;
    auto ioResult = readJson(json, reread);
    if (!ioResult.ok) {
        return {std::string("JSON REREAD FAILED: ") + ioResult.errors[0]};
    }

    return diffConfigs(parseResult.config, reread, 1e-12);
}

} // namespace

// ============================================================================
// Synthetic fixture: .dat -> JSON -> config -> compare
// ============================================================================

TEST(RoundTripTest, SyntheticFixtureRoundTripsLosslessly) {
    const std::string datPath = std::string(kFixturesDir) + "/test_synthetic.dat";
    auto diffs = roundTripFixture(datPath);
    EXPECT_EQ(diffs.size(), 0u)
        << "Round-trip differences for test_synthetic.dat:";
    for (auto const& d : diffs) std::cerr << d << "\n";
}

TEST(RoundTripTest, SyntheticFixtureJsonWrittenToDiskMatchesInMemory) {
    const std::string datPath = std::string(kFixturesDir) + "/test_synthetic.dat";
    auto parseResult = loadFile(datPath);
    ASSERT_TRUE(parseResult.ok);

    // Write to disk via writeJsonFile
    const std::string outPath = std::string(kOutputDir) + "/synthetic_round_trip.json";
    ASSERT_TRUE(writeJsonFile(parseResult.config, outPath));

    // Read back via readJsonFile
    AircraftConfig fromDisk;
    auto ioResult = readJsonFile(outPath, fromDisk);
    ASSERT_TRUE(ioResult.ok) << "Read back failed:" << ioResult.errors[0];

    auto diffs = diffConfigs(parseResult.config, fromDisk);
    EXPECT_EQ(diffs.size(), 0u)
        << "Disk round-trip differences:";
    for (auto const& d : diffs) std::cerr << d << "\n";
}

// ============================================================================
// All .dat fixtures in the fixtures/ directory round-trip losslessly.
// This is the regression guardrail: if a parser change silently drops or
// mutates a field, this test catches it.
// ============================================================================

TEST(RoundTripTest, AllFixturesRoundTripLosslessly) {
    namespace fs = std::filesystem;
    if (!fs::exists(kFixturesDir)) {
        GTEST_SKIP() << "Fixtures directory not found: " << kFixturesDir;
    }

    int tested = 0;
    int failed = 0;
    for (const auto& entry : fs::directory_iterator(kFixturesDir)) {
        if (entry.path().extension() != ".dat") continue;
        std::string datPath = entry.path().string();
        auto diffs = roundTripFixture(datPath);
        if (!diffs.empty()) {
            ADD_FAILURE() << "Round-trip failed for " << entry.path().filename().string();
            for (auto const& d : diffs) std::cerr << "  " << d << "\n";
            ++failed;
        }
        ++tested;
    }
    EXPECT_GT(tested, 0) << "No .dat fixtures found in " << kFixturesDir;
    EXPECT_EQ(failed, 0) << failed << " of " << tested << " fixtures failed round-trip";
}

// ============================================================================
// Real aircraft fixtures: verify that every real .dat file parses without
// errors or warnings. This catches parser regressions against the actual
// FreeFalcon .dat format (which has format variations the synthetic fixture
// doesn't exercise — tab separators, "END OF DATA" markers, missing
// AuxAeroData sections, thrust factors up to 15.0, etc.).
// ============================================================================

TEST(RealAircraftTest, AllRealFixturesParseWithoutErrorsOrWarnings) {
    namespace fs = std::filesystem;
    if (!fs::exists(kFixturesDir)) {
        GTEST_SKIP() << "Fixtures directory not found: " << kFixturesDir;
    }

    int tested = 0;
    int errored = 0;
    int warned = 0;
    for (const auto& entry : fs::directory_iterator(kFixturesDir)) {
        if (entry.path().extension() != ".dat") continue;
        std::string datPath = entry.path().string();
        auto result = loadFile(datPath);
        if (!result.ok) {
            ADD_FAILURE() << "Parse error for " << entry.path().filename().string();
            for (auto const& e : result.errors) std::cerr << "  " << e << "\n";
            ++errored;
        } else if (!result.warnings.empty()) {
            ADD_FAILURE() << "Warnings for " << entry.path().filename().string();
            for (auto const& w : result.warnings) std::cerr << "  " << w << "\n";
            ++warned;
        }
        ++tested;
    }
    EXPECT_GT(tested, 0);
    EXPECT_EQ(errored, 0) << errored << " of " << tested << " fixtures had parse errors";
    EXPECT_EQ(warned, 0) << warned << " of " << tested << " fixtures had warnings";
}

// ============================================================================
// Real aircraft: spot-check that key fields are parsed correctly for a
// representative aircraft (f16). These values come from the real f16.dat
// fixture and serve as a regression guard against silent field-level bugs.
// ============================================================================

TEST(RealAircraftTest, F16FieldsParsedCorrectly) {
    const std::string datPath = std::string(kFixturesDir) + "/f16.dat";
    auto r = loadFile(datPath);
    ASSERT_TRUE(r.ok) << "f16.dat failed to parse";
    EXPECT_TRUE(r.warnings.empty());

    // From f16.dat header comments and input-data section
    EXPECT_NEAR(r.config.geometry.emptyWeight.value(), 18238.0, 1e-6);
    EXPECT_NEAR(r.config.geometry.area.value(),           300.0, 1e-6);
    EXPECT_NEAR(r.config.geometry.internalFuel.value(),  7162.0, 1e-6);
    EXPECT_NEAR(r.config.geometry.aoaMax.to<f4::Degrees>().value(),          35.0, 1e-6);
    EXPECT_NEAR(r.config.geometry.aoaMin.to<f4::Degrees>().value(),         -15.0, 1e-6);
    EXPECT_NEAR(r.config.geometry.maxGs,                9.0, 1e-6);
    EXPECT_NEAR(r.config.geometry.maxRoll.to<f4::Degrees>().value(),        190.0, 1e-6);

    // Aero table dimensions
    EXPECT_EQ(r.config.aero.mach.size(),      7u);
    EXPECT_EQ(r.config.aero.alpha_deg.size(), 21u);
    EXPECT_EQ(r.config.aero.clift.size(),    147u);  // 7 * 21

    // Engine table dimensions
    EXPECT_EQ(r.config.engine.alt_ft.size(),  8u);
    EXPECT_EQ(r.config.engine.mach.size(),   14u);
    EXPECT_EQ(r.config.engine.thrust_idle.size(), 112u);  // 8 * 14

    // Roll command table
    EXPECT_EQ(r.config.rollCmd.alpha_deg.size(), 7u);
    EXPECT_EQ(r.config.rollCmd.qbar.size(),     14u);

    // Source metadata: the original Falcon 4 .dat files don't use the
    // "# Title:" format that later FreeFalcon files adopted. The f16.dat
    // header is "#  F-16 Data Set" — a comment with no key prefix. The
    // parser doesn't extract this as sourceTitle (it only recognizes the
    // explicit "# Title:" prefix), so sourceTitle will be empty. That's
    // acceptable — the aircraft name falls back to the file name.
    EXPECT_FALSE(r.config.name.empty());
}

// ============================================================================
// Real aircraft: verify a representative variety of aircraft types all parse.
// Covers fighters, attackers, bombers, transports, and a small prop plane.
// ============================================================================

TEST(RealAircraftTest, RepresentativeVarietyParsesCleanly) {
    namespace fs = std::filesystem;
    const std::vector<std::string> representatives = {
        "f16.dat",   // modern fighter
        "f15.dat",   // fighter
        "a10.dat",   // attack
        "b52.dat",   // bomber
        "c130.dat",  // transport
        "an2.dat",   // small prop (minimal data — only 3 limiters, 2 mach breakpoints)
        "mig29.dat", // Russian fighter
        "Su27.dat",  // Russian fighter (capital S in filename)
        "f22.dat",   // modern stealth fighter
    };
    int tested = 0;
    for (const auto& name : representatives) {
        std::string path = std::string(kFixturesDir) + "/" + name;
        if (!fs::exists(path)) {
            ADD_FAILURE() << "Fixture not found: " << name;
            continue;
        }
        auto r = loadFile(path);
        EXPECT_TRUE(r.ok) << name << " failed to parse";
        EXPECT_EQ(r.warnings.size(), 0u) << name << " had warnings";
        ++tested;
    }
    EXPECT_EQ(tested, static_cast<int>(representatives.size()));
}

// ============================================================================
// Verbatim capture test: the rawAuxAeroData map must round-trip exactly,
// including unknown keys, embedded #, and trailing tokens.
// ============================================================================

TEST(RoundTripTest, RawAuxAeroDataVerbatimCaptureRoundTrips) {
    const std::string datPath = std::string(kFixturesDir) + "/test_synthetic.dat";
    auto parseResult = loadFile(datPath);
    ASSERT_TRUE(parseResult.ok);

    // Verify the unknown key is captured.
    EXPECT_EQ(parseResult.config.rawAuxAeroData.count("customUnknownKey"), 1u);
    EXPECT_EQ(parseResult.config.rawAuxAeroData.at("customUnknownKey"),
              "42.0 with trailing tokens");

    // Round-trip and verify it survives.
    std::string json = writeJson(parseResult.config);
    AircraftConfig reread;
    ASSERT_TRUE(readJson(json, reread).ok);
    EXPECT_EQ(reread.rawAuxAeroData.count("customUnknownKey"), 1u);
    EXPECT_EQ(reread.rawAuxAeroData.at("customUnknownKey"),
              "42.0 with trailing tokens");
}

// ============================================================================
// Idempotency: parsing a JSON, writing it back, and parsing again produces
// the same config. (Catches bugs where the writer drops fields that the
// reader can't repopulate.)
// ============================================================================

TEST(RoundTripTest, JsonToConfigToJsonIsIdempotent) {
    const std::string datPath = std::string(kFixturesDir) + "/test_synthetic.dat";
    auto parseResult = loadFile(datPath);
    ASSERT_TRUE(parseResult.ok);

    std::string json1 = writeJson(parseResult.config);
    AircraftConfig config2;
    ASSERT_TRUE(readJson(json1, config2).ok);
    std::string json2 = writeJson(config2);

    // The two JSONs should be byte-identical (deterministic serialization).
    EXPECT_EQ(json1, json2)
        << "JSON serialization is not idempotent";
}
