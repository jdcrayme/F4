// test_airport_geometry.cpp — unit tests for build_airport_geometry.
//
// Verifies that the synthesized airport geometry is sane:
//   - Runway surface quad has the expected corners (perpendicular to
//     the threshold→end line, with the configured width).
//   - Threshold bars are placed at the threshold end and span the
//     runway width.
//   - Centerline dashes are along the runway, between threshold and end.
//   - Taxi route lines connect consecutive waypoints.
//   - Markers (parking, hold-short, runway-end) are at the expected
//     positions.
//   - Compass rose has at least the 4 cardinal-direction segments.
//
// These tests use hand-constructed Scenarios (no JSON parsing) so they
// can run without the asset files (KoreaObj.HDR, f16.json, etc.).

#include <gtest/gtest.h>

#include "f4/scenario_player/airport_geometry.hpp"

#include <f4/simulation/scenario.hpp>
#include <f4/geo/position.hpp>

#include <cmath>

using namespace f4::scenario_player;
using namespace f4::simulation;

namespace {

Scenario make_simple_scenario() {
    Scenario s;
    s.name = "test";
    s.airfield.threshold_position = f4::geo::WorldPosition{100.0, 8000.0, 50.0};
    s.airfield.runway_end_position = f4::geo::WorldPosition{100.0, 13000.0, 50.0};
    s.airfield.taxi_route = {
        f4::geo::WorldPosition{0.0, 0.0, 50.0},
        f4::geo::WorldPosition{100.0, 8000.0, 50.0}  // ends at the threshold
    };
    ScenarioAircraft a;
    a.callsign = "TEST1";
    a.parking_spot = f4::geo::WorldPosition{0.0, 0.0, 50.0};
    s.aircraft.push_back(a);
    return s;
}

}  // namespace

TEST(AirportGeometry, EmptyScenarioProducesEmptyGeometry) {
    Scenario s;
    s.name = "empty";
    // No taxi route, no aircraft
    auto g = build_airport_geometry(s);
    // Runway surface quad is built but degenerate (zero-size).
    // Taxi route lines are empty.
    EXPECT_TRUE(g.taxi_route_lines.empty());
}

TEST(AirportGeometry, RunwaySurfaceSpansThresholdToEnd) {
    auto s = make_simple_scenario();
    auto g = build_airport_geometry(s);

    // Threshold is at (100, 8000), end at (100, 13000).
    // Runway is along +Y, width 100 ft → perpendicular is along X.
    // The perpendicular vector (-dy, dx)/len for a north-pointing
    // (dx=0, dy=+) runway is (-, 0) = west. So:
    //   p0 = threshold - perp = (100 - (-50), 8000) = (150, 8000)
    //   p1 = threshold + perp = (100 + (-50), 8000) = (50, 8000)
    //   p2 = end + perp        = (50, 13000)
    //   p3 = end - perp        = (150, 13000)
    const auto& q = g.runway_surface;
    EXPECT_NEAR(q.p[0].x, 150.0, 0.01);
    EXPECT_NEAR(q.p[0].y, 8000.0, 0.01);
    EXPECT_NEAR(q.p[0].z, 50.0, 0.01);
    EXPECT_NEAR(q.p[1].x, 50.0, 0.01);
    EXPECT_NEAR(q.p[1].y, 8000.0, 0.01);
    EXPECT_NEAR(q.p[2].x, 50.0, 0.01);
    EXPECT_NEAR(q.p[2].y, 13000.0, 0.01);
    EXPECT_NEAR(q.p[3].x, 150.0, 0.01);
    EXPECT_NEAR(q.p[3].y, 13000.0, 0.01);
}

TEST(AirportGeometry, ThresholdBarsAreAtThresholdEnd) {
    auto s = make_simple_scenario();
    auto g = build_airport_geometry(s);

    // Should have 8 threshold bars.
    EXPECT_EQ(g.threshold_bars.size(), 8u);

    // All bars should be near the threshold (Y ≈ 8000 + small offset).
    for (const auto& b : g.threshold_bars) {
        const double avg_y = (b.p[0].y + b.p[1].y + b.p[2].y + b.p[3].y) / 4.0;
        EXPECT_NEAR(avg_y, 8000.0, 50.0);  // within 50 ft of threshold
    }
}

TEST(AirportGeometry, CenterlineDashesAreBetweenThresholdAndEnd) {
    auto s = make_simple_scenario();
    auto g = build_airport_geometry(s);

    // Should have multiple dashes (5000-ft runway, 120-ft dash + 80-ft gap
    // = ~25 dashes, minus margins).
    EXPECT_GT(g.centerline_dashes.size(), 5u);

    // Each dash should be between Y=8000 and Y=13000.
    for (const auto& d : g.centerline_dashes) {
        const double y0 = d.p[0].y, y1 = d.p[1].y, y2 = d.p[2].y, y3 = d.p[3].y;
        const double min_y = std::min(std::min(y0, y1), std::min(y2, y3));
        const double max_y = std::max(std::max(y0, y1), std::max(y2, y3));
        EXPECT_GE(min_y, 8000.0);
        EXPECT_LE(max_y, 13000.0);
    }
}

TEST(AirportGeometry, TaxiRouteConnectsWaypoints) {
    auto s = make_simple_scenario();
    auto g = build_airport_geometry(s);

    // 2 waypoints → 1 line segment.
    ASSERT_EQ(g.taxi_route_lines.size(), 1u);
    const auto& l = g.taxi_route_lines[0];
    EXPECT_NEAR(l.a.x, 0.0, 0.01);
    EXPECT_NEAR(l.a.y, 0.0, 0.01);
    EXPECT_NEAR(l.b.x, 100.0, 0.01);
    EXPECT_NEAR(l.b.y, 8000.0, 0.01);

    // Lines are lifted 1 ft above ground to avoid z-fighting.
    EXPECT_NEAR(l.a.z, 51.0, 0.01);
    EXPECT_NEAR(l.b.z, 51.0, 0.01);

    // Color is yellow.
    EXPECT_NEAR(l.r, 1.0f, 0.01f);
    EXPECT_NEAR(l.g, 0.85f, 0.01f);
    EXPECT_NEAR(l.blue, 0.0f, 0.01f);
}

