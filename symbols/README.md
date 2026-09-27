# Map symbols — the SVG authoring directory

Every `.svg` in this directory is loaded at viewer startup and REPLACES
the symbol whose key matches the filename stem: `obj_airbase.svg`
redefines `obj_airbase`, `unit_armor.svg` redefines `unit_armor`. A file
with a new stem adds a new key (rendered only when something references
that key — the campaign-icon wiring is the follow-on).

## The workflow

1. Export (or copy) the symbol you want to touch:
   `f4-world-viewer --export-symbols symbols` writes every loaded symbol
   as `<key>.svg` here. All 75 are committed, so you can usually just
   edit in place.
2. Edit the file in Inkscape (or any SVG editor that keeps the subset:
   paths, rects, circles, ellipses, lines, polylines, polygons; group
   transforms; no filters, masks, clip-paths, CSS classes, or opacity —
   those fail the import loudly).
3. Launch the viewer (or View > Reload symbol library). The status bar
   reports how many corpus symbols and SVG overrides loaded; broken files
   are named in the load errors and skipped — they never blank the map.

## Color rules

Symbols must not carry absolute colors — they render in the owning
team's palette. Use the color-role convention:

- `currentColor` fill  → the team fill color (opaque)
- `fill-opacity="0.85"` → the team fill at 85% (fill_blend; overlaps read
  as translucency, matching the procedural vocabulary)
- plain black/white stroke or fill → the contrast outline

The exporter writes these as `currentColor` plus `data-color-role`
attributes; Inkscape preserves unknown `data-*` attributes on round-trip.

## Geometry conventions

- Coordinates live in `viewBox="-1 -1 2 2"`: (0,0) is the symbol center,
  ±1 the half-extent. The renderer scales this box to the icon size.
- Holes are subpaths of the same `<path>` (evenodd fill): the donut in
  `obj_harts` is the worked example.
- Unit frames (`frame_*`) and glyphs (`glyph_*`) are separate; the
  composed `unit_*` symbols already have their frame baked in.
