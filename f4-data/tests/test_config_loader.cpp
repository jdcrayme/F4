#include "f4/data/config_loader.hpp"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>

using namespace f4::data;

// ============================================================================
// Load all 25 generated JSON fixtures (produced by f4-convert at build time)
// and verify each loads cleanly and validates.
//
// The fixtures live in F4_GENERATED_FIXTURES_DIR (a CMake cache variable
// pointing to ${BUILD_DIR}/generated_fixtures/).
// ============================================================================

namespace {

constexpr const char* kFixturesDir = F4_GENERATED_FIXTURES_DIR;

bool fixturesExist() {
    return std::filesystem::exists(kFixturesDir);
}

} // namespace

TEST(ConfigLoaderTest, AllGeneratedFixturesLoadCleanly) {
    if (!fixturesExist()) GTEST_SKIP() << "Generated fixtures not found at " << kFixturesDir;

    int tested = 0;
    int load_failures = 0;
    int validation_failures = 0;
    for (const auto& entry : std::filesystem::directory_iterator(kFixturesDir)) {
        if (entry.path().extension() != ".json") continue;
        std::string path = entry.path().string();
        auto result = loadConfig(path);
        if (!result.ok) {
            ADD_FAILURE() << "Failed to load " << entry.path().filename().string();
            for (auto const& e : result.errors) std::cerr << "  " << e << "\n";
            ++load_failures;
            ++tested;
            continue;
        }
        auto vr = result.config.validate();
        if (!vr.ok()) {
            ADD_FAILURE() << "Validation failed for " << entry.path().filename().string();
            std::cerr << vr.format();
            ++validation_failures;
        }
        ++tested;
    }
    EXPECT_GT(tested, 0) << "No JSON fixtures found in " << kFixturesDir;
    EXPECT_EQ(load_failures, 0);
    EXPECT_EQ(validation_failures, 0);
}

// ============================================================================
// Spot-check the f16 fixture: known field values from the real f16.dat.
// ============================================================================

TEST(ConfigLoaderTest, F16FixtureFieldsCorrect) {
    if (!fixturesExist()) GTEST_SKIP();

    const std::string path = std::string(kFixturesDir) + "/f16.json";
    if (!std::filesystem::exists(path)) GTEST_SKIP() << "f16.json not found";

    auto result = loadConfig(path);
    ASSERT_TRUE(result.ok) << "Failed to load f16.json";

    // From the real f16.dat file
    EXPECT_NEAR(result.config.geometry.emptyWeight_lbs, 18238.0, 1e-6);
    EXPECT_NEAR(result.config.geometry.area_ft2,           300.0, 1e-6);
    EXPECT_NEAR(result.config.geometry.internalFuel_lbs,  7162.0, 1e-6);
    EXPECT_NEAR(result.config.geometry.aoaMax_deg,          35.0, 1e-6);
    EXPECT_NEAR(result.config.geometry.aoaMin_deg,         -15.0, 1e-6);
    EXPECT_NEAR(result.config.geometry.maxGs,                9.0, 1e-6);
    EXPECT_NEAR(result.config.geometry.maxRoll_deg,        190.0, 1e-6);

    EXPECT_EQ(result.config.aero.mach.size(),      7u);
    EXPECT_EQ(result.config.aero.alpha_deg.size(), 21u);
    EXPECT_EQ(result.config.aero.clift.size(),    147u);

    EXPECT_EQ(result.config.engine.alt_ft.size(),  8u);
    EXPECT_EQ(result.config.engine.mach.size(),   14u);
    EXPECT_EQ(result.config.engine.thrust_idle.size(), 112u);

    EXPECT_EQ(result.config.rollCmd.alpha_deg.size(), 7u);
    EXPECT_EQ(result.config.rollCmd.qbar.size(),     14u);

    // f16 should validate cleanly
    auto vr = result.config.validate();
    EXPECT_TRUE(vr.ok()) << vr.format();
}

// ============================================================================
// Round-trip: load -> write -> re-load -> compare
// ============================================================================

TEST(ConfigLoaderTest, RoundTripPreservesConfig) {
    if (!fixturesExist()) GTEST_SKIP();

    const std::string path = std::string(kFixturesDir) + "/f16.json";
    if (!std::filesystem::exists(path)) GTEST_SKIP();

    auto r1 = loadConfig(path);
    ASSERT_TRUE(r1.ok);

    std::string json = writeConfig(r1.config);
    auto r2 = loadConfigFromString(json);
    ASSERT_TRUE(r2.ok);

    // Every field should round-trip exactly.
    EXPECT_EQ(r1.config.name, r2.config.name);
    EXPECT_NEAR(r1.config.geometry.emptyWeight_lbs,
                r2.config.geometry.emptyWeight_lbs, 1e-12);
    EXPECT_EQ(r1.config.aero.clift.size(), r2.config.aero.clift.size());
    EXPECT_EQ(r1.config.aero.clift, r2.config.aero.clift);
    EXPECT_EQ(r1.config.engine.thrust_mil, r2.config.engine.thrust_mil);
    EXPECT_EQ(r1.config.rawAuxAeroData, r2.config.rawAuxAeroData);
}

// ============================================================================
// Error handling
// ============================================================================

TEST(ConfigLoaderTest, MissingFileReturnsError) {
    auto r = loadConfig("/nonexistent/path.json");
    EXPECT_FALSE(r.ok);
    ASSERT_FALSE(r.errors.empty());
    EXPECT_NE(r.errors[0].find("Could not open"), std::string::npos);
}

TEST(ConfigLoaderTest, MalformedJsonReturnsError) {
    auto r = loadConfigFromString("{ this is not valid json ");
    EXPECT_FALSE(r.ok);
    ASSERT_FALSE(r.errors.empty());
}

TEST(ConfigLoaderTest, EmptyJsonReturnsDefaultConfig) {
    // An empty JSON object "{}" should parse OK and produce a default config.
    auto r = loadConfigFromString("{}");
    EXPECT_TRUE(r.ok);
    // The default config won't validate (no aero tables), but loading
    // itself should succeed.
    auto vr = r.config.validate();
    EXPECT_FALSE(vr.ok());
}

// ============================================================================
// Write to file
// ============================================================================

TEST(ConfigLoaderTest, WriteConfigToDiskRoundTrips) {
    if (!fixturesExist()) GTEST_SKIP();

    const std::string srcPath = std::string(kFixturesDir) + "/f16.json";
    if (!std::filesystem::exists(srcPath)) GTEST_SKIP();

    auto r1 = loadConfig(srcPath);
    ASSERT_TRUE(r1.ok);

    const std::string outPath =
        (std::filesystem::temp_directory_path() / "f4_data_write_test.json").string();
    ASSERT_TRUE(writeConfig(r1.config, outPath));

    auto r2 = loadConfig(outPath);
    ASSERT_TRUE(r2.ok);

    EXPECT_EQ(r1.config.aero.clift, r2.config.aero.clift);
    EXPECT_EQ(r1.config.engine.thrust_ab, r2.config.engine.thrust_ab);
}
