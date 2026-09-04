// f4-renderer/src/symbol_library.cpp
//
// Implementation of the data-driven symbol library: JSON I/O via f4-json
// + render helpers that walk a SymbolDefinition's polylines + polygons
// into either an ImGui draw list or raylib primitives.
//
// Coordinate convention
// ----------------------
// All stored points are in normalized [-1, +1] space where (0, 0) is
// the symbol center and ±1 is the half-extent of the symbol's bounding
// box. To convert a stored point (px, py) to screen pixels at center
// (sx, sy) with extent size_px:
//
//   screen_x = sx + px * (size_px * 0.5f)
//   screen_y = sy + py * (size_px * 0.5f)
//
// This matches the existing convention in symbols.cpp where `r = size_px
// * 0.5f` and every shape is expressed as a fraction of r. The render
// helpers below use exactly this transform so a library symbol and a
// procedural symbol at the same (sx, sy, size_px) line up perfectly.
//
// Filled polygons
// ---------------
// Filled rendering has two paths per polygon:
//   - convex + hole-free  -> fast path: raylib DrawTriangleFan /
//                            ImGui AddConvexPolyFilled
//   - concave or holed    -> the earcut triangle cache in
//                            SymbolPolygon::triangles, computed by
//                            refresh_fill_caches() (see below), rendered
//                            per-triangle. Earcut preserves the outer
//                            ring's winding, so triangles and the fan
//                            path orient identically.
// Outline rendering works for any simple polygon (line strip + close).

#include <f4/renderer/symbol_library.hpp>

#include <f4/json/f4_json.hpp>

#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable : 4100)  // unreferenced formal parameter (templates)
#endif
#include <mapbox/earcut.hpp>     // vendored header-only ear-clip triangulator
#ifdef _MSC_VER
#pragma warning(pop)
#endif

#include <imgui.h>
#include <raylib.h>

#include <cstdint>

#include <algorithm>
#include <cmath>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>

