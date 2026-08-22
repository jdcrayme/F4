// f4-scenario-player/include/f4/scenario_player/airfield_overlays.hpp
//
// Scenario-specific 3D overlays for the scenario player:
//   - taxi route (yellow line strip, parking → hold-short → threshold)
//   - flight-plan route (cyan lines at altitude + drop lines + markers)
//   - approach reference (orange extended centerline + 3° glide slope)
//   - taxi-in route (purple line strip, runway exit → parking)
//   - parking-spot / hold-short / runway-end markers
//   - compass rose (N/E/S/W lines + tick marks)
//
// All overlays use the shared f4::renderer primitive types
// (LayoutLine / LayoutMarker from ground_layout_models.hpp) so they render
// through the same draw_layout_line / draw_layout_marker code path as the
// real airfield geometry — no per-app draw helpers.
//
// The actual runway / taxiway / parking-lot geometry (when the scenario
// has no real campaign layout) is synthesized into GroundLayoutList form
// and built via f4::renderer::build_airfield_geometry_3d() — the renderer
// doesn't care whether the lists came from a PHD file or from this module.
//
// Dependencies: f4-simulation (Scenario), f4-renderer (LayoutLine/Marker,
// AirfieldGeometry3D), f4-geo (WorldPosition). C++20.

#pragma once

#include <f4/simulation/scenario.hpp>
#include <f4/geo/position.hpp>
#include <f4/renderer/ground_layout_models.hpp>  // LayoutLine, LayoutMarker, AirfieldGeometry3D

#include <vector>

namespace f4::scenario_player {

/// One scenario's renderable airfield state. Built once at load_scenario()
/// time from the (possibly derived) Scenario; drawn every frame.
///
/// The shared AirfieldGeometry3D carries the runway/taxiway/parking
/// primitives (either from a real campaign layout or synthesized from the
/// scenario's threshold/end/taxi_route). The remaining fields are
/// scenario-specific overlays that don't have a PHD equivalent.
struct AirfieldOverlays {
    /// Runway / taxiway / parking geometry. When `has_real_layout` is
    /// true, the geometry came from a real campaign objective's
    /// GroundLayoutComponent (build_airfield_geometry_3d on layout_lists);
    /// origin_enu places the objective-local geometry in the world.
    /// Otherwise the geometry was synthesized from the scenario's
    /// threshold/end/taxi_route (still via build_airfield_geometry_3d,
    /// but on a synthesized GroundLayoutList set — origin is 0,0,0).
    f4::renderer::AirfieldGeometry3D geometry;
    bool has_real_layout = false;
    float origin_enu_x = 0.0f;
    float origin_enu_y = 0.0f;
    float origin_enu_z = 0.0f;
    bool built = false;

    // ── Scenario-specific overlays (ENU feet, drawn with shared primitives)
    std::vector<f4::renderer::LayoutLine>   taxi_route_lines;       // yellow
    std::vector<f4::renderer::LayoutLine>   flightplan_lines;      // cyan
    std::vector<f4::renderer::LayoutLine>   flightplan_drop_lines; // dim cyan
    std::vector<f4::renderer::LayoutMarker> flightplan_waypoints;  // cyan cubes
    std::vector<f4::renderer::LayoutLine>   approach_lines;        // orange
    std::vector<f4::renderer::LayoutMarker> approach_markers;     // orange cubes
    std::vector<f4::renderer::LayoutLine>   taxi_in_route_lines;  // purple
    std::vector<f4::renderer::LayoutLine>   compass_rose;          // pale grey
    f4::renderer::LayoutMarker parking_spot{};                      // green
    f4::renderer::LayoutMarker hold_short{};                        // yellow
    f4::renderer::LayoutMarker runway_end{};                        // red
};

/// Build the airfield overlays + the shared AirfieldGeometry3D from a
/// scenario. The scenario's airfield block must have at least 2
/// taxi_route waypoints (validated by load_scenario). When the scenario
/// carries layout_lists (real campaign airbase), those are used directly;
/// otherwise a synthetic GroundLayoutList set is derived from the
/// scenario's threshold/end/taxi_route.
[[nodiscard]] AirfieldOverlays build_airfield_overlays(const f4::simulation::Scenario& s);

} // namespace f4::scenario_player
