// f4-convert/tests/test_sensor_parser.cpp
//
// Tests for the SENSDATA sensor text-file parser. Anchored against the
// SHIPPED game fixture (fixtures/simdata/SENSDATA/ — IRST.LST's 8
// seekers, RWR.LST's 2 receivers, VISUAL.LST's 3 visual sensors).

#include "f4/convert/sensor_parser.hpp"

#include <gtest/gtest.h>

#include <string>

using namespace f4::convert;
using namespace f4::data;

namespace {

IrstParseResult loadShippedIrst() {
    return loadIrstListFile(
        F4_CONVERT_TEST_FIXTURES_DIR "/simdata/SENSDATA/IRST/IRST.LST");
}

RwrParseResult loadShippedRwr() {
    return loadRwrListFile(
        F4_CONVERT_TEST_FIXTURES_DIR "/simdata/SENSDATA/RWR/RWR.LST");
}

VisualParseResult loadShippedVisual() {
    return loadVisualListFile(
        F4_CONVERT_TEST_FIXTURES_DIR "/simdata/SENSDATA/VISUAL/VISUAL.LST");
}

} // namespace

// ---------------------------------------------------------------------------
// IRST
// ---------------------------------------------------------------------------
TEST(SensorParser, IrstShippedListParsesEight) {
    auto r = loadShippedIrst();
    ASSERT_TRUE(r.ok) << (r.errors.empty() ? "" : r.errors[0]);
    EXPECT_EQ(r.data.sensors.size(), 8u);
}

TEST(SensorParser, IrstAim9lMatchesFile) {
    auto r = loadShippedIrst();
    ASSERT_TRUE(r.ok);
    // AIM9L.IRS: 60 az / 60 el / 10 NM / 0.001 ground / 0.2 flare.
    const auto* aim9l = r.data.find("AIM9L");
    ASSERT_NE(aim9l, nullptr);
    EXPECT_DOUBLE_EQ(aim9l->data.az_limit_deg, 60.0);
    EXPECT_DOUBLE_EQ(aim9l->data.el_limit_deg, 60.0);
    EXPECT_DOUBLE_EQ(aim9l->data.nominal_range_nm, 10.0);
    EXPECT_DOUBLE_EQ(aim9l->data.ground_factor, 0.001);
    EXPECT_DOUBLE_EQ(aim9l->data.flare_chance, 0.2);
}

TEST(SensorParser, IrstAgm65bHasDoubleGroundFactor) {
    auto r = loadShippedIrst();
    ASSERT_TRUE(r.ok);
    // agm65b.irs: 60/60/5 NM/2.0 ground/0 flare — the Maverick's seeker
    // doubles range against ground targets and never bites flares.
    const auto* m = r.data.find("agm65b");
    ASSERT_NE(m, nullptr);
    EXPECT_DOUBLE_EQ(m->data.nominal_range_nm, 5.0);
    EXPECT_DOUBLE_EQ(m->data.ground_factor, 2.0);
    EXPECT_DOUBLE_EQ(m->data.flare_chance, 0.0);
}

TEST(SensorParser, IrstFindIsCaseInsensitive) {
    auto r = loadShippedIrst();
    ASSERT_TRUE(r.ok);
    EXPECT_NE(r.data.find("SA7"), nullptr);
    EXPECT_NE(r.data.find("sa14"), nullptr);
    EXPECT_EQ(r.data.find("nope"), nullptr);
}

TEST(SensorParser, IrstJSONRoundTrip) {
    auto r = loadShippedIrst();
    ASSERT_TRUE(r.ok);
    const std::string json = writeIrstSensorData(r.data);
    auto back = loadIrstSensorDataFromString(json);
    ASSERT_TRUE(back.ok);
    ASSERT_EQ(back.data.sensors.size(), r.data.sensors.size());
    const auto* aim9l = back.data.find("aim9l");
    ASSERT_NE(aim9l, nullptr);
    EXPECT_DOUBLE_EQ(aim9l->data.az_limit_deg, 60.0);
    EXPECT_DOUBLE_EQ(aim9l->data.flare_chance, 0.2);
}

// ---------------------------------------------------------------------------
// RWR
// ---------------------------------------------------------------------------
TEST(SensorParser, RwrShippedListParsesTwo) {
    auto r = loadShippedRwr();
    ASSERT_TRUE(r.ok) << (r.errors.empty() ? "" : r.errors[0]);
    EXPECT_EQ(r.data.sensors.size(), 2u);
}

