// f4-simulation/tests/test_real_layout_derivation.cpp
//
// Tests derive_airfield_from_objective against the REAL Korea PHD
// template (numbers captured from korea_real.world.json — objective
// idx 1660, grid (626,475), the "02_20 Airbase 2" class shared by all 42
// Korean airbases in the Steam theater DB).
//
// Real list structure (runway list, heading 020):
//   [0] PT_RUNWAY      (2699, 2956)   far-end marker
//   [1] PT_TAKEOFF     ( 416,-3348)   takeoff position
//   [2] PT_TAKE_RUNWAY ( 305,-3637)   hold-short / runway access
//   [3..] PT_TAXI      19 points, ordered runway->ramp, ending (680,-798)
// Runway Dim quad (rwy 0): (2985,3950)(3152,3892)(249,-4030)(83,-3974)
//   -> 8438 x 176 ft.

#include <gtest/gtest.h>

#include "f4/simulation/campaign_bridge.hpp"

#include <f4/entities/types.hpp>
#include <f4/world/detail/world_state.hpp>
#include <f4/world_types/layout_types.hpp>

#include <cmath>

using namespace f4::simulation;
using namespace f4::entities;
using namespace f4::world;
using namespace f4::world_types;

namespace {

constexpr double PI = 3.14159265358979323846;
constexpr double FT_PER_GRID = 1024.0;

GroundLayoutPoint pt(float x, float y, uint8_t type) {
    return GroundLayoutPoint{x, y, type, 0};
}

// The real 02_20 airbase template (see file header).
ObjectiveState make_real_airbase() {
    ObjectiveState obj;
    obj.x = 626;
    obj.y = 475;
    obj.z = 0.0f;

    {
        GroundLayoutList l;  // runway 0, heading 020
        l.type = PLT_RUNWAY; l.runway_num = 0; l.heading_deg = 20.0f;
        l.points = {
            pt(2699, 2956, PT_RUNWAY),
            pt(416, -3348, PT_TAKEOFF),
            pt(305, -3637, PT_TAKE_RUNWAY),
            pt(93, -3543, PT_TAXI), pt(-111, -3466, PT_TAXI),
            pt(-243, -3403, PT_TAXI), pt(-221, -3228, PT_TAXI),
            pt(-149, -3050, PT_TAXI), pt(-76, -2881, PT_TAXI),
            pt(-17, -2721, PT_TAXI), pt(35, -2563, PT_TAXI),
            pt(90, -2415, PT_TAXI), pt(147, -2244, PT_TAXI),
            pt(220, -2076, PT_TAXI), pt(279, -1916, PT_TAXI),
            pt(331, -1758, PT_TAXI), pt(386, -1610, PT_TAXI),
            pt(442, -1433, PT_TAXI), pt(514, -1264, PT_TAXI),
            pt(573, -1104, PT_TAXI), pt(625, -946, PT_TAXI),
            pt(680, -798, PT_TAXI),
        };
        obj.ground_layout.push_back(l);
    }
    {
        GroundLayoutList l;  // runway 0, heading 200 (reciprocal)
        l.type = PLT_RUNWAY; l.runway_num = 0; l.heading_deg = 200.0f;
        l.points = {
            pt(516, -3060, PT_RUNWAY),
            pt(-2410, 2406, PT_TAKEOFF),
            pt(-2521, 2695, PT_TAKE_RUNWAY),
            pt(200, -500, PT_TAXI), pt(0, 0, PT_TAXI),
        };
        obj.ground_layout.push_back(l);
    }
    {
        GroundLayoutList l;  // dims quad for runway 0
        l.type = PLT_RUNWAY_DIM; l.runway_num = 0; l.heading_deg = 236.0f;
        l.points = {
            pt(2985, 3950, PT_RUNWAY_DIM), pt(3152, 3892, PT_RUNWAY_DIM),
            pt(249, -4030, PT_RUNWAY_DIM), pt(83, -3974, PT_RUNWAY_DIM),
        };
        obj.ground_layout.push_back(l);
    }
    return obj;
}

} // anonymous namespace

// ============================================================================
// Runway geometry
// ============================================================================

TEST(RealLayoutDerivation, HeadingAndName) {
    auto af = derive_airfield_from_objective(make_real_airbase(), 2);
    ASSERT_TRUE(af.has_value());
    EXPECT_NEAR(af->runway_heading_rad, 20.0 * PI / 180.0, 1e-6);
    EXPECT_EQ(af->active_runway_name, "Rwy 02");
}

TEST(RealLayoutDerivation, RunwayEndIsTheFarMarker) {
    auto af = derive_airfield_from_objective(make_real_airbase(), 2);
    ASSERT_TRUE(af.has_value());
    // far-end marker (2699,2956) + objective center (626,475)*1024
    EXPECT_NEAR(af->runway_end_position.x, 626 * FT_PER_GRID + 2699, 1.0);
    EXPECT_NEAR(af->runway_end_position.y, 475 * FT_PER_GRID + 2956, 1.0);
}

TEST(RealLayoutDerivation, ThresholdIsTheReciprocalMarker) {
    auto af = derive_airfield_from_objective(make_real_airbase(), 2);
    ASSERT_TRUE(af.has_value());
    // The reciprocal list's PT_RUNWAY marker (516,-3060) sits ~305 ft
    // ahead of the takeoff point — that is the painted threshold.
    EXPECT_NEAR(af->threshold_position.x, 626 * FT_PER_GRID + 516, 1.0);
    EXPECT_NEAR(af->threshold_position.y, 475 * FT_PER_GRID - 3060, 1.0);
    // Threshold -> end must run along the runway heading, ~6705 ft.
    const double dx = af->runway_end_position.x - af->threshold_position.x;
    const double dy = af->runway_end_position.y - af->threshold_position.y;
    EXPECT_NEAR(std::hypot(dx, dy), 6400.0, 60.0);   // 6705 roll - 305 threshold offset
    EXPECT_NEAR(std::atan2(dx, dy), af->runway_heading_rad, 0.01);
}

