---
Task ID: SYMBOL-SVG-2-FILL
Agent: main
Task: The user-visible regression from SYMBOL-SVG-2's first live run —
every map icon rendered unfilled and in the dim outline color instead of
the primary/secondary faction colors.

Work Log:
- Reproduced headlessly: testcamp.world.json screenshot showed hollow
  magenta outlines where filled team-colored icons belonged.
- Pixel probe (test_symbol_draw.cpp, new GPU-context test): drew
  draw_library_symbol over black and read pixels. Single-pixel probes
  proved treacherous — obj_city's building seams pass exactly through
  the symbol center, and glyph strokes cover the fighter's frame
  interior at plausible probe points — so the shipped assertion counts
  FILL-DOMINANT pixels across the whole render (> 400 at 64 px;
  fill-less renders leave ~0). LoadImageFromScreen also proved racy
  (reads must happen inside the frame); the test uses a render texture
  + LoadImageFromTexture with a mirrored row (FBO bottom-left origin).
- Root cause: draw_library_symbol's convex-fill path called raylib's
  DrawTriangleFan, which silently rasterized NOTHING for these vertex
  lists (the exact mechanism inside raylib 5.0 was not pinned down —
  the path was never visually exercised anywhere: the Creator
  previewed through ImGui's AddConvexPolyFilled). Replaced with an
  explicit centroid fan of DrawTriangle calls. The concave/holed path
  already used DrawTriangle over the earcut cache.
- Verified: probe counts green fills for corpus, viewer-merged
  (corpus + symbols/ overrides), and unit kinds; headless screenshot
  shows filled team-colored icons across the theater.
- The earlier "MSVC numerics"-adjacent theories are dead — this was a
  dead draw call, full stop.

Deliberately NOT done: the legend panel still draws procedurally via
draw_symbol_imgui (the library imgui path exists; switching the legend
is a follow-on when the procedural vocabulary deletes).

Tests: test_symbol_draw 3/3; f4-renderer + f4-world-viewer ctest
103/103; headless screenshot visually confirmed.

