// f4-world-vis/src/svg_map.cpp — layered SVG renderer with terrain support.

#include <f4/vis/svg_map.hpp>

#include <algorithm>
#include <cmath>
#include <set>
#include <sstream>
#include <stdexcept>

namespace f4::vis {

std::unordered_map<int, TeamColor> default_team_colors() {
    return {
        {0, {"Neutral",  "#9ca3af", "#6b7280"}},
        {1, {"Enemy",    "#E60000", "#990000"}},
        {2, {"Friendly", "#00C5CD", "#008B8B"}},
        {3, {"ROK",      "#00C5CD", "#008B8B"}},
        {4, {"Japan",    "#00C5CD", "#008B8B"}},
        {5, {"DPRK",     "#E60000", "#990000"}},
        {6, {"PRC",      "#E60000", "#990000"}},
        {7, {"Team 7",   "#a855f7", "#7e22ce"}},
    };
}

namespace {

std::string svg_escape(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        switch (c) {
            case '<': out += "&lt;"; break;
            case '>': out += "&gt;"; break;
            case '&': out += "&amp;"; break;
            case '"': out += "&quot;"; break;
            default: out += c;
        }
    }
    return out;
}

double to_px_x(int16_t gx, double scale) { return static_cast<double>(gx) * scale; }
double to_px_y(int16_t gy, double scale, int grid_size) {
    return static_cast<double>(grid_size - gy) * scale;
}

const TeamColor* color_for(
    const std::unordered_map<int, TeamColor>& colors, uint8_t owner) {
    auto it = colors.find(owner);
    return (it != colors.end()) ? &it->second : nullptr;
}

// Land mask from objective density (used when no terrain data available).
std::set<std::pair<int,int>> build_land_mask(
    const std::vector<ObjectivePoint>& objs, const MapRendererConfig& cfg) {
    const int cs = cfg.land_cell_size;
    const int dim = cfg.grid_size / cs;
    std::set<std::pair<int,int>> cells;
    for (const auto& o : objs) {
        if (o.x < 0 || o.x >= cfg.grid_size || o.y < 0 || o.y >= cfg.grid_size) continue;
        cells.insert({o.x / cs, o.y / cs});
    }
    std::set<std::pair<int,int>> dilated = cells;
    for (const auto& [cx, cy] : cells) {
        for (int dx = -1; dx <= 1; ++dx)
        for (int dy = -1; dy <= 1; ++dy) {
            int nx = cx + dx, ny = cy + dy;
            if (nx >= 0 && nx < dim && ny >= 0 && ny < dim) dilated.insert({nx, ny});
        }
    }
    return dilated;
}

bool is_likely_airbase(const ObjectivePoint& o) {
    return o.type >= 2120 && o.type <= 2300 && o.priority >= 20;
}

// --- Layer emitters ---

void emit_water(std::ostringstream& o, const MapRendererConfig& cfg) {
    if (!cfg.show_water) return;
    const double dim = cfg.grid_size * cfg.pixels_per_cell;
    o << "  <g id=\"layer-water\" class=\"layer\">\n";
    o << "    <rect x=\"0\" y=\"0\" width=\"" << dim << "\" height=\"" << dim
      << "\" fill=\"" << FalconPalette::WATER << "\"/>\n";
    o << "  </g>\n";
}

void emit_land_mask(std::ostringstream& o,
                    const std::vector<ObjectivePoint>& objs,
                    const MapRendererConfig& cfg) {
    if (!cfg.show_land_mask) return;
    auto mask = build_land_mask(objs, cfg);
    const double cs_px = cfg.land_cell_size * cfg.pixels_per_cell;
    o << "  <g id=\"layer-landmask\" class=\"layer\">\n";
    for (const auto& [cx, cy] : mask) {
        const double px = cx * cs_px;
        const double py = static_cast<double>(cfg.grid_size - (cy + 1) * cfg.land_cell_size)
                        * cfg.pixels_per_cell;
        o << "    <rect x=\"" << px << "\" y=\"" << py
          << "\" width=\"" << cs_px << "\" height=\"" << cs_px
          << "\" fill=\"" << FalconPalette::LAND << "\"/>\n";
    }
    o << "  </g>\n";
}

// Real terrain tiles from THEATER.MEA elevation data.
void emit_terrain_tiles(std::ostringstream& o,
                        const f4::terrain::TerrainData& td,
                        const MapRendererConfig& cfg) {
    o << "  <g id=\"layer-terrain\" class=\"layer\">\n";
    // Each terrain cell (td.header.width x td.header.height) maps to a block
    // of theater grid cells. Scale = grid_size / terrain_width.
    const double cell_px = static_cast<double>(cfg.grid_size) / td.header.width
                         * cfg.pixels_per_cell;
    for (uint32_t ty = 0; ty < td.header.height; ++ty) {
        for (uint32_t tx = 0; tx < td.header.width; ++tx) {
            f4::terrain::Color4 c = td.terrain_color(tx, ty);
            // Flip y for SVG (y-down). sim y=0=north -> SVG top.
            const double px = tx * cell_px;
            const double py = (td.header.height - 1 - ty) * cell_px;
            o << "    <rect x=\"" << px << "\" y=\"" << py
              << "\" width=\"" << cell_px + 0.5 << "\" height=\"" << cell_px + 0.5
              << "\" fill=\"" << c.hex() << "\"/>\n";
        }
    }
    o << "  </g>\n";
}

void emit_grid(std::ostringstream& o, const MapRendererConfig& cfg) {
    o << "  <g id=\"layer-grid\" class=\"layer\">\n";
    if (!cfg.show_grid) { o << "  </g>\n"; return; }
    const double dim = cfg.grid_size * cfg.pixels_per_cell;
    for (int i = 0; i <= cfg.grid_size; i += 64) {
        const double p = i * cfg.pixels_per_cell;
        o << "    <line x1=\"" << p << "\" y1=\"0\" x2=\"" << p
          << "\" y2=\"" << dim << "\" stroke=\"#ffffff\" stroke-width=\"0.5\" opacity=\"0.3\"/>\n";
        o << "    <line x1=\"0\" y1=\"" << p << "\" x2=\"" << dim
          << "\" y2=\"" << p << "\" stroke=\"#ffffff\" stroke-width=\"0.5\" opacity=\"0.3\"/>\n";
    }
    o << "  </g>\n";
}

void emit_objectives(std::ostringstream& o,
                     const std::vector<ObjectivePoint>& objs,
                     const std::unordered_map<int, TeamColor>& colors,
                     const MapRendererConfig& cfg) {
    if (!cfg.show_objectives) return;
    o << "  <g id=\"layer-objectives\" class=\"layer\">\n";
    for (const auto& ob : objs) {
        const double px = to_px_x(ob.x, cfg.pixels_per_cell);
        const double py = to_px_y(ob.y, cfg.pixels_per_cell, cfg.grid_size);
        const TeamColor* tc = color_for(colors, ob.owner);
        const std::string fill = tc ? tc->fill : "#9ca3af";
        const std::string stroke = tc ? tc->stroke : "#6b7280";
        if (is_likely_airbase(ob)) {
            const double rw = 5.0, rh = 1.5;
            o << "    <g transform=\"translate(" << px << "," << py << ")\">\n";
            o << "      <rect x=\"" << -rw/2 << "\" y=\"" << -rh/2
              << "\" width=\"" << rw << "\" height=\"" << rh
              << "\" fill=\"" << fill << "\" stroke=\"" << stroke << "\" stroke-width=\"0.5\"/>\n";
            o << "      <line x1=\"" << -rw/2 << "\" y1=\"0\" x2=\"" << -rw/2-1
              << "\" y2=\"0\" stroke=\"" << stroke << "\" stroke-width=\"1\"/>\n";
            o << "      <line x1=\"" << rw/2 << "\" y1=\"0\" x2=\"" << rw/2+1
              << "\" y2=\"0\" stroke=\"" << stroke << "\" stroke-width=\"1\"/>\n";
            o << "      <title>Airbase owner=" << static_cast<int>(ob.owner)
              << " at (" << ob.x << "," << ob.y << ")</title>\n";
            o << "    </g>\n";
        } else {
            const double r = 1.5 + std::min<double>(ob.priority, 30) / 15.0;
            o << "    <circle cx=\"" << px << "\" cy=\"" << py << "\" r=\"" << r
              << "\" fill=\"" << fill << "\" stroke=\"" << stroke
              << "\" stroke-width=\"0.4\" opacity=\"0.85\">\n";
            o << "      <title>Objective type=" << ob.type << " owner="
              << static_cast<int>(ob.owner) << " at (" << ob.x << "," << ob.y
              << ")</title>\n";
            o << "    </circle>\n";
        }
    }
    o << "  </g>\n";
}

void emit_units(std::ostringstream& o,
                const std::vector<UnitPoint>& units,
                const std::unordered_map<int, TeamColor>& colors,
                const MapRendererConfig& cfg) {
    if (!cfg.show_units || units.empty()) return;
    o << "  <g id=\"layer-units\" class=\"layer\">\n";
    for (const auto& u : units) {
        const double px = to_px_x(u.x, cfg.pixels_per_cell);
        const double py = to_px_y(u.y, cfg.pixels_per_cell, cfg.grid_size);
        const TeamColor* tc = color_for(colors, u.owner);
        const std::string fill = tc ? tc->fill : "#9ca3af";
        const std::string stroke = tc ? tc->stroke : "#6b7280";
        const double s = 2.5;
        o << "    <rect x=\"" << px - s/2 << "\" y=\"" << py - s/2
          << "\" width=\"" << s << "\" height=\"" << s
          << "\" fill=\"" << fill << "\" stroke=\"" << stroke
          << "\" stroke-width=\"0.4\" opacity=\"0.9\">\n";
        o << "      <title>Unit type=" << u.type << " owner="
          << static_cast<int>(u.owner) << " at (" << u.x << "," << u.y << ")</title>\n";
        o << "    </rect>\n";
        if (u.dest_x != 0 || u.dest_y != 0) {
            const double dpx = to_px_x(u.dest_x, cfg.pixels_per_cell);
            const double dpy = to_px_y(u.dest_y, cfg.pixels_per_cell, cfg.grid_size);
            o << "    <line x1=\"" << px << "\" y1=\"" << py << "\" x2=\"" << dpx
              << "\" y2=\"" << dpy << "\" stroke=\"" << stroke
              << "\" stroke-width=\"0.4\" stroke-dasharray=\"1.5,1.5\" opacity=\"0.5\"/>\n";
        }
    }
    o << "  </g>\n";
}

void emit_legend(std::ostringstream& o,
                 const std::vector<ObjectivePoint>& objs,
                 const std::vector<UnitPoint>& units,
                 const std::unordered_map<int, TeamColor>& colors,
                 const MapRendererConfig& cfg) {
    if (!cfg.show_legend) return;
    std::unordered_map<int, int> obj_counts;
    for (const auto& ob : objs) obj_counts[ob.owner]++;
    std::unordered_map<int, int> unit_counts;
    for (const auto& u : units) unit_counts[u.owner]++;
    std::set<int> owners;
    for (const auto& [k, v] : obj_counts) owners.insert(k);
    for (const auto& [k, v] : unit_counts) owners.insert(k);
    const int row_h = 14;
    const int rows = static_cast<int>(owners.size());
    o << "  <g id=\"layer-legend\" class=\"layer\" transform=\"translate(8, 8)\">\n";
    o << "    <rect x=\"0\" y=\"0\" width=\"170\" height=\"" << 26 + rows * row_h
      << "\" fill=\"#000000\" opacity=\"0.7\" rx=\"4\"/>\n";
    o << "    <text x=\"8\" y=\"15\" font-size=\"11\" font-weight=\"bold\" fill=\"#ffffff\">F4 World Map</text>\n";
    o << "    <text x=\"8\" y=\"28\" font-size=\"9\" fill=\"#cccccc\">"
      << objs.size() << " objectives, " << units.size() << " units</text>\n";
    int y = 40;
    for (int owner : owners) {
        auto it = colors.find(owner);
        std::string name = (it != colors.end()) ? it->second.name : "Team " + std::to_string(owner);
        std::string fill = (it != colors.end()) ? it->second.fill : "#9ca3af";
        std::string stroke = (it != colors.end()) ? it->second.stroke : "#6b7280";
        const int oc = obj_counts.count(owner) ? obj_counts[owner] : 0;
        const int uc = unit_counts.count(owner) ? unit_counts[owner] : 0;
        o << "    <rect x=\"8\" y=\"" << y - 4 << "\" width=\"8\" height=\"8\" fill=\""
          << fill << "\" stroke=\"" << stroke << "\" stroke-width=\"0.5\"/>\n";
        o << "    <text x=\"22\" y=\"" << y + 3 << "\" font-size=\"10\" fill=\"#ffffff\">"
          << svg_escape(name) << " (" << oc << "/" << uc << ")</text>\n";
        y += row_h;
    }
    o << "  </g>\n";
}

// Common SVG head + style.
std::ostringstream start_svg(const MapRendererConfig& cfg) {
    std::ostringstream o;
    const double dim = cfg.grid_size * cfg.pixels_per_cell;
    o << "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n";
    o << "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"" << dim
      << "\" height=\"" << dim << "\" viewBox=\"0 0 " << dim << " " << dim << "\">\n";
    o << "<style>.layer{transition:opacity .2s}.layer.hidden{opacity:0}</style>\n";
    return o;
}

} // namespace