namespace f4::renderer {

// ===========================================================================
// SymbolLibrary — find / add_or_replace / erase
// ===========================================================================

const SymbolDefinition* SymbolLibrary::find(const std::string& key) const noexcept {
    for (const auto& s : symbols_) {
        if (s.key == key) return &s;
    }
    return nullptr;
}

SymbolDefinition* SymbolLibrary::find(const std::string& key) noexcept {
    for (auto& s : symbols_) {
        if (s.key == key) return &s;
    }
    return nullptr;
}

void SymbolLibrary::add_or_replace(SymbolDefinition def) {
    if (auto* existing = find(def.key)) {
        *existing = std::move(def);
    } else {
        symbols_.push_back(std::move(def));
    }
}

bool SymbolLibrary::erase(const std::string& key) noexcept {
    for (auto it = symbols_.begin(); it != symbols_.end(); ++it) {
        if (it->key == key) {
            symbols_.erase(it);
            return true;
        }
    }
    return false;
}

// ===========================================================================
// JSON I/O — using f4::json::Reader / Writer (the project's own minimal
// dependency-free JSON library; same pattern as settings.cpp).
// ===========================================================================

namespace {

using f4::json::Reader;
using f4::json::Writer;

// Read a single point object: { "x": <number>, "y": <number> }.
// Assumes the caller has already consumed the opening '{'.
// Tolerates missing fields (defaults to 0.0) and unknown keys (skipped).
// Read a "color_role" string value (shared by every primitive type).
SymbolColorRole read_color_role(Reader& r) {
    const std::string role = r.read_string();
    if      (role == "fill_blend") return SymbolColorRole::FillBlend;
    else if (role == "outline")    return SymbolColorRole::Outline;
    else                           return SymbolColorRole::Fill;
}

SymbolPoint read_point(Reader& r) {
    SymbolPoint p;
    for (;;) {
        std::string k = r.read_string();
        r.expect(':');
        if      (k == "x") p.x = static_cast<float>(r.read_number());
        else if (k == "y") p.y = static_cast<float>(r.read_number());
        else               r.skip_value();
        if (r.consume('}')) break;
        r.expect(',');
    }
    return p;
}

// Read an array of points: [ {x,y}, {x,y}, ... ].
std::vector<SymbolPoint> read_points(Reader& r) {
    std::vector<SymbolPoint> out;
    r.expect('[');
    if (r.consume(']')) return out;
    for (;;) {
        r.expect('{');
        out.push_back(read_point(r));
        if (r.consume(']')) break;
        r.expect(',');
    }
    return out;
}

// Read one polyline object. Assumes '{' has NOT been consumed yet.
SymbolPolyline read_polyline(Reader& r) {
    SymbolPolyline pl;
    pl.width = 1.0f;        // default if absent
    pl.closed = false;      // default if absent
    r.expect('{');
    for (;;) {
        std::string k = r.read_string();
        r.expect(':');
        if      (k == "width")      pl.width   = static_cast<float>(r.read_number());
        else if (k == "closed")     pl.closed  = r.read_bool();
        else if (k == "points")     pl.points  = read_points(r);
        else if (k == "color_role") pl.color_role = read_color_role(r);
        else                        r.skip_value();
        if (r.consume('}')) break;
        r.expect(',');
    }
    return pl;
}

// Read one polygon object. Assumes '{' has NOT been consumed yet.
// v2 fields "color_role" and "holes" are optional; older v1 files load
// unchanged (defaults: Fill role, no holes).
SymbolPolygon read_polygon(Reader& r) {
    SymbolPolygon pg;
    pg.filled = true;       // default if absent
    r.expect('{');
    for (;;) {
        std::string k = r.read_string();
        r.expect(':');
        if      (k == "filled")  pg.filled = r.read_bool();
        else if (k == "points")  pg.points  = read_points(r);
        else if (k == "color_role") {
            const std::string role = r.read_string();
            if      (role == "fill_blend") pg.color_role = SymbolColorRole::FillBlend;
            else if (role == "outline")    pg.color_role = SymbolColorRole::Outline;
            else                           pg.color_role = SymbolColorRole::Fill;
        }
        else if (k == "holes") {
            r.expect('[');
            if (r.consume(']')) { /* empty */ }
            else {
                for (;;) {
                    pg.holes.push_back(read_points(r));
                    if (r.consume(']')) break;
                    r.expect(',');
                }
            }
        }
        else                     r.skip_value();
        if (r.consume('}')) break;
        r.expect(',');
    }
    return pg;
}

// v2 sugar primitive: { x, y, w, h, filled, color_role } -> polygon.
SymbolPolygon read_rectangle(Reader& r) {
    float x = 0, y = 0, w = 0, h = 0;
    bool filled = true;
    SymbolColorRole role = SymbolColorRole::Fill;
    r.expect('{');
    for (;;) {
        std::string k = r.read_string();
        r.expect(':');
        if      (k == "x")          x     = static_cast<float>(r.read_number());
        else if (k == "y")          y     = static_cast<float>(r.read_number());
        else if (k == "w")          w     = static_cast<float>(r.read_number());
        else if (k == "h")          h     = static_cast<float>(r.read_number());
        else if (k == "filled")     filled = r.read_bool();
        else if (k == "color_role") role   = read_color_role(r);
        else                        r.skip_value();
        if (r.consume('}')) break;
        r.expect(',');
    }
    SymbolPolygon pg;
    pg.filled = filled;
    pg.color_role = role;
    pg.points = { {x, y}, {x + w, y}, {x + w, y + h}, {x, y + h} };
    return pg;
}

// v2 sugar primitive: { x1, y1, x2, y2, width, color_role } -> polyline.
SymbolPolyline read_line(Reader& r) {
    float x1 = 0, y1 = 0, x2 = 0, y2 = 0, width = 1.0f;
    SymbolColorRole role = SymbolColorRole::Outline;
    r.expect('{');
    for (;;) {
        std::string k = r.read_string();
        r.expect(':');
        if      (k == "x1")         x1    = static_cast<float>(r.read_number());
        else if (k == "y1")         y1    = static_cast<float>(r.read_number());
        else if (k == "x2")         x2    = static_cast<float>(r.read_number());
        else if (k == "y2")         y2    = static_cast<float>(r.read_number());
        else if (k == "width")      width = static_cast<float>(r.read_number());
        else if (k == "color_role") role  = read_color_role(r);
        else                        r.skip_value();
        if (r.consume('}')) break;
        r.expect(',');
    }
    SymbolPolyline pl;
    pl.width = width;
    pl.color_role = role;
    pl.points = { {x1, y1}, {x2, y2} };
    return pl;
}

// v2 sugar primitive: { cx, cy, radius, color_role } -> filled octagon.
SymbolPolygon read_dot(Reader& r) {
    float cx = 0, cy = 0, radius = 0.05f;
    SymbolColorRole role = SymbolColorRole::Outline;
    r.expect('{');
    for (;;) {
        std::string k = r.read_string();
        r.expect(':');
        if      (k == "cx")         cx     = static_cast<float>(r.read_number());
        else if (k == "cy")         cy     = static_cast<float>(r.read_number());
        else if (k == "radius")     radius = static_cast<float>(r.read_number());
        else if (k == "color_role") role   = read_color_role(r);
        else                        r.skip_value();
        if (r.consume('}')) break;
        r.expect(',');
    }
    SymbolPolygon pg;
    pg.filled = true;
    pg.color_role = role;
    pg.points.reserve(8);
    for (int i = 0; i < 8; ++i) {
        const double th = 2.0 * 3.14159265358979323846 * i / 8.0;
        pg.points.push_back({
            cx + static_cast<float>(radius * std::cos(th)),
            cy + static_cast<float>(radius * std::sin(th))});
    }
    return pg;
}

// Read one symbol definition. Assumes '{' has NOT been consumed yet.
//
// v2 "sugar" primitives (rectangles / lines / dots — the corpus's
// authoring shorthand) are NORMALIZED here into the canonical model:
//   rectangle -> 4-point polygon (filled or outline, color role kept)
//   line      -> 2-point polyline (width + color role kept)
//   dot       -> filled octagon (color role kept)
// The writer emits only the canonical form, so one load/save cycle
// canonicalizes a v2 file. Rendering note: dot polygons always render
// in the polygon pass (below lines) — at dot sizes this is invisible.
SymbolDefinition read_symbol(Reader& r) {
    SymbolDefinition def;
    r.expect('{');
    for (;;) {
        std::string k = r.read_string();
        r.expect(':');
        if      (k == "key")          def.key          = r.read_string();
        else if (k == "display_name") def.display_name = r.read_string();
        else if (k == "category")     def.category     = r.read_string();
        else if (k == "description")  def.description  = r.read_string();
        else if (k == "polylines") {
            r.expect('[');
            if (r.consume(']')) { /* empty */ }
            else {
                for (;;) {
                    def.polylines.push_back(read_polyline(r));
                    if (r.consume(']')) break;
                    r.expect(',');
                }
            }
        }
        else if (k == "polygons") {
            r.expect('[');
            if (r.consume(']')) { /* empty */ }
            else {
                for (;;) {
                    def.polygons.push_back(read_polygon(r));
                    if (r.consume(']')) break;
                    r.expect(',');
                }
            }
        }
        else if (k == "rectangles") {
            r.expect('[');
            if (r.consume(']')) { /* empty */ }
            else {
                for (;;) {
                    def.polygons.push_back(read_rectangle(r));
                    if (r.consume(']')) break;
                    r.expect(',');
                }
            }
        }
        else if (k == "lines") {
            r.expect('[');
            if (r.consume(']')) { /* empty */ }
            else {
                for (;;) {
                    def.polylines.push_back(read_line(r));
                    if (r.consume(']')) break;
                    r.expect(',');
                }
            }
        }
        else if (k == "dots") {
            r.expect('[');
            if (r.consume(']')) { /* empty */ }
            else {
                for (;;) {
                    def.polygons.push_back(read_dot(r));
                    if (r.consume(']')) break;
                    r.expect(',');
                }
            }
        }
        else {
            r.skip_value();
        }
        if (r.consume('}')) break;
        r.expect(',');
    }
    return def;
}

} // namespace

SymbolLibrary load_symbol_library_from_string(const std::string& json) {
    SymbolLibrary lib;
    Reader r(json);
    try {
        r.skip_ws();
        r.expect('{');
        if (r.consume('}')) return lib;  // empty object
        for (;;) {
            std::string k = r.read_string();
            r.expect(':');
            if (k == "version") {
                (void)r.read_int();  // accept any version; we skip unknown fields
            } else if (k == "symbols") {
                r.expect('[');
                if (r.consume(']')) { /* empty */ }
                else {
                    for (;;) {
                        lib.add_or_replace(read_symbol(r));
                        if (r.consume(']')) break;
                        r.expect(',');
                    }
                }
            } else {
                r.skip_value();
            }
            if (r.consume('}')) break;
            r.expect(',');
        }
    } catch (const std::exception& e) {
        // Re-throw with position context for easier diagnosis.
        throw std::runtime_error(
            std::string("f4::renderer::load_symbol_library: parse failed at position ") +
            std::to_string(r.position()) + ": " + e.what());
    }
    // Derived data (earcut fills) is not serialized — compute it now.
    for (auto& s : lib.mutable_symbols()) refresh_fill_caches(s);
    return lib;
}

SymbolLibrary load_symbol_library(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        throw std::runtime_error(
            "f4::renderer::load_symbol_library: cannot open '" +
            path.string() + "'");
    }
    std::ostringstream ss;
    ss << in.rdbuf();
    return load_symbol_library_from_string(ss.str());
}

