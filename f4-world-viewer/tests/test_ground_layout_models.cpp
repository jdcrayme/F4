// tests/test_ground_layout_models.cpp
//
// Unit tests for build_airfield_geometry_3d() — the pure
// layout→geometry converter in f4-world-viewer/src/ground_layout_models.cpp.
//
// These tests run WITHOUT a GL context (no Raylib, no ImGui) — they
// only verify the pure data-conversion logic:
//   - Empty layouts → empty geometry
//   - Runway L+R edges → filled runway surface quad with expected corners
//   - Runway centerline list → dashed centerline along the threshold→end line
//   - Threshold bars are placed at the threshold end, span the runway width
//   - Parking lists → labeled "P1", "P2", ... markers
//   - Helipad lists → labeled "H1", "H2", ... markers
//   - Non-runway, non-parking lists → filled taxiway strips + centerlines
//   - Building footprints from FeatureSetComponent.features
//   - Bounding box is computed correctly
//
// The tests use hand-constructed GroundLayoutList vectors (no JSON
// parsing, no file I/O) so they can run anywhere — no fixture files
// required.

#include <gtest/gtest.h>

#include "../src/ground_layout_models.hpp"

#include <f4/entities/types.hpp>

#include <cmath>
#include <cstdio>
#include <cstring>

using f4::entities::GroundLayoutList;
using f4::entities::GroundLayoutPoint;
using f4::entities::FeatureEntryState;
using f4::viewer::AirfieldGeometry3D;
using f4::viewer::build_airfield_geometry_3d;
using f4::viewer::is_runway_centerline_type;
using f4::viewer::is_runway_edge_type;
using f4::viewer::is_parking_type;
using f4::viewer::is_taxiway_list_type;

namespace {

GroundLayoutPoint mk_pt(float x, float y, uint8_t flags = 0) {
    GroundLayoutPoint p;
    p.x = x; p.y = y; p.type = 0; p.flags = flags;
    return p;
}

GroundLayoutList mk_list(uint8_t type, uint8_t runway_num = 0,
                         float heading_deg = 0.0f, int8_t ltrt = 0) {
    GroundLayoutList l;
    l.type = type;
    l.runway_num = runway_num;
    l.heading_deg = heading_deg;
    l.ltrt = ltrt;
    return l;
}

// A simple runway: threshold at (0, 8000), end at (0, 13000), width 100 ft.
// LT[0] = (-50, 8000), LT[1] = (-50, 13000)
// RT[0] = (+50, 8000), RT[1] = (+50, 13000)
// CL: (0, 8000) → (0, 13000)  — heading 0° (north)
struct SimpleRunway {
    std::vector<GroundLayoutList> layouts;

    SimpleRunway() {
        GroundLayoutList lt = mk_list(12 /*PLT_RUNWAY_LT*/, /*runway_num=*/1,
                                       /*heading_deg=*/0.0f, /*ltrt=*/-1);
        lt.points.push_back(mk_pt(-50.0f, 8000.0f));
        lt.points.push_back(mk_pt(-50.0f, 13000.0f));
        layouts.push_back(lt);

        GroundLayoutList rt = mk_list(13 /*PLT_RUNWAY_RT*/, 1, 0.0f, +1);
        rt.points.push_back(mk_pt(+50.0f, 8000.0f));
        rt.points.push_back(mk_pt(+50.0f, 13000.0f));
        layouts.push_back(rt);

        GroundLayoutList cl = mk_list(1 /*PLT_RUNWAY*/, 1, 0.0f, 0);
        cl.points.push_back(mk_pt(0.0f, 8000.0f));
        cl.points.push_back(mk_pt(0.0f, 13000.0f));
        layouts.push_back(cl);
    }
};

} // anonymous namespace

// ---------------------------------------------------------------------------
// Type predicates
// ---------------------------------------------------------------------------

TEST(GroundLayoutModels, TypePredicatesMatchFalcon4PointListTypes) {
    EXPECT_TRUE(is_runway_centerline_type(1));
    EXPECT_TRUE(is_runway_edge_type(12));
    EXPECT_TRUE(is_runway_edge_type(13));
    EXPECT_TRUE(is_parking_type(11));

    // Taxiway = anything not in the runway/placement-point set.
    EXPECT_TRUE(is_taxiway_list_type(15));   // PLT_FOLLOW_ME
    EXPECT_TRUE(is_taxiway_list_type(17));   // PLT_TRACK
    EXPECT_TRUE(is_taxiway_list_type(0));    // PLT_NONE / unknown
    EXPECT_TRUE(is_taxiway_list_type(99));   // unknown

    // Not taxiways:
    EXPECT_FALSE(is_taxiway_list_type(1));   // runway centerline
    EXPECT_FALSE(is_taxiway_list_type(12));   // runway LT
    EXPECT_FALSE(is_taxiway_list_type(13));   // runway RT
    EXPECT_FALSE(is_taxiway_list_type(11));  // parking
    EXPECT_FALSE(is_taxiway_list_type(14));  // helipad
    EXPECT_FALSE(is_taxiway_list_type(4));   // SAM
}

