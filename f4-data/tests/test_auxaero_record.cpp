// f4-data/tests/test_auxaero_record.cpp
//
// The complete AuxAeroData record (aux_aero_record.hpp): the schema table's
// integrity, the JSON load-side type resolution (Vector vs Chart
// disambiguation is rosetta-guided — a chart can legitimately have exactly
// three tokens), and the write -> load round-trip.
//
// C++20. GoogleTest.

#include <f4/data/aux_aero_record.hpp>
#include <f4/data/auxaero_rosetta.hpp>
#include <f4/data/config_loader.hpp>

#include <gtest/gtest.h>

using namespace f4::data;

// ============================================================================
// The schema table itself.
// ============================================================================

TEST(AuxAeroRosettaTest, TableHasExpectedSize) {
    EXPECT_EQ(kAuxAeroRosettaCount, std::size_t{443});
    // The extern array is declared with the same bound.
    EXPECT_EQ(kAuxAeroRosettaCount, std::size_t{443});
}

TEST(AuxAeroRosettaTest, KeysAreUniqueAndNonNull) {
    for (std::size_t i = 0; i < kAuxAeroRosettaCount; ++i) {
        ASSERT_NE(kAuxAeroRosetta[i].key, nullptr);
        ASSERT_NE(kAuxAeroRosetta[i].default_tokens, nullptr);
        for (std::size_t j = i + 1; j < kAuxAeroRosettaCount; ++j) {
            // 443^2/2 = ~98k string compares — trivial for a test.
            ASSERT_NE(std::string(kAuxAeroRosetta[i].key),
                      std::string(kAuxAeroRosetta[j].key))
                << "duplicate key: " << kAuxAeroRosetta[i].key;
        }
    }
}

TEST(AuxAeroRosettaTest, DefaultsParsePerType) {
    // Two defaults are verbatim legacy artifacts (readin.cpp data defects,
    // parsed with strtod semantics + zero-fill by completeAuxAeroRecord and
    // warned about):
    //   vortexAOALimit — a 3-vector default with only one token ("29.5");
    //   FlareVec4      — "0 0,200", whose "0,200" strtod-parses as 0.
    for (std::size_t i = 0; i < kAuxAeroRosettaCount; ++i) {
        const RosettaEntry& e = kAuxAeroRosetta[i];
        std::istringstream vs(e.default_tokens);
        switch (e.type) {
            case RosettaType::Float: {
                double d = 0.0;
                EXPECT_TRUE(vs >> d) << e.key;
                break;
            }
            case RosettaType::Int: {
                long long v = 0;
                EXPECT_TRUE(vs >> v) << e.key;
                break;
            }
            case RosettaType::Vector: {
                if (std::string(e.key) == "vortexAOALimit" ||
                    std::string(e.key) == "FlareVec4") {
                    double d = 0.0;
                    EXPECT_TRUE(vs >> d) << e.key;
                    break;
                }
                double x = 0.0, y = 0.0, z = 0.0;
                EXPECT_TRUE(vs >> x >> y >> z) << e.key;
                break;
            }
            case RosettaType::LookupTable:
            case RosettaType::TwoDTable: {
                double d = 0.0;
                int n = 0;
                while (vs >> d) ++n;
                EXPECT_GE(n, 1) << e.key;
                break;
            }
        }
    }
}

TEST(AuxAeroRosettaTest, FindEntry) {
    EXPECT_NE(findAuxAeroEntry("normSpoolRate"), nullptr);
    EXPECT_EQ(findAuxAeroEntry("normSpoolRate")->type, RosettaType::Float);
    EXPECT_NE(findAuxAeroEntry("gunLocation"), nullptr);
    EXPECT_EQ(findAuxAeroEntry("gunLocation")->type, RosettaType::Vector);
    EXPECT_NE(findAuxAeroEntry("sndAero1AOAChart"), nullptr);
    EXPECT_EQ(findAuxAeroEntry("sndAero1AOAChart")->type, RosettaType::LookupTable);
    // Case-sensitive, matching FreeFalcon's strcmp-based lookup.
    EXPECT_EQ(findAuxAeroEntry("normspoolrate"), nullptr);
    EXPECT_EQ(findAuxAeroEntry("notARealKey"), nullptr);
}

// ============================================================================
// JSON load-side type resolution.
// ============================================================================

namespace {

LoadResult loadFrom(const std::string& jsonText) {
    return loadConfigFromString(jsonText);
}

} // namespace

