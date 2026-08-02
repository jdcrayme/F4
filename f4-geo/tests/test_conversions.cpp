// test_conversions.cpp — the conversion lattice.
//
// Three families:
//   1. LatLonAlt <-> ECEFPosition : exact WGS84, validated against known
//      points on the ellipsoid (equator, pole, mid-latitude) plus round-trip.
//   2. WorldPosition <-> LatLonAlt : flat-earth via TheaterDatum, validated
//      by round-trip identity and by directional sanity (north offset
//      increases latitude; heading rotation rotates the sim axes).
//   3. WorldPosition <-> ECEFPosition : composed round-trip.

#include <gtest/gtest.h>
#include <f4/geo/f4_geo.hpp>

using namespace f4::geo;

// ============================================================================
// LatLonAlt <-> ECEFPosition (exact WGS84)
// ============================================================================

TEST(LlaEcef, EquatorPrimeMeridianZeroAlt) {
    // (lat=0, lon=0, h=0) -> ECEF = (a, 0, 0).
    LatLonAlt lla(0.0, 0.0, 0.0);
    ECEFPosition e = to_ecef(lla);
    EXPECT_NEAR(e.x, WGS84_A, 1e-6);
    EXPECT_NEAR(e.y, 0.0, 1e-6);
    EXPECT_NEAR(e.z, 0.0, 1e-6);
}

TEST(LlaEcef, NorthPoleZeroAlt) {
    // (lat=pi/2, lon=0, h=0) -> ECEF = (0, 0, b).
    LatLonAlt lla(PI / 2.0, 0.0, 0.0);
    ECEFPosition e = to_ecef(lla);
    EXPECT_NEAR(e.x, 0.0, 1e-6);
    EXPECT_NEAR(e.y, 0.0, 1e-6);
    EXPECT_NEAR(e.z, WGS84_B, 1e-3);
}

TEST(LlaEcef, RoundTripMultiplePoints) {
    // A spread of points: equator, mid-lat, high-lat, with altitude.
    const std::vector<LatLonAlt> points = {
        {0.0,            0.0,             0.0},
        {38.95 * DEG_TO_RAD, -77.0 * DEG_TO_RAD, 30000.0},   // ~Washington DC, FL300
        {60.0 * DEG_TO_RAD,  120.0 * DEG_TO_RAD, 0.0},       // high northern latitude
        {-33.0 * DEG_TO_RAD, 151.0 * DEG_TO_RAD, 5000.0},    // ~Sydney
        {0.0,            90.0 * DEG_TO_RAD, 0.0},
    };
    for (const auto& orig : points) {
        const ECEFPosition e = to_ecef(orig);
        const LatLonAlt back = to_lla(e);
        EXPECT_NEAR(back.lat, orig.lat, 1e-12) << "lat mismatch at lon=" << orig.lon;
        EXPECT_NEAR(back.lon, orig.lon, 1e-12) << "lat=" << orig.lat;
        // Altitude round-trip drifts ~1e-6 ft due to the p/cos(lat) division
        // in the closed-form ECEF->LLA (float64 noise). 1e-3 ft is sub-mm.
        EXPECT_NEAR(back.alt, orig.alt, 1e-3)  << "alt mismatch";
    }
}

// ============================================================================
// WorldPosition <-> LatLonAlt (flat-earth, via TheaterDatum)
// ============================================================================

TEST(WorldLla, RoundTripIsIdentityAtOriginDatum) {
    // Datum with origin on the equator, sim frame aligned to ENU.
    TheaterDatum d(LatLonAlt{0.0, 0.0, 0.0}, 0.0);
    const std::vector<WorldPosition> pts = {
        {0.0, 0.0, 0.0},
        {10000.0, 20000.0, 5000.0},     // ~3 NM east, ~6 NM north
        {-50000.0, 80000.0, 30000.0},
        {0.0, 6076.1154855643, 0.0},    // 1 NM north
    };
    for (const auto& w : pts) {
        const LatLonAlt lla = to_lla(w, d);
        const WorldPosition back = to_world(lla, d);
        EXPECT_NEAR(back.x, w.x, 1e-6) << "x round-trip";
        EXPECT_NEAR(back.y, w.y, 1e-6) << "y round-trip";
        EXPECT_NEAR(back.z, w.z, 1e-9) << "z round-trip";
    }
}