// ---------------------------------------------------------------------------
// Empty input
// ---------------------------------------------------------------------------

TEST(GroundLayoutModels, EmptyLayoutsProduceEmptyGeometry) {
    auto g = build_airfield_geometry_3d({});
    EXPECT_TRUE(g.empty);
    EXPECT_TRUE(g.runway_surfaces.empty());
    EXPECT_TRUE(g.threshold_bars.empty());
    EXPECT_TRUE(g.centerline_dashes.empty());
    EXPECT_TRUE(g.taxiway_strips.empty());
    EXPECT_TRUE(g.parking_spots.empty());
    EXPECT_TRUE(g.helipads.empty());
    EXPECT_TRUE(g.runway_ends.empty());
    EXPECT_TRUE(g.feature_footprints.empty());
}

TEST(GroundLayoutModels, EmptyFeaturesVectorIsIgnored) {
    std::vector<FeatureEntryState> empty_features;
    auto g = build_airfield_geometry_3d({}, &empty_features);
    EXPECT_TRUE(g.empty);
}

// ---------------------------------------------------------------------------
// Runway surface from L + R edges
// ---------------------------------------------------------------------------

TEST(GroundLayoutModels, RunwaySurfaceSpansLeftAndRightEdges) {
    SimpleRunway sr;
    auto g = build_airfield_geometry_3d(sr.layouts);
    ASSERT_EQ(g.runway_surfaces.size(), 1u);

    // Quad corners (CCW): LT[0], RT[0], RT[1], LT[1].
    const auto& q = g.runway_surfaces[0];
    EXPECT_NEAR(q.x[0], -50.0f, 0.01f);   // LT[0]
    EXPECT_NEAR(q.y[0], 8000.0f, 0.01f);
    EXPECT_NEAR(q.x[1], +50.0f, 0.01f);   // RT[0]
    EXPECT_NEAR(q.y[1], 8000.0f, 0.01f);
    EXPECT_NEAR(q.x[2], +50.0f, 0.01f);   // RT[1]
    EXPECT_NEAR(q.y[2], 13000.0f, 0.01f);
    EXPECT_NEAR(q.x[3], -50.0f, 0.01f);   // LT[1]
    EXPECT_NEAR(q.y[3], 13000.0f, 0.01f);

    // Runway surface is dark grey.
    EXPECT_LT(q.r, 100);
    EXPECT_LT(q.g, 100);
    EXPECT_LT(q.b, 100);
}

TEST(GroundLayoutModels, RunwaySurfaceFallbackUsesCenterlineWhenNoEdges) {
    // Only the centerline list — no LT/RT. The surface should be built
    // from CL with a default 100 ft width (50 ft half-width).
    std::vector<GroundLayoutList> layouts;
    GroundLayoutList cl = mk_list(1 /*PLT_RUNWAY*/, /*runway_num=*/1,
                                   /*heading_deg=*/0.0f, 0);
    cl.points.push_back(mk_pt(0.0f, 8000.0f));
    cl.points.push_back(mk_pt(0.0f, 13000.0f));
    layouts.push_back(cl);

    auto g = build_airfield_geometry_3d(layouts);
    ASSERT_EQ(g.runway_surfaces.size(), 1u);
    const auto& q = g.runway_surfaces[0];
    // Corners: perpendicular to the threshold→end direction (which is +Y),
    // so perpendicular is ±X. Half-width = 50 ft.
    EXPECT_NEAR(q.x[0], -50.0f, 0.01f);
    EXPECT_NEAR(q.y[0], 8000.0f, 0.01f);
    EXPECT_NEAR(q.x[1], +50.0f, 0.01f);
    EXPECT_NEAR(q.y[1], 8000.0f, 0.01f);
    EXPECT_NEAR(q.x[2], +50.0f, 0.01f);
    EXPECT_NEAR(q.y[2], 13000.0f, 0.01f);
    EXPECT_NEAR(q.x[3], -50.0f, 0.01f);
    EXPECT_NEAR(q.y[3], 13000.0f, 0.01f);
}

// ---------------------------------------------------------------------------
// Threshold bars
// ---------------------------------------------------------------------------