TEST(RealLayoutDerivation, DimensionsFromDimQuad) {
    auto af = derive_airfield_from_objective(make_real_airbase(), 2);
    ASSERT_TRUE(af.has_value());
    EXPECT_NEAR(af->runway_length_ft, 8438.0, 15.0);
    EXPECT_NEAR(af->runway_width_ft, 176.0, 5.0);
}

// ============================================================================
// Taxi routes
// ============================================================================

TEST(RealLayoutDerivation, TaxiOutRouteRunsRampToHoldShort) {
    auto af = derive_airfield_from_objective(make_real_airbase(), 2);
    ASSERT_TRUE(af.has_value());
    ASSERT_GE(af->taxi_route.size(), 4u);
    // First: ramp terminus (680,-798). Last: hold-short takeoff point.
    EXPECT_NEAR(af->taxi_route.front().x, 626 * FT_PER_GRID + 680, 1.0);
    EXPECT_NEAR(af->taxi_route.front().y, 475 * FT_PER_GRID - 798, 1.0);
    EXPECT_NEAR(af->taxi_route.back().x, 626 * FT_PER_GRID + 416, 1.0);
    EXPECT_NEAR(af->taxi_route.back().y, 475 * FT_PER_GRID - 3348, 1.0);
    // Second-to-last: the access point.
    EXPECT_NEAR(af->taxi_route[af->taxi_route.size() - 2].x,
                626 * FT_PER_GRID + 305, 1.0);
    // Monotone: each leg under ~300 ft (polyline spacing 146-218).
    for (std::size_t i = 1; i < af->taxi_route.size(); ++i) {
        const double d = std::hypot(af->taxi_route[i].x - af->taxi_route[i - 1].x,
                                    af->taxi_route[i].y - af->taxi_route[i - 1].y);
        EXPECT_LT(d, 400.0) << "leg " << i;   // polyline ~150-220; access->takeoff ~310
    }
}

TEST(RealLayoutDerivation, TaxiInRouteRunsHoldShortToRamp) {
    auto af = derive_airfield_from_objective(make_real_airbase(), 2);
    ASSERT_TRUE(af.has_value());
    ASSERT_GE(af->taxi_in_route.size(), 4u);
    EXPECT_NEAR(af->taxi_in_route.front().x, 626 * FT_PER_GRID + 416, 1.0);
    EXPECT_NEAR(af->taxi_in_route.back().x, 626 * FT_PER_GRID + 680, 1.0);
}

// ============================================================================
// Parking synthesis
// ============================================================================

TEST(RealLayoutDerivation, ParkingSpotsSynthesizedOffRamp) {
    auto af = derive_airfield_from_objective(make_real_airbase(), 2);
    ASSERT_TRUE(af.has_value());
    ASSERT_FALSE(af->parking_spots.empty());
    // Spot 0: ~90 ft perpendicular from the ramp terminus, on the side
    // AWAY from the runway axis (the +cross side).
    const double cx = 626 * FT_PER_GRID + 680;
    const double cy = 475 * FT_PER_GRID - 798;
    const auto& s0 = af->parking_spots.front();
    const double d0 = std::hypot(s0.position.x - cx, s0.position.y - cy);
    EXPECT_NEAR(d0, 90.0, 5.0);
    // Runway axis passes through the takeoff point along heading 20; the
    // spot must be FARTHER from that line than the terminus.
    const double hx = std::sin(20.0 * PI / 180.0), hy = std::cos(20.0 * PI / 180.0);
    const double tx = 626 * FT_PER_GRID + 416, ty = 475 * FT_PER_GRID - 3348;
    const double cross_spot = hx * (s0.position.y - ty) - hy * (s0.position.x - tx);
    const double cross_term = hx * (cy - ty) - hy * (cx - tx);
    EXPECT_GT(std::abs(cross_spot), std::abs(cross_term));
    // Facing: back along the polyline (ready to taxi straight out toward
    // the access point) — heading ~= reciprocal of the ramp-out direction.
    const double expect_face = std::atan2(-(680 - 625), -(-798 + 946));
    EXPECT_NEAR(s0.heading_rad, expect_face, 0.05);
    // Spots are spaced along the ramp.
    ASSERT_GE(af->parking_spots.size(), 2u);
    const auto& s1 = af->parking_spots[1];
    EXPECT_NEAR(std::hypot(s1.position.x - s0.position.x, s1.position.y - s0.position.y),
                80.0, 5.0);
}

// ============================================================================
// Runway-direction selection
// ============================================================================

TEST(RealLayoutDerivation, ActiveRunwayIdSelectsDirection) {
    // id 20 (heading 200) must select the reciprocal list: its takeoff
    // point (-2410, 2406) becomes the hold-short end.
    auto af = derive_airfield_from_objective(make_real_airbase(), 20);
    ASSERT_TRUE(af.has_value());
    EXPECT_NEAR(af->runway_heading_rad, 200.0 * PI / 180.0, 1e-6);
    EXPECT_NEAR(af->taxi_route.back().x, 626 * FT_PER_GRID - 2410, 1.0);
}
