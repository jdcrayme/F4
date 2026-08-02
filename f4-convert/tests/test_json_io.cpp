#include "f4/convert/json_io.hpp"

#include <gtest/gtest.h>

#include <string>

using namespace f4::convert;
using namespace f4::data;

// ============================================================================
// Round-trip: write a config to JSON, re-read it, compare.
// ============================================================================

namespace {

AircraftConfig makeSampleConfig() {
    AircraftConfig c;
    c.name = "Test Aircraft";
    c.description = "for round-trip testing";
    c.sourceTitle = "Test Title";
    c.sourceAuthor = "Test Author";
    c.sourceRevision = "rev 1";
    c.sourceFile = "test.dat";

    c.geometry.emptyWeight_lbs  = 19900.5;
    c.geometry.area_ft2         = 300.25;
    c.geometry.internalFuel_lbs = 7162.0;
    c.geometry.maxFuel_lbs      = 7162.0;
    c.geometry.aoaMax_deg       = 40.0;
    c.geometry.aoaMin_deg       = -8.0;
    c.geometry.betaMax_deg      = 30.0;
    c.geometry.betaMin_deg      = -30.0;
    c.geometry.maxGs            = 9.0;
    c.geometry.maxRoll_deg      = 190.0;
    c.geometry.minVcas_kts      = 250.0;
    c.geometry.maxVcas_kts      = 850.0;
    c.geometry.cornerVcas_kts   = 420.0;
    c.geometry.thetaMax_rad     = 35.0 * 0.017453292519943295;
    c.geometry.cgLoc_ft         = 27.0;
    c.geometry.length_ft        = 47.0;
    c.geometry.span_ft          = 32.0;
    c.geometry.fusRadius_ft     = 2.5;
    c.geometry.tailHt_ft        = 4.5;
    c.geometry.gear = {
        {16.5,  0.0, 5.88, 90.0},
        {30.0, -3.88, 5.88, 90.0},
        {30.0,  3.88, 5.88, 90.0},
    };

    c.aux.normSpoolRate        = 3.0;
    c.aux.abSpoolRate          = 10.5;
    c.aux.fuelFlowFactorNormal = 0.87;
    c.aux.fuelFlowFactorAb     = 2.0;
    c.aux.minFuelFlow          = 868.0;
    c.aux.hasLef               = true;
    c.aux.hasTef               = true;
    c.aux.tefMaxAngle          = 21.5;
    c.aux.lefMaxAngle          = 25.0;
    c.aux.pitchMomentum        = 1.3;
    c.aux.rollCouple           = -0.05;
    c.aux.elevatorRolls        = true;
    c.aux.nEngines             = 1;
    c.aux.typeEngine           = 5;

    c.aero.mach     = {0.0, 0.5, 1.0};
    c.aero.alpha_deg = {0.0, 5.0, 10.0, 15.0};
    c.aero.clift    = {0.10, 0.30, 0.60, 0.90,
                       0.10, 0.35, 0.70, 1.05,
                       0.05, 0.20, 0.40, 0.60};
    c.aero.cdrag    = {0.03, 0.06, 0.12, 0.24,
                       0.045, 0.09, 0.18, 0.36,
                       0.15, 0.30, 0.60, 1.20};
    c.aero.cy       = {0.0, -0.01, -0.02, -0.03,
                       0.0, -0.02, -0.04, -0.06,
                       0.0, -0.05, -0.10, -0.15};
    c.aero.clFactor = 1.0;
    c.aero.cdFactor = 1.0;
    c.aero.cyFactor = 1.0;

    c.engine.thrustFactor   = 1.0;
    c.engine.fuelFlowFactor = 1.0;
    c.engine.alt_ft = {0.0, 10000.0, 30000.0, 50000.0};
    c.engine.mach   = {0.0, 0.4, 0.8, 1.2, 2.0};
    c.engine.thrust_idle = {2000, 1800, 1200, 600,
                            2000, 1800, 1200, 600,
                            2000, 1800, 1200, 600,
                            2000, 1800, 1200, 600};
    c.engine.thrust_mil  = c.engine.thrust_idle;  // dummy, will differ below
    for (auto& v : c.engine.thrust_mil) v *= 9.0;
    c.engine.thrust_ab   = c.engine.thrust_mil;
    for (auto& v : c.engine.thrust_ab) v *= 1.5;
    c.engine.fuelflow_idle = {100, 90, 60, 30, 100, 90, 60, 30,
                              100, 90, 60, 30, 100, 90, 60, 30};
    c.engine.fuelflow_mil  = c.engine.fuelflow_idle;
    for (auto& v : c.engine.fuelflow_mil) v *= 9.0;
    c.engine.fuelflow_ab   = c.engine.fuelflow_mil;
    for (auto& v : c.engine.fuelflow_ab) v *= 1.5;

    c.rollCmd.alpha_deg = {0.0, 10.0, 20.0};
    c.rollCmd.qbar      = {0.0, 100.0, 500.0, 1000.0};
    c.rollCmd.rollRate  = {10, 20, 30, 40, 20, 40, 60, 80, 30, 60, 90, 120};
    c.rollCmd.scale     = 1.0;

    c.setLimiter(LimiterKey::NegGLimiter,
                 Limiter{LimiterType::Line, -4.0, -3.0, 0.0, -1.0, 0, 0});
    c.setLimiter(LimiterKey::PosGLimiter,
                 Limiter{LimiterType::Line, 4.0, 3.0, 30.0, 9.0, 0, 0});
    c.setLimiter(LimiterKey::CatIIICommandType,
                 Limiter{LimiterType::Value, 0.0, 0, 0, 0, 0, 0});
    c.setLimiter(LimiterKey::CatIIIRollRateLimiter,
                 Limiter{LimiterType::Percent, 1.0, 0, 0, 0, 0, 0});

    c.aeroOptions   = {"test_aeropt"};
    c.engineOptions = {"fuelflow"};
    c.rawAuxAeroData = {
        {"normSpoolRate",      "3.0"},
        {"abSpoolRate",        "10.5"},
        {"customUnknownKey",   "42.0 with trailing tokens"},
    };
    return c;
}

} // namespace