TEST(AirportGeometry, ParkingSpotMarkerIsAtAircraftSpawn) {
    auto s = make_simple_scenario();
    auto g = build_airport_geometry(s);

    EXPECT_NEAR(g.parking_spot.center.x, 0.0, 0.01);
    EXPECT_NEAR(g.parking_spot.center.y, 0.0, 0.01);
    EXPECT_NEAR(g.parking_spot.center.z, 50.0, 0.01);
    EXPECT_GT(g.parking_spot.size_ft, 0.0f);

    // Green color.
    EXPECT_NEAR(g.parking_spot.r, 0.2f, 0.01f);
    EXPECT_NEAR(g.parking_spot.g, 0.9f, 0.01f);
    EXPECT_NEAR(g.parking_spot.blue, 0.2f, 0.01f);
}

TEST(AirportGeometry, HoldShortMarkerIsAtSecondToLastWaypoint) {
    auto s = make_simple_scenario();
    // taxi_route has 2 waypoints → hold_short is at index 0.
    auto g = build_airport_geometry(s);

    EXPECT_NEAR(g.hold_short.center.x, 0.0, 0.01);
    EXPECT_NEAR(g.hold_short.center.y, 0.0, 0.01);

    // Now test with 3 waypoints. push_back adds to the end, so the new
    // last is (50, 4000), and hold_short is at index size-2 = 1 →
    // the original threshold (100, 8000).
    s.airfield.taxi_route.push_back(f4::geo::WorldPosition{50.0, 4000.0, 50.0});
    g = build_airport_geometry(s);
    EXPECT_NEAR(g.hold_short.center.x, 100.0, 0.01);
    EXPECT_NEAR(g.hold_short.center.y, 8000.0, 0.01);
}

TEST(AirportGeometry, RunwayEndMarkerIsAtRunwayEnd) {
    auto s = make_simple_scenario();
    auto g = build_airport_geometry(s);

    EXPECT_NEAR(g.runway_end.center.x, 100.0, 0.01);
    EXPECT_NEAR(g.runway_end.center.y, 13000.0, 0.01);
}

TEST(AirportGeometry, CompassRoseHasCardinalDirections) {
    auto s = make_simple_scenario();
    auto g = build_airport_geometry(s);

    // Compass rose should have at least 4 cardinal-direction segments
    // plus 4 tick marks = 8 segments.
    EXPECT_GE(g.compass_rose.size(), 8u);

    // All segments should be at the parking spot's altitude (z=50 or 51 —
    // the cardinal-direction segments start at the center z=50 and end
    // at the cardinal point z=51; the tick marks are at z=51).
    for (const auto& l : g.compass_rose) {
        EXPECT_GE(l.a.z, 50.0 - 0.01);
        EXPECT_LE(l.a.z, 51.0 + 0.01);
        EXPECT_GE(l.b.z, 50.0 - 0.01);
        EXPECT_LE(l.b.z, 51.0 + 0.01);
    }
}

TEST(AirportGeometry, RunwaySurfaceIsGrey) {
    auto s = make_simple_scenario();
    auto g = build_airport_geometry(s);

    // Dark grey (0.20, 0.20, 0.22).
    EXPECT_NEAR(g.runway_surface.r, 0.20f, 0.01f);
    EXPECT_NEAR(g.runway_surface.g, 0.20f, 0.01f);
    EXPECT_NEAR(g.runway_surface.blue, 0.22f, 0.01f);
}

TEST(AirportGeometry, ThresholdBarsAndDashesAreWhite) {
    auto s = make_simple_scenario();
    auto g = build_airport_geometry(s);

    for (const auto& b : g.threshold_bars) {
        EXPECT_NEAR(b.r, 1.0f, 0.01f);
        EXPECT_NEAR(b.g, 1.0f, 0.01f);
        EXPECT_NEAR(b.blue, 1.0f, 0.01f);
    }
    for (const auto& d : g.centerline_dashes) {
        EXPECT_NEAR(d.r, 1.0f, 0.01f);
        EXPECT_NEAR(d.g, 1.0f, 0.01f);
        EXPECT_NEAR(d.blue, 1.0f, 0.01f);
    }
}

TEST(AirportGeometry, HandlesZeroLengthRunway) {
    // Edge case: threshold == end. Should not crash; should produce a
    // degenerate (zero-area) runway surface and no threshold bars or
    // centerline dashes.
    Scenario s;
    s.name = "degenerate";
    s.airfield.threshold_position = f4::geo::WorldPosition{100.0, 8000.0, 50.0};
    s.airfield.runway_end_position = f4::geo::WorldPosition{100.0, 8000.0, 50.0};  // same point
    s.airfield.taxi_route = {
        f4::geo::WorldPosition{0.0, 0.0, 50.0},
        f4::geo::WorldPosition{100.0, 8000.0, 50.0}
    };
    ScenarioAircraft a;
    a.callsign = "TEST";
    a.parking_spot = f4::geo::WorldPosition{0.0, 0.0, 50.0};
    s.aircraft.push_back(a);

    auto g = build_airport_geometry(s);
    // No crash; just empty threshold bars / centerline dashes.
    EXPECT_TRUE(g.threshold_bars.empty());
    EXPECT_TRUE(g.centerline_dashes.empty());
}