// ---------------------------------------------------------------------------
// Serialization — pretty-printed with 2-space indentation for git-friendliness.
// ---------------------------------------------------------------------------

namespace {

void write_point(Writer& w, const SymbolPoint& p, int indent) {
    for (int i = 0; i < indent; ++i) w.raw(" ");
    w.raw("{ \"x\": ");
    w.number(static_cast<double>(p.x));
    w.raw(", \"y\": ");
    w.number(static_cast<double>(p.y));
    w.raw(" }");
}

void write_points(Writer& w, const std::vector<SymbolPoint>& pts, int indent) {
    if (pts.empty()) {
        w.raw("[]");
        return;
    }
    w.raw("[\n");
    for (std::size_t i = 0; i < pts.size(); ++i) {
        write_point(w, pts[i], indent);
        if (i + 1 < pts.size()) w.raw(",");
        w.raw("\n");
    }
    for (int i = 0; i < indent - 2; ++i) w.raw(" ");
    w.raw("]");
}

void write_polyline(Writer& w, const SymbolPolyline& pl, int indent) {
    for (int i = 0; i < indent; ++i) w.raw(" ");
    w.raw("{\n");
    for (int i = 0; i < indent + 2; ++i) w.raw(" ");
    w.raw("\"width\": ");   w.number(static_cast<double>(pl.width));   w.raw(",\n");
    for (int i = 0; i < indent + 2; ++i) w.raw(" ");
    w.raw("\"closed\": ");  w.raw(pl.closed ? "true" : "false");       w.raw(",\n");
    if (pl.color_role != SymbolColorRole::Outline) {
        for (int i = 0; i < indent + 2; ++i) w.raw(" ");
        w.raw("\"color_role\": ");
        switch (pl.color_role) {
            case SymbolColorRole::Fill:      w.raw("\"fill\"");       break;
            case SymbolColorRole::FillBlend: w.raw("\"fill_blend\""); break;
            case SymbolColorRole::Outline:   w.raw("\"outline\"");    break;
        }
        w.raw(",\n");
    }
    for (int i = 0; i < indent + 2; ++i) w.raw(" ");
    w.raw("\"points\": ");  write_points(w, pl.points, indent + 4);    w.raw("\n");
    for (int i = 0; i < indent; ++i) w.raw(" ");
    w.raw("}");
}

void write_polygon(Writer& w, const SymbolPolygon& pg, int indent) {
    for (int i = 0; i < indent; ++i) w.raw(" ");
    w.raw("{\n");
    for (int i = 0; i < indent + 2; ++i) w.raw(" ");
    w.raw("\"filled\": ");  w.raw(pg.filled ? "true" : "false");       w.raw(",\n");
    if (pg.color_role != SymbolColorRole::Fill) {
        for (int i = 0; i < indent + 2; ++i) w.raw(" ");
        w.raw("\"color_role\": ");
        switch (pg.color_role) {
            case SymbolColorRole::FillBlend: w.raw("\"fill_blend\""); break;
            case SymbolColorRole::Outline:   w.raw("\"outline\"");    break;
            case SymbolColorRole::Fill:      w.raw("\"fill\"");       break;
        }
        w.raw(",\n");
    }
    for (int i = 0; i < indent + 2; ++i) w.raw(" ");
    w.raw("\"points\": ");  write_points(w, pg.points, indent + 4);    w.raw("\n");
    if (!pg.holes.empty()) {
        for (int i = 0; i < indent + 2; ++i) w.raw(" ");
        w.raw("\"holes\": [\n");
        for (std::size_t i = 0; i < pg.holes.size(); ++i) {
            write_points(w, pg.holes[i], indent + 4);
            if (i + 1 < pg.holes.size()) w.raw(",");
            w.raw("\n");
        }
        for (int i = 0; i < indent + 2; ++i) w.raw(" ");
        w.raw("]\n");
    }
    for (int i = 0; i < indent; ++i) w.raw(" ");
    w.raw("}");
}

} // namespace