TEST(GroundLayoutModels, ThresholdBarsAreAtThresholdEnd) {
    SimpleRunway sr;
    auto g = build_airfield_geometry_3d(sr.layouts);

    // Should have RUNWAY_N_THRESHOLD_BARS bars (= 8).
    EXPECT_EQ(g.threshold_bars.size(), 8u);

    // All bars should be near the threshold Y (= 8000) and span the
    // runway width (LT[0]→RT[0] = 100 ft).
    for (const auto& b : g.threshold_bars) {
        const float avg_y = (b.y[0] + b.y[1] + b.y[2] + b.y[3]) * 0.25f;
        EXPECT_NEAR(avg_y, 8000.0f, 100.0f);  // within ~100 ft of threshold
        const float min_x = std::min(std::min(b.x[0], b.x[1]),
                                     std::min(b.x[2], b.x[3]));
        const float max_x = std::max(std::max(b.x[0], b.x[1]),
                                     std::max(b.x[2], b.x[3]));
        EXPECT_GE(min_x, -50.0f);
        EXPECT_LE(max_x, +50.0f);
    }
}

// ---------------------------------------------------------------------------
// Centerline dashes
// ---------------------------------------------------------------------------

TEST(GroundLayoutModels, CenterlineDashesRunBetweenThresholdAndEnd) {
    SimpleRunway sr;
    auto g = build_airfield_geometry_3d(sr.layouts);

    // 5000-ft runway, 120 ft dash + 80 ft gap = ~25 dashes, minus the
    // start/stop margins (80 ft on each end). Expect > 5.
    EXPECT_GT(g.centerline_dashes.size(), 5u);

    // Each dash is at X≈0 (centerline) and Y between 8000 and 13000.
    for (const auto& d : g.centerline_dashes) {
        const float avg_x = (d.x[0] + d.x[1] + d.x[2] + d.x[3]) * 0.25f;
        const float avg_y = (d.y[0] + d.y[1] + d.y[2] + d.y[3]) * 0.25f;
        EXPECT_NEAR(avg_x, 0.0f, 5.0f);   // near centerline
        EXPECT_GE(avg_y, 8000.0f - 100.0f);
        EXPECT_LE(avg_y, 13000.0f + 100.0f);
    }
}

// ---------------------------------------------------------------------------
// Runway-end marker
// ---------------------------------------------------------------------------

TEST(GroundLayoutModels, RunwayEndMarkerIsAtFarEndOfRunway) {
    SimpleRunway sr;
    auto g = build_airfield_geometry_3d(sr.layouts);

    ASSERT_EQ(g.runway_ends.size(), 1u);
    const auto& m = g.runway_ends[0];
    EXPECT_NEAR(m.x, 0.0f, 0.01f);
    EXPECT_NEAR(m.y, 13000.0f, 0.01f);
    EXPECT_GT(m.size_ft, 0.0f);

    // Label is "RWY NN" where NN = heading/10 (rounded). Heading = 0°
    // → NN = 0/10 = 0 → "RWY 36" (since rwy_num 0 is remapped to 36).
    EXPECT_FALSE(m.label.empty());
    EXPECT_TRUE(m.label.find("RWY") != std::string::npos)
        << "label was: " << m.label;
}

// ---------------------------------------------------------------------------
// Parking markers
// ---------------------------------------------------------------------------

TEST(GroundLayoutModels, ParkingSpotsAreLabeledSequentially) {
    std::vector<GroundLayoutList> layouts;
    GroundLayoutList park = mk_list(11 /*PLT_PARK*/, 0, 0.0f, 0);
    park.points.push_back(mk_pt(100.0f, 200.0f));
    park.points.push_back(mk_pt(150.0f, 200.0f));
    park.points.push_back(mk_pt(200.0f, 200.0f));
    layouts.push_back(park);

    auto g = build_airfield_geometry_3d(layouts);
    ASSERT_EQ(g.parking_spots.size(), 3u);

    EXPECT_EQ(g.parking_spots[0].label, "P1");
    EXPECT_EQ(g.parking_spots[1].label, "P2");
    EXPECT_EQ(g.parking_spots[2].label, "P3");

    EXPECT_NEAR(g.parking_spots[0].x, 100.0f, 0.01f);
    EXPECT_NEAR(g.parking_spots[0].y, 200.0f, 0.01f);
    EXPECT_NEAR(g.parking_spots[1].x, 150.0f, 0.01f);
    EXPECT_NEAR(g.parking_spots[2].x, 200.0f, 0.01f);
}

