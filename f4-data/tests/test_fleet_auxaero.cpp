// f4-data/tests/test_fleet_auxaero.cpp
//
// Certifies the COMMITTED fleet (Data/Aircraft/*.json, 25 aircraft) against
// the AuxAeroData schema (auxaero_rosetta.hpp):
//   - every aircraft carries the complete 443-key record;
//   - every record key is in the schema, with the schema's value type;
//   - the record and the typed `aux` view agree on the keys a .dat override
//     touches (rawAuxAeroData ∩ typed mapping — currently f16's
//     criticalAOA), so the record is not a parallel universe;
//   - the F-16's criticalAOA is the .dat's 25.0 (the stall guard the
//     FreeFalcon fleet data intends — the previously committed 0.0 silently
//     DISABLED the F-16's stall model).
//
// C++20. GoogleTest.

#include <f4/data/aux_aero_record.hpp>
#include <f4/data/auxaero_rosetta.hpp>
#include <f4/data/config_loader.hpp>
#include <f4/units.hpp>

#include <gtest/gtest.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <sstream>

using namespace f4::data;

namespace {

constexpr const char* kFleetDir = F4_SOURCE_DIR "/Data/Aircraft";

std::vector<std::filesystem::path> fleetFiles() {
    std::vector<std::filesystem::path> out;
    for (const auto& e : std::filesystem::directory_iterator(kFleetDir)) {
        if (e.path().extension() == ".json") out.push_back(e.path());
    }
    std::sort(out.begin(), out.end());
    return out;
}

bool fleetExists() {
    return std::filesystem::is_directory(kFleetDir) && !fleetFiles().empty();
}

/// The typed-view counterpart conversion for the angle/mach keys the parser
/// maps into the typed AuxAero view. Returns nullopt for keys without a
/// typed counterpart (the vast majority of the schema).
std::optional<double> typedViewRadians(const std::string& key, const AuxAeroValue& v) {
    auto deg = [&](double d) {
        return f4::Quantity<f4::Degrees>(d).to<f4::Radians>().value();
    };
    if (v.type != AuxAeroValueType::Float) return std::nullopt;
    static const std::array<const char*, 7> kAngleKeys = {
        "criticalAOA", "landingAOA", "lefMaxAngle", "tefMaxAngle",
        "rudderMaxAngle", "aileronMaxAngle", "airbrakeMaxAngle",
    };
    for (const char* k : kAngleKeys) {
        if (key == k) return deg(v.f);
    }
    return std::nullopt;
}

} // namespace

TEST(FleetAuxAeroTest, FleetIsPresent) {
    if (!fleetExists()) GTEST_SKIP() << "committed Data/Aircraft not found";
    EXPECT_EQ(fleetFiles().size(), std::size_t{25});
}

TEST(FleetAuxAeroTest, EveryAircraftCarriesTheCompleteSchema) {
    if (!fleetExists()) GTEST_SKIP() << "committed Data/Aircraft not found";

    for (const auto& path : fleetFiles()) {
        auto r = loadConfig(path.string());
        ASSERT_TRUE(r.ok) << path.filename().string();
        EXPECT_TRUE(r.warnings.empty())
            << path.filename().string() << ": " << r.warnings[0];

        const AuxAeroRecord& rec = r.config.auxAero;
        ASSERT_EQ(rec.size(), kAuxAeroRosettaCount)
            << path.filename().string() << " record is incomplete";

        // Every key is a schema key, with the schema's value type.
        for (const auto& [key, val] : rec) {
            const RosettaEntry* e = findAuxAeroEntry(key);
            ASSERT_NE(e, nullptr)
                << path.filename().string() << ": unknown record key " << key;
            switch (e->type) {
                case RosettaType::Float:
                    EXPECT_EQ(val.type, AuxAeroValueType::Float) << key;
                    break;
                case RosettaType::Int:
                    EXPECT_EQ(val.type, AuxAeroValueType::Int) << key;
                    break;
                case RosettaType::Vector:
                    EXPECT_EQ(val.type, AuxAeroValueType::Vector) << key;
                    break;
                case RosettaType::LookupTable:
                case RosettaType::TwoDTable:
                    EXPECT_EQ(val.type, AuxAeroValueType::Chart) << key;
                    break;
            }
        }
    }
}

