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
    EXPECT_EQ(p, (WorldPosition{0.0, 0.0, 0.0}));
}

TEST(Position, WorldPositionComparisonIsValueBased) {
    EXPECT_EQ(WorldPosition(1, 2, 3), WorldPosition(1, 2, 3));
    EXPECT_NE(WorldPosition(1, 2, 3), WorldPosition(1, 2, 4));
}

TEST(Position, WorldPositionOffsetArithmetic) {
    WorldPosition a(1000.0, 2000.0, 5000.0);
    WorldPosition b(   0.0,  500.0,   50.0);
    EXPECT_EQ(a + b, (WorldPosition{1000.0, 2500.0, 5050.0}));
    EXPECT_EQ(a - b, (WorldPosition{1000.0, 1500.0, 4950.0}));
    a += b;
    EXPECT_EQ(a, (WorldPosition{1000.0, 2500.0, 5050.0}));
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
    EXPECT_EQ(lla, (LatLonAlt{0.0, 0.0, 0.0}));
    // 1000 ft altitude -> meters.
    LatLonAlt up(0, 0, 1000.0);
    EXPECT_NEAR(up.alt_meters(), 304.8, 1e-9);
}

TEST(Position, ECEFFieldsAreAccessible) {
    ECEFPosition e(6378137.0, 0.0, 0.0);
    EXPECT_DOUBLE_EQ(e.x, 6378137.0);
    EXPECT_EQ(e, (ECEFPosition{6378137.0, 0.0, 0.0}));
}

TEST(Position, NoImplicitConversionBetweenFrames) {
    // This is a compile-time contract, not a runtime test. The following
    // would fail to compile if uncommented, which is the desired behaviour:
    //
    //   WorldPosition w;
    //   LatLonAlt lla = w;            // no implicit conversion
    //   ECEFPosition e = lla;          // no implicit conversion
    //   void f(LatLonAlt);
    //   f(w);                          // no implicit conversion
    //
    // Every crossing must be a named to_lla()/to_ecef()/to_world() call.
    SUCCEED();
}
