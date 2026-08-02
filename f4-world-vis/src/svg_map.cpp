// f4-world-vis/src/svg_map.cpp — layered SVG renderer.

#include <f4/vis/svg_map.hpp>

#include <cmath>
#include <sstream>
#include <stdexcept>

namespace f4::vis {

std::unordered_map<int, TeamColor> default_team_colors() {
    return {
        {0, {"Neutral", "#9ca3af", "#6b7280"}},   // gray
        {1, {"Enemy",   "#ef4444", "#b91c1c"}},   // red
        {2, {"Ally",    "#3b82f6", "#1d4ed8"}},   // blue
        {3, {"Team 3",  "#22c55e", "#15803d"}},   // green
        {4, {"Team 4",  "#a855f7", "#7e22ce"}},   // purple
        {5, {"Team 5",  "#f59e0b", "#b45309"}},   // amber
        {6, {"Team 6",  "#ec4899", "#be185d"}},   // pink
        {7, {"Team 7",  "#14b8a6", "#0f766e"}},   // teal
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

// Grid (x, y) -> SVG (px, py). y is flipped (SVG y-down).
double to_px_x(int16_t gx, double scale) { return static_cast<double>(gx) * scale; }
double to_px_y(int16_t gy, double scale, int grid_size) {
    return static_cast<double>(grid_size - gy) * scale;
}

const TeamColor* color_for(
    const std::unordered_map<int, TeamColor>& colors, uint8_t owner) {
    auto it = colors.find(owner);
    return (it != colors.end()) ? &it->second : nullptr;
}

void emit_terrain_placeholder(std::ostringstream& o, const MapRendererConfig& cfg) {
    if (!cfg.show_terrain_placeholder) return;
    o << "  <g id=\"layer-terrain\" class=\"layer\">\n";
    // A flat water-colored background until real terrain tiles are available.
    o << "    <rect x=\"0\" y=\"0\" width=\"" << cfg.grid_size * cfg.pixels_per_cell
      << "\" height=\"" << cfg.grid_size * cfg.pixels_per_cell
      << "\" fill=\"#dbeafe\" opacity=\"0.3\"/>\n";
    o << "  </g>\n";
}

void emit_grid(std::ostringstream& o, const MapRendererConfig& cfg) {
    if (!cfg.show_grid) return;
    const double dim = cfg.grid_size * cfg.pixels_per_cell;
    // Grid line every 64 cells; labeled with the cell index.
    o << "  <g id=\"layer-grid\" class=\"layer\">\n";
    for (int i = 0; i <= cfg.grid_size; i += 64) {
        const double p = i * cfg.pixels_per_cell;
        o << "    <line x1=\"" << p << "\" y1=\"0\" x2=\"" << p
          << "\" y2=\"" << dim << "\" stroke=\"#e5e7eb\" stroke-width=\"1\"/>\n";
        o << "    <line x1=\"0\" y1=\"" << p << "\" x2=\"" << dim
          << "\" y2=\"" << p << "\" stroke=\"#e5e7eb\" stroke-width=\"1\"/>\n";
        o << "    <text x=\"" << p + 2 << "\" y=\"12\" font-size=\"9\" fill=\"#9ca3af\">"
          << i << "</text>\n";
        o << "    <text x=\"2\" y=\"" << p - 2 << "\" font-size=\"9\" fill=\"#9ca3af\">"
          << (cfg.grid_size - i) << "</text>\n";
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
        // Circle marker; radius scaled by priority (bigger = more important).
        const double r = 2.0 + std::min<double>(ob.priority, 30) / 10.0;
        o << "    <circle cx=\"" << px << "\" cy=\"" << py << "\" r=\"" << r
          << "\" fill=\"" << fill << "\" stroke=\"" << stroke
          << "\" stroke-width=\"0.5\" opacity=\"0.85\">\n";
        o << "      <title>Objective type=" << ob.type << " owner=" << static_cast<int>(ob.owner)
          << " at (" << ob.x << "," << ob.y << ") prio=" << static_cast<int>(ob.priority)
          << "</title>\n";
        o << "    </circle>\n";
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
        // Square marker for units (vs circle for objectives).
        const double s = 3.0;
        o << "    <rect x=\"" << px - s/2 << "\" y=\"" << py - s/2
          << "\" width=\"" << s << "\" height=\"" << s
          << "\" fill=\"" << fill << "\" stroke=\"" << stroke
          << "\" stroke-width=\"0.5\" opacity=\"0.9\">\n";
        o << "      <title>Unit type=" << u.type << " owner=" << static_cast<int>(u.owner)
          << " at (" << u.x << "," << u.y << ") dest=(" << u.dest_x << "," << u.dest_y
          << ")</title>\n";
        o << "    </rect>\n";
        // Destination line (if the unit has a non-zero destination).
        if (u.dest_x != 0 || u.dest_y != 0) {
            const double dpx = to_px_x(u.dest_x, cfg.pixels_per_cell);
            const double dpy = to_px_y(u.dest_y, cfg.pixels_per_cell, cfg.grid_size);
            o << "    <line x1=\"" << px << "\" y1=\"" << py << "\" x2=\"" << dpx
              << "\" y2=\"" << dpy << "\" stroke=\"" << stroke
              << "\" stroke-width=\"0.5\" stroke-dasharray=\"2,2\" opacity=\"0.6\"/>\n";
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
    // Count objectives per owner for the legend.
    std::unordered_map<int, int> obj_counts;
    for (const auto& ob : objs) obj_counts[ob.owner]++;
    std::unordered_map<int, int> unit_counts;
    for (const auto& u : units) unit_counts[u.owner]++;

    o << "  <g id=\"layer-legend\" class=\"layer\" transform=\"translate(8, 8)\">\n";
    o << "    <rect x=\"0\" y=\"0\" width=\"160\" height=\""
      << 20 + static_cast<int>(colors.size()) * 16
      << "\" fill=\"white\" stroke=\"#d1d5db\" stroke-width=\"1\" opacity=\"0.95\"/>\n";
    o << "    <text x=\"6\" y=\"14\" font-size=\"11\" font-weight=\"bold\" fill=\"#374151\">Legend</text>\n";
    int y = 28;
    for (const auto& [owner, tc] : colors) {
        const int oc = obj_counts.count(owner) ? obj_counts[owner] : 0;
        const int uc = unit_counts.count(owner) ? unit_counts[owner] : 0;
        o << "    <circle cx=\"12\" cy=\"" << y << "\" r=\"4\" fill=\"" << tc.fill
          << "\" stroke=\"" << tc.stroke << "\" stroke-width=\"0.5\"/>\n";
        o << "    <text x=\"22\" y=\"" << y + 3 << "\" font-size=\"10\" fill=\"#374151\">"
          << svg_escape(tc.name) << " (obj:" << oc << " unit:" << uc << ")</text>\n";
        y += 16;
    }
    o << "  </g>\n";
}

} // namespace

std::string render_svg(
    const std::vector<ObjectivePoint>& objectives,
    const std::vector<UnitPoint>& units,
    const std::unordered_map<int, TeamColor>& team_colors,
    const MapRendererConfig& config) {

    const double dim = config.grid_size * config.pixels_per_cell;
    std::ostringstream o;
    o << "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n";
    o << "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"" << dim
      << "\" height=\"" << dim << "\" viewBox=\"0 0 " << dim << " " << dim << "\">\n";
    o << "<style>\n";
    o << "  .layer { transition: opacity 0.2s; }\n";
    o << "  .layer.hidden { opacity: 0; }\n";
    o << "</style>\n";
    o << "<rect width=\"100%\" height=\"100%\" fill=\"#f9fafb\"/>\n";

    emit_terrain_placeholder(o, config);
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
    o << "<!DOCTYPE html>\n<html lang=\"en\">\n<head>\n";
    o << "<meta charset=\"UTF-8\">\n";
    o << "<title>F4 World Map</title>\n";
    o << "<style>\n";
    o << "  body { margin: 0; font-family: system-ui, sans-serif; background: #f3f4f6; }\n";
    o << "  .toolbar { position: sticky; top: 0; background: white; border-bottom: 1px solid #d1d5db; padding: 8px 12px; z-index: 10; display: flex; gap: 12px; align-items: center; flex-wrap: wrap; }\n";
    o << "  .toolbar h1 { font-size: 14px; margin: 0; margin-right: 16px; }\n";
    o << "  .toolbar label { font-size: 12px; display: flex; align-items: center; gap: 4px; cursor: pointer; }\n";
    o << "  .map-wrap { padding: 12px; overflow: auto; }\n";
    o << "  svg { background: white; box-shadow: 0 1px 3px rgba(0,0,0,0.1); max-width: none; }\n";
    o << "  .layer.hidden { opacity: 0 !important; pointer-events: none; }\n";
    o << "</style>\n</head>\n<body>\n";
    o << "<div class=\"toolbar\">\n";
    o << "  <h1>F4 World Map</h1>\n";
    o << "  <label><input type=\"checkbox\" checked onchange=\"toggle('layer-terrain', this)\"> Terrain</label>\n";
    o << "  <label><input type=\"checkbox\" checked onchange=\"toggle('layer-grid', this)\"> Grid</label>\n";
    o << "  <label><input type=\"checkbox\" checked onchange=\"toggle('layer-objectives', this)\"> Objectives ("
      << objectives.size() << ")</label>\n";
    o << "  <label><input type=\"checkbox\" checked onchange=\"toggle('layer-units', this)\"> Units ("
      << units.size() << ")</label>\n";
    o << "  <label><input type=\"checkbox\" checked onchange=\"toggle('layer-legend', this)\"> Legend</label>\n";
    o << "</div>\n";
    o << "<div class=\"map-wrap\">\n" << svg << "</div>\n";
    o << "<script>\n";
    o << "  function toggle(id, cb) {\n";
    o << "    const el = document.getElementById(id);\n";
    o << "    if (el) el.classList.toggle('hidden', !cb.checked);\n";
    o << "  }\n";
    o << "</script>\n";
    o << "</body>\n</html>\n";
    return o.str();
}

} // namespace f4::vis
