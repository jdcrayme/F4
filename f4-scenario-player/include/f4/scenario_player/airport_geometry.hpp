// f4-scenario-player/include/f4/scenario_player/airport_geometry.hpp
//
// AirportGeometry — synthesize renderable line strips + polygons for an
// airbase from a Scenario's airfield block. v0 does NOT pull real ground
// layout from f4-world::WorldState (that's Phase 2 per
// Docs/SCENARIO_PLAYER_PLAN.md §8). Instead it derives a plausible visual
// representation from the scenario's runway threshold/end + taxi route:
//
//   - Runway rectangle: from threshold_position to runway_end_position,
//     with a fixed width (100 ft ≈ 30 m, typical for a fighter base).
//   - Runway centerline dashes: along the runway length.
//   - Taxi route: a yellow line strip through the scenario's taxi_route
//     waypoints (parking → hold short → threshold).
//   - Parking spot marker: a small green cube at the aircraft's spawn.
//   - Runway threshold bars: white rectangles at the threshold end.
//
// All coordinates are ENU feet (east, north, up) — the same frame
// TransformComponent uses. The renderer converts to Raylib's RH Y-up
// at draw time.
//
// Dependencies: f4-simulation (Scenario), f4-geo (WorldPosition). C++20.

#pragma once

#include <f4/simulation/scenario.hpp>
#include <f4/geo/position.hpp>
#include <f4/renderer/ground_layout_models.hpp>

#include <vector>

namespace f4::scenario_player {

/// One colored line segment in ENU feet.
struct GeoLine {
    f4::geo::WorldPosition a;
    f4::geo::WorldPosition b;
    float r = 1.0f, g = 1.0f, blue = 1.0f, a_ = 1.0f;  // RGBA 0..1
};

/// One colored quad (two triangles) in ENU feet, drawn as a flat polygon
/// on the ground plane (z = the altitude of the first vertex).
struct GeoQuad {
    f4::geo::WorldPosition p[4];
    float r = 1.0f, g = 1.0f, blue = 1.0f, a_ = 1.0f;
};

/// One small cube marker (e.g. parking spot, hold-short point).
struct GeoMarker {
    f4::geo::WorldPosition center;
    float size_ft = 10.0f;
    float r = 1.0f, g = 1.0f, blue = 1.0f, a_ = 1.0f;
};

/// Renderable airport geometry, derived from a Scenario.
///
/// Construction is cheap (a few hundred float ops + vector pushes).
/// The renderer holds one AirportGeometry per scenario and reuploads
/// nothing per frame — the geometry is static for the scenario's
/// lifetime.
struct AirportGeometry {
    /// Runway surface (one big dark-grey quad).
    GeoQuad runway_surface;

    /// Runway threshold bars (white quads at the threshold end).
    std::vector<GeoQuad> threshold_bars;

    /// Runway centerline dashes (white quads spaced along the length).
    std::vector<GeoQuad> centerline_dashes;

    /// Taxi route line strip (yellow segments connecting waypoints).
    std::vector<GeoLine> taxi_route_lines;

    /// Flight-plan route line strip (cyan) through the scenario's
    /// waypoints AT their altitudes, plus vertical drop lines (dim cyan)
    /// from each waypoint down to the airfield ground level.
    std::vector<GeoLine> flightplan_lines;
    std::vector<GeoLine> flightplan_drop_lines;
    std::vector<GeoMarker> flightplan_waypoints;

    /// Approach reference (orange): the runway's extended centerline
    /// (~4 NM before the threshold) and the 3-deg glide-slope line rising
    /// from the threshold along it.
    std::vector<GeoLine> approach_lines;
    std::vector<GeoMarker> approach_markers;

    /// Taxi-in route line strip (purple): runway exit back to parking.
    std::vector<GeoLine> taxi_in_route_lines;

    /// Parking-spot marker (small green cube at the spawn position).
    GeoMarker parking_spot{};

    /// Hold-short marker (small yellow cube near the threshold).
    GeoMarker hold_short{};

    /// Runway end marker (small red cube at the far end).
    GeoMarker runway_end{};

    /// Compass rose — N/E/S/W line segments centered on the scenario's
    /// parking spot. Helps the user orient themselves in the orbit view.
    std::vector<GeoLine> compass_rose;

    /// REAL airbase layout (from airbase_source): objective-local
    /// geometry from the shared f4-renderer builder + its ENU origin.
    /// Drawn INSTEAD of the synthetic runway/taxi shapes when non-empty.
    /// `real_layout` is empty iff the scenario had no layout_lists.
    bool has_real_layout = false;
    f4::renderer::AirfieldGeometry3D real_layout;
    double layout_origin_x = 0.0;   ///< ENU feet
    double layout_origin_y = 0.0;
    double layout_origin_z = 0.0;
};

/// Build an AirportGeometry from a Scenario. The scenario's airfield
/// block must have at least 2 taxi_route waypoints (validated by
/// load_scenario); the runway bounds come from threshold_position and
/// runway_end_position.
[[nodiscard]] AirportGeometry build_airport_geometry(const f4::simulation::Scenario& s);

} // namespace f4::scenario_player
