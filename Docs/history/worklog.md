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

---
Task ID: ATO-START-1
Agent: main
Task: "Running the campaign from save0-2 doesn't ever generate any ATO
missions" + two symbol-rendering bugs in the Inkscape re-saves (the
bridge's filled path not drawn; the intersection's fill-only paths
drawn with a stroke).

Work Log:
- ATO: reproduced headlessly from the install's stock save1 — sim
  1681s, "cycles 0 missions 0, next tasking cycle in 1:59". Root
  cause: Campaign::tick fires the air tasking when
  next_cycle_ + air_task_cycle_sec <= clock_ with next_cycle_ starting
  at 0 — the first cycle lands at 1800 WAR seconds (the ground war's
  own orders cycle fires at clock 0; the air ladder's didn't). A
  loaded stock save carries no ATO in this engine (the wire's
  pre-planned missions are not decoded), so every install-loaded
  session opened with an empty ATO for ~30 war-minutes — longer than
  most runs. TestCamp runs that showed missions had simply crossed
  the boundary (testcamp.world.json is save0 lineage — same epoch,
  same ladder).
- Fix: Campaign::run_initial_tasking_cycle() — one cycle at the
  current clock, counted in cycles_fired_, next_cycle_ untouched (the
  scheduled cadence is unchanged). Gated behind
  CampaignSessionOptions::initial_tasking_cycle (default false — the
  byte-pinned QC ledgers never see it); the viewer sets it. save1
  verification: 30 wall-seconds in, "cycles 1 missions 100 routes 20",
  flights table populated, the initial cycle's tasking_cycle event in
  the log at D375 00:00:01.
- Symbols: the user's diagnosis was exactly right — "overwrite the
  fill on every object that has a fill with the background, the
  stroke on every object that has a stroke with the foreground, and
  don't add or remove either". Two violations found: (1)
  draw_library_symbol (raylib AND ImGui paths) drew an unconditional
  1px outline_col outline on every polygon — the intersection bars'
  phantom stroke; now outlines render only for outline-only shapes
  (filled=false hover/selection, unfilled polygons). (2) The path
  emitter's if/else dropped the STROKE of any filled+stroked path
  (the bridge road), and paint_role mapped its #333333 fill to the
  contrast color (≈invisible on the map); now a shape with both
  emits both, fill_role() maps ANY fill to the background (explicit
  data-color-role still wins — the corpus's contrast glyphs carry
  it), stroke_role() keeps currentColor strokes team-colored (the
  dashed-border convention) and editor colors contrast.