// --- Land-mask-based render (no terrain data) ---
std::string render_svg(
    const std::vector<ObjectivePoint>& objectives,
    const std::vector<UnitPoint>& units,
    const std::unordered_map<int, TeamColor>& team_colors,
    const MapRendererConfig& config) {
    auto o = start_svg(config);
    emit_water(o, config);
    emit_land_mask(o, objectives, config);
    emit_grid(o, config);
    emit_objectives(o, objectives, team_colors, config);
    emit_units(o, units, team_colors, config);
    emit_legend(o, objectives, units, team_colors, config);
    o << "</svg>\n";
    return o.str();
}

std::string render_html(
    const std::vector<ObjectivePoint>& objectives,
    const std::vector<UnitPoint>& units,
    const std::unordered_map<int, TeamColor>& team_colors,
    const MapRendererConfig& config) {
    std::string svg = render_svg(objectives, units, team_colors, config);
    std::ostringstream o;
    o << "<!DOCTYPE html>\n<html lang=\"en\">\n<head>\n<meta charset=\"UTF-8\">\n<title>F4 World Map</title>\n<style>\n";
    o << "body{margin:0;font-family:system-ui,sans-serif;background:#1a1a2e}\n";
    o << ".toolbar{position:sticky;top:0;background:#16213e;border-bottom:1px solid #0f3460;padding:8px 12px;z-index:10;display:flex;gap:10px;align-items:center;flex-wrap:wrap}\n";
    o << ".toolbar h1{font-size:13px;margin:0;margin-right:12px;color:#e0e0e0}\n";
    o << ".toolbar label{font-size:11px;display:flex;align-items:center;gap:3px;cursor:pointer;color:#c0c0c0}\n";
    o << ".map-wrap{padding:8px;overflow:auto}\n";
    o << "svg{background:" << FalconPalette::WATER << ";box-shadow:0 2px 8px rgba(0,0,0,.4);max-width:none}\n";
    o << ".layer.hidden{opacity:0!important;pointer-events:none}\n";
    o << "</style>\n</head>\n<body>\n<div class=\"toolbar\">\n";
    o << "<h1>F4 World Map</h1>\n";
    o << "<label><input type=\"checkbox\" checked onchange=\"toggle('layer-water',this)\"> Water</label>\n";
    o << "<label><input type=\"checkbox\" checked onchange=\"toggle('layer-landmask',this)\"> Land</label>\n";
    o << "<label><input type=\"checkbox\" " << (config.show_grid?"checked":"")
      << " onchange=\"toggle('layer-grid',this)\"> Grid</label>\n";
    o << "<label><input type=\"checkbox\" checked onchange=\"toggle('layer-objectives',this)\"> Objectives ("
      << objectives.size() << ")</label>\n";
    o << "<label><input type=\"checkbox\" checked onchange=\"toggle('layer-units',this)\"> Units ("
      << units.size() << ")</label>\n";
    o << "<label><input type=\"checkbox\" checked onchange=\"toggle('layer-legend',this)\"> Legend</label>\n";
    o << "</div>\n<div class=\"map-wrap\">\n" << svg << "</div>\n<script>\n";
    o << "function toggle(id,cb){const e=document.getElementById(id);if(e)e.classList.toggle('hidden',!cb.checked)}\n";
    o << "</script>\n</body>\n</html>\n";
    return o.str();
}