TEST(FleetAuxAeroTest, F16CriticalAOAIsTheDatOverride) {
    if (!fleetExists()) GTEST_SKIP() << "committed Data/Aircraft not found";

    auto r = loadConfig(std::string(kFleetDir) + "/f16.json");
    ASSERT_TRUE(r.ok);

    // The .dat's verbatim override...
    ASSERT_EQ(r.config.rawAuxAeroData.count("criticalAOA"), std::size_t{1});
    EXPECT_EQ(r.config.rawAuxAeroData.at("criticalAOA"), "25.0");
    // ...drives the record...
    ASSERT_EQ(r.config.auxAero.count("criticalAOA"), std::size_t{1});
    EXPECT_EQ(r.config.auxAero.at("criticalAOA").type, AuxAeroValueType::Float);
    EXPECT_EQ(r.config.auxAero.at("criticalAOA").f, 25.0);
    // ...and lands in the typed view the stall guard reads (25 deg).
    EXPECT_NEAR(r.config.aux.criticalAOA.to<f4::Degrees>().value(), 25.0, 1e-9);
}

TEST(FleetAuxAeroTest, RecordAndTypedViewAgreeOnOverriddenAngles) {
    if (!fleetExists()) GTEST_SKIP() << "committed Data/Aircraft not found";

    for (const auto& path : fleetFiles()) {
        auto r = loadConfig(path.string());
        ASSERT_TRUE(r.ok) << path.filename().string();

        // For every raw .dat override that is a SCHEMA key mapping to a
        // typed angle, the typed view must equal the record's value
        // converted to radians. Raw overrides outside the schema (verbatim
        // capture by design — e.g. f18.dat's stray "1.0F" token) are not
        // record keys.
        for (const auto& [key, raw] : r.config.rawAuxAeroData) {
            if (findAuxAeroEntry(key) == nullptr) continue;

            auto recIt = r.config.auxAero.find(key);
            ASSERT_NE(recIt, r.config.auxAero.end())
                << path.filename().string() << ": raw override " << key
                << " missing from the record";
            auto typed = typedViewRadians(key, recIt->second);
            if (!typed.has_value()) continue;

            if (key == "criticalAOA") {
                EXPECT_EQ(r.config.aux.criticalAOA.value(), *typed)
                    << path.filename().string() << ": " << key;
            } else {
                ADD_FAILURE() << path.filename().string()
                              << ": unverified angle override " << key
                              << " — extend this test's typed mapping";
            }
        }
    }
}

TEST(FleetAuxAeroTest, KC10ReconstructedRecordCarriesTheKnownOverrides) {
    if (!fleetExists()) GTEST_SKIP() << "committed Data/Aircraft not found";

    // kc10.dat is not among the committed fixtures (the one fleet aircraft
    // converted before the fixture set existed); its record is reconstructed
    // from its typed view (scripts/reconstruct_kc10_auxaero.py, documented in
    // CHANGES.md). The overrides we KNOW the real .dat set are visible here.
    auto r = loadConfig(std::string(kFleetDir) + "/kc10.json");
    ASSERT_TRUE(r.ok);

    const AuxAeroRecord& rec = r.config.auxAero;
    EXPECT_EQ(rec.size(), kAuxAeroRosettaCount);

    EXPECT_EQ(rec.at("hasLef").i, 0);       // typed view: hasLef = false
    EXPECT_EQ(rec.at("hasTef").i, 0);       // typed view: hasTef = false
    EXPECT_EQ(rec.at("typeEngine").i, 2);
    EXPECT_EQ(rec.at("jfsSpoolUpRate").f, 10.0);
    EXPECT_EQ(rec.at("jfsSpoolUpLimit").f, 0.7);
    EXPECT_EQ(rec.at("lefMaxMach").f, 1.0);
    EXPECT_EQ(rec.at("tefTakeoff").f, 20.0);
    EXPECT_EQ(rec.at("elevatorRoll").i, 0);
}

TEST(FleetAuxAeroTest, RecordSurvivesAWriteLoadRoundTrip) {
    if (!fleetExists()) GTEST_SKIP() << "committed Data/Aircraft not found";

    for (const auto& path : fleetFiles()) {
        auto r = loadConfig(path.string());
        ASSERT_TRUE(r.ok) << path.filename().string();

        auto r2 = loadConfigFromString(writeConfig(r.config));
        ASSERT_TRUE(r2.ok) << path.filename().string();

        ASSERT_EQ(r2.config.auxAero.size(), r.config.auxAero.size())
            << path.filename().string();
        for (const auto& [key, expected] : r.config.auxAero) {
            const AuxAeroValue& actual = r2.config.auxAero.at(key);
            EXPECT_EQ(actual.type, expected.type) << key;
            switch (expected.type) {
                case AuxAeroValueType::Float:  EXPECT_EQ(actual.f, expected.f); break;
                case AuxAeroValueType::Int:    EXPECT_EQ(actual.i, expected.i); break;
                case AuxAeroValueType::Vector: EXPECT_EQ(actual.v, expected.v); break;
                case AuxAeroValueType::Chart:  EXPECT_EQ(actual.t, expected.t); break;
            }
        }
    }
}
