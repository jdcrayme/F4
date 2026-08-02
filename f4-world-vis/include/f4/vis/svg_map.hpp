// f4-world-vis/include/f4/vis/svg_map.hpp
//
// SVG tile-map renderer for world data. Reads the typed WorldState (teams,
// objectives, units) and emits a layered SVG document that can be opened in
// any browser or embedded in HTML.
//
// LAYERED DESIGN (the key architectural decision):
//   The SVG is built as stacked <g> layers, each toggleable via CSS. This
//   mirrors how a real tactical map works and lets us add layers
//   incrementally without rewriting the renderer:
//
//     layer-terrain     — color-coded tiles by terrain type (future: needs
//                         theater terrain data; currently a placeholder grid)
//     layer-grid        — coordinate grid lines + labels
//     layer-threat      — cell threat-score heatmap (future: from f4-campaign
//                         ThreatMap; currently omitted)
//     layer-routes      — rail/road overlays (future: from objective links)
//     layer-objectives  — icons per objective, colored by owner team
//     layer-units       — icons per unit, colored by owner team
//     layer-paths       — flight/mission paths (future: from flight plans)
//     layer-bullseye    — theater bullseye marker
//     layer-legend      — color key + counts
//
// COORDINATE SYSTEM:
//   FreeFalcon grid coordinates are GridIndex (int16) x=column, y=row.
//   The renderer maps (x, y) -> SVG (px, py) with y flipped (SVG y-down).
//   Scale: configurable pixels-per-grid-cell (default 2). A 1024-cell
//   theater at 2 px/cell = 2048x2048 SVG.
//
// TEAM COLORS:
//   Owner values (Control uchar) map to team colors. The .cmp decoder gives
//   us the team names; we map slot->name->color. Without the .cmp mapping
//   we use a default palette keyed on owner value:
//     0 = neutral (gray), 1 = red/enemy, 2 = blue/ally, 3+ = others.

#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>
#include <f4/terrain/terrain_data.hpp>

namespace f4::vis {

struct MapRendererConfig {
    double pixels_per_cell = 2.0;       // SVG px per grid cell
    int     grid_size = 1024;           // theater grid dimensions (cells)
    int     land_cell_size = 8;         // land-mask cell size in grid units
    bool    show_grid = false;          // off by default (matches screenshot)
    bool    show_objectives = true;
    bool    show_units = true;
    bool    show_legend = true;
    bool    show_land_mask = true;      // derived coastline (no terrain data)
    bool    show_water = true;          // deep blue background
};

struct ObjectivePoint {
    int16_t  x = 0;
    int16_t  y = 0;
    uint8_t  owner = 0;
    int16_t  type = 0;                  // entity_type (class table index)
    int16_t  nameid = 0;
    uint8_t  priority = 0;
};

struct UnitPoint {
    int16_t  x = 0;
    int16_t  y = 0;
    uint8_t  owner = 0;
    int16_t  type = 0;
    int16_t  dest_x = 0;
    int16_t  dest_y = 0;
};

struct TeamColor {
    std::string name;
    std::string fill;       // CSS color
    std::string stroke;
};

/// Falcon 4 palette constants for terrain/water (matches the campaign screenshot).
struct FalconPalette {
    static constexpr const char* WATER     = "#006994";
    static constexpr const char* LAND      = "#B5A188";
    static constexpr const char* LAND_DARK = "#8C7D66";
};

/// Default team color palette keyed by owner (Control) value.
/// owner 0 = neutral, 1 = enemy (red), 2 = ally (blue), 3..7 = others.
[[nodiscard]] std::unordered_map<int, TeamColor> default_team_colors();

/// Render the world data to an SVG document string.
/// The SVG contains layered <g> elements (terrain, grid, objectives, units,
/// legend) that can be toggled via CSS. Opens standalone in any browser.
[[nodiscard]] std::string render_svg(
    const std::vector<ObjectivePoint>& objectives,
    const std::vector<UnitPoint>& units,
    const std::unordered_map<int, TeamColor>& team_colors,
    const MapRendererConfig& config = {});

/// Convenience: render a complete standalone HTML document wrapping the SVG,
/// with layer-toggle checkboxes and a legend. This is the deliverable you
/// open in a browser to inspect the decoded world.
[[nodiscard]] std::string render_html(
    const std::vector<ObjectivePoint>& objectives,
    const std::vector<UnitPoint>& units,
    const std::unordered_map<int, TeamColor>& team_colors,
    const MapRendererConfig& config = {});

/// Render with real terrain tiles (from f4-terrain). Replaces the derived
/// land-mask with actual color-coded elevation tiles: deep blue water,
/// tan lowlands, brown mountains, white peaks. The terrain grid (128×128)
/// is scaled up to fill the theater grid (1024×1024); each terrain cell
/// becomes an 8×8 block of SVG pixels.
[[nodiscard]] std::string render_svg_with_terrain(
    const std::vector<ObjectivePoint>& objectives,
    const std::vector<UnitPoint>& units,
    const std::unordered_map<int, TeamColor>& team_colors,
    const terrain::TerrainData& terrain,
    const MapRendererConfig& config = {});

/// HTML wrapper for the terrain-aware render.
[[nodiscard]] std::string render_html_with_terrain(
    const std::vector<ObjectivePoint>& objectives,
    const std::vector<UnitPoint>& units,
    const std::unordered_map<int, TeamColor>& team_colors,
    const terrain::TerrainData& terrain,
    const MapRendererConfig& config = {});

} // namespace f4::vis
