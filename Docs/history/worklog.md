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

---
Task ID: NAMES-1
Agent: main
Task: Integrate the nameid table — the Event Log printed "#NNN" for
every small front objective because nothing resolved
ObjectivePriorityComponent::nameid to a string.

Work Log:
- Found the mechanism in the FreeFalcon source the repo already vendors
  as reference (E:/Code/FreeFalcon-master CAMPAIGN/CAMPLIB/Name.cpp):
  LoadNames reads <theater>.idx = short NameEntries + short
  NameIndex[NameEntries]; LoadNameStream reads <theater>.wch = the raw
  string stream sized NameIndex[NameEntries-1]; ReadNameString(sid) =
  stream[NameIndex[sid] .. NameIndex[sid+1]). Entry 0 is "Nowhere".
  The vanilla install carries the pair at campaign/SAVE/korea.idx +
  korea.wch (the .ICD in terrdata/objects is 162 bytes of floats — a
  red herring; Strings.wch is UI sentences, not geography).
- Verified against the real install before writing code: korea.idx has
  1632 entries; nameid 744 (the village TestCamp's capture events
  report as #135's row) resolves to "Posong-ni".
- scripts/export_names.py rewrites the pair as
  Data/Theater/korea/names.json (f4.theater.names/1, 1631 names —
  count-1 usable, the last offset is the stream length). Exported from
  the user's install and committed; Data/manifest.json regenerated
  over it (--check green).
- theater_names.hpp/cpp (pure, f4-json, unit-tested): parse the
  document + a bounds-checked theater_name_for_id. viewer_app.cpp
  loads it at startup next to the symbol library (same Data/ upward
  walk); load errors land in symbol_load_errors/status.
- objective_display_name (viewer_state.hpp) gains the hop: class_name
  → theater_names[nameid] → "". Everything that shares the resolver
  names things now: the Event Log rows, the session feed, the ATO
  target column (label only — click/selection semantics untouched),
  the inspector's objective header + Name row.

Squadron/aircraft naming (asked alongside): the name table DOES carry
squadron name strings (SquadronUIInfo::name_id), but TestCamp's save
references name_ids 3817+ while the vanilla install's korea.idx tops
out at 1632 — those bounds-check to "" (the save was made under a
larger table). And AIRCRAFT TYPE names ("F-16C") are NOT name-table
data at all: they come from the class table (FALCON4.ct → UCD) via the
squadron's d_index/vehicle row — the session squadron entities carry
UnitCoreComponent and the viewer already has class-table plumbing
(class_table_json_path), so a "Type:" row in the live-aircraft
inspector is a small, separate follow-on.

Tests: test_theater_names 3/3; f4-world-viewer ctest 98/98; headless
330 s session screenshot shows the Event Log naming captured
objectives.

---
Task ID: CT-NAMES-1
Agent: main
Task: The class-table work the user green-lit after NAMES-1 — aircraft
TYPE names ("F-16C") in the inspector and the flights table.

Work Log:
- Traced the naming data sources to their forks: the nameid table
  (NAMES-1) carries OBJECTIVE and squadron-name strings; aircraft TYPE
  names live in the class-table chain — CT entity_type row → UCD/VCD
  data row → name. The runtime ClassTable (falcon4.ct.json) parses no
  names; the names ride CAMP-SCALE-1's f4.theater.tables/1 document
  (cam2json --emit-tables), whose runtime reader (f4-world's
  TheaterTables) existed tested but loaded for nothing display-side.
- Exported Data/Theater/korea/tables.json from the install (cam2json
  TestCamp.cam --theater-data <install>/terrdata/objects --emit-tables;
  296 UCD + 285 VCD + 203 WCD rows); committed with the manifest
  regenerated over it (--check green).
- resolve_entity_type_name (f4-world/theater_tables, the
  resolve_countermeasures pattern): VEHICLE rows name themselves; UNIT
  rows resolve their FIRST vehicle_type → VCD name ("F-16C") and fall
  back to the unit-class name ("Airlift") when the group link fails.
  Empirically pinned against the exported tables: squadron CT row 481 →
  UCD 381 "Attack" → vehicle 183 "M-9 ACE" — a ground unit, but the
  chain closes end to end.
- Viewer: Impl gains class_table (loaded from the committed
  Data/Classes/falcon4.ct.json — the session's own ct_ is private) +
  theater_tables, both loaded at startup fail-soft; unit_type_name(vu)
  = session unit_id_map → UnitCoreComponent.class_table_index →
  resolve_entity_type_name. The live-aircraft inspector's Type: row
  prefers the flight's own row (org->flight_vu) then the squadron's;
  the selection header names live aircraft; the Campaign Session
  window's flights table gains a type column (renders in headless
  screenshots — that's the acceptance proof).

Deliberately NOT done: wiring the session's theater_tables OPTION (the
CAMP-SCALE-1 supply chain stays as-is — this tranche is display-only);
squadron UI names via SquadronUIInfo::name_id (TestCamp's save is OOB
for the vanilla table — NAMES-1's bounds check covers it if ever).

Tests: test_theater_tables 5/5 (the new ResolveEntityTypeNameChain
covers vehicle-self-name, unit→vehicle, unit-class fallback, OOR,
empty); f4-world 405/405; f4-world-viewer 99/99.

---
Task ID: SVG-TOLERANCE-1
Agent: main
Task: Two user-visible failures after the naming tranches: (1) Inkscape
re-saves of symbols (obj_intersection.svg, obj_bridge.svg) "no longer
open", (2) "still no names for units or objectives".

Work Log:
- Read the user's actual re-saves: paints moved into CSS style=""
  attributes (fill:#333333 / stroke:#b3b3b3 — Inkscape's default
  palette), vector-effect:non-scaling-stroke hairlines at
  stroke-width:1, <defs>/<sodipodi:namedview>/<metadata> blocks,
  namespaced attributes everywhere. The importer failed on ALL of it:
  style was in is_dangerous_attr, defs/namedview hit the unsupported-
  element branch, #333333 wasn't a parseable paint.
- svg_import.cpp: apply_style_attr parses the style property list with
  CSS precedence over presentation attributes (two-pass so
  vector-effect sets the px interpretation before stroke-width reads);
  parse_paint maps hex colors by Rec.601 luminance (<1/3 black,
  >2/3 white, mid-gray fails as ambiguous — Inkscape's #333333/#b3b3b3
  both pass); non-scaling px convert at the kSymbolReferenceSizePx
  convention (1 px @ 64 px = 0.03125 viewBox units — the exporter's
  own hairline); defs/metadata/namespaced elements skip whole. Header
  contract updated. Four new tests (InkscapeResaveImports,
  StyleAttributeOverridesPresentation, GrayPaintsMapByLuminance,
  UnknownStylePropertyFailsByName).
- Verified against the REAL files: --export-symbols round-trip now
  carries the edited 14-segment geometry (the corpus's was 4 paths) —
  the overrides apply.
- Names not showing: the asset loaders resolved Data/ with a 2-level
  CWD walk — a viewer launched deeper than Build/ silently ran with
  empty tables. resolve_data_path adds the F4_SOURCE_DIR baked path;
  both loaders print what they loaded (or why not) to stdout. Verified
  from the exe directory: "theater names: 1631", "theater tables: 296
  units / 285 vehicles / 203 weapons".
- Units never had names wired (the "units" half of the report):
  the inspector's unit branch now resolves the instance name
  (PropertyBag name_id through the theater table), the CT type name
  (resolve_entity_type_name), and shows both plus a Type name row; the
  header falls back class_name → type name → "Unit".

Tests: f4-renderer 15/15 (svg 20/20), f4-world 405/405,
f4-world-viewer 99/99; headless 330 s session from the exe directory
shows the Event Log naming captures (Iksan, Weondang, Chigung-ni...)
and the flights table type column filled.