TEST(AuxAeroRecordJsonTest, VectorAndChartDisambiguatedBySchema) {
    // "1 0 0" as a Vector (gunLocation) vs the SAME three tokens as a Chart
    // (sndAero1AOAChart) — only the rosetta can tell them apart.
    const std::string jsonText = R"({
        "name": "t",
        "aux": {},
        "aero": {},
        "engine": {},
        "rollCmd": {},
        "limiters": [],
        "auxAero": {
            "gunLocation": [2.0, -2.0, 0.0],
            "sndAero1AOAChart": [1.0, 0.0, 0.0],
            "criticalAOA": 25.0,
            "nEngines": 2
        }
    })";

    auto r = loadFrom(jsonText);
    ASSERT_TRUE(r.ok) << (r.errors.empty() ? "" : r.errors[0]);
    EXPECT_TRUE(r.warnings.empty()) << (r.warnings.empty() ? "" : r.warnings[0]);

    const AuxAeroRecord& rec = r.config.auxAero;
    ASSERT_EQ(rec.size(), std::size_t{4});

    EXPECT_EQ(rec.at("gunLocation").type, AuxAeroValueType::Vector);
    EXPECT_EQ(rec.at("gunLocation").v[0], 2.0);
    EXPECT_EQ(rec.at("gunLocation").v[1], -2.0);
    EXPECT_EQ(rec.at("gunLocation").v[2], 0.0);

    EXPECT_EQ(rec.at("sndAero1AOAChart").type, AuxAeroValueType::Chart);
    ASSERT_EQ(rec.at("sndAero1AOAChart").t.size(), std::size_t{3});
    EXPECT_EQ(rec.at("sndAero1AOAChart").t[0], 1.0);

    // A JSON float loads as Float even though it is integral-looking...
    EXPECT_EQ(rec.at("criticalAOA").type, AuxAeroValueType::Float);
    EXPECT_EQ(rec.at("criticalAOA").f, 25.0);
    // ...and a JSON integer loads as Int.
    EXPECT_EQ(rec.at("nEngines").type, AuxAeroValueType::Int);
    EXPECT_EQ(rec.at("nEngines").i, 2);
}

TEST(AuxAeroRecordJsonTest, UnknownKeyWarnsAndCaptures) {
    const std::string jsonText = R"({
        "name": "t",
        "aux": {},
        "auxAero": { "notInTheSchema": [1.0, 2.0] }
    })";

    auto r = loadFrom(jsonText);
    ASSERT_TRUE(r.ok);
    ASSERT_EQ(r.warnings.size(), std::size_t{1});
    EXPECT_NE(r.warnings[0].find("not in the schema"), std::string::npos);

    const AuxAeroRecord& rec = r.config.auxAero;
    ASSERT_EQ(rec.size(), std::size_t{1});
    EXPECT_EQ(rec.at("notInTheSchema").type, AuxAeroValueType::Chart);
    ASSERT_EQ(rec.at("notInTheSchema").t.size(), std::size_t{2});
}

TEST(AuxAeroRecordJsonTest, WrongShapedVectorWarns) {
    const std::string jsonText = R"({
        "name": "t",
        "aux": {},
        "auxAero": { "gunLocation": [1.0, 2.0] }
    })";

    auto r = loadFrom(jsonText);
    ASSERT_TRUE(r.ok);
    ASSERT_EQ(r.warnings.size(), std::size_t{1});
    EXPECT_NE(r.warnings[0].find("schema says 3"), std::string::npos);
    EXPECT_EQ(r.config.auxAero.at("gunLocation").type, AuxAeroValueType::Chart);
}

// ============================================================================
// Write -> load round-trip.
// ============================================================================

TEST(AuxAeroRecordJsonTest, RoundTripPreservesValuesAndTypes) {
    AircraftConfig cfg;
    cfg.name = "roundtrip";

    auto setF = [&](const char* k, double v) {
        AuxAeroValue val; val.type = AuxAeroValueType::Float; val.f = v;
        cfg.auxAero[k] = val;
    };
    auto setI = [&](const char* k, int64_t v) {
        AuxAeroValue val; val.type = AuxAeroValueType::Int; val.i = v;
        cfg.auxAero[k] = val;
    };
    auto setV = [&](const char* k, double x, double y, double z) {
        AuxAeroValue val; val.type = AuxAeroValueType::Vector; val.v = {x, y, z};
        cfg.auxAero[k] = val;
    };
    auto setT = [&](const char* k, std::vector<double> t) {
        AuxAeroValue val; val.type = AuxAeroValueType::Chart; val.t = std::move(t);
        cfg.auxAero[k] = val;
    };

    setF("normSpoolRate", 0.7);
    setF("criticalAOA", 25.0);
    setI("nEngines", 2);
    setI("hasLef", 2);
    setV("gunLocation", 2.0, -2.0, 0.0);
    setT("sndAbIntPitchChart", {2.0, 0.0, 0.0, 1.03, 1.47});

    const std::string text = writeConfig(cfg);
    auto r = loadConfigFromString(text);
    ASSERT_TRUE(r.ok);
    EXPECT_TRUE(r.warnings.empty());

    const AuxAeroRecord& rec = r.config.auxAero;
    ASSERT_EQ(rec.size(), cfg.auxAero.size());
    for (const auto& [key, expected] : cfg.auxAero) {
        ASSERT_TRUE(rec.count(key)) << key;
        const AuxAeroValue& actual = rec.at(key);
        EXPECT_EQ(actual.type, expected.type) << key;
        switch (expected.type) {
            case AuxAeroValueType::Float:  EXPECT_EQ(actual.f, expected.f); break;
            case AuxAeroValueType::Int:    EXPECT_EQ(actual.i, expected.i); break;
            case AuxAeroValueType::Vector: EXPECT_EQ(actual.v, expected.v); break;
            case AuxAeroValueType::Chart:  EXPECT_EQ(actual.t, expected.t); break;
        }
    }
}