std::string symbol_library_to_json(const SymbolLibrary& lib) {
    Writer w;
    w.raw("{\n");
    w.raw("  \"version\": 2,\n");
    w.raw("  \"symbols\":");
    if (lib.symbols().empty()) {
        w.raw(" []\n");
    } else {
        w.raw(" [\n");
        const auto& syms = lib.symbols();
        for (std::size_t i = 0; i < syms.size(); ++i) {
            const auto& s = syms[i];
            w.raw("  {\n");
            w.raw("    \"key\": ");          w.string(s.key);                       w.raw(",\n");
            w.raw("    \"display_name\": "); w.string(s.display_name);              w.raw(",\n");
            w.raw("    \"category\": ");     w.string(s.category);                  w.raw(",\n");
            w.raw("    \"description\": ");  w.string(s.description);               w.raw(",\n");
            w.raw("    \"polylines\":");
            if (s.polylines.empty()) {
                w.raw(" [],\n");
            } else {
                w.raw(" [\n");
                for (std::size_t j = 0; j < s.polylines.size(); ++j) {
                    write_polyline(w, s.polylines[j], 6);
                    if (j + 1 < s.polylines.size()) w.raw(",");
                    w.raw("\n");
                }
                w.raw("    ],\n");
            }
            w.raw("    \"polygons\":");
            if (s.polygons.empty()) {
                w.raw(" []\n");
            } else {
                w.raw(" [\n");
                for (std::size_t j = 0; j < s.polygons.size(); ++j) {
                    write_polygon(w, s.polygons[j], 6);
                    if (j + 1 < s.polygons.size()) w.raw(",");
                    w.raw("\n");
                }
                w.raw("    ]\n");
            }
            w.raw("  }");
            if (i + 1 < syms.size()) w.raw(",");
            w.raw("\n");
        }
        w.raw("  ]\n");
    }
    w.raw("}\n");
    return w.str();
}

