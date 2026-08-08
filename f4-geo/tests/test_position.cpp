// test_position.cpp — the absolute position strong types.
//
// Verifies construction, comparison, offset arithmetic, and the unit
// accessors. The point of these tests is partly behavioural and partly
// contract: these types must remain distinct (no implicit conversion) and
// default-constructible, and WorldPosition arithmetic must behave as an
// offset vector.

#include <gtest/gtest.h>
#include <f4/geo/f4_geo.hpp>

using namespace f4::geo;

TEST(Position, WorldPositionDefaultConstructsToZero) {
    WorldPosition p;
    EXPECT_EQ(p, WorldPosition(0.0, 0.0, 0.0));
}

TEST(Position, WorldPositionComparisonIsValueBased) {
    EXPECT_EQ(WorldPosition(1, 2, 3), WorldPosition(1, 2, 3));
    EXPECT_NE(WorldPosition(1, 2, 3), WorldPosition(1, 2, 4));
}

TEST(Position, WorldPositionOffsetArithmetic) {
    WorldPosition a(1000.0, 2000.0, 5000.0);
    WorldPosition b(   0.0,  500.0,   50.0);
    EXPECT_EQ(a + b, WorldPosition(1000.0, 2500.0, 5050.0));
    EXPECT_EQ(a - b, WorldPosition(1000.0, 1500.0, 4950.0));
    a += b;
    EXPECT_EQ(a, WorldPosition(1000.0, 2500.0, 5050.0));
}

TEST(Position, WorldPositionDistance3D) {
    WorldPosition a(0, 0, 0);
    WorldPosition b(3.0, 4.0, 12.0);   // 3-4-12 -> 13
    EXPECT_DOUBLE_EQ(a.distance_to(b), 13.0);
}

TEST(Position, WorldPositionDistanceHorizontalIgnoresAltitude) {
    WorldPosition a(0, 0, 0);
    WorldPosition b(3.0, 4.0, 99999.0);
    EXPECT_DOUBLE_EQ(a.distance_horiz_to(b), 5.0);
}

TEST(Position, LatLonAltDefaultsAndAccessors) {
    LatLonAlt lla;
    EXPECT_EQ(lla, LatLonAlt(0.0, 0.0, 0.0));
    // 1000 ft altitude -> meters.
    LatLonAlt up(0, 0, 1000.0);
    EXPECT_NEAR(up.alt_meters(), 304.8, 1e-9);
}

TEST(Position, ECEFFieldsAreAccessible) {
    ECEFPosition e(6378137.0, 0.0, 0.0);
    EXPECT_DOUBLE_EQ(e.x, 6378137.0);
    EXPECT_EQ(e, ECEFPosition(6378137.0, 0.0, 0.0));
}

TEST(Position, FromDegreesFactoryConvertsCorrectly) {
    // The from_degrees factory is the recommended way to construct a
    // LatLonAlt from human-readable coordinates. It applies DEG_TO_RAD
    // so callers don't have to remember the radians convention.
    LatLonAlt lla = LatLonAlt::from_degrees(38.0, -77.0, 1000.0);
    EXPECT_NEAR(lla.lat, 38.0 * DEG_TO_RAD, 1e-12);
    EXPECT_NEAR(lla.lon, -77.0 * DEG_TO_RAD, 1e-12);
    EXPECT_NEAR(lla.alt, 1000.0, 1e-12);
    EXPECT_NEAR(lla.alt_meters(), 304.8, 1e-9);
}

TEST(Position, NoImplicitConversionBetweenFrames) {
    // This is a compile-time contract, not a runtime test. The following
    // would fail to compile if uncommented, which is the desired behaviour:
    //
    //   WorldPosition w;
    //   LatLonAlt lla = w;            // no implicit conversion
    //   ECEFPosition e = lla;          // no implicit conversion
    //   NEDPosition n = w;             // no implicit conversion
    //   void f(LatLonAlt);
    //   f(w);                          // no implicit conversion
    //
    // Every crossing must be a named to_lla()/to_ecef()/to_world()/to_ned()/to_enu() call.
    //
    // Additionally, the 3-arg ctors are explicit — so this also fails:
    //
    //   LatLonAlt lla = {0.0, 0.0, 0.0};   // copy-init from brace init
    //   void g(LatLonAlt);
    //   g({0.0, 0.0, 0.0});                 // implicit conversion
    //
    // Callers must use either `LatLonAlt{...}` (direct-init, allowed) or
    // the typed factory `LatLonAlt::from_degrees(...)`.
    SUCCEED();
}