// --- Terrain-aware render ---
std::string render_svg_with_terrain(
    const std::vector<ObjectivePoint>& objectives,
    const std::vector<UnitPoint>& units,
    const std::unordered_map<int, TeamColor>& team_colors,
    const f4::terrain::TerrainData& terrain,
    const MapRendererConfig& config) {
    auto o = start_svg(config);
    emit_terrain_tiles(o, terrain, config);
    emit_grid(o, config);
    emit_objectives(o, objectives, team_colors, config);
    emit_units(o, units, team_colors, config);
    emit_legend(o, objectives, units, team_colors, config);
    o << "</svg>\n";
    return o.str();
}

std::string render_html_with_terrain(
    const std::vector<ObjectivePoint>& objectives,
    const std::vector<UnitPoint>& units,
    const std::unordered_map<int, TeamColor>& team_colors,
    const f4::terrain::TerrainData& terrain,
    const MapRendererConfig& config) {
    std::string svg = render_svg_with_terrain(objectives, units, team_colors, terrain, config);
    std::ostringstream o;
    o << "<!DOCTYPE html>\n<html lang=\"en\">\n<head>\n<meta charset=\"UTF-8\">\n<title>F4 World Map (Terrain)</title>\n<style>\n";
    o << "body{margin:0;font-family:system-ui,sans-serif;background:#1a1a2e}\n";
    o << ".toolbar{position:sticky;top:0;background:#16213e;border-bottom:1px solid #0f3460;padding:8px 12px;z-index:10;display:flex;gap:10px;align-items:center;flex-wrap:wrap}\n";
    o << ".toolbar h1{font-size:13px;margin:0;margin-right:12px;color:#e0e0e0}\n";
    o << ".toolbar label{font-size:11px;display:flex;align-items:center;gap:3px;cursor:pointer;color:#c0c0c0}\n";
    o << ".map-wrap{padding:8px;overflow:auto}\n";
    o << "svg{background:" << FalconPalette::WATER << ";box-shadow:0 2px 8px rgba(0,0,0,.4);max-width:none}\n";
    o << ".layer.hidden{opacity:0!important;pointer-events:none}\n";
    o << "</style>\n</head>\n<body>\n<div class=\"toolbar\">\n";
    o << "<h1>F4 World Map — Korea Terrain</h1>\n";
    o << "<label><input type=\"checkbox\" checked onchange=\"toggle('layer-terrain',this)\"> Terrain</label>\n";
    o << "<label><input type=\"checkbox\" " << (config.show_grid?"checked":"")
      << " onchange=\"toggle('layer-grid',this)\"> Grid</label>\n";
    o << "<label><input type=\"checkbox\" checked onchange=\"toggle('layer-objectives',this)\"> Objectives ("
      << objectives.size() << ")</label>\n";
    o << "<label><input type=\"checkbox\" checked onchange=\"toggle('layer-units',this)\"> Units ("
      << units.size() << ")</label>\n";
    o << "<label><input type=\"checkbox\" checked onchange=\"toggle('layer-legend',this)\"> Legend</label>\n";
    o << "</div>\n<div class=\"map-wrap\">\n" << svg << "</div>\n<script>\n";
    o << "function toggle(id,cb){const e=document.getElementById(id);if(e)e.classList.toggle('hidden',!cb.checked)}\n";
    o << "</script>\n</body>\n</html>\n";
    return o.str();
}

} // namespace f4::vis