void save_symbol_library(const SymbolLibrary& lib,
                          const std::filesystem::path& path) {
    const std::string json = symbol_library_to_json(lib);
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) {
        throw std::runtime_error(
            "f4::renderer::save_symbol_library: cannot open '" +
            path.string() + "' for writing");
    }
    out.write(json.data(), static_cast<std::streamsize>(json.size()));
    if (!out) {
        throw std::runtime_error(
            "f4::renderer::save_symbol_library: write failed for '" +
            path.string() + "'");
    }
}

// ---------------------------------------------------------------------------
// make_default_symbol_library — 3 trivial examples so the user has
// something to look at on first launch and can immediately try save/load.
// ---------------------------------------------------------------------------

SymbolLibrary make_default_symbol_library() {
    SymbolLibrary lib;

    // example_square — filled square outline + diagonal stroke.
    {
        SymbolDefinition s;
        s.key = "example_square";
        s.display_name = "Square";
        s.category = "example";
        s.description = "A filled square with a diagonal stroke.";
        s.polygons.push_back({
            { {-0.6f, -0.6f}, {0.6f, -0.6f}, {0.6f, 0.6f}, {-0.6f, 0.6f} },
            true  // filled
        });
        s.polylines.push_back({
            { {-0.6f, -0.6f}, {0.6f, 0.6f} },
            1.5f,  // width
            false  // not closed
        });
        lib.add_or_replace(std::move(s));
    }

    // example_triangle — outline triangle.
    {
        SymbolDefinition s;
        s.key = "example_triangle";
        s.display_name = "Triangle";
        s.category = "example";
        s.description = "An outline triangle pointing up.";
        s.polygons.push_back({
            { {0.0f, -0.7f}, {0.7f, 0.5f}, {-0.7f, 0.5f} },
            false  // outline only
        });
        lib.add_or_replace(std::move(s));
    }

    // example_diamond — filled diamond + horizontal bar.
    {
        SymbolDefinition s;
        s.key = "example_diamond";
        s.display_name = "Diamond";
        s.category = "example";
        s.description = "A filled diamond with a horizontal center bar.";
        s.polygons.push_back({
            { {0.0f, -0.7f}, {0.7f, 0.0f}, {0.0f, 0.7f}, {-0.7f, 0.0f} },
            true  // filled
        });
        s.polylines.push_back({
            { {-0.5f, 0.0f}, {0.5f, 0.0f} },
            2.0f,  // width
            false  // not closed
        });
        lib.add_or_replace(std::move(s));
    }

    return lib;
}

