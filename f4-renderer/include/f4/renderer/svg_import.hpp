// f4-renderer/include/f4/renderer/svg_import.hpp
//
// PUBLIC HEADER — SVG import/export for SymbolLibrary definitions.
//
// SVG is the AUTHORING format (editable in Inkscape/Illustrator/Figma,
// readily generated and edited by AI tools); the SymbolLibrary is the
// RUNTIME format. Import flattens everything once at load time; nothing
// SVG-related happens per frame.
//
// Supported subset
// ----------------
//   root:      <svg> with viewBox (width/height ignored). <title> becomes
//              display_name, <desc> becomes description.
//   structure: <g> with transform="translate|scale|rotate|matrix(...)";
//              nesting composes. Presentation attributes (fill, stroke,
//              stroke-width, fill-rule) inherit down the tree.
//   shapes:    path (commands M m L l H h V v C c S s Q q T t A a Z z),
//              rect (incl. rounded rx/ry), circle, ellipse, line,
//              polyline, polygon.
//   fill:      "currentColor" -> SymbolColorRole::Fill (also the default
//              when the attribute is absent — a deliberate deviation from
//              SVG's black default, for team-colored tactical symbols),
//              "none" -> outline-only,
//              "#000000"/"black" -> Outline role,
//              "#ffffff"/"white" -> Fill role.
//              Two-tone authoring contract: icons are drawn black-on-white
//              (the MIL-STD-2525 convention) and BOTH placeholder paints
//              are replaced at draw time by the owning team's palette —
//              white areas become the team's PRIMARY color (the icon
//              background / frame fill), black areas become the team's
//              SECONDARY color (the glyph / contrast outline). See
//              f4-world-viewer's team_palette_for_owner() for the palette.
//              data-color-role="fill|fill_blend|outline" overrides the
//              paint-to-role mapping (this is how the exporter round-trips
//              all three roles).
//   stroke:    "none" or any fill value above; stroked outlines become
//              polylines. Stroked FILLED shapes keep the model's built-in
//              1px contrast outline (stroke-width on filled shapes is not
//              honored).
//   fill-rule: "nonzero" (default) and "evenodd" — for either rule, a
//              subpath contained inside another becomes a hole.
//
// NOT supported — import throws std::runtime_error NAMING the feature:
//   gradients, patterns, filters, masks, clip-paths, opacity, <style>/
//   class-based CSS, <text>, <image>, <use>, <defs>, <script>, skewX/
//   skewY transforms, and any element/attribute outside the list above.
//   Failing loudly keeps "limited SVG" honest: a symbol that imports is
//   a symbol that renders.
//
// Geometry conventions
// --------------------
//   Coordinates map from the viewBox into normalized symbol space
//   [-1, +1] with UNIFORM scale 1 / max(halfWidth, halfHeight) (aspect
//   preserved, viewBox center at the origin). SVG's y-down axis is kept
//   as-is — the symbol model is screen-like (negative y renders above
//   the center). Curves flatten at import time: bezier/arc segments
//   sample 16 points, circles/ellipses 32, rounded-rect corners 4.
//
//   Stroke widths: SymbolPolyline::width is absolute pixels at render
//   time; SVG stroke-width is in viewBox units. Import/export convert
//   through a 64 px reference extent (kSymbolReferenceSizePx), using the
//   ROOT viewBox scale only (a scaled <g> scales geometry but not the
//   stored stroke width — a documented subset simplification).

#pragma once

#include <f4/renderer/symbol_library.hpp>

#include <filesystem>
#include <string>

namespace f4::renderer {

/// Reference render extent for converting stroke widths between the
/// symbol model (absolute pixels at render time) and SVG (viewBox units).
inline constexpr float kSymbolReferenceSizePx = 64.0f;

/// Parse `svg` (a document in the subset above) into a SymbolDefinition
/// with the given key. Fill caches are refreshed. Throws
/// std::runtime_error naming the offending feature on out-of-subset
/// input, or a parse error description on malformed XML.
[[nodiscard]] SymbolDefinition import_symbol_from_svg_string(
    const std::string& svg, const std::string& key);

/// Load one .svg file; the two-arg overload takes the key explicitly,
/// the one-arg overload derives it from the filename stem.
[[nodiscard]] SymbolDefinition import_symbol_from_svg_file(
    const std::filesystem::path& path, const std::string& key);
[[nodiscard]] SymbolDefinition import_symbol_from_svg_file(
    const std::filesystem::path& path);

/// Serialize a definition back into an SVG in the subset above
/// (viewBox="-1 -1 2 2"). Holes round-trip as subpaths of the same
/// <path>; color roles via currentColor + data-color-role.
[[nodiscard]] std::string symbol_to_svg(const SymbolDefinition& def);

/// Write symbol_to_svg(def) to `path`. Throws std::runtime_error on I/O
/// failure.
void save_symbol_as_svg(const SymbolDefinition& def,
                        const std::filesystem::path& path);

} // namespace f4::renderer
