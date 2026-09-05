# Icon corpus

Authored SVGs for the SymbolLibrary pipeline (subset contract in
`f4-renderer/include/f4/renderer/svg_import.hpp`). One icon per file;
the filename stem is the dictionary key.

## Two-tone painting

Every icon is authored black-on-white (MIL-STD-2525 convention). Both
placeholder paints are RUNTIME-SUBSTITUTED with the owning team's
palette — nothing in these files is a real color:

- white (`#ffffff` / `white`) → Fill role → the team's PRIMARY
  (icon background, frame fill)
- black (`#000000` / `black`) → Outline role → the team's SECONDARY
  (glyph strokes, contrast outlines)
- `currentColor` → Fill role (equivalent to white; the exporter writes
  Fill-role geometry this way)
- `data-color-role="fill|fill_blend|outline"` overrides the mapping
  (e.g. `unit_*` frame fills use `fill_blend` for translucent overlap)

The palettes live in `f4-world-viewer/src/viewer_state.hpp`
(`team_palette_for_owner`).

## Directories of the vocabulary

- `obj_*.svg` — objective/feature icons (airbase, bridge, SAM site, ...).
  Keyed by `key_for_objective_type()` in `entity_render.cpp`.
- `unit_*.svg` — COMPOSITE unit icons, one per unit type: the frame and
  the type glyph authored together (e.g. `unit_armor` = battalion
  rectangle + tank ellipse). Keyed by `unit_symbol_key_for()`. The
  frame/glyph runtime split is retired — no glyph is reused across
  frames, so each unit type owns exactly one file.
- `frame_*.svg` — bare frames. Still live: they are the icon for
  Flight/Package (which have no subtype glyph) and the fallback for any
  mapped class with an unknown subtype.
- `glyph_*.svg` — bare type glyphs. Not drawn on the map anymore; kept
  as the type-icon vocabulary for the disaggregated aircraft/vehicle
  markers (the per-type rotating icons land in the deaggregation
  tranche) and as the authoring source the `unit_*` composites were
  built from.