TEST(GroundLayoutModels, MultipleParkingListsContinueNumbering) {
    // Two PLT_PARK lists — the counter should continue across them.
    std::vector<GroundLayoutList> layouts;
    GroundLayoutList park1 = mk_list(11, 0, 0.0f, 0);
    park1.points.push_back(mk_pt(100.0f, 200.0f));
    layouts.push_back(park1);

    GroundLayoutList park2 = mk_list(11, 0, 0.0f, 0);
    park2.points.push_back(mk_pt(200.0f, 200.0f));
    park2.points.push_back(mk_pt(300.0f, 200.0f));
    layouts.push_back(park2);

    auto g = build_airfield_geometry_3d(layouts);
    ASSERT_EQ(g.parking_spots.size(), 3u);
    EXPECT_EQ(g.parking_spots[0].label, "P1");
    EXPECT_EQ(g.parking_spots[1].label, "P2");
    EXPECT_EQ(g.parking_spots[2].label, "P3");
}

// ---------------------------------------------------------------------------
// Helipad markers
// ---------------------------------------------------------------------------

TEST(GroundLayoutModels, HelipadsAreLabeledH1H2Etc) {
    std::vector<GroundLayoutList> layouts;
    GroundLayoutList heli = mk_list(14 /*PLT_HELICOPTER*/, 0, 0.0f, 0);
    heli.points.push_back(mk_pt(100.0f, 200.0f));
    heli.points.push_back(mk_pt(200.0f, 200.0f));
    layouts.push_back(heli);

    auto g = build_airfield_geometry_3d(layouts);
    // helipads vector contains both helipads (H1, H2) — these are the
    // only entries since no placement-point types were used.
    ASSERT_GE(g.helipads.size(), 2u);
    // The first two should be the helipads (H1, H2).
    bool found_h1 = false, found_h2 = false;
    for (const auto& m : g.helipads) {
        if (m.label == "H1") found_h1 = true;
        if (m.label == "H2") found_h2 = true;
    }
    EXPECT_TRUE(found_h1);
    EXPECT_TRUE(found_h2);
}

// ---------------------------------------------------------------------------
// Taxiway strips inferred from non-runway/non-parking lists
// ---------------------------------------------------------------------------

TEST(GroundLayoutModels, TrackListBecomesTaxiwayStrip) {
    std::vector<GroundLayoutList> layouts;
    GroundLayoutList track = mk_list(17 /*PLT_TRACK*/, 0, 0.0f, 0);
    track.points.push_back(mk_pt(0.0f,    0.0f));
    track.points.push_back(mk_pt(0.0f, 1000.0f));
    track.points.push_back(mk_pt(500.0f, 1500.0f));
    layouts.push_back(track);

    auto g = build_airfield_geometry_3d(layouts);

    // Two segments → two filled quads.
    ASSERT_EQ(g.taxiway_strips.size(), 2u);

    // First strip segment: from (0,0) to (0,1000). Perpendicular = (1,0).
    // Half-width = 25 ft. So corners should be at X ≈ ±25, Y ≈ 0 and 1000.
    const auto& q0 = g.taxiway_strips[0];
    EXPECT_NEAR(q0.x[0], -25.0f, 0.5f);
    EXPECT_NEAR(q0.y[0],    0.0f, 0.5f);
    EXPECT_NEAR(q0.x[1], +25.0f, 0.5f);
    EXPECT_NEAR(q0.y[1],    0.0f, 0.5f);
    EXPECT_NEAR(q0.x[2], +25.0f, 0.5f);
    EXPECT_NEAR(q0.y[2], 1000.0f, 0.5f);
    EXPECT_NEAR(q0.x[3], -25.0f, 0.5f);
    EXPECT_NEAR(q0.y[3], 1000.0f, 0.5f);

    // Each strip segment also produces one yellow centerline line.
    ASSERT_EQ(g.taxiway_centerlines.size(), 2u);
    const auto& l0 = g.taxiway_centerlines[0];
    EXPECT_NEAR(l0.x0, 0.0f, 0.01f);
    EXPECT_NEAR(l0.y0, 0.0f, 0.01f);
    EXPECT_NEAR(l0.x1, 0.0f, 0.01f);
    EXPECT_NEAR(l0.y1, 1000.0f, 0.01f);
    // Yellow color: r and g are both high, b is low. (Pure yellow has
    // r == g; our taxiway color is slightly red-biased but the key
    // distinguishing trait is high r+g, low b.)
    EXPECT_GT(l0.r, 150);
    EXPECT_GT(l0.g, 150);
    EXPECT_LT(l0.b, 100);
}