// ===========================================================================
// Fill caches — earcut triangulation for concave / holed fills.
// ===========================================================================

namespace {

// Convexity test: sign consistency of edge cross products. Collinear
// vertices (cross ~ 0) are tolerated — they don't break the fan.
bool polygon_is_convex(const std::vector<SymbolPoint>& pts) {
    if (pts.size() < 4) return true;
    bool has_pos = false, has_neg = false;
    for (std::size_t i = 0; i < pts.size(); ++i) {
        const SymbolPoint& a = pts[i];
        const SymbolPoint& b = pts[(i + 1) % pts.size()];
        const SymbolPoint& c = pts[(i + 2) % pts.size()];
        const float cross =
            (b.x - a.x) * (c.y - b.y) - (b.y - a.y) * (c.x - b.x);
        if (cross > 1e-9f)  has_pos = true;
        if (cross < -1e-9f) has_neg = true;
        if (has_pos && has_neg) return false;
    }
    return true;
}

} // namespace

void refresh_fill_caches(SymbolDefinition& def) {
    for (auto& pg : def.polygons) {
        pg.triangles.clear();
        if (!pg.filled || pg.points.size() < 3) continue;
        if (pg.holes.empty() && polygon_is_convex(pg.points)) continue;

        // earcut consumes tuple-like points (std::get<I>); feed arrays and
        // keep a flat vertex list to map its concatenated ring indices back
        // to SymbolPoints. Triangles follow the outer ring's winding, so
        // they render with the same orientation as the convex fan path.
        std::vector<SymbolPoint> flat;
        std::vector<std::vector<std::array<double, 2>>> loops;
        auto add_loop = [&](const std::vector<SymbolPoint>& loop) {
            if (loop.size() < 3) return;
            std::vector<std::array<double, 2>> ring;
            ring.reserve(loop.size());
            for (const auto& p : loop) {
                ring.push_back({ static_cast<double>(p.x),
                                 static_cast<double>(p.y) });
                flat.push_back(p);
            }
            loops.push_back(std::move(ring));
        };
        add_loop(pg.points);
        for (const auto& h : pg.holes) add_loop(h);
        if (loops.empty()) continue;

        const std::vector<std::uint32_t> indices =
            mapbox::earcut<std::uint32_t>(loops);
        pg.triangles.reserve(indices.size() / 3);
        for (std::size_t i = 0; i + 2 < indices.size(); i += 3) {
            pg.triangles.push_back({ flat[indices[i]],
                                     flat[indices[i + 1]],
                                     flat[indices[i + 2]] });
        }
    }
}