TEST(SensorParser, RwrGenericAndHarmMatchFiles) {
    auto r = loadShippedRwr();
    ASSERT_TRUE(r.ok);
    // generic.rwr: 180 az / 90 el / sensitivity 1.0.
    const auto* g = r.data.find("generic");
    ASSERT_NE(g, nullptr);
    EXPECT_DOUBLE_EQ(g->data.az_limit_deg, 180.0);
    EXPECT_DOUBLE_EQ(g->data.el_limit_deg, 90.0);
    EXPECT_DOUBLE_EQ(g->data.sensitivity, 1.0);

    // harm.rwr: 60/60 with sensitivity 2.0 — the HARM's passive
    // receiver hears emitters twice as far.
    const auto* h = r.data.find("harm");
    ASSERT_NE(h, nullptr);
    EXPECT_DOUBLE_EQ(h->data.az_limit_deg, 60.0);
    EXPECT_DOUBLE_EQ(h->data.el_limit_deg, 60.0);
    EXPECT_DOUBLE_EQ(h->data.sensitivity, 2.0);
}

TEST(SensorParser, RwrJSONRoundTrip) {
    auto r = loadShippedRwr();
    ASSERT_TRUE(r.ok);
    auto back = loadRwrSensorDataFromString(writeRwrSensorData(r.data));
    ASSERT_TRUE(back.ok);
    const auto* h = back.data.find("harm");
    ASSERT_NE(h, nullptr);
    EXPECT_DOUBLE_EQ(h->data.sensitivity, 2.0);
}

// ---------------------------------------------------------------------------
// Visual
// ---------------------------------------------------------------------------
TEST(SensorParser, VisualShippedListParsesThree) {
    auto r = loadShippedVisual();
    ASSERT_TRUE(r.ok) << (r.errors.empty() ? "" : r.errors[0]);
    EXPECT_EQ(r.data.sensors.size(), 3u);
}

TEST(SensorParser, VisualGenericGainImpliesTenNm) {
    auto r = loadShippedVisual();
    ASSERT_TRUE(r.ok);
    // generic.vss: 181/91 (everything) and gain 3.7e9 = (10 NM in
    // feet)^2 within 1% — the original signal model's detection range.
    const auto* g = r.data.find("generic");
    ASSERT_NE(g, nullptr);
    EXPECT_DOUBLE_EQ(g->data.az_limit_deg, 181.0);
    EXPECT_DOUBLE_EQ(g->data.el_limit_deg, 91.0);
    EXPECT_DOUBLE_EQ(g->data.gain, 3.7e9);
    EXPECT_NEAR(g->data.nominal_range_nm(), 10.0, 0.05);
}

TEST(SensorParser, VisualMavAndTpodAreNarrowSeekers) {
    auto r = loadShippedVisual();
    ASSERT_TRUE(r.ok);
    // mav.vss / tpod.vss: 5-degree cones, gain 3.7e11 (~100 NM).
    for (const char* name : {"mav", "tpod"}) {
        const auto* s = r.data.find(name);
        ASSERT_NE(s, nullptr) << name;
        EXPECT_DOUBLE_EQ(s->data.az_limit_deg, 5.0);
        EXPECT_DOUBLE_EQ(s->data.el_limit_deg, 5.0);
        EXPECT_DOUBLE_EQ(s->data.gain, 3.7e11);
        EXPECT_NEAR(s->data.nominal_range_nm(), 100.0, 0.5) << name;
    }
}

TEST(SensorParser, VisualJSONRoundTrip) {
    auto r = loadShippedVisual();
    ASSERT_TRUE(r.ok);
    auto back =
        loadVisualSensorDataFromString(writeVisualSensorData(r.data));
    ASSERT_TRUE(back.ok);
    ASSERT_EQ(back.data.sensors.size(), 3u);
    const auto* m = back.data.find("mav");
    ASSERT_NE(m, nullptr);
    EXPECT_DOUBLE_EQ(m->data.az_limit_deg, 5.0);
}

// ---------------------------------------------------------------------------
// Error paths
// ---------------------------------------------------------------------------
TEST(SensorParser, MissingListEntryIsAnError) {
    auto r = loadIrstListString("1\nnonexistent.irs\n", "", "<string>");
    EXPECT_FALSE(r.ok);
    EXPECT_FALSE(r.errors.empty());
}

TEST(SensorParser, ShortSensorFileIsAnError) {
    auto r = loadIrstString("# only two values\n60.0 60.0\n", "short.irs");
    EXPECT_FALSE(r.ok);
}

TEST(SensorParser, NonNumericValueIsAnError) {
    auto r = loadRwrString("180.0\n90.0\nabc\n", "bad.rwr");
    EXPECT_FALSE(r.ok);
}

TEST(SensorParser, IrstImplausibleFlareChanceIsAnError) {
    auto r = loadIrstString("60.0\n60.0\n10.0\n0.001\n1.5\n", "bad.irs");
    EXPECT_FALSE(r.ok);
}

TEST(SensorParser, WrongJSONKindIsRejected) {
    auto back =
        loadVisualSensorDataFromString(R"({"kind": "f4.irstdata"})");
    EXPECT_FALSE(back.ok);
}

TEST(SensorParser, ListCountMismatchIsAnError) {
    // Count says 3, only 2 names follow.
    auto r = loadVisualListString("3\ngeneric.vss\nmav.vss\n", "", "<lst>");
    EXPECT_FALSE(r.ok);
}