TEST(GroundLayoutModels, FollowMeListBecomesTaxiwayStrip) {
    // Type 15 (PLT_FOLLOW_ME) is also treated as a taxiway.
    std::vector<GroundLayoutList> layouts;
    GroundLayoutList fm = mk_list(15, 0, 0.0f, 0);
    fm.points.push_back(mk_pt(0.0f, 0.0f));
    fm.points.push_back(mk_pt(100.0f, 0.0f));
    layouts.push_back(fm);

    auto g = build_airfield_geometry_3d(layouts);
    ASSERT_EQ(g.taxiway_strips.size(), 1u);
    ASSERT_EQ(g.taxiway_centerlines.size(), 1u);
}

// ---------------------------------------------------------------------------
// Feature footprints
// ---------------------------------------------------------------------------

TEST(GroundLayoutModels, FeaturesBecomeBuildingFootprints) {
    std::vector<FeatureEntryState> features;
    FeatureEntryState f1;
    f1.index = 100;
    f1.offset_x = 200.0f;
    f1.offset_y = 300.0f;
    f1.facing = 0;       // no rotation
    f1.damage_state = 0; // intact → green
    f1.name = "Hangar A";
    features.push_back(f1);

    FeatureEntryState f2;
    f2.index = 101;
    f2.offset_x = 500.0f;
    f2.offset_y = 600.0f;
    f2.facing = 90;       // rotate 90°
    f2.damage_state = 2;  // destroyed → red-orange
    f2.name = "Tower";
    features.push_back(f2);

    auto g = build_airfield_geometry_3d({}, &features);
    ASSERT_EQ(g.feature_footprints.size(), 2u);

    // Intact feature is at (200, 300).
    EXPECT_NEAR(g.feature_footprints[0].x[0], 200.0f - 18.0f, 0.5f);
    EXPECT_NEAR(g.feature_footprints[0].y[0], 300.0f - 18.0f, 0.5f);
    // Intact color is green-ish.
    EXPECT_GT(g.feature_footprints[0].g, g.feature_footprints[0].r);
    EXPECT_GT(g.feature_footprints[0].g, g.feature_footprints[0].b);

    // Destroyed color is red-orange.
    EXPECT_GT(g.feature_footprints[1].r, g.feature_footprints[1].g);
}

TEST(GroundLayoutModels, PlaceholderFeaturesAreSkipped) {
    // Features with index=0 and offset (0,0) are placeholders emitted by
    // the bridge — they should be skipped.
    std::vector<FeatureEntryState> features;
    FeatureEntryState placeholder;
    placeholder.index = 0;
    placeholder.offset_x = 0.0f;
    placeholder.offset_y = 0.0f;
    features.push_back(placeholder);

    FeatureEntryState real;
    real.index = 100;
    real.offset_x = 200.0f;
    real.offset_y = 300.0f;
    features.push_back(real);

    auto g = build_airfield_geometry_3d({}, &features);
    EXPECT_EQ(g.feature_footprints.size(), 1u);
}

// ---------------------------------------------------------------------------
// Bbox computation
// ---------------------------------------------------------------------------

TEST(GroundLayoutModels, BboxSpansAllGeometry) {
    SimpleRunway sr;
    auto g = build_airfield_geometry_3d(sr.layouts);
    EXPECT_FALSE(g.empty);

    // Runway edges are at X = ±50, Y = 8000..13000.
    EXPECT_LE(g.min_x, -50.0f);
    EXPECT_GE(g.max_x, +50.0f);
    EXPECT_LE(g.min_y, 8000.0f);
    EXPECT_GE(g.max_y, 13000.0f);

    // Threshold bars at Y=8000 extend across the runway width
    // (X = -50 .. +50). They may slightly exceed because of the
    // 90% usable width — but that's still within ±50.
    // (The threshold bar offset is 10 ft past threshold, so Y is
    // 8010 ± 25 = 7985..8035 — within the bbox.)
}

TEST(GroundLayoutModels, BboxIncludesParkingAndTaxiways) {
    std::vector<GroundLayoutList> layouts;
    SimpleRunway sr;
    layouts = sr.layouts;

    // Add a parking spot far to the south.
    GroundLayoutList park = mk_list(11, 0, 0.0f, 0);
    park.points.push_back(mk_pt(0.0f, -2000.0f));
    layouts.push_back(park);

    // Add a taxi track far to the east.
    GroundLayoutList track = mk_list(17, 0, 0.0f, 0);
    track.points.push_back(mk_pt(5000.0f, 0.0f));
    track.points.push_back(mk_pt(5000.0f, 1000.0f));
    layouts.push_back(track);

    auto g = build_airfield_geometry_3d(layouts);
    EXPECT_FALSE(g.empty);
    EXPECT_LE(g.min_y, -2000.0f);
    EXPECT_GE(g.max_x, 5000.0f);
}

