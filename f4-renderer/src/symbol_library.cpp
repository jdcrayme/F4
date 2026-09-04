// f4-renderer/src/symbol_library.cpp
//
// The runtime symbol system: the SymbolLibrary data model, the lazy
// SVG-backed SymbolDirectory, the fallback square, and the render
// helpers that walk a SymbolDefinition's polylines + polygons into
// either an ImGui draw list or raylib primitives.
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

#include <f4/renderer/svg_import.hpp>

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
// Fallback square + SymbolDirectory — lazy SVG loading.
// ===========================================================================

SymbolDefinition make_fallback_square() {
    SymbolDefinition s;
    s.key = "__fallback__";
    s.display_name = "Missing symbol";
    s.description = "Requested key has no SVG in the symbol directory.";
    SymbolPolygon pg;
    pg.filled = false;
    pg.points = { {-0.6f, -0.6f}, {0.6f, -0.6f}, {0.6f, 0.6f}, {-0.6f, 0.6f} };
    s.polygons.push_back(std::move(pg));
    return s;
}

SymbolDirectory::SymbolDirectory(std::filesystem::path dir)
    : dir_(std::move(dir)) {}

void SymbolDirectory::ensure_loaded(const std::string& key) {
    if (lib_.find(key)) return;
    const std::filesystem::path file = dir_ / (key + ".svg");
    std::error_code ec;
    if (std::filesystem::exists(file, ec)) {
        try {
            lib_.add_or_replace(import_symbol_from_svg_file(file, key));
            return;
        } catch (const std::exception&) {
            // Unparseable SVG falls through to the square — the subset
            // parser's error is recoverable at this layer.
        }
    }
    SymbolDefinition fb = make_fallback_square();
    fb.key = key;  // cache under the requested key: probe the disk once
    lib_.add_or_replace(std::move(fb));
    failed_.push_back(key);
}

void SymbolDirectory::draw(const std::string& key, float center_x,
                           float center_y, float size_px, RlColor fill_color,
                           RlColor outline_color, bool filled) {
    ensure_loaded(key);
    draw_library_symbol(lib_, key, center_x, center_y, size_px,
                        fill_color, outline_color, filled);
}

void SymbolDirectory::draw_imgui(const std::string& key, ImDrawList* dl,
                                 ImVec2 center, float size_px,
                                 unsigned int fill_col, unsigned int outline_col,
                                 bool filled) {
    if (!dl) return;
    ensure_loaded(key);
    draw_library_symbol(dl, lib_, key, center, size_px,
                        fill_col, outline_col, filled);
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
// the "overlapping translucency" convention from the original corpus.
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
                // convexity).
                dl->AddConvexPolyFilled(pts.data(), static_cast<int>(pts.size()), col);
            }
        }
        // Always draw the outline on top so the shape stays legible at
        // small sizes.
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
                // DrawTriangleFan signature: (Vector2* points, int pointCount,
                //   Color color). raylib 5.0 expects the points array to
                // include the center as the first element. We build a
                // temporary array with center prepended; the fan implicitly
                // closes back to the first ring vertex.
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
