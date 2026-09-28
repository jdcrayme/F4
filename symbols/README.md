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
team's palette. The rule is mechanical: **a fill paints the background
(the team color); a stroke paints the foreground (the contrast
color)**. Fills and strokes come from the file exactly as drawn —
nothing is added or removed by the importer or the renderer.

- Any fill (including `currentColor` and editor grays) → the team fill
  color. `fill="none"` → no fill.
- `stroke="currentColor"` → a team-colored stroke (the dashed-border
  convention); any other stroke paint → the contrast foreground.
  `stroke="none"` or no stroke → no stroke.
- Contrast GLYPHS inside a team-colored frame (the artillery dot, the
  helo silhouette) use near-black/near-white fills and carry an
  explicit `data-color-role="outline"` — the exporter writes it; keep
  it when editing by hand. `data-color-role="fill|fill_blend|outline"`
  overrides the mapping on any shape.
- A mid-gray (neither near-black nor near-white) fails the import as
  ambiguous.
- `fill-opacity="0.85"` → the team fill at 85% (fill_blend; overlaps
  read as translucency).

The exporter writes these as `currentColor` plus `data-color-role`
attributes; Inkscape preserves unknown `data-*` attributes on
round-trip — and Inkscape re-saves import: `style=""` attributes, gray
paints, and editor blocks (`<defs>`, `<sodipodi:namedview>`) are all
tolerated.

## Geometry conventions

- Coordinates live in `viewBox="-1 -1 2 2"`: (0,0) is the symbol center,
  ±1 the half-extent. The renderer scales this box to the icon size.
- Holes are subpaths of the same `<path>` (evenodd fill): the donut in
  `obj_harts` is the worked example.
- Unit frames (`frame_*`) and glyphs (`glyph_*`) are separate; the
  composed `unit_*` symbols already have their frame baked in.