// ---------------------------------------------------------------------------
// Multiple runways
// ---------------------------------------------------------------------------

TEST(GroundLayoutModels, MultipleRunwayNumsProduceMultipleSurfaces) {
    std::vector<GroundLayoutList> layouts;

    // Runway 1: north-south.
    GroundLayoutList lt1 = mk_list(12, 1, 0.0f, -1);
    lt1.points.push_back(mk_pt(-50.0f, 8000.0f));
    lt1.points.push_back(mk_pt(-50.0f, 13000.0f));
    layouts.push_back(lt1);
    GroundLayoutList rt1 = mk_list(13, 1, 0.0f, +1);
    rt1.points.push_back(mk_pt(+50.0f, 8000.0f));
    rt1.points.push_back(mk_pt(+50.0f, 13000.0f));
    layouts.push_back(rt1);

    // Runway 2: east-west (different runway_num).
    GroundLayoutList lt2 = mk_list(12, 2, 90.0f, -1);
    lt2.points.push_back(mk_pt(8000.0f, -50.0f));
    lt2.points.push_back(mk_pt(13000.0f, -50.0f));
    layouts.push_back(lt2);
    GroundLayoutList rt2 = mk_list(13, 2, 90.0f, +1);
    rt2.points.push_back(mk_pt(8000.0f, +50.0f));
    rt2.points.push_back(mk_pt(13000.0f, +50.0f));
    layouts.push_back(rt2);

    auto g = build_airfield_geometry_3d(layouts);
    EXPECT_EQ(g.runway_surfaces.size(), 2u);
    EXPECT_EQ(g.runway_ends.size(), 2u);
}

// ---------------------------------------------------------------------------
// Real-PHD-shape data: runway_num=0, multiple CL lists per runway
// ---------------------------------------------------------------------------
//
// Real Falcon4 PHD data uses 0-based runway_num (0 for the first runway,
// NOT 1 as the comment in theater_data.hpp suggests), and emits multiple
// CL lists (type=1) per runway — one per threshold at headings 180°
// apart. There are NO separate LT/RT lists (types 12, 13) in real data.
// This test reproduces that exact shape and verifies that:
//   - A runway surface IS built (the runway_num=0 case used to be
//     silently skipped, leaving the panel empty).
//   - Two runway-end markers are emitted (one per CL list / threshold).
//   - The runway-end labels carry the correct runway number derived
//     from each CL list's heading_deg.

TEST(GroundLayoutModels, RealPhdShapeRunwayNumZeroIsAccepted) {
    std::vector<GroundLayoutList> layouts;

    // CL list 1: threshold at (0, 0), end at (0, 8000), heading 0° (north).
    GroundLayoutList cl1 = mk_list(1 /*PLT_RUNWAY*/, /*runway_num=*/0,
                                    /*heading_deg=*/0.0f, /*ltrt=*/-1);
    cl1.points.push_back(mk_pt(0.0f,    0.0f));
    cl1.points.push_back(mk_pt(0.0f, 8000.0f));
    layouts.push_back(cl1);

    // CL list 2: opposite threshold at (0, 8000), end at (0, 0), heading 180°.
    GroundLayoutList cl2 = mk_list(1, 0, 180.0f, +1);
    cl2.points.push_back(mk_pt(0.0f, 8000.0f));
    cl2.points.push_back(mk_pt(0.0f,    0.0f));
    layouts.push_back(cl2);

    auto g = build_airfield_geometry_3d(layouts);

    // Critical regression: runway_num=0 must NOT be skipped.
    EXPECT_FALSE(g.empty) << "runway_num=0 lists must produce geometry";
    EXPECT_EQ(g.runway_surfaces.size(), 1u)
        << "exactly one surface per runway (built from the first CL list)";

    // Two CL lists → two runway-end markers (one per threshold).
    EXPECT_EQ(g.runway_ends.size(), 2u);
    // One marker should carry RWY 36 (heading 0°→0/10=0→36), the other RWY 18.
    bool found_36 = false, found_18 = false;
    for (const auto& m : g.runway_ends) {
        if (m.label == "RWY 36") found_36 = true;
        if (m.label == "RWY 18") found_18 = true;
    }
    EXPECT_TRUE(found_36) << "expected RWY 36 label for heading 0° threshold";
    EXPECT_TRUE(found_18) << "expected RWY 18 label for heading 180° threshold";

    // Bbox must span the runway extent.
    EXPECT_LE(g.min_y, 0.0f);
    EXPECT_GE(g.max_y, 8000.0f);
}