// ===========================================================================
// Render helpers — walk a SymbolDefinition and emit primitives.
// ===========================================================================

namespace {

// Convert a normalized point to screen-space.
inline ImVec2 to_screen_imgui(ImVec2 center, float half, const SymbolPoint& p) {
    return ImVec2(center.x + p.x * half, center.y + p.y * half);
}

inline Vector2 to_screen_raylib(float sx, float sy, float half, const SymbolPoint& p) {
    return Vector2{ sx + p.x * half, sy + p.y * half };
}

// Role -> color resolution. FillBlend takes the fill color at 85% alpha —
// the "overlapping translucency" convention from f4_symbols.json.
inline unsigned int role_color(SymbolColorRole role, unsigned int fill_col,
                               unsigned int outline_col) {
    switch (role) {
        case SymbolColorRole::Fill:
            return fill_col;
        case SymbolColorRole::FillBlend: {
            const unsigned int a = ((fill_col >> 24) & 0xFFu) * 217u / 255u;
            return (fill_col & 0x00FFFFFFu) | (a << 24);
        }
        case SymbolColorRole::Outline:
            return outline_col;
    }
    return outline_col;
}

inline Color role_color(SymbolColorRole role, Color fill, Color outline) {
    switch (role) {
        case SymbolColorRole::Fill:
            return fill;
        case SymbolColorRole::FillBlend: {
            Color c = fill;
            c.a = static_cast<unsigned char>(c.a * 0.85f);
            return c;
        }
        case SymbolColorRole::Outline:
            return outline;
    }
    return outline;
}

} // namespace

void draw_library_symbol(ImDrawList* dl, const SymbolLibrary& lib,
                          const std::string& key, ImVec2 center,
                          float size_px, unsigned int fill_col,
                          unsigned int outline_col, bool filled) {
    if (!dl) return;
    const SymbolDefinition* def = lib.find(key);
    if (!def) return;
    const float half = size_px * 0.5f;

    // Polygons first (filled below outlines so outlines stay crisp).
    for (const auto& pg : def->polygons) {
        if (pg.points.size() < 2) continue;
        // Build a temporary array of ImVec2 (AddConvexPolyFilled / AddPolyline
        // require contiguous ImVec2*).
        std::vector<ImVec2> pts;
        pts.reserve(pg.points.size());
        for (const auto& p : pg.points) pts.push_back(to_screen_imgui(center, half, p));
        if (pg.filled && filled && pts.size() >= 3) {
            const unsigned int col = role_color(pg.color_role, fill_col, outline_col);
            if (!pg.triangles.empty()) {
                // Concave / holed fill: per-triangle earcut cache.
                for (const auto& t : pg.triangles) {
                    dl->AddTriangleFilled(to_screen_imgui(center, half, t[0]),
                                          to_screen_imgui(center, half, t[1]),
                                          to_screen_imgui(center, half, t[2]),
                                          col);
                }
            } else {
                // Convex fast path (ImGui's AddConvexPolyFilled requires
                // convexity — see file header comment).
                dl->AddConvexPolyFilled(pts.data(), static_cast<int>(pts.size()), col);
            }
        }
        // Always draw the outline on top so the shape stays legible at
        // small sizes (matches the existing symbols.cpp convention).
        if (pts.size() >= 2) {
            // AddPolyline with closed=true connects last point back to first.
            // We pass a thickness of 1.0 here since the polygon doesn't
            // carry its own width; the caller's outline_col provides contrast.
            dl->AddPolyline(pts.data(), static_cast<int>(pts.size()), outline_col,
                            pg.points.size() >= 3 ? ImDrawFlags_Closed : 0, 1.0f);
        }
    }

    // Polylines on top of polygons.
    for (const auto& pl : def->polylines) {
        if (pl.points.size() < 2) continue;
        std::vector<ImVec2> pts;
        pts.reserve(pl.points.size());
        for (const auto& p : pl.points) pts.push_back(to_screen_imgui(center, half, p));
        const float thickness = pl.width > 0.0f ? pl.width : 1.0f;
        dl->AddPolyline(pts.data(), static_cast<int>(pts.size()),
                        role_color(pl.color_role, fill_col, outline_col),
                        pl.closed && pl.points.size() >= 3 ? ImDrawFlags_Closed : 0,
                        thickness);
    }
}

