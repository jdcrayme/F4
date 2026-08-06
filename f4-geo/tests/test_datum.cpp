// test_datum.cpp — the TheaterDatum type itself (conversions that use a
// datum are exercised in test_conversions.cpp).

#include <gtest/gtest.h>
#include <f4/geo/f4_geo.hpp>

using namespace f4::geo;

TEST(Datum, DefaultConstructsToZeroOriginZeroHeading) {
    TheaterDatum d;
    EXPECT_EQ(d.origin, LatLonAlt(0.0, 0.0, 0.0));
    EXPECT_DOUBLE_EQ(d.heading_rad, 0.0);
}

TEST(Datum, ConstructsWithOriginAndHeading) {
    LatLonAlt origin{38.0 * DEG_TO_RAD, -77.0 * DEG_TO_RAD, 0.0};
    TheaterDatum d(origin, 0.5);
    EXPECT_EQ(d.origin, origin);
    EXPECT_DOUBLE_EQ(d.heading_rad, 0.5);
}

TEST(Datum, ComparisonIsValueBased) {
    TheaterDatum a(LatLonAlt{1.0, 2.0, 3.0}, 0.1);
    TheaterDatum b(LatLonAlt{1.0, 2.0, 3.0}, 0.1);
    TheaterDatum c(LatLonAlt{1.0, 2.0, 3.0}, 0.2);
    EXPECT_EQ(a, b);
    EXPECT_NE(a, c);
}

TEST(Datum, IdentityFactory) {
    TheaterDatum d = TheaterDatum::identity();
    EXPECT_EQ(d.origin, LatLonAlt(0.0, 0.0, 0.0));
    EXPECT_DOUBLE_EQ(d.heading_rad, 0.0);
}
