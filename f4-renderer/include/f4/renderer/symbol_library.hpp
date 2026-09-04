// f4-renderer/include/f4/renderer/symbol_library.hpp
//
// PUBLIC HEADER — the runtime symbol vocabulary for F4 rendering.
//
// Design
// ------
// Symbols are authored as SVG files in a directory (one symbol per file,
// filename stem = key — e.g. symbols/obj_airbase.svg). SymbolDirectory
// parses each file ON DEMAND the first time its key is requested and
// caches the result; SVG is never touched per frame. The parsed
// SymbolDefinition is a flat list of polylines + polygons in normalized
// [-1, +1] coordinates where (0, 0) is the symbol center and ±1 is the
// half-extent of the symbol's bounding box:
//
//   screen_x = sx + px * (size_px * 0.5f)
//   screen_y = sy + py * (size_px * 0.5f)
//
// Colors are ROLES, not values: every primitive picks one of the two
// colors the caller supplies (fill = team color, outline = contrast), so
// authored symbols never fight the team palette. The SVG subset contract
// (supported elements/attributes, the loud-failure policy) is documented
// in svg_import.hpp.
//
// A key whose SVG is missing or fails to parse gets the built-in fallback
// square (make_fallback_square) — the only non-file symbol in the system.
//
// Rendering
// ---------
// Two render paths:
//   draw_library_symbol(ImDrawList*, ...) — ImGui draw list (panels, legends)
//   draw_library_symbol(...)              — raylib direct (canvas)
// Both take the library + key + center + size_px + fill/outline colors,
// look up the definition, and walk its polylines + polygons. Polylines
// use DrawLineEx / AddPolyline; filled polygons use DrawTriangleFan /
// AddConvexPolyFilled when convex and hole-free, else their earcut
// triangle cache (AddTriangleFilled / DrawTriangle); outline polygons
// use AddPolyline with closed=true.

#pragma once

#include <array>
#include <filesystem>
#include <string>
#include <vector>

struct ImDrawList;
struct ImVec2;

namespace f4::renderer {

// Forward-declared Raylib Color (POD struct of 4 ubytes) so this header
// doesn't need raylib.h.
struct RlColor { unsigned char r, g, b, a; };

// ---------------------------------------------------------------------------
// Data model — pure value types, no raylib/imgui deps in this header.
// ---------------------------------------------------------------------------

/// A single 2D point in normalized symbol space.
/// (0, 0) = symbol center. ±1 = half-extent of the symbol's bounding box.
/// Values outside [-1, +1] are permitted (symbols can draw slightly past
/// their nominal extent — e.g. glyphs above a unit frame).
struct SymbolPoint {
    float x = 0.0f;
    float y = 0.0f;
};

/// Which of the caller's two runtime colors a primitive paints with.
/// The draw helpers receive (fill, outline) — typically team color and a
/// contrast color — and every primitive selects one of them, so authored
/// symbols never carry absolute colors (which would fight the team
/// palette).
enum class SymbolColorRole {
    Fill,       // the caller's fill color (team color), opaque
    FillBlend,  // the fill color at 85% alpha — overlapping translucency
    Outline,    // the caller's outline color (contrast)
};

/// An open or closed polyline (line strip).
/// `width` is in pixels at render time (NOT normalized) so lines stay
/// visually consistent across symbol sizes. `closed = true` connects the
/// last point back to the first.
struct SymbolPolyline {
    std::vector<SymbolPoint> points;
    float  width  = 1.0f;
    bool   closed = false;
    SymbolColorRole color_role = SymbolColorRole::Outline;
};

/// A filled or outline polygon.
/// `filled = true` renders as a filled shape; `filled = false` renders
/// as an outline only. Filled rendering has two paths:
///   - convex, no holes -> fast path (triangle fan / convex fill)
///   - concave or holed -> the `triangles` cache (earcut triangulation)
/// `holes` holds interior rings (SVG evenodd subpaths contained in the
/// outer loop) and renders as cut-outs. `triangles` is DERIVED data —
/// computed by refresh_fill_caches(), never serialized, and empty when
/// the fast path applies. Outline rendering works for any simple
/// polygon.
struct SymbolPolygon {
    std::vector<SymbolPoint> points;
    bool filled = true;
    SymbolColorRole color_role = SymbolColorRole::Fill;
    std::vector<std::vector<SymbolPoint>> holes;
    std::vector<std::array<SymbolPoint, 3>> triangles;
};

/// One complete symbol definition.
/// `key` is the dictionary lookup string (e.g. "obj_airbase" — the SVG
/// filename stem). `display_name` and `description` come from the SVG's
/// <title>/<desc>.
struct SymbolDefinition {
    std::string key;
    std::string display_name;
    std::string description;
    std::vector<SymbolPolyline> polylines;
    std::vector<SymbolPolygon>  polygons;
};

/// A library of symbol definitions, keyed by `key`.
/// Maintains insertion order for stable iteration. `find()` returns a
/// pointer into the vector (so it's stable across non-mutating
/// operations); `add_or_replace()` preserves order if updating an
/// existing key, appends otherwise.
class SymbolLibrary {
public:
    /// Look up a symbol by key. Returns nullptr if not found.
    [[nodiscard]] const SymbolDefinition* find(const std::string& key) const noexcept;
    [[nodiscard]] SymbolDefinition* find(const std::string& key) noexcept;