- Audited the corpus first: every black/white fill in symbols/*.svg
  carries explicit data-color-role="outline" (16 of them), and 8
  symbols use stroke="currentColor" for team-colored dashes — the new
  rule regresses nothing.

Tests: InitialCyclePlansAtClockZero (tick tests 10/10); GrayPaints
updated to the both-emit rule (svg 20/20); f4-campaign 372,
f4-simulation 387, f4-renderer 15, f4-world 405, f4-world-viewer 99 —
all green.

---
Task ID: SVG-HAIRLINE-1
Agent: main
Task: "unit_fighter.svg doesn't render correctly — the closed poly of
the frame border renders as perpendicular lines."

Work Log:
- Reproduced with the real importer: compiled svg_import.cpp (plus
  pugixml, SymbolLibrary stubs) into a dump driver and imported the
  checked-in symbols/*.svg overrides. unit_fighter's dome (the closed
  squadron frame border) imported with correct geometry — 34 points,
  closed — but width=32 px (half the 64 px reference extent); fighter's
  third glyph stroke likewise. A 32 px stroke over the dome's ~7-15 px
  arc segments renders as fat bars perpendicular to the curve — the
  report's "perpendicular lines".
- Root cause: Inkscape's hairline pen writes vector-effect:
  non-scaling-stroke + -inkscape-stroke:hairline with NO stroke-width.
  Style's default stroke_width (1.0, viewBox units) then fell through
  add_stroke's stroke_width * vb_scale * (kSymbolReferenceSizePx * 0.5)
  math = 32 px. The existing InkscapeResaveImports test only pinned the
  explicit stroke-width:1 case.
- Corpus scan: exactly 3 widthless-hairline paths in 75 overrides —
  unit_fighter.svg (2) and unit_transport.svg (1, the same dome).
- Fix in svg_import.cpp: Style gains stroke_width_set (set by BOTH the
  presentation and style stroke-width branches); the vector-effect
  branch seeds stroke_width = 1/(kSymbolReferenceSizePx*0.5) (1 screen
  px) when non_scaling_px turns on and no width has arrived.
  apply_style_attr's existing vector-effect-first ordering makes a
  later style width overwrite the seed; presentation widths (applied
  before the style attr) and inherited widths are respected via the
  flag. Header subset contract updated (svg_import.hpp).
- Verification: corpus-wide before/after import diff — all 75 import,
  geometry identical, exactly 3 width changes 32 -> 1, max width now
  2.5 px; pinned regressions hold (viewBox 0.1 -> 3.2 px, explicit
  non-scaling 1 -> 1 px). Rendered before/after eye-views of the dome:
  the 32 px blob collapses to the thin closed arc matching the
  browser's rendering of the source SVG.
- Also fixed unit_fighter.svg's title/desc/RDF title ("Transport" —
  the clone the fighter was redrawn from) to Fighter/"Squadron +
  fighter silhouette", matching the corpus JSON description (feeds
  display_name on import).
- Tests added to test_svg_import.cpp: InkscapeHairlineWithoutStroke
  WidthIsOnePx, HairlineRespectsAnInheritedExplicitWidth.

Stage Summary:
- Hairline strokes (vector-effect without stroke-width) import as
  1 screen px; the squadron dome frame renders as a hairline closed
  arc again. Renderer-only change — no geometry, role, or exporter
  changes; the round-trip stays lossless (exporter writes explicit
  viewBox-unit widths).

---
Task ID: AVIONICS-1
Agent: main
Task: "Land the avionics plan's AVIONICS-1 tranche — the f4-avionics
scaffold (CMake target, the PilotInput/IAircraftState seam), the INS
(alignment SM, the stored heading/altitude/position chain, the drift
integral as a seeded deterministic walk keyed on the airframe's nav
data), and steerpoints (ranges/bearings read through the INS; the
drift-zero case byte-identical to raw positions)."

Work Log:
- Read the plan's §4 AVIONICS-1 done-when and the consumers' seams
  (f4-flight-api's IAircraftState, f4-geo's WorldPosition/BRA/to_bra,
  f4-state-machine's Builder) before writing anything.
- Library shape: header-only INTERFACE (the f4-geo discipline), namespace
  f4::avionics, umbrella f4/avionics/f4_avionics.hpp over ins.hpp +
  steerpoint.hpp. CMake links f4-flight-api, f4-geo, f4-state-machine and
  nothing else; root CMakeLists gained the ordered add_subdirectory (after
  f4-flight-api) and the runtime-side f4_mark_side row.
- The alignment SM is a PURE transition table (make_ins_machine(), no
  captures) — Off→Aligning on PowerOn, →Aligned on AlignTimer, →Off on
  Shutdown from both live states. Side effects live in InsUnit::update()
  (the stall-SM polling→event bridge): the align clock accrues only while
  the fed truth is on_ground() (the plan's "ground clock"), AlignTimer is
  sent when it passes align_time_s, and completion zeroes the chain and
  seeds the walk.
- The drift integral: a bounded rate walk — per update each axis's error
  rate takes one clamped uniform step (fixed sample order east, north,
  up, heading) and the position error integrates the rate. Seed = config
  seed, else FNV-1a over nav_data_key; nav_data_age_days scales the step
  and mixes into the seed. The sampler is a fully specified splitmix64 +
  53-bit uniform — deliberately NOT std::mt19937/std:: distributions,
  whose sequences are implementation-defined — so the same seed fed the
  same update stream is byte-identical cross-platform. Clamps bound the
  rate; sigma scales with age (old data saturates the limit, never
  exceeds it).
- Scope decision recorded in the header and the plan: v1 is an ERROR
  MODEL over the IAircraftState seam (believed = truth + drift), not an
  open-loop acceleration integrator — every avionics consumer reads the
  believed chain relative to steerpoints, and the error model gives that
  read deterministically without inventing an Euler-integration
  divergence no consumer asked for. The strapdown integrator is a named
  later tranche behind the same interface.
- Steerpoints: Steerpoint/SteerpointSequence (loud out_of_range on
  empty/out-of-range; next() reports the end wall — overflight is the
  host's decision), to_steer() = f4-geo's to_bra over the supplied
  position (slant range, true bearing wrapped [0,2π)), steer_cue() (the
  HSI cue: signed error wrapped [−π,π], shortest-way flag, dead-astern
  pins right — pinned), plus current_steer()/current_steer_cue() reading
  through the INS.
- Boundary verifier: f4-avionics marked runtime side; configure with
  -DF4_ENFORCE_BOUNDARY=ON passes (all GUI targets off, CI's headless
  shape).
- Docs: AVIONICS_PLAN.md banner + the §4 AVIONICS-1 as-built note,
  Docs/README.md index row, ARCHITECTURE PROPOSAL §3 (mermaid node +
  edges, the summary table row, the target count 30→31), CHANGELOG entry.

Tests: 24/24 green (label f4-avionics; test_avionics_ins +
test_avionics_steer): the ground-clock align completes at exactly
align_time_s and freezes airborne; align_time 0 completes on the first
ground tick; shutdown discards the chain from both live states;
re-alignment resets the walk; determinism per seed AND per update stream
(same simulated time, different dt stream → different walk); keyed on
nav data (key and age both move it); drift stays inside the clamps; the
drift-zero twin compares equal to raw truth member-for-member through
200 ticks of moving truth (including the through-INS steer read and
cue); the cue pins (shortest way, dead astern right). f4-geo/f4-flight-
api/f4-state-machine suites re-run green after the root CMake change.

Stage Summary:
- f4-avionics exists: the avionics layer's substrate (INS + steerpoint
  nav) is landed, engine-agnostic and deterministic; AVIONICS-2 (the FCR
  page SM) is the next rung and consumes f4-sensors beside this scaffold.
- The v1 INS is an error model over the IAircraftState seam — the scope
  decision is written into ins.hpp's header comment and the plan's
  as-built note so the next tranche doesn't relitigate it.

---

Task ID: DATALINK-1
Agent: main
Task: "Close out AI_IMPLEMENTATION_PLAN §15 Step 13's sim-side remainder —
the scenario-mode datalink-node stamp, the host walk's liveness
discipline, and the named test_datalink_tiers suite — with the
twin/byte-identity contracts held."

Work Log:
- Mapped the landed f4-batch-2 host half (push_air_picture_'s node
  collection + bitmask fill + the gated handoff, the campaign stamps in
  campaign_bridge.cpp, the CombatConfig fields) and found the three
  gaps: the scenario "awacs" flag parsed but never consumed (the
  combat_bridge.hpp header claimed the stamp), test_datalink_tiers
  named in tests/CMakeLists.txt but absent, and the walk collecting
  nodes/masks with no killed check (the GCI-ghost the tranche's own
  comments warned about).
- The stamp: attach_combat_loadout adds AwacsComponent when
  ScenarioAircraft::awacs — unconditional, mirroring the campaign
  paths' mission_is_datalink stamps; the walk's gci_datalink gate
  stays the fidelity switch (a stamped node with the gate off is
  inert data).
- Liveness on both mask sides: node collection skips
  DamageStateComponent::killed entities (a dead AWACS/objective radar
  stops broadcasting the same walk; entities without the component —
  bare objectives — are alive by definition), and the mask fill gives
  killed contacts a 0 entry (the policy corpse early-out extended to
  the net leg) while never resolving aggregate flight VUs through the
  entity database (the first_aggregate_index boundary captured before
  the FID-5 feed appends).
- test_datalink_tiers.cpp (8 cases, label f4-simulation): the stamp +
  defaults, the no-nodes twin (gate on == gate off, TargetInfo
  member-for-member across sequential deterministic runs), the
  coverage commit (151 NM contact through the net alone,
  threat_target commits, radar leg provably dark), per-team bitmask
  isolation (blue node lights blue's leg and never red's, one run),
  node death, the corpse rule, the live horizon clamp (component
  mutation drops and re-lights the leg), and the ground-site arm
  (white-box bare-entity radar objective; arm off leaves the same
  world dark).
- Bug-catch verification: stashed the simulation.cpp liveness fix,
  rebuilt, ran the two liveness tests — both FAIL without the fix
  exactly as the plan's Step-13 test list predicts; popped, rebuilt,
  8/8 green.
- Docs: AI_IMPLEMENTATION_PLAN §Step 13 LANDED note rewritten (both
  halves, the as-built AwacsComponent shape with no station field);
  CHANGELOG DATALINK-1 entry; tests/CMakeLists.txt stale follow-up
  comment replaced (test_flight_persistence's FID-P0 pins had already
  landed inside test_fidelity_tiers §9).

Tests: test_datalink_tiers 8/8; neighbors green — test_passive_fusion
8/8, test_combat_integration 29/29, test_sensor_fidelity 7/7,
test_countermeasure_e2e 4/4, f4-ai test_datalink_net 15/15; boundary
verifier PASS at configure (headless shape, -DF4_ENFORCE_BOUNDARY=ON
shape). Full fast f4-simulation tier re-run for the walk change
(results in the session log; known pre-existing reds unchanged).

Stage Summary:
- Step 13 is CLOSED: the GCI datalink is now armed end to end from
  scenario JSON ("awacs" per-aircraft + "gci_datalink"/
  "gci_ground_sites" combat gates), through the campaign spawn paths,
  the single host walk, to the fusion's net leg — red/blue information
  asymmetry is live, and the picture decays the same tick a node dies.
- The gate-off byte-identity contract holds by construction (every
  change lives behind datalink_gate_on); the plan's v2 notes (operator
  lag decay window, beam-physics node geometry) stay named-not-built.

---

Task ID: AVIONICS-2
Agent: main
Task: "Land the avionics plan's AVIONICS-2 tranche — the FCR page model
(the radar page as an SM): the RWS/TWS/VS mode machine, the lock state
with the reference's lock rules, the renderer-facing RadarPageModel,
and the f4-sensors lock hand-off the fire-control gate already honors —
done when the page's lock drives the AI's MissileModule can_fire path
from page inputs in a scenario test."

Work Log:
- Mapped the sensors/AI surfaces first (the Explore pass): RadarSimComponent
  already owns RadarMode{Search,Track} + command_track/command_search with
  the reference's refusal rule and the auto-drop; the fire-control chain
  (live track -> RadarBackedDetectionPolicy radar leg -> MissileModule::
  should_fire) was already end-to-end. Design consequence, written into
  the plan's as-built note: AVIONICS-2 is a PAGE over those primitives —
  f4-sensors gained nothing, f4-ai gained nothing, the whole tranche is
  additive (zero sim-loop bytes; AI-only runs byte-identical by
  construction).
- fcr_page.hpp (header-only, namespace f4::avionics): FcrState/FcrEvent +
  make_fcr_machine() (pure table, the make_ins_machine discipline);
  FcrPageModel (InsUnit pattern) with power_on/off, select_rws/tws/vs,
  designate (refused page-Off; takes the radar's command_track answer —
  page lock == radar lock, no divergence), break_lock, and update()
  (pure snapshot recompute + exactly ONE write: the track-death mirror).
  FcrPageSnapshot/FcrSymbol: page-relative azimuth wrap [-pi,pi],
  elevation, slant range, signed closure (positive = closing), IFF,
  Established-or-Coasting flag, designated flag, the live scan frame;
  VS = velocity-only display semantics (closing-only symbols +
  velocity_only flag) — no new sensor physics. Symbols ascending
  entity_id; snapshot comparable (operator<=>) for the two-renderers
  golden rule.
- Wiring: f4-avionics links f4-sensors (beside f4-flight-api/f4-geo/
  f4-state-machine; still never f4-ai); umbrella updated; root boundary
  verifier re-run (PASS at configure).
- test_avionics_fcr.cpp (18 cases, label f4-avionics): the full mode
  table incl. refusals (double power-on, self-select no-ops,
  everything-from-Off), power-off clearing the lock from every live
  state, mode switches keeping the lock, the designate refusal set
  (Off / untracked / Dropped), the no-divergence lock landing, break
  lock parking the radar, the track-death mirror, symbol geometry pins
  (az wrap both directions, closure signs, IFF, the quality ladder,
  designated), the VS filter, snapshot determinism, and the scan-frame
  pass-through.
- test_fcr_page_flight.cpp (label f4-simulation, links f4-avionics):
  the done-when E2E. Two-ship scenario; player brain combat-disabled
  (the radar belongs to the page); the AI gate wired standalone (radar
  track store -> RadarBackedDetectionPolicy -> MissileModule::
  should_fire). Bandit starts inside the +-60 deg bar at 12 NM, files
  east out of it: search alone -> track decays -> radar leg dies ->
  gate closes; designate before the exit -> Track mode scans the locked
  target regardless of volume -> the leg stays lit 100 s of flight;
  break_lock -> the leg decays again. Page inputs driving the AI's own
  can_fire path.
- Docs: AVIONICS_PLAN.md status banner (AVIONICS-2 LANDED) + the §4
  as-built note; CHANGELOG entry; this worklog entry.

Tests: test_avionics_fcr 18/18; test_fcr_page_flight green (first
run); test_avionics_ins + test_avionics_steer re-run green; boundary
verifier PASS. f4-simulation/f4-ai/f4-sensors sources untouched by this
tranche — the fast sim tier's green set is expected to be unchanged.

Stage Summary:
- The FCR page exists as engine-agnostic avionics logic: mode SM + lock
  rules + renderer-facing snapshot, deterministic, render-ready.
- The player-in-the-loop path is now OPEN end to end in library form:
  page inputs (designate/break_lock) drive the same fire-control gate
  the digi brains use. AVIONICS-3 (the HUD view model) is the next
  rung; its first consumer is named (the world viewer's sp_draw_hud).

---

Task ID: AGG-2b
Agent: main
Task: "Land AGGREGATE_CLOCK_PLAN §4's AGG-2b — wire f4-entities'
SpatialIndex as the radar/detection membership term (FID_OPT §5's
20.8 s residual) and give the air picture per-unit detection cadences
with the AGG-2a stagger primitive — with the walks' byte contracts
held."

Work Log:
- Synced to origin/main first: the user had applied the previous
  session's DATALINK-1 + AVIONICS-2 patches AND folded a CAMP-TOT-PACE
  improvement into the DATALINK-1 commit (c540866); the local
  redundant commits were dropped via reset to origin (the trees were
  verified: origin superset).
- Studied the two hot walks' full shape: the radar scan's Search
  branch (with_component_ref bucket copy + inline clutter/range
  pre-gates, ~7,400 candidates/scan/radar, 99.8% rejected by
  is_ground_clutter) and push_air_picture_ (with_component id copy +
  per-entity handle resolution, ~4,400/walk at 10 Hz under demand).
  Key design finding: the dominant rejector is the CLUTTER predicate
  (stationary AND below 8,000 ft), not range — the 8x cutoff ball
  covers the theater at fighter radar ranges, so the win is a
  membership cache, not a positional ball prune.
- Landed AirPictureRoster (f4-entities/air_roster.hpp/.cpp): the
  non-clutter membership in entity-index order (the transform ref
  bucket's order — the uncached walks' candidate/contact order) +
  a SpatialIndex over member positions captured at rebuild (the
  within_radius convenience surface). Maintenance rule: rebuild on
  structural-epoch movement, revalidate on the caller-driven cadence
  (host-stamped sim time, never wall time; interval <= 0 = every
  call). EntityWorld owns one lazily through a unique_ptr (the
  forward declaration + out-of-line dtor/move-ops dance — the
  incomplete-type constraint shaped the API); the move ops leave the
  destination's instance empty, the moved-from world's stale roster
  self-heals through the epoch compare.
- The radar scan's Search branch now refreshes the roster (sim_time +
  roster_revalidate_s, default 1.0 s, data) and walks members with
  FRESH transform reads, re-applying clutter + cutoff idempotently.
  Track mode untouched. The scan_phase_s field (default 0.0) primes
  the sweep timer once on the first update (the first-tick bake
  shape); the fmod carry keeps the cadence exact for any phase.
- push_air_picture_ now walks the roster for contacts (same values,
  same order, fresh clutter gate per member) and collects the
  Step-13 datalink nodes from their own populations: the
  AwacsComponent + entities::RadarComponent ref buckets merged by
  slot index and deduped (dual-carriers), reproducing the interleaved
  walk's node order and team-intern order byte-for-byte; node
  liveness stays a fresh per-walk read (the GCI-ghost kill keeps its
  <= 100 ms bound). The walk's signature gained the stamped now_s.
- The per-unit detection cadences (the AGG-2a primitive's named
  consumer): scenario combat key "stagger_sensor_phases" (scenario.cpp
  reader + the session JSON writer + CampaignSessionOptions, default
  off) gates the armed spawn paths filling radar.scan_phase_s from
  f4-campaign's vu_hash — the campaign path keys the flight VU per
  arm index, the scenario path the radar seed + index, both folding
  into 1,000 phase slots (the reference's HOTSPOT_FIX jitter, made
  replay-stable; default off = byte-identical, the golden-identity
  rule).
- Tests: test_entities_air_roster (11 cases: priming, the epoch's
  instant rebuilds, the cadence's behavioral flips both directions,
  entity-index order, the radius surface, move-op self-healing, the
  interval knobs); 4 radar pins (BehavioralFlipJoinsAtTheRosterCadence,
  LandedMemberLeavesTheCandidatePoolImmediately,
  ScanPhaseShiftsTheSweepScheduleExactly,
  PhaseShiftMovesTimingNotOutcomes — plus the pre-existing
  DetectionTimelineInvariantToClutterPopulation pin now running
  through the roster path byte-for-byte); 2 sim pins
  (AirPictureRosterHoldsTheNonClutterPopulation,
  StaggerSensorPhasesKeyPrimesTheRadarPhases).
- Full suite audited clean-tree vs work-tree (the stash-and-rebuild
  comparison): 13 failures are PRE-EXISTING from the CAMP-TOT-PACE
  commit (DtoGoldens.IntentView's mission_over golden; the
  ResultSink.DirtySync bomb-impact log row; the EventStream,
  ArmedWar, CombatDeagg, CmdRetask/Abort/Journal, CampaignSession
  BigCatchUp/Straddled, CampaignWarHarness.RunsCertifies, and
  CampaignSchedulingSession.ArmOff ledger/timeline re-pins — the same
  set on both trees). The 5 ctest Timeout/Failed stragglers
  (CampaignInitWarsFast x4, WvrMergeHarness) run green solo on BOTH
  trees with identical timings (30.3 s vs 30.5 s, 16.0 s vs 16.0 s) —
  the 60 s ctest caps are load artifacts of this container, not
  regressions. The slow-labeled 24 h harnesses exceed the session
  budget (CampaignVerdict.TheFrontMoving + CampaignInitWars.SmallWar
  run green on the work tree; the rest re-certify on the next
  certificate run, the AGG-1 precedent).

Stage Summary:
- The radar/detection term is wired: one shared membership index
  serves every radar scan and the picture walk; the per-pass cost of
  both is O(air picture) (~100-200 members) instead of O(theater)
  (~7,400 transforms), and the per-scan ref-bucket copy is gone with
  it. The per-unit sweep phases (opt-in) kill the once-per-interval
  co-mounted radar spike.
- The byte contracts held: candidate sets, order, RNG streams,
  contacts, node order, and node liveness are the uncached walks';
  the only delta is the documented <= 1 s behavioral-flip latency
  (the AGG plan §5 re-pin doctrine), pinned in both directions.
- AGG-2b is the last named blocker before AGG-3 (aggregate-first
  spawn policy) — the FID_OPT §5 residual list now holds only the FM
  floor (physics) and the brain's diffuse glue.
- Produced 0003-AGG-2b-*.patch for the user's commit-and-push flow.

---
Task ID: AGG-3
Agent: Z User (session agent)
Task: "Committed and pushed. Proceed as planned." — the roadmap's next
  item after AGG-2b: AGG-3, the aggregate-first spawn policy
  (Docs/AGGREGATE_CLOCK_PLAN.md §4).

Work Log:
- Reconciled /home/z/F4 to origin/main (a9f7af3): the user's push
  contained the applied AGG-2b patch PLUS a folded CAMP-SAVE-WAVE rev 2
  (the initial wave's per-base launch queue — one departure per 15 min
  per base, bases concurrent) + CAMP-GATE-ROLL (the parking hold
  stretches to the takeoff gate; the flights table's takeoff TIME
  column). Local redundant commit dropped via reset — origin verified
  as the superset.
- Enabling discovery: AGG-3's CORE (synthetic ATM intents spawn as
  AGGREGATES, not Tier-B) already landed with FID-5 —
  `synthetic_as_aggregates` defaults true and the generated war rides
  the tier machinery (FIDELITY_TIERS_PLAN §4.5 as-built). The AGG-3
  section was stale. The section's remaining half is the optional
  authenticity knob: the reference's DoCompressionLoop rule
  (freefalcon-central campaign.cpp:2394-2520) — auto-1x while any
  deaggregated aircraft is live in the observer bubble.
- Engine (f4-simulation): `Stats::bubble_live` — deaggregated flights
  whose live lead sits inside the observer bubble (the DEAGG radius —
  the bubble trigger's own test — over `live_lead_position_`), computed
  per `refresh_stats_` via the new `count_bubble_live_()`
  (O(live flights), usually 0). The session publishes; it never paces
  (plan §2.2 — pacing is a host concern).
- Wire (f4-campaign-api): the `stats` query carries `bubble_live` at
  the StatsView tail (additive, plan §11; kProtocolVersion stays 1).
  Host adapter fills it; the StatsView golden + the host's
  StatsQueryCarriesTheCounterVocabulary pin re-pinned with the new
  last key.
- Host (f4-world-viewer): `CampaignClientRunner::set_bubble_action()`
  (atomic, the set_paused_flag shape) — the frame scope mirrors the
  engine's state from the snapshot refresh (once per advance, the same
  cadence the engine recomputes at), and the worker holds its feed at
  1x while the flag is up (`preset = min(speed, 1x)`). The preset radio
  keeps the user's request; the AIMD governor's scale carries over; no
  governor reset on clamp lift. The speed row surfaces the hold
  (TextDisabled `1x — action in bubble`). The camera-bubble checkbox is
  the knob (bubble off → no bubble deaggs → the clamp never fires).
  The stats query parser (+ SessionStats) learned the new tail key.
- Tests: `FidelityTiers.BubbleLiveCountsObserverBubbleAction` (no
  bubble = 0; force deagg OUTSIDE a far bubble = 0 — deaggregated is
  not enough; bubble over the live lead = 1; explicit fold = 0),
  `CampaignClientRunner.BubbleActionHoldsTheFeedAtOneX` (600 ms at 60x
  with the flag: held rate ~1x; the no-flag control runs at the
  preset) + `BubbleActionClearResumesThePreset` (the EMA climbs back
  after the flag drops). The viewer tests ran HEADLESS via a manual
  g++ link (the runner + queries are std:: + f4-campaign-api only) —
  raylib's FetchContent clone cannot complete in this container, so
  the viewer's CMake tier stays OFF here (the campaign_session_view.cpp
  wiring is symbol-checked against the real types: unique_ptr<
  CampaignClientRunner>, ImGui::TextDisabled/SameLine in use
  elsewhere).
- Full-suite audit: every failure on the work tree is the documented
  CAMP-TOT-PACE pre-existing set, verified unchanged (the two
  EventStream pins reproduced on the STASHED pristine tree this
  session; BigCatchUp/Straddled, ArmOff, RunsCertifies match the AGG-2b
  baseline). DtoGoldens.IntentView pre-existing. Zero new failures
  from this tranche.

Stage Summary:
- AGG-3 is CLOSED as-built: the spawn half rode FID-5; the DoCompressionLoop
  clamp is wired end to end — engine state, wire tail, host clamp, UI
  readout. Watching a deaggregated fight at 60x now runs it at 1x (the
  reference's rule); the war resumes the preset when the bubble clears.
- The AGG-3 plan section rewritten as-built (the FID-5 discovery
  recorded); the banner now reads AGG-4..5 as the open roadmap.
- The rule's placement is the architecture's own: the session publishes
  state, the host paces — any contract host can now implement the rule
  from the `stats` tail without engine changes.
- Produced 0004-AGG-3.patch for the user's commit-and-push flow.