TEST(WorldLla, NorthOffsetIncreasesLatitude) {
    TheaterDatum d(LatLonAlt{0.0, 0.0, 0.0}, 0.0);
    WorldPosition one_nm_north(0.0, 6076.1154855643, 0.0);  // 1 NM = 1852 m
    LatLonAlt lla = to_lla(one_nm_north, d);
    EXPECT_GT(lla.lat, 0.0);
    // 1 NM should be ~1 minute of latitude = 1/60 degree.
    EXPECT_NEAR(lla.lat * RAD_TO_DEG, 1.0 / 60.0, 1e-4);
    EXPECT_NEAR(lla.lon, 0.0, 1e-12);
}

TEST(WorldLla, EastOffsetIncreasesLongitude) {
    TheaterDatum d(LatLonAlt{0.0, 0.0, 0.0}, 0.0);
    WorldPosition one_nm_east(6076.1154855643, 0.0, 0.0);
    LatLonAlt lla = to_lla(one_nm_east, d);
    EXPECT_NEAR(lla.lat, 0.0, 1e-12);
    EXPECT_GT(lla.lon, 0.0);
    // At the equator, 1 NM east ~ 1 minute of longitude.
    EXPECT_NEAR(lla.lon * RAD_TO_DEG, 1.0 / 60.0, 1e-4);
}

TEST(WorldLla, HeadingRotationMapsSimYToEast) {
    // Datum heading = +90 deg: sim +Y points east.
    TheaterDatum d(LatLonAlt{0.0, 0.0, 0.0}, PI / 2.0);
    WorldPosition one_nm_along_sim_y(0.0, 6076.1154855643, 0.0);
    LatLonAlt lla = to_lla(one_nm_along_sim_y, d);
    // Going 1 NM along sim +Y with heading 90 should move us EAST, so lon>0, lat~0.
    EXPECT_NEAR(lla.lat, 0.0, 1e-12);
    EXPECT_GT(lla.lon, 0.0);
    EXPECT_NEAR(lla.lon * RAD_TO_DEG, 1.0 / 60.0, 1e-4);
}

TEST(WorldLla, HeadingRoundTripIsIdentity) {
    // Round-trip must hold for any heading, not just zero.
    TheaterDatum d(LatLonAlt{38.0 * DEG_TO_RAD, -77.0 * DEG_TO_RAD, 0.0}, 0.7);
    WorldPosition w(12345.6, -7890.1, 25000.0);
    LatLonAlt lla = to_lla(w, d);
    WorldPosition back = to_world(lla, d);
    EXPECT_NEAR(back.x, w.x, 1e-6);
    EXPECT_NEAR(back.y, w.y, 1e-6);
    EXPECT_NEAR(back.z, w.z, 1e-9);
}

TEST(WorldLla, AltitudeIsPreservedAcrossFrames) {
    TheaterDatum d(LatLonAlt{0.0, 0.0, 0.0}, 0.0);
    WorldPosition w(0.0, 0.0, 25000.0);   // FL250
    LatLonAlt lla = to_lla(w, d);
    EXPECT_NEAR(lla.alt, 25000.0, 1e-9);
}

// ============================================================================
// WorldPosition <-> ECEFPosition (composed via LatLonAlt)
// ============================================================================

TEST(WorldEcef, RoundTripComposed) {
    TheaterDatum d(LatLonAlt{38.0 * DEG_TO_RAD, -77.0 * DEG_TO_RAD, 0.0}, 0.0);
    WorldPosition w(50000.0, -30000.0, 20000.0);
    ECEFPosition e = to_ecef(w, d);
    WorldPosition back = to_world(e, d);
    // Composed round-trip: flat-earth World->LLA is exact on round-trip,
    // LLA->ECEF is exact, so the composed round-trip should be near-exact.
    EXPECT_NEAR(back.x, w.x, 1e-3);
    EXPECT_NEAR(back.y, w.y, 1e-3);
    EXPECT_NEAR(back.z, w.z, 1e-6);
}
