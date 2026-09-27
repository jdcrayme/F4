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


---
Task ID: EVENT-LOG-1
Agent: main
Task: The world viewer needed the original game's running theater log —
the animated yellow circles (capture_markers: decaying rings on freshly
captured objectives) show THAT something happened but never say WHAT.

Work Log:
- Found the machinery already in place: the CAMP-HOST-2 event stream is
  armed at adopt (all kinds) and drained every frame into
  `session_events` (capped 100) in refresh_session_snapshot; the
  Campaign Session window's "Events" collapsing header rendered a
  14-row tail. But its formatter's `default: return false` silently
  dropped the pilot_assigned/pilot_lost/pilot_recovered/roe_changed/
  slot_denied families (the comment deferred them to "when the war
  books grow a face" — the books landed as CAMP-DOM-3/4 and nobody
  circled back).
- New `event_log.hpp/cpp` (f4-world-viewer/src, deliberately
  ImGui-free): EventLogRow = {arrived_wall, abs_t (epoch + envelope t),
  team slot (0xFF = teamless), frozen label[176]}.
  event_log_append formats the row AT ARRIVAL — the objective-name
  resolver (new Impl::objective_display_name on the render-plane seam)
  runs while the session is alive, so the log reads back after the
  session stops. Store = deque capped 2000, newest last, cleared at
  adopt.
- The window (ViewerApp::draw_event_log_view, in
  campaign_session_view.cpp): substring filter, Follow tail-pin
  (SetScrollHereY at the bottom), Clear, per-row team color dot from
  color_for_owner (the map palette, one axis with the canvas).
  Auto-opens with the session start (show_event_log_window), Windows
  menu item alongside Campaign Session.
- The compact feed in the Campaign Session window now delegates to the
  SAME format_campaign_event_label — it gains the personnel/scheduling
  lines for free; its local 95-line lambdas (event_time +
  format_event_label) deleted.
- Formatting quirks worth knowing: kill lines carry "[killer_sq vs
  victim_sq]" (the feed's old line didn't name squadrons); slot_denied
  renders the airbase name through the objective map (falls back to
  "#id"); roe_changed renders level + scope ("RoE TIGHT: team 1").

Found + fixed en route: objective_display_name's first cut resolved
through the SESSION engine's objective_id_map — which only carries
mission targets (the spawner feeds it), so every ground capture printed
"#NNN". The POP map (PopulatedWorld.objective_id_map, populated by
populate_world) carries EVERY world objective; the resolver now checks
pop first and the session map second (Impl::objective_entity returns
the entity + which world owns it). The canvas capture rings carried
the SAME latent gap — they resolved through the session map only, so
the yellow rings silently never drew for ground captures (only for
air-mission targets); the marker path now shares objective_entity.
- Name resolution floor: small front objectives print as #<vu> because
  class_name is EMPTY for them in the parsed data — a temporary
  [PROBE] stderr instrument proved ids resolve in the pop map but the
  objective rows carry no name string. The theater name table behind
  ObjectivePriorityComponent::nameid is loaded NOWHERE in the repo
  (only the inspector prints the raw nameid). Resolving it is a data-
  pipeline tranche of its own, deliberately left.

Deliberately NOT done: no per-kind suppression checkboxes (the
substring filter covers "hide the tasking cycles" by typing a kind
name); no persistence across app restarts (the journal is the wire's
job, not the viewer's).

Tests: test_event_log 7/7 (every-kind-formats across all 15 kinds, name
resolution + #id fallback, personnel faces, envelope accessors, cap/
order, case-insensitive filter); f4-world-viewer ctest 95/95; headless
--session --play --smoke-seconds 330 screenshot shows the log filling
with tasking/mission lines as the first ATM cycle crosses.
