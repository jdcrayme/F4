// f4-renderer/include/f4/renderer/ground_layout_models.hpp
//
// Shared airfield-layout geometry builder (moved from f4-world-viewer so
//
// Pure layout-to-geometry conversion: takes a selected objective's
// GroundLayoutComponent.layouts (+ optional FeatureSetComponent.features)
// and produces a structured AirfieldGeometry3D containing renderable
// primitives (filled quads, line segments, labeled markers) in the
// objective-local ENU feet frame.
//
// This module is deliberately Raylib- and ImGui-free so it can be
// unit-tested without a GL context. The 3D panel (ground_layout_3d.cpp)
// consumes the result and renders it via Raylib's BeginMode3D.
//
// Coordinate convention:
//   - All output coordinates are in feet, origin = objective center,
//     +X = East, +Y = North, +Z = Up. (Same frame as
//     TransformComponent::position and the f4-scenario-player renderer.)
//   - The renderer converts ENU feet to Raylib's RH Y-up at draw time:
//     raylib_x = enu_x, raylib_y = enu_z, raylib_z = -enu_y.
//
// See Docs/SCENARIO_PLAYER_PLAN.md §5.5 for the coordinate-frame rationale.

#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace f4::entities {
struct GroundLayoutList;       // forward-decl — full def in f4/entities/types.hpp
struct FeatureEntryState;
} // namespace f4::entities

