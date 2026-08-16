// f4-renderer/include/f4/renderer/symbol_library.hpp
//
// PUBLIC HEADER — data-driven symbol vocabulary for F4 rendering.
//
// Design
// ------
// The existing f4::renderer::symbols.hpp defines a fixed enum (SymbolKind)
// with hard-coded procedural drawing in symbols.cpp. That works for the
// existing objective + unit glyphs but doesn't scale to user-defined
// symbols or rapid iteration on symbol geometry.
//
// This header introduces a data-driven alternative: a SymbolLibrary is a
// flat collection of SymbolDefinitions keyed by string. Each definition
// is a list of polylines + polygons expressed in normalized [-1, +1]
// coordinates where (0, 0) is the symbol center and ±1 is the half-extent
// of the symbol's bounding box. This matches the existing convention in
// symbols.cpp where `r = size_px * 0.5f` and every shape is computed as a
// fraction of r — so a stored point of (0.5, -0.25) renders at
// (sx + 0.5 * r, sy - 0.25 * r) on screen.
//
// Persistence
// -----------
// Libraries are loaded from / saved to JSON via the existing f4-json
// library (zero new deps). The schema is intentionally flat and
// forward-compatible — unknown keys are skipped on read so future
// extensions (stroke color per primitive, hole polygons, etc.) don't
// break older builds.
//
//   {
//     "version": 1,
//     "symbols": [
//       {
//         "key": "example_square",
//         "display_name": "Square",
//         "category": "example",
//         "description": "A simple filled square",
//         "polylines": [
//           { "width": 1.0, "closed": false,
//             "points": [ {"x": -0.5, "y": -0.5}, {"x": 0.5, "y": 0.5} ] }
//         ],
//         "polygons": [
//           { "filled": true,
//             "points": [ {"x": -0.5, "y": -0.5}, {"x": 0.5, "y": -0.5},
//                         {"x": 0.5, "y": 0.5},  {"x": -0.5, "y": 0.5} ] }
//         ]
//       }
//     ]
//   }
//
// Rendering
// ---------
// Two render paths mirror the existing symbols.hpp API:
//   draw_library_symbol(ImDrawList*, ...) — ImGui draw list (panels, legends)
//   draw_library_symbol(...)              — raylib direct (canvas)
// Both take the library + key + center + size_px + fill/outline colors,
// look up the definition, and walk its polylines + polygons. Polylines
// use DrawLineEx / AddPolyline; filled polygons use DrawTriangleFan /
// AddConvexPolyFilled; outline polygons use DrawTriangleLines /
// AddPolyline with closed=true.
//
// FUTURE: the eventual refactor of symbols.cpp will replace the hard-coded
// switch in draw_symbol() with a lookup into a loaded SymbolLibrary,
// falling back to the existing procedural shapes when a key isn't found.
// This header is the seam for that refactor — the data model + render
// helpers are designed to be consumable directly by symbols.cpp without
// further changes.

#pragma once

#include <f4/renderer/symbols.hpp>  // for RlColor (POD struct of 4 ubytes)

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

struct ImDrawList;
struct ImVec2;