// Real PHD CL lists are HYBRID: pts[0] = threshold, pts[1] = OPPOSITE
// threshold (~runway length away), pts[2] = runway access point,
// pts[3..N-1] = embedded taxiway exit back to the apron. The runway's
// two endpoints are pts[0] and pts[1] — NOT pts[0] and pts.back().
//
// This test uses a real-shape 12-point CL list (matching the structure
// observed in 02_20 Airbase 2's PHD chain) and verifies that:
//   1. The runway surface spans pts[0]→pts[1] (not pts[0]→pts.back()).
//   2. The end marker sits at pts[1] (the opposite threshold), not at
//      pts.back() (the last taxiway node, which would land in the
//      middle of the airbase and make multiple runways look
//      "converging").
//   3. The embedded taxiway exit (pts[2..N-1]) is rendered as a
//      taxiway strip — otherwise the entire taxiway network connected
//      to runway exits would be silently dropped.
//   4. The centerline dashes run along pts[0]→pts[1] (along the
//      runway), not along the taxiway exit.
//
// Coordinates mimic 02_20 Airbase 2's runway 0 / CL list 1 (heading 20°)
// but scaled down to fit in a small synthetic test footprint.
TEST(GroundLayoutModels, RealPhdShapeHybridClListUsesPts0AndPts1AsRunwayEnds) {
    std::vector<GroundLayoutList> layouts;

    // CL list — 12 points matching the real PHD shape:
    //   pts[0]      PT_RUNWAY      threshold at (0, 0)
    //   pts[1]      PT_TAKEOFF     opposite threshold at (0, 6000)  (~6000 ft away)
    //   pts[2]      PT_TAKE_RUNWAY access point at (50, 6100)       (just off far end)
    //   pts[3..11]  PT_TAXI        taxiway exit path zigzagging back to (200, 500)
    GroundLayoutList cl = mk_list(1 /*PLT_RUNWAY*/, /*runway_num=*/0,
                                  /*heading_deg=*/20.0f, /*ltrt=*/-1);
    cl.points.push_back(mk_pt(   0.0f,    0.0f));  // pts[0] threshold
    cl.points.push_back(mk_pt(   0.0f, 6000.0f));  // pts[1] opposite threshold
    cl.points.push_back(mk_pt(  50.0f, 6100.0f));  // pts[2] access point
    cl.points.push_back(mk_pt( 100.0f, 5500.0f));  // pts[3] taxi node
    cl.points.push_back(mk_pt( 150.0f, 4500.0f));  // pts[4]
    cl.points.push_back(mk_pt( 180.0f, 3500.0f));  // pts[5]
    cl.points.push_back(mk_pt( 200.0f, 2500.0f));  // pts[6]
    cl.points.push_back(mk_pt( 220.0f, 1500.0f));  // pts[7]
    cl.points.push_back(mk_pt( 200.0f,  500.0f));  // pts[8..11] — back at apron
    cl.points.push_back(mk_pt( 180.0f,  300.0f));
    cl.points.push_back(mk_pt( 150.0f,  200.0f));
    cl.points.push_back(mk_pt( 100.0f,  100.0f));
    layouts.push_back(cl);

    auto g = build_airfield_geometry_3d(layouts);

    // ---- (1) Runway surface: 1 surface spanning pts[0]→pts[1] ----
    ASSERT_EQ(g.runway_surfaces.size(), 1u);
    const auto& surf = g.runway_surfaces[0];
    // The quad corners are p0 ± perp*half_width and p1 ± perp*half_width.
    // pts[0]=(0,0) and pts[1]=(0,6000) — so the surface's X range must
    // straddle 0 (the perp offset) and Y range must span 0..6000.
    // Critically: Y MUST reach ~6000 (the opposite threshold), NOT just
    // ~500 (the last taxiway node — that would be the old buggy behavior).
    EXPECT_LE(surf.y[0], 50.0f);     // near pts[0] (with perp offset)
    EXPECT_GE(surf.y[2], 5950.0f);   // near pts[1] (with perp offset)
    // And the surface must NOT extend down to the taxiway end at Y=100.
    // (Old buggy code would have produced a surface from pts[0]=(0,0) to
    // pts[11]=(100,100), making the surface only ~100 ft long.)
    EXPECT_GT(surf.y[2], 5000.0f) << "surface far end must be at opposite threshold, "
                                    << "not at the last taxiway node";

    // ---- (2) Runway-end marker at pts[1] (opposite threshold), not pts.back() ----
    ASSERT_EQ(g.runway_ends.size(), 1u);
    const auto& m = g.runway_ends[0];
    // pts[1] = (0, 6000). Marker must be at (0, 6000), NOT at pts[11] = (100, 100).
    EXPECT_NEAR(m.x, 0.0f, 5.0f)    << "marker X must be near pts[1].x=0";
    EXPECT_NEAR(m.y, 6000.0f, 5.0f) << "marker Y must be near pts[1].y=6000 (opposite "
                                      << "threshold), not pts.back()=(100,100)";
    EXPECT_EQ(m.label, "RWY 02") << "heading 20° → round(20/10)=2 → 'RWY 02'";

    // ---- (3) Embedded taxiway exit rendered as a taxiway strip ----
    // The taxiway portion (pts[2..11] = 10 points → 9 segments) must be
    // rendered as taxiway strips. Without Fix B, this would be 0 strips
    // (the third pass used to `continue` past CL lists).
    EXPECT_EQ(g.taxiway_strips.size(), 9u)
        << "embedded taxiway exit (pts[2..N-1]) must be rendered as strips";
    EXPECT_EQ(g.taxiway_centerlines.size(), 9u)
        << "taxiway centerlines must accompany the strips";

    // ---- (4) Centerline dashes run along pts[0]→pts[1], not along taxiway ----
    // Length of pts[0]→pts[1] = 6000 ft. With 80ft margins at each end and
    // 120ft dash + 80ft gap pattern, we expect (6000-160)/(120+80) ≈ 29 dashes.
    // Critically: if the old buggy code drew dashes from pts[0]=(0,0) to
    // pts[11]=(100,100), the path length would only be ~141 ft — well below
    // the (dash+gap)=200ft minimum, so NO dashes would be emitted.
    EXPECT_GE(g.centerline_dashes.size(), 20u)
        << "centerline dashes must run along the runway (pts[0]→pts[1], ~6000ft), "
        << "not be skipped because the path was too short";

    // Bbox must span the runway (Y up to ~6000) AND the taxiway (down to ~100).
    EXPECT_LE(g.min_y, 0.0f);
    EXPECT_GE(g.max_y, 6000.0f);
    EXPECT_LE(g.min_x, 0.0f);
    EXPECT_GE(g.max_x, 220.0f);  // taxiway extends to X=220
}