void draw_library_symbol(const SymbolLibrary& lib, const std::string& key,
                          float sx, float sy, float size_px,
                          RlColor fill_color, RlColor outline_color,
                          bool filled) {
    const SymbolDefinition* def = lib.find(key);
    if (!def) return;
    const float half = size_px * 0.5f;
    const Color fc = { fill_color.r, fill_color.g, fill_color.b, fill_color.a };
    const Color oc = { outline_color.r, outline_color.g, outline_color.b, outline_color.a };

    // Polygons first.
    for (const auto& pg : def->polygons) {
        if (pg.points.size() < 2) continue;
        std::vector<Vector2> pts;
        pts.reserve(pg.points.size());
        for (const auto& p : pg.points) pts.push_back(to_screen_raylib(sx, sy, half, p));

        if (pg.filled && filled && pts.size() >= 3) {
            const Color pc = role_color(pg.color_role, fc, oc);
            if (!pg.triangles.empty()) {
                // Concave / holed fill: per-triangle earcut cache. Same
                // winding as the fan path (earcut follows the outer ring),
                // so visibility matches.
                for (const auto& t : pg.triangles) {
                    DrawTriangle(to_screen_raylib(sx, sy, half, t[0]),
                                 to_screen_raylib(sx, sy, half, t[1]),
                                 to_screen_raylib(sx, sy, half, t[2]), pc);
                }
            } else {
                // DrawTriangleFan takes a center vertex + the ring vertices.
                // For a convex polygon, the centroid is a safe center.
                Vector2 centroid = { 0, 0 };
                for (const auto& p : pts) { centroid.x += p.x; centroid.y += p.y; }
                centroid.x /= static_cast<float>(pts.size());
                centroid.y /= static_cast<float>(pts.size());
                // DrawTriangleFan signature: (Vector2 center, Vector2* points,
                //   int pointCount, Color color). The first point is the center,
                //   followed by the ring vertices (the function implicitly closes
                //   the fan back to the first ring vertex).
                // raylib 5.0 expects the points array to include the center as
                // the first element. We build a temporary array with center
                // prepended.
                std::vector<Vector2> fan;
                fan.reserve(pts.size() + 1);
                fan.push_back(centroid);
                fan.insert(fan.end(), pts.begin(), pts.end());
                DrawTriangleFan(fan.data(), static_cast<int>(fan.size()), pc);
            }
        }
        // Outline on top.
        if (pts.size() >= 2) {
            for (std::size_t i = 0; i + 1 < pts.size(); ++i) {
                DrawLineEx(pts[i], pts[i + 1], 1.0f, oc);
            }
            if (pg.points.size() >= 3) {
                // Close the loop.
                DrawLineEx(pts.back(), pts.front(), 1.0f, oc);
            }
        }
    }

    // Polylines on top.
    for (const auto& pl : def->polylines) {
        if (pl.points.size() < 2) continue;
        std::vector<Vector2> pts;
        pts.reserve(pl.points.size());
        for (const auto& p : pl.points) pts.push_back(to_screen_raylib(sx, sy, half, p));
        const float thickness = pl.width > 0.0f ? pl.width : 1.0f;
        const Color lc = role_color(pl.color_role, fc, oc);
        for (std::size_t i = 0; i + 1 < pts.size(); ++i) {
            DrawLineEx(pts[i], pts[i + 1], thickness, lc);
        }
        if (pl.closed && pl.points.size() >= 3) {
            DrawLineEx(pts.back(), pts.front(), thickness, lc);
        }
    }
}

} // namespace f4::renderer