namespace f4::renderer {

// ---------------------------------------------------------------------------
// Data model — pure value types, no raylib/imgui deps in this header.
// ---------------------------------------------------------------------------

/// A single 2D point in normalized symbol space.
/// (0, 0) = symbol center. ±1 = half-extent of the symbol's bounding box.
/// Values outside [-1, +1] are permitted (symbols can draw slightly past
/// their nominal extent — e.g. the existing ObjAirbase draws its
/// aerodrome circle at radius 0.75 of r, but the Unit* frames draw
/// markers at -1.5 of r above the frame).
struct SymbolPoint {
    float x = 0.0f;
    float y = 0.0f;
};

/// An open or closed polyline (line strip).
/// `width` is in pixels at the reference size_px (NOT normalized) so
/// lines stay visually consistent across symbol sizes. `closed = true`
/// connects the last point back to the first.
struct SymbolPolyline {
    std::vector<SymbolPoint> points;
    float  width  = 1.0f;
    bool   closed = false;
};

/// A filled or outline polygon.
/// `filled = true` renders as a filled shape; `filled = false` renders
/// as an outline only. The polygon is assumed convex for filled rendering
/// (raylib's DrawTriangleFan + ImGui's AddConvexPolyFilled both require
/// convexity). Outline rendering works for any simple polygon.
struct SymbolPolygon {
    std::vector<SymbolPoint> points;
    bool filled = true;
};

/// One complete symbol definition.
/// `key` is the dictionary lookup string (e.g. "obj_airbase").
/// `display_name` is the human-readable label shown in the editor.
/// `category` is an optional grouping ("objective", "unit", "example", ...).
/// `description` is a free-form note shown in the editor.
struct SymbolDefinition {
    std::string key;
    std::string display_name;
    std::string category;
    std::string description;
    std::vector<SymbolPolyline> polylines;
    std::vector<SymbolPolygon>  polygons;
};

/// A library of symbol definitions, keyed by `key`.
/// Maintains insertion order for stable iteration in the editor's
/// library browser. `find()` returns a pointer into the vector (so
/// it's stable across non-mutating operations); `add_or_replace()`
/// preserves order if updating an existing key, appends otherwise.
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
    /// false if the key wasn't present.
    bool erase(const std::string& key) noexcept;

    /// Number of definitions.
    [[nodiscard]] std::size_t size() const noexcept { return symbols_.size(); }
    [[nodiscard]] bool empty() const noexcept { return symbols_.empty(); }

    /// Read-only access to the underlying vector (for iteration).
    [[nodiscard]] const std::vector<SymbolDefinition>& symbols() const noexcept { return symbols_; }

    /// Mutable access (for in-place edits by the editor). Use carefully —
    /// the editor is responsible for keeping `key` unique if it mutates
    /// definitions directly.
    [[nodiscard]] std::vector<SymbolDefinition>& mutable_symbols() noexcept { return symbols_; }

private:
    std::vector<SymbolDefinition> symbols_;
};

// ---------------------------------------------------------------------------
// JSON I/O — implemented in symbol_library.cpp using f4-json.
// Throws std::runtime_error on I/O or parse failure.
// ---------------------------------------------------------------------------

/// Load a SymbolLibrary from a JSON file on disk.
/// Throws std::runtime_error if the file can't be opened, or if the JSON
/// is malformed. Unknown fields are skipped (forward-compat).
[[nodiscard]] SymbolLibrary load_symbol_library(const std::filesystem::path& path);

/// Parse a SymbolLibrary from an in-memory JSON string.
/// Useful for unit tests and for embedding a default library as a literal.
/// Not marked [[nodiscard]] because EXPECT_THROW macros in tests
/// intentionally discard the return value when checking error paths.
SymbolLibrary load_symbol_library_from_string(const std::string& json);

/// Serialize a SymbolLibrary to an in-memory JSON string.
/// Pretty-printed with 2-space indentation for human readability and
/// git-friendly diffs.
[[nodiscard]] std::string symbol_library_to_json(const SymbolLibrary& lib);

/// Save a SymbolLibrary to a JSON file on disk.
/// Throws std::runtime_error on I/O failure. The file is overwritten if
/// it already exists.
void save_symbol_library(const SymbolLibrary& lib,
                          const std::filesystem::path& path);

/// Return a small library with 3 trivial example symbols (square,
/// triangle, diamond). Used by the Symbol Creator tool as the initial
/// content on first launch, and by unit tests as a known-good fixture.
[[nodiscard]] SymbolLibrary make_default_symbol_library();

// ---------------------------------------------------------------------------
// Rendering — mirrors the existing symbols.hpp API.
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