namespace f4::renderer {

// ---------------------------------------------------------------------------
// Output primitives
// ---------------------------------------------------------------------------

/// One flat quad on the ground plane. Corners are counter-clockwise when
/// viewed from above with +Z up. The 3D renderer treats it as two
/// triangles (p0,p1,p2) and (p0,p2,p3).
struct LayoutQuad {
    float x[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    float y[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    float z = 0.0f;
    uint8_t r = 255, g = 255, b = 255, a = 255;
};

/// One line segment (centerline dash, taxi route edge, runway dim mark).
struct LayoutLine {
    float x0 = 0.0f, y0 = 0.0f;
    float x1 = 0.0f, y1 = 0.0f;
    float z = 0.0f;
    uint8_t r = 255, g = 255, b = 255, a = 255;
    bool dashed = false;
};

/// One labeled marker (parking spot, helipad, runway end).
struct LayoutMarker {
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
    float size_ft = 10.0f;          // cube extent (half-side)
    float heading_deg = 0.0f;       // for orientation indicator
    uint8_t r = 255, g = 255, b = 255, a = 255;
    std::string label;              // e.g. "P1", "RWY 09"
    uint8_t shape = 0;              // 0=cube, 1=cylinder, 2=cone
};

/// All renderable primitives for one objective's airfield, in
/// objective-local ENU feet. Build it once per selection change,
/// cache it, draw it every frame.
struct AirfieldGeometry3D {
    /// Filled runway surfaces (dark grey). One per runway_num.
    std::vector<LayoutQuad> runway_surfaces;

    /// Painted threshold bars (white, perpendicular to runway at the
    /// threshold end).
    std::vector<LayoutQuad> threshold_bars;

    /// Centerline dashes along each runway (white, dashed). Each dash is
    /// a small filled quad perpendicular to the runway direction.
    std::vector<LayoutQuad> centerline_dashes;

    /// Filled taxiway strips (dark grey-brown). Built from non-runway,
    /// non-parking line lists — see is_taxiway_list_type().
    std::vector<LayoutQuad> taxiway_strips;

    /// Yellow centerline line strips on top of taxiway_strips.
    std::vector<LayoutLine> taxiway_centerlines;

    /// Parking-spot markers (small green cubes with "P1", "P2", ... labels).
    std::vector<LayoutMarker> parking_spots;

    /// Helipad markers (cyan cylinders with "H1", "H2", ... labels).
    std::vector<LayoutMarker> helipads;

    /// Runway-end markers (red cubes with the runway number label).
    std::vector<LayoutMarker> runway_ends;

    /// Building footprints from FeatureSetComponent (colored by damage_state).
    std::vector<LayoutQuad> feature_footprints;

    /// Bounding box of all geometry, in feet. Used by the 3D panel to
    /// frame the orbit camera. When `empty` is true, the bbox is undefined.
    float min_x = 0.0f, min_y = 0.0f;
    float max_x = 0.0f, max_y = 0.0f;
    bool empty = true;
};

// ---------------------------------------------------------------------------
// Layout-type predicates
// ---------------------------------------------------------------------------
//
// The Falcon4 PHD format (PtHeaderData) carries a `type` byte per list.
// The known values (PointListType enum in f4-world-convert/theater_data.hpp):
//
//   PLT_RUNWAY        = 1   // runway centerline (threshold + end)
//   PLT_RUNWAY_DIM    = 8   // runway dimensional marks
//   PLT_RUNWAY_LT     = 12  // runway left-side edge points
//   PLT_RUNWAY_RT     = 13  // runway right-side edge points
//   PLT_PARK          = 11  // parking spots (small + large mixed)
//   PLT_HELICOPTER    = 14  // helicopter landing spots
//   PLT_FOLLOW_ME     = 15  // follow-me truck route
//   PLT_DOCK          = 16  // dock
//   PLT_TRACK         = 17  // taxi track
//   PLT_SAM           = 4   // SAM placement points
//   PLT_ARTILLERY     = 5   // artillery placement points
//   PLT_AAA           = 6   // AAA placement points
//   PLT_STATIC_RADAR  = 10  // static radar placement
//
// We treat runway-related types (1, 8, 12, 13) and placement-point types
// (4, 5, 6, 10, 11, 14, 16) as known — they are handled by dedicated
// branches. Everything else (types 15, 17, 0, or any unknown value)
// is treated as a taxiway/path and rendered as a filled strip + a
// yellow centerline.

[[nodiscard]] inline bool is_runway_centerline_type(uint8_t t) noexcept {
    return t == 1;   // PLT_RUNWAY
}

[[nodiscard]] inline bool is_runway_edge_type(uint8_t t) noexcept {
    return t == 12 || t == 13;   // PLT_RUNWAY_LT / PLT_RUNWAY_RT
}

[[nodiscard]] inline bool is_runway_dim_type(uint8_t t) noexcept {
    return t == 8;   // PLT_RUNWAY_DIM
}

[[nodiscard]] inline bool is_parking_type(uint8_t t) noexcept {
    return t == 11;  // PLT_PARK
}

[[nodiscard]] inline bool is_helipad_type(uint8_t t) noexcept {
    return t == 14;  // PLT_HELICOPTER
}

[[nodiscard]] inline bool is_placement_point_type(uint8_t t) noexcept {
    // SAM (4), Artillery (5), AAA (6), Static radar (10), Parking (11),
    // Helipad (14), Dock (16). These are single-point placement markers
    // — not line lists — and are handled as markers, not strips.
    switch (t) {
        case 4: case 5: case 6: case 10: case 11: case 14: case 16:
            return true;
        default:
            return false;
    }
}

[[nodiscard]] inline bool is_taxiway_list_type(uint8_t t) noexcept {
    // Anything that isn't a runway type (1, 8, 12, 13) and isn't a
    // placement-point type (4, 5, 6, 10, 11, 14, 16). That includes
    // PLT_FOLLOW_ME (15), PLT_TRACK (17), PLT_NONE (0), and any unknown
    // value — the user explicitly chose "infer from any list".
    if (is_runway_centerline_type(t)) return false;
    if (is_runway_edge_type(t))       return false;
    if (is_runway_dim_type(t))        return false;
    if (is_placement_point_type(t))   return false;
    return true;
}

// ---------------------------------------------------------------------------
// Builder
// ---------------------------------------------------------------------------

/// Build an AirfieldGeometry3D from a selected objective's ground-layout
/// lists and (optionally) its feature placements.
///
/// The function is pure: no I/O, no GL, no allocations beyond the output
/// vectors. It is safe to call from a unit test.
///
/// `layouts`  — the GroundLayoutComponent::layouts vector (may be empty).
/// `features` — optional pointer to FeatureSetComponent::features. Pass
///              nullptr to skip feature footprints.
///
/// Returns an AirfieldGeometry3D. If `layouts` and `features` are both
/// empty (or contain no usable points), the result has `empty == true`
/// and the bbox is undefined.
[[nodiscard]] AirfieldGeometry3D build_airfield_geometry_3d(
    const std::vector<f4::entities::GroundLayoutList>& layouts,
    const std::vector<f4::entities::FeatureEntryState>* features = nullptr);

} // namespace f4::renderer