TEST(JsonIoTest, WriteProducesValidJson) {
    auto cfg = makeSampleConfig();
    std::string json = writeJson(cfg);
    // Smoke test: it parses as JSON via nlohmann.
    // (We re-read it via readJson below; if that succeeds, the JSON is valid.)
    AircraftConfig reread;
    auto result = readJson(json, reread);
    EXPECT_TRUE(result.ok) << "JSON failed to parse back:";
    for (auto const& e : result.errors) std::cerr << "  " << e << "\n";
}

TEST(JsonIoTest, RoundTripPreservesAllFields) {
    auto original = makeSampleConfig();
    std::string json = writeJson(original);
    AircraftConfig reread;
    auto result = readJson(json, reread);
    ASSERT_TRUE(result.ok);

    auto diffs = diffConfigs(original, reread, 1e-12);
    EXPECT_EQ(diffs.size(), 0u)
        << "Round-trip produced differences:";
    for (auto const& d : diffs) std::cerr << d << "\n";
}

TEST(JsonIoTest, RoundTripPreservesRawAuxAeroData) {
    auto original = makeSampleConfig();
    original.rawAuxAeroData["tricky_key"] = "value with spaces and # hash";
    original.rawAuxAeroData["unicode_key"] = "value with quotes \" inside";
    std::string json = writeJson(original);
    AircraftConfig reread;
    ASSERT_TRUE(readJson(json, reread).ok);

    EXPECT_EQ(reread.rawAuxAeroData.at("tricky_key"),
              original.rawAuxAeroData.at("tricky_key"));
    EXPECT_EQ(reread.rawAuxAeroData.at("unicode_key"),
              original.rawAuxAeroData.at("unicode_key"));
}

TEST(JsonIoTest, WriteJsonFileCreatesReadableFile) {
    auto cfg = makeSampleConfig();
    std::string path = F4_CONVERT_TEST_OUTPUT_DIR "/test_write.json";
    ASSERT_TRUE(writeJsonFile(cfg, path));

    AircraftConfig reread;
    auto result = readJsonFile(path, reread);
    ASSERT_TRUE(result.ok);
    auto diffs = diffConfigs(cfg, reread);
    EXPECT_EQ(diffs.size(), 0u);
}

TEST(JsonIoTest, ReadJsonFileReturnsErrorForMissingFile) {
    AircraftConfig cfg;
    auto result = readJsonFile("/nonexistent/path.json", cfg);
    EXPECT_FALSE(result.ok);
    ASSERT_FALSE(result.errors.empty());
    EXPECT_NE(result.errors[0].find("Could not open"), std::string::npos);
}

TEST(JsonIoTest, ReadJsonRejectsMalformedJson) {
    AircraftConfig cfg;
    auto result = readJson("{ this is not valid json ", cfg);
    EXPECT_FALSE(result.ok);
}

TEST(JsonIoTest, DiffConfigsReportsDifferences) {
    auto a = makeSampleConfig();
    auto b = a;
    b.geometry.emptyWeight_lbs = 99999.0;
    b.aux.normSpoolRate = 99.0;
    b.aero.mach[0] = -1.0;
    auto diffs = diffConfigs(a, b);
    EXPECT_GE(diffs.size(), 3u);
}

TEST(JsonIoTest, DiffConfigsReturnsEmptyForEqualConfigs) {
    auto a = makeSampleConfig();
    auto b = a;
    auto diffs = diffConfigs(a, b);
    EXPECT_EQ(diffs.size(), 0u);
}

TEST(JsonIoTest, DiffConfigsRespectsTolerance) {
    auto a = makeSampleConfig();
    auto b = a;
    b.geometry.emptyWeight_lbs += 1e-9;  // tiny perturbation
    // With tolerance 1e-6, should be considered equal.
    auto diffs = diffConfigs(a, b, 1e-6);
    EXPECT_EQ(diffs.size(), 0u);
    // With tolerance 1e-15, should be considered different.
    diffs = diffConfigs(a, b, 1e-15);
    EXPECT_GE(diffs.size(), 1u);
}