    /// Insert or replace a definition. If a definition with the same key
    /// already exists, it is replaced in place (preserving order);
    /// otherwise the new definition is appended.
    void add_or_replace(SymbolDefinition def);

    /// Remove the definition with the given key. Returns true if removed,
    /// false if the key wasn't found.
    bool erase(const std::string& key) noexcept;

    /// Number of definitions.
    [[nodiscard]] std::size_t size() const noexcept { return symbols_.size(); }
    [[nodiscard]] bool empty() const noexcept { return symbols_.empty(); }

    /// Read-only access to the underlying vector (for iteration).
    [[nodiscard]] const std::vector<SymbolDefinition>& symbols() const noexcept { return symbols_; }

    /// Mutable access (for in-place edits). Use carefully — the caller
    /// is responsible for keeping `key` unique.
    [[nodiscard]] std::vector<SymbolDefinition>& mutable_symbols() noexcept { return symbols_; }

private:
    std::vector<SymbolDefinition> symbols_;
};

// ---------------------------------------------------------------------------
// Fill caches — earcut triangulation for concave / holed fills.
// ---------------------------------------------------------------------------

/// Recompute derived fill caches for every filled polygon in `def`:
/// concave polygons and polygons with holes get an earcut triangulation
/// into SymbolPolygon::triangles; convex hole-free polygons keep the
/// fan fast path (empty cache). Called by the SVG importer; call it
/// again after mutating points/holes — rendering trusts the cache.
void refresh_fill_caches(SymbolDefinition& def);

// ---------------------------------------------------------------------------
// The fallback symbol + the lazy SVG directory.
// ---------------------------------------------------------------------------

/// The only procedural symbol left: an unfilled square outline drawn
/// when a requested key has no SVG in the directory. Data-defined and
/// rendered by the normal library paths — no dedicated drawing code.
[[nodiscard]] SymbolDefinition make_fallback_square();

/// A directory of SVG symbol files, parsed on demand.
///
/// get()/draw() resolve a key by reading <dir>/<key>.svg ONCE; the
/// parsed definition is cached in the backing SymbolLibrary. A key whose
/// file is missing or fails the subset parser gets the fallback square
/// cached under that key (so a missing symbol costs exactly one file
/// probe for the process lifetime) and is recorded in failed_keys().
class SymbolDirectory {
public:
    /// `dir` may not exist (everything falls back); resolution policy is
    /// the caller's (the world-viewer probes exe-relative + CWD-relative
    /// candidates; tests pass explicit paths).
    explicit SymbolDirectory(std::filesystem::path dir);

    [[nodiscard]] const std::filesystem::path& dir() const noexcept { return dir_; }
    [[nodiscard]] const SymbolLibrary& library() const noexcept { return lib_; }

    /// Keys that fell back (missing/unparseable SVGs), in request order.
    /// For diagnostics — e.g. a viewer status line or test assertions.
    [[nodiscard]] const std::vector<std::string>& failed_keys() const noexcept { return failed_; }

    /// Draw `key` with raylib primitives (loads on first use).
    /// No-op if the key somehow has no cached definition.
    void draw(const std::string& key, float center_x, float center_y,
              float size_px, RlColor fill_color, RlColor outline_color,
              bool filled = true);

    /// Draw `key` into an ImGui draw list (loads on first use).
    void draw_imgui(const std::string& key, ImDrawList* dl, ImVec2 center,
                    float size_px, unsigned int fill_col,
                    unsigned int outline_col, bool filled = true);

private:
    /// Parse <dir_>/<key>.svg into lib_ if not present; on any failure,
    /// cache the fallback square under `key`.
    void ensure_loaded(const std::string& key);

    std::filesystem::path dir_;
    SymbolLibrary lib_;
    std::vector<std::string> failed_;
};

// ---------------------------------------------------------------------------
// Rendering — walk a definition and emit primitives.
// Both paths look up `key` in `lib` and no-op if not found.
// ---------------------------------------------------------------------------

/// Render a library symbol into an ImGui draw list.
///   dl          — target draw list (e.g. ImGui::GetWindowDrawList())
///   lib         — the symbol library to look up in
///   key         — which symbol to draw
///   center      — screen-space center of the symbol
///   size_px     — overall symbol extent (width = height = size_px)
///   fill_col    — ImGui-packed fill color (use IM_COL32(r,g,b,a))
///   outline_col — ImGui-packed outline color
///   filled      — if false, polygons render as outlines only
void draw_library_symbol(ImDrawList* dl, const SymbolLibrary& lib,
                          const std::string& key, ImVec2 center,
                          float size_px, unsigned int fill_col,
                          unsigned int outline_col, bool filled = true);

/// Render a library symbol using raylib primitives directly.
///   lib         — the symbol library to look up in
///   key         — which symbol to draw
///   center      — screen-space center of the symbol
///   size_px     — overall symbol extent (width = height = size_px)
///   fill_color  — fill color (typically team color)
///   outline_col — outline color
///   filled      — if false, polygons render as outlines only
void draw_library_symbol(const SymbolLibrary& lib, const std::string& key,
                          float center_x, float center_y, float size_px,
                          RlColor fill_color, RlColor outline_color,
                          bool filled = true);

} // namespace f4::renderer