// ============================================================================
// NEDPosition — North-East-Down frame
// ============================================================================

TEST(NEDPosition, DefaultConstructsToZero) {
    NEDPosition p;
    EXPECT_EQ(p, NEDPosition(0.0, 0.0, 0.0));
}

TEST(NEDPosition, ComparisonIsValueBased) {
    EXPECT_EQ(NEDPosition(1, 2, 3), NEDPosition(1, 2, 3));
    EXPECT_NE(NEDPosition(1, 2, 3), NEDPosition(1, 2, 4));
}

TEST(NEDPosition, OffsetArithmetic) {
    NEDPosition a(1000.0, 2000.0, -5000.0);
    NEDPosition b(   0.0,  500.0,   -50.0);
    EXPECT_EQ(a + b, NEDPosition(1000.0, 2500.0, -5050.0));
    EXPECT_EQ(a - b, NEDPosition(1000.0, 1500.0, -4950.0));
    a += b;
    EXPECT_EQ(a, NEDPosition(1000.0, 2500.0, -5050.0));
}

TEST(NEDPosition, Distance3D) {
    NEDPosition a(0, 0, 0);
    NEDPosition b(3.0, 4.0, 12.0);   // 3-4-12 -> 13
    EXPECT_DOUBLE_EQ(a.distance_to(b), 13.0);
}

TEST(NEDPosition, AltitudeIsNegativeZ) {
    NEDPosition p(0, 0, -10000.0);  // 10000 ft up
    EXPECT_DOUBLE_EQ(p.altitude_ft(), 10000.0);
    NEDPosition ground(0, 0, 0.0);
    EXPECT_DOUBLE_EQ(ground.altitude_ft(), 0.0);
}

// ============================================================================
// Frame crossing: ENU ↔ NED
// ============================================================================

TEST(FrameCrossing, EnuToNedToEnuRoundTrip) {
    WorldPosition enu(100.0, 200.0, 5000.0);  // E=100, N=200, U=5000
    NEDPosition ned = to_ned(enu);
    // NED: x=N=200, y=E=100, z=D=-5000
    EXPECT_DOUBLE_EQ(ned.x, 200.0);
    EXPECT_DOUBLE_EQ(ned.y, 100.0);
    EXPECT_DOUBLE_EQ(ned.z, -5000.0);

    WorldPosition roundtrip = to_enu(ned);
    EXPECT_DOUBLE_EQ(roundtrip.x, 100.0);
    EXPECT_DOUBLE_EQ(roundtrip.y, 200.0);
    EXPECT_DOUBLE_EQ(roundtrip.z, 5000.0);
}

TEST(FrameCrossing, NedToEnuToNedRoundTrip) {
    NEDPosition ned(300.0, 400.0, -10000.0);  // N=300, E=400, D=-10000
    WorldPosition enu = to_enu(ned);
    // ENU: x=E=400, y=N=300, z=U=10000
    EXPECT_DOUBLE_EQ(enu.x, 400.0);
    EXPECT_DOUBLE_EQ(enu.y, 300.0);
    EXPECT_DOUBLE_EQ(enu.z, 10000.0);

    NEDPosition roundtrip = to_ned(enu);
    EXPECT_DOUBLE_EQ(roundtrip.x, 300.0);
    EXPECT_DOUBLE_EQ(roundtrip.y, 400.0);
    EXPECT_DOUBLE_EQ(roundtrip.z, -10000.0);
}

TEST(FrameCrossing, AltitudePreservedAcrossFrames) {
    WorldPosition enu(0, 0, 25000.0);  // 25000 ft up in ENU
    NEDPosition ned = to_ned(enu);
    EXPECT_DOUBLE_EQ(ned.altitude_ft(), 25000.0);
}

TEST(FrameCrossing, ZeroIsOriginInBothFrames) {
    WorldPosition enu_zero;
    NEDPosition ned_zero = to_ned(enu_zero);
    EXPECT_EQ(ned_zero, NEDPosition(0, 0, 0));

    NEDPosition ned_origin;
    WorldPosition enu_origin = to_enu(ned_origin);
    EXPECT_EQ(enu_origin, WorldPosition(0, 0, 0));
}