// Real PHD data also includes placement-point lists (SAM/AAA/etc.) with
// runway_num = 255 (= int8_t -1, the "not a runway" sentinel). Those
// lists must NOT be mis-grouped into a runway group keyed on 255.

TEST(GroundLayoutModels, PlacementPointsWithSentinelRunwayNumAreNotRunways) {
    std::vector<GroundLayoutList> layouts;

    // AAA placement list with runway_num=255 (= -1 sentinel).
    GroundLayoutList aaa = mk_list(6 /*PLT_AAA*/, 255, 0.0f, 0);
    aaa.points.push_back(mk_pt(100.0f, 100.0f));
    aaa.points.push_back(mk_pt(200.0f, 200.0f));
    layouts.push_back(aaa);

    auto g = build_airfield_geometry_3d(layouts);

    // No runway surfaces or end markers should be produced.
    EXPECT_TRUE(g.runway_surfaces.empty());
    EXPECT_TRUE(g.runway_ends.empty());

    // But the AAA placement markers should be present (rendered as cones).
    // The placement markers go into the helipads vector (see builder code).
    EXPECT_EQ(g.helipads.size(), 2u);
    // And the bbox should still cover the AAA points.
    EXPECT_FALSE(g.empty);
    EXPECT_LE(g.min_x, 100.0f);
    EXPECT_GE(g.max_x, 200.0f);
}

// ---------------------------------------------------------------------------
// Edge case: degenerate runway (zero-length)
// ---------------------------------------------------------------------------

TEST(GroundLayoutModels, DegenerateRunwayProducesNoThresholdBarsOrDashes) {
    std::vector<GroundLayoutList> layouts;
    GroundLayoutList lt = mk_list(12, 1, 0.0f, -1);
    lt.points.push_back(mk_pt(-50.0f, 8000.0f));
    lt.points.push_back(mk_pt(-50.0f, 8000.0f));  // zero-length
    layouts.push_back(lt);
    GroundLayoutList rt = mk_list(13, 1, 0.0f, +1);
    rt.points.push_back(mk_pt(+50.0f, 8000.0f));
    rt.points.push_back(mk_pt(+50.0f, 8000.0f));  // zero-length
    layouts.push_back(rt);

    auto g = build_airfield_geometry_3d(layouts);
    // No crash; threshold bars / centerline dashes should be skipped.
    EXPECT_TRUE(g.threshold_bars.empty());
    EXPECT_TRUE(g.centerline_dashes.empty());
}
