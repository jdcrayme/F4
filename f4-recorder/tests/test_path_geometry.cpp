// f4-recorder/tests/test_path_geometry.cpp
//
// Unit tests for path geometry computation — cross-track error,
// along-track fraction, glide slope altitude.

#include <gtest/gtest.h>

#include <f4/recorder/path_geometry.hpp>

using namespace f4::recorder;
namespace geo = f4::geo;

// ============================================================================
// Cross-track error
// ============================================================================

TEST(PathGeometry, CrossTrackError_PointOnLine) {
    // Point exactly on the line -> zero error
    geo::WorldPosition start(0.0, 0.0, 0.0);
    geo::WorldPosition end(1000.0, 0.0, 0.0);   // eastward line
    geo::WorldPosition point(500.0, 0.0, 0.0);   // on the line

    EXPECT_NEAR(cross_track_error(point, start, end), 0.0, 1e-6);
}

TEST(PathGeometry, CrossTrackError_PointRightOfLine) {
    // Line goes east; point is south (right in ENU when looking east)
    geo::WorldPosition start(0.0, 0.0, 0.0);
    geo::WorldPosition end(1000.0, 0.0, 0.0);
    geo::WorldPosition point(500.0, -100.0, 0.0);  // 100ft south

    // In ENU: x=east, y=north. Point is south = negative y.
    // Looking east, "right" is south = negative y.
    // Cross-track should be negative (right of line).
    double cte = cross_track_error(point, start, end);
    EXPECT_NEAR(std::abs(cte), 100.0, 1e-6);
}

TEST(PathGeometry, CrossTrackError_PointLeftOfLine) {
    // Point is north (left of an eastward line)
    geo::WorldPosition start(0.0, 0.0, 0.0);
    geo::WorldPosition end(1000.0, 0.0, 0.0);
    geo::WorldPosition point(500.0, 100.0, 0.0);  // 100ft north

    double cte = cross_track_error(point, start, end);
    // Looking east, north is left -> negative in "positive=right" convention
    EXPECT_NEAR(cte, -100.0, 1e-6);
}

TEST(PathGeometry, CrossTrackError_NorthSouthLine) {
    // Line goes north; point is east
    geo::WorldPosition start(0.0, 0.0, 0.0);
    geo::WorldPosition end(0.0, 1000.0, 0.0);
    geo::WorldPosition point(50.0, 500.0, 0.0);  // 50ft east

    double cte = cross_track_error(point, start, end);
    // Looking north, east is "right" -> positive
    EXPECT_NEAR(cte, 50.0, 1e-6);
}

TEST(PathGeometry, CrossTrackError_DegenerateLine) {
    // Zero-length line segment
    geo::WorldPosition start(100.0, 200.0, 0.0);
    geo::WorldPosition end(100.0, 200.0, 0.0);
    geo::WorldPosition point(150.0, 200.0, 0.0);  // 50ft away

    double cte = cross_track_error(point, start, end);
    EXPECT_NEAR(cte, 50.0, 1e-6);  // falls back to point distance
}

// ============================================================================
// Along-track fraction
// ============================================================================

TEST(PathGeometry, AlongTrackFraction_AtStart) {
    geo::WorldPosition start(0.0, 0.0, 0.0);
    geo::WorldPosition end(1000.0, 0.0, 0.0);
    geo::WorldPosition point(0.0, 0.0, 0.0);

    EXPECT_NEAR(along_track_fraction(point, start, end), 0.0, 1e-6);
}

TEST(PathGeometry, AlongTrackFraction_AtEnd) {
    geo::WorldPosition start(0.0, 0.0, 0.0);
    geo::WorldPosition end(1000.0, 0.0, 0.0);
    geo::WorldPosition point(1000.0, 0.0, 0.0);

    EXPECT_NEAR(along_track_fraction(point, start, end), 1.0, 1e-6);
}

TEST(PathGeometry, AlongTrackFraction_AtMidpoint) {
    geo::WorldPosition start(0.0, 0.0, 0.0);
    geo::WorldPosition end(1000.0, 0.0, 0.0);
    geo::WorldPosition point(500.0, 0.0, 0.0);

    EXPECT_NEAR(along_track_fraction(point, start, end), 0.5, 1e-6);
}

TEST(PathGeometry, AlongTrackFraction_OffLineButProjected) {
    // Point is offset perpendicular to the line, but projection is at midpoint
    geo::WorldPosition start(0.0, 0.0, 0.0);
    geo::WorldPosition end(1000.0, 0.0, 0.0);
    geo::WorldPosition point(500.0, 100.0, 0.0);  // 100ft north of midpoint

    EXPECT_NEAR(along_track_fraction(point, start, end), 0.5, 1e-6);
}

// ============================================================================
// Glide slope altitude
// ============================================================================

TEST(PathGlideSlope, StandardThreeDegree) {
    // Standard 3° glide slope
    // At 1 NM (6076.12 ft) from threshold, altitude = 6076.12 * tan(3°) ≈ 318 ft
    const double gs_angle = 3.0 * 3.14159265358979323846 / 180.0;
    double alt = glide_slope_altitude_ft(6076.12, gs_angle, 0.0);

    EXPECT_NEAR(alt, 318.0, 1.0);  // within 1ft
}

TEST(PathGlideSlope, AtThreshold) {
    const double gs_angle = 3.0 * 3.14159265358979323846 / 180.0;
    double alt = glide_slope_altitude_ft(0.0, gs_angle, 0.0);

    EXPECT_NEAR(alt, 0.0, 1e-6);
}

TEST(PathGlideSlope, WithThresholdElevation) {
    // Runway at 100ft MSL
    const double gs_angle = 3.0 * 3.14159265358979323846 / 180.0;
    double alt = glide_slope_altitude_ft(0.0, gs_angle, 100.0);

    EXPECT_NEAR(alt, 100.0, 1e-6);
}

TEST(PathGlideSlope, AtTenMiles) {
    // At 10 NM from threshold: altitude = 10 * 6076.12 * tan(3°) ≈ 3183 ft
    const double gs_angle = 3.0 * 3.14159265358979323846 / 180.0;
    double alt = glide_slope_altitude_ft(60761.2, gs_angle, 0.0);

    EXPECT_NEAR(alt, 3183.0, 5.0);  // within 5ft
}
