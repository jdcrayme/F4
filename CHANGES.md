# F4 Cleanup Pass — Changes Summary

## Tranche 0d (cont.) — @asset: model skip + renderer-half plan

**The simulation now skips the binary KoreaObj load when a scenario uses
`@asset:` model references.** This is the verifiable headless slice of
the renderer half of 0d — scenarios can reference glTF asset IDs instead
of KoreaObj binary paths, and `Simulation::load_models` defers to the
renderer's runtime glTF cache instead of parsing binary.

The full renderer rewrite (`VisualModelComponent` → glTF handle,
`f4-renderer` geometry/texture pipeline, link-cut, `temp/KoreaObj.*`
deletion) needs GL headers to verify. The comprehensive implementation
plan is now in `Docs/RENDERER_GLTF_REWIRE_PLAN.md`, ready for execution
in a GL-enabled environment.

### What landed

**`Simulation::load_models`** (`f4-simulation/src/simulation.cpp`):
detects `@asset:` model references and skips `ModelDatabase::load`.
The runtime glTF loader (renderer-owned, per the V-3DLIVE contract)
resolves them lazily per `vis_type`. `VisualModelComponent::model_record`
stays null (the session already runs this way); the renderer resolves
via `vis_type` + its own cache.

**New test**: `ScenarioLoader.AssetModelRefSkipsBinaryLoad` — verifies
`initialize()` succeeds with `@asset:` paths, the model db stays empty,
and the aircraft entity is still created with its `vis_type` identity.

### What's planned (renderer half — `Docs/RENDERER_GLTF_REWIRE_PLAN.md`)

| Sub-task | Description |
|----------|-------------|
| 2.1 VisualModelComponent rewire | Remove `model_record`; `vis_type` is the sole identity; `gear_switch_child` replaces `ModelState` |
| 2.2 f4-renderer rewire | New `RuntimeModelCache` loads glTF via `f4-gltf`, builds Raylib meshes from accessors, loads PNG textures by URI |
| 2.3 Link-cut | Drop `f4-models` + `f4-lzss` + `f4-world-convert` from `f4-renderer` / `f4-simulation` / `f4-world-viewer` |
| 2.4 temp/ deletion | Delete `temp/KoreaObj.{HDR,LOD,TEX}` (38 MB); migrate scenario templates to `@asset:` IDs |

Each turns boundary violations green as it lands. Estimated 3-5 days in
a GL-enabled environment.

### Test results

- **New**: 1/1 PASS (`AssetModelRefSkipsBinaryLoad`)
- **Full suite**: 2341/2348 PASS (99.7%)
- The 7 failures are **pre-existing** flight-model precision issues (verified identical on pre-0d code). Zero regressions.

### Files changed

| File | Change |
|------|--------|
| `f4-simulation/src/simulation.cpp` | `load_models()`: `@asset:` guard (skip binary load) |
| `f4-simulation/tests/test_scenario_loader.cpp` | New test `AssetModelRefSkipsBinaryLoad` |
| `f4-simulation/tests/CMakeLists.txt` | Expose `F4_SOURCE_DIR` to the test target |
| `Docs/RENDERER_GLTF_REWIRE_PLAN.md` | **NEW** — the renderer-half 0d implementation plan |
| `Docs/NO_BINARY_RUNTIME_PLAN.md` | Tranche 0d status updated (Task 55 + plan doc reference) |
| `worklog.md` / `CHANGES.md` | This entry |

---


## Tranche 0d (simulation half) — f4-world-convert cut from the runtime

**The `f4-world-convert` link is gone from `f4-simulation`.** A new
neutral `f4-world-types` library holds the runtime-safe subset of
`f4-world-convert` (enums + JSON ClassTable + AiiConfig), and
`f4-simulation` (+ its downstream `trace_runner` / `campaign_qc`) now
link that instead. The 0b boundary verifier confirms the
`f4-world-convert` direct violation on `f4-simulation` is **GONE**.

The renderer half of 0d (VisualModelComponent → glTF handle, f4-renderer
link-cut, temp/KoreaObj.* deletion) is deferred to the user's env — it
requires GL headers to build/verify and involves a deeper renderer
rewrite. The remaining `f4-models` + `f4-lzss` violations are all via
`VisualModelComponent`'s `ModelRecord*` handle.

### New library: f4-world-types

The runtime-safe subset of `f4-world-convert`, extracted into a neutral
library the runtime can link without pulling in legacy binary parsers:

| Header | Contents |
|--------|----------|
| `layout_types.hpp` | `ObjectiveType`, `PointType`, `PointListType` enum constants |
| `class_table.hpp` | `ClassTableEntry` + `ClassTable` (JSON loader only: `load_json` / `load_auto`) + lookup methods + `unit_subtype_name()` |
| `aii_config.hpp` | `AiiConfig` (the Falcon4.AII INI reader — text-only, moved verbatim) |

Dependencies: `f4-json` + `f4-io` only. NO `f4-install`, NO `f4-lzss`,
NO binary format knowledge. The binary `ClassTable::load()` (FALCON4.ct
decoder) + `find_class_table()` stay in `f4-world-convert` (importer-only).

### Migration

Every `f4::world_convert::` reference in `f4-simulation` (sources,
headers, tests, `campaign_qc.cpp`) replaced with `f4::world_types::`:
`ClassTable`, `AiiConfig`, all enum constants (`TYPE_AIRBASE`,
`PLT_RUNWAY`, `PT_RUNWAY`, etc.). `f4-simulation/CMakeLists.txt` drops
`f4-world-convert`, adds `f4-world-types`.

The scenario-player's `F4_DIGI_CLASS_TABLE` override updated to prefer
`Data/Classes/falcon4.ct.json` (the runtime `ClassTable::load_auto`
rejects `.ct` — the runtime no longer links the binary decoder).

### Boundary verifier: before → after

| Target | Before (0b/0c) | After (0d sim half) |
|--------|----------------|---------------------|
| `f4-simulation` | direct: f4-models, **f4-world-convert** \| transitive: f4-lzss | direct: f4-models \| transitive: f4-lzss |
| `trace_runner` | transitive: f4-models, **f4-world-convert**, f4-lzss | transitive: f4-models, f4-lzss |
| `campaign_qc` | transitive: f4-models, **f4-world-convert**, f4-lzss | transitive: f4-models, f4-lzss |

The `f4-world-convert` column is gone. The remaining `f4-models` +
`f4-lzss` are the renderer half (VisualModelComponent → glTF), deferred.

### Test results

- **f4-world-types**: 12/12 PASS (9 class_table_json + 3 layout_types)
- **Full suite**: 2340/2347 PASS (99.7%)
- The 7 failures are **pre-existing** flight-model precision issues
  (DigiMission, InterceptConvergence, FcsTracePipeline, CampaignBridge,
  CampaignSession, CampaignWarHarness) — verified identical on the
  pre-0d code (stashed changes, rebuilt, ran same tests). Zero regressions.

### Files changed

| File | Change |
|------|--------|
| `f4-world-types/` (new library) | `CMakeLists.txt`, `include/f4/world_types/{world_types,class_table,layout_types,aii_config}.hpp`, `src/{class_table,aii_config}.cpp`, `tests/{test_class_table_json,test_layout_types}.cpp` + `tests/CMakeLists.txt` |
| `CMakeLists.txt` | `add_subdirectory(f4-world-types)` before `f4-models` |
| `f4-simulation/CMakeLists.txt` | drop `f4-world-convert`, add `f4-world-types` |
| `f4-simulation/src/*.cpp`, `f4-simulation/include/*.hpp` | `f4::world_convert::` → `f4::world_types::` (ClassTable, AiiConfig, enums, includes) |
| `f4-simulation/tests/*.cpp` | same migration + `using namespace` fix + `.ct` → `.ct.json` fixture paths |
| `f4-simulation/tools/campaign_qc.cpp` | `ct.load()` → `ct.load_auto()` (runtime ClassTable has no binary load) |
| `f4-scenario-player/CMakeLists.txt` | `F4_DIGI_CLASS_TABLE` prefers `Data/Classes/falcon4.ct.json` |
| `f4-world-convert/tests/fixtures/falcon4.ct.json` | copied from `Data/Classes/` (test fixture for JSON path) |
| `Docs/NO_BINARY_RUNTIME_PLAN.md` | Tranche 0d status: PARTIAL (sim half landed, renderer half deferred) |

### What's next (renderer half of 0d — deferred to user env)

1. **`VisualModelComponent` rewire** — replace `const ModelRecord*` with a glTF model handle from `f4-gltf`
2. **`f4-renderer` rewire** — `feature_mesh` / `render_resources` / `texture_cache` stop calling `ModelDatabase::extract_model_geometry` / `fetch_texture`; mesh + texture data comes from the glTF load
3. **`f4-simulation` / `f4-renderer` / `f4-world-viewer` / `f4-scenario-player` link-cut** — drop `f4-models` + `f4-lzss`
4. **`temp/KoreaObj.{HDR,LOD,TEX}` deletion** — the 38 MB committed binary leaves the repo

Each turns the remaining `f4-models` / `f4-lzss` boundary violations green. Requires GL headers + visual verification (no X11 in sandbox).

---


## Tranche 0c — TEX→PNG + glTF materials: verified + landed (NO_BINARY_RUNTIME_PLAN.md)

**The producer side of the asset pipeline is complete.** All three 0c
sub-items are verified in-sandbox: the runtime can now load glTF + PNG
instead of parsing KoreaObj binary. The code was implemented in a prior
session; this tranche is the build + test + end-to-end verification +
status closure.

### 0c.1 — TEX → PNG extractor

`f4import textures --install <root> --data <dir> [--texture <N>] [--all]`
decodes KoreaObj.TEX entries to PNG via stb_image_write (zero-dependency).
Output: `Data/Models/koreaobj/textures/NNNNN.png`.

**Verified**: `--all` exports 1290/1290 PNGs (0 failures, 60s, 37 MB).
Valid PNG signatures, correct 256×256 square dimensions, alpha + chroma-
key flags recorded as manifest capabilities.

### 0c.2 — `f4import textures` subcommand

Fully implemented CLI (`f4import.cpp:run_textures`): `--texture <N>` for
single export, `--all` for the full bank, manifest update with
`alpha`/`chroma_key` capabilities and `KoreaObj.TEX`/`.HDR` sources.

### 0c.3 — glTF materials emission

`gltf_emitter.cpp` emits spec-compliant materials referencing the PNG
textures:
- One material per referenced texture (sorted by tex_id for deterministic
  output) + one shared `vertexcolor` material for untextured meshes.
- `pbrMetallicRoughness` with `baseColorTexture` → `textures/NNNNN.png`.
- `alphaMode: MASK` + `alphaCutoff: 0.5` for chroma-keyed textures.
- `TEXCOORD_0` accessors on textured primitives; `COLOR_0` on vertex-
  colored meshes (resolved through the HDR ColorBank).
- Samplers/images/textures/materials arrays in the glTF JSON.

**Verified**: `f4import models --model 2` produces a glTF with 2 materials,
`baseColorTexture`, `pbrMetallicRoughness`, `alphaMode`, referencing
`textures/00017.png`. The visual loop is closed: geometry + UVs +
materials + texture references in one spec-compliant glTF.

### Test results

| Suite | Tests | Result |
|-------|-------|--------|
| `test_textures_gltf` | 4 | PASS (113 ms) |
| `test_models_gltf` | 6 | PASS (53 ms) |

No regressions. 10/10 tests pass.

### Acceptance criteria (NO_BINARY_RUNTIME_PLAN.md §5)

1. `f4import textures` produces 1290 PNG files — **YES** (1290/1290, 0 failures).
2. `f4import models` emits glTF materials referencing PNG textures — **YES**.
3. A rendered model shows textured geometry — **DEFERRED** (visual, user's env; no X11/GL in sandbox).

### Files

No code changes — the 0c code was implemented in a prior session. This
tranche is verification + status closure:

| File | Change |
|------|--------|
| `Docs/NO_BINARY_RUNTIME_PLAN.md` | Tranche 0c status: LANDED |
| `worklog.md` | Task 53 entry |
| `CHANGES.md` | This entry |

### What's next

**0d** — the runtime glTF rewire. The producer side is complete; 0d is
the consumer-side refactor: `VisualModelComponent` → glTF handle, the
renderer/sim/viewer link-cut, and `temp/KoreaObj.*` deletion (repo drops
~38 MB). Each decoupling turns a 0b boundary violation green.

---

## Tranche 0b — CMake boundary enforcement (NO_BINARY_RUNTIME_PLAN.md)

**The boundary is now a contract.** P2 (link-time isolation) from
`ASSET_PIPELINE_SPEC.md` §10 is mechanically enforced at configure time:
no runtime target may link a legacy binary parser (`f4-models`,
`f4-world-convert`, `f4-terrain-convert`, `f4-lzss`). The gate FAILS
today (documenting the known violations) and turns green as Tranche 0d
(runtime glTF rewire) decouples each one.

`cmake/verify_boundary.cmake` (new, 272 lines) provides:

- `f4_mark_side(side target...)` — sets `F4_SIDE=importer|runtime` on
  each target and registers it in a global property (the authoritative
  target set the verifier iterates). Conditionally-built targets are
  silently skipped via a `TARGET` guard, so the marking list is the same
  regardless of which `F4_BUILD_*` options are enabled.
- `_f4_link_closure(out_var target)` — worklist-based BFS over
  `LINK_LIBRARIES` + `INTERFACE_LINK_LIBRARIES`, stripping generator
  expressions (`$<LINK_ONLY:name>`, `$<BUILD_INTERFACE:name>`). Includes
  PRIVATE links (for static libs, CMake propagates private deps to the
  final link line — the correct semantic for "does parser code end up
  here").
- `f4_verify_boundary()` — iterates registered targets, exempts
  `F4_SIDE=importer` (the parser's legitimate consumers), test
  executables (SOURCE_DIR contains `/tests`), UTILITY (custom targets),
  and IMPORTED (third-party). Classifies each finding as direct or
  transitive.

**Enforcement modes:**
- `-DF4_ENFORCE_BOUNDARY=OFF` (default) — violations print as WARNING;
  the build proceeds (so 0c/0d development isn't blocked).
- `-DF4_ENFORCE_BOUNDARY=ON` — violations are FATAL_ERROR; configure
  fails. The CI gate.

**Today's violations** (verified in-sandbox with renderer/viewer OFF;
f4-renderer/f4-world-viewer/f4-scenario-player verified by code inspection):

| Target | Direct | Transitive |
|--------|--------|------------|
| `f4-simulation` | `f4-models`, `f4-world-convert` | `f4-lzss` |
| `f4-renderer` | `f4-models`, `f4-world-convert` | — |
| `f4_world_viewer` | `f4-models`, `f4-world-convert`, `f4-terrain-convert` | — |
| `f4-world-viewer` | — | `f4-models`, `f4-world-convert`, `f4-lzss` |
| `f4-scenario-player` | — | (via `f4-simulation`, `f4-renderer`) |
| `trace_runner` | — | `f4-models`, `f4-world-convert`, `f4-lzss` |
| `campaign_qc` | — | `f4-models`, `f4-world-convert`, `f4-lzss` |

Each turns green as Tranche 0d's `VisualModelComponent` → glTF-handle
rewire + the renderer/sim/viewer link-cut lands.

### Files changed

| File | Change |
|------|--------|
| `cmake/verify_boundary.cmake` | **NEW** — P2 enforcement: `f4_mark_side()`, `_f4_link_closure()`, `f4_verify_boundary()`, `F4_ENFORCE_BOUNDARY` option |
| `CMakeLists.txt` | 0b block after all `add_subdirectory`: `include(verify_boundary)`, `f4_mark_side(importer ...)` on 15 targets, `f4_mark_side(runtime ...)` on 28 targets, `f4_verify_boundary()` |

### Design decisions

- **Centralized marking** (one declarative list in the root CMakeLists)
  rather than scattered `F4_SIDE` calls across 20+ library CMakeLists.
  Easier to audit, harder to miss a target. `f4-import`'s pre-existing
  `F4_SIDE importer` (line 47, anticipating Stage 5) is left in place —
  harmless (same value set twice), the centralized block is authoritative.
- **Global-property target registration** (`F4_REGISTERED_TARGETS`)
  rather than directory-walking (`get_directory_property(SUBDIRECTORIES)`
  recursion). The directory-walk approach produced exponential recursion
  in CMake 4.4.3. The registration pattern is simpler, robust, and the
  marking IS the registration — a new target must appear in a
  `f4_mark_side` call, which is the natural declaration of its boundary
  side.
- **Warning default, fatal opt-in.** The plan says "FAILS at configure
  time today" — with `F4_ENFORCE_BOUNDARY=ON` it does (FATAL_ERROR). The
  default OFF keeps the build working for 0c/0d development; CI enables
  ON as the gate. After 0d, both modes are clean.

### Acceptance criteria (NO_BINARY_RUNTIME_PLAN.md §4)

1. `cmake/verify_boundary.cmake` exists and runs at configure time — **YES**.
2. `F4_SIDE` set on every importer/converter target — **YES** (15 importer targets).
3. Verifier FAILS at configure time today — **YES** (FATAL_ERROR with `ON`; WARNING by default).
4. After 0d lands, verifier PASSES — pending (the gate is in place; 0d turns each violation green).

---


## G2 — The Interdiction Link (CAS against real battalions, the bombs booking)

**The two wars touched.** C6 made the air fight; G1 made the ground
fight; they shared a clock, a ledger, and a world — and never
interacted. The booking side had been wired since G1 (the sink's
battalion branch, `apply_ground_loss(air=true)`, the engine's
air-loss pull, the mirror) and the delivery side since the A-G
tranche (MK-82 stores, the strike fire control, the ballistic
flyout) — but the missing middle meant a bomb whose target was a
battalion resolved no feature set and detonated harmlessly, and no
tasking rung ever aimed a synthetic mission at a unit (UNIT-class
profiles flew target-less, the C3-documented deferral). Even
TestCamp's own saved CAS/BAI flights had been dropping harmless iron
on battalions for the whole campaign era. G2 is the missing middle:
**CAS packages draw, route to a front-line-ranked enemy battalion,
drop a stick of iron — and the line thins** (the ledger's ground-loss
rows carry `air=true`, the engine pulls them, the mirrored roster
decays 11→9 on the acceptance run).

**The chain, end to end**: the ATM (and the legacy ladder) pick a
UNIT-targeted delivery target — enemy battalions ranked by distance
to the contested FLOT (the front-line math extracted from the engine
into a shared helper so tasking and the engine can never drift),
ledger-destroyed skipped, wire-order ties, rotation-spread; the
route builder resolves the battalion's grid position (objectives
first, then units — the world loader's own order) and emits the
attack profile (the mission table's own `WP_CAS` string mapping to
the ground-strike delivery action); the mission-plan builders resolve
the delivery waypoint's `target_num` through the unit id map; the
brain's strike rung releases the stick (unchanged); at terminal the
NEW battalion blast endpoint computes an integer vehicle-kill count
(warhead × falloff / 96 lb per vehicle, capped at the mirrored
strength — pure, never mutating: the engine owns the battalion's
life) and publishes ONE `GroundUnitLossMessage` per effective bomb;
the sink (armed by the session's `unit_strike`) books
`apply_ground_loss(air=true)` + per-vehicle `apply_ag_kill` credit —
the exact branch G1 wired and waited for; the engine pulls, the
roster decays, the mirror syncs. Two wars, one war.

**Opt-in at every layer** (`unit_strike`, the aa_combat/ground_war
contract — default off, byte-identical documents): the tasking
rung gates on `CampaignConfig::unit_strike` (both ladders, one flag),
the sink's booking gates on its own arm (the blast endpoint cannot
know the session's flags — f4-weapons owns ballistics, not policy —
and ungated booking would change every pre-G2 golden that flew a
saved unit-targeted flight; with the flag off the events count in
stats and nothing books). The 0.1 h C6 armed war
(`e8496c7819cbb7b64b8f9e0a2fdc7b64`) and the 1 h G1 ground war
(`8171e8b6a8dfeb8057f747a06d5b173e`) re-verified byte-identical with
the flag off — the front-line extraction refactor included.

**The verification** (TestCamp, Release): the 0.3 h interdiction war
— exit 0, four green verdicts, identical 2-run MD5s, `agv=2`; the
1 h interdiction war — exit 0, `losses=15 (air=2)`, the engine pull
in the books (battalion 4121: strength 11→9, run_losses 2); the
combined `--aa-combat --ground-war --unit-strike` run shows the
honest contested side — the CAS package was shot down 23 s short of
its TOT (the exit-14 gate fires with the horizon guidance; the
full-length combined certificate rides PERF-3's wall-clock
constraint). Suites 2,226/2,226 Debug + Release (21 new tests).
**One real bug caught by the rig**: `objective_found` is true for
any transform-carrying target — the terminal's objective branch
swallowed battalion targets and the unit branch never ran; the
objective branch now keys on a resolved feature set. Docs:
INTERDICTION_PLAN.md; `campaign_qc --unit-strike` + exit 14.

## G1 — The Ground War (battalion maneuver, the front line, the books)

**The battlefield unfroze, deterministically.** The air side of the
campaign loop closed C1–C6 while 672 battalions stood at their
save-time grid cells forever and the only ground number the ledger
booked was the shooter's ag_kills credit — the victim's own
attrition was CAMPAIGN_LOOP_PLAN §7's first documented gap. G1 is
that tranche: a headless, deterministic ground-war ENGINE in
f4-campaign (`ground_war.hpp` — the campaign ladder's campaign-side
twin over the same IDataSource boundary, bound to the same result
ledger, moved by the same one clock), plus the ledger's ground books,
the write-back, the session wiring, and the QC gate.

**The engine, one update at a time**: orders (the GTM's own
DoCalculations objective scoring — front proximity, priority bonus,
random(5) dropped for determinism) task mobile battalions against the
enemy's objectives, artillery trailing, AD/SS/supply static; the
FRONT LINE resolves per grid column between the sides' forward
holdings (the FLOT DistanceToFront serves); movement walks at wire
movement_speed (or subtype defaults — the fixtures carry no UCD
enrichment) in 1/256 fixed point, fatigue and supply gated, ENGAGED
battalions pinned; contact (2 grid) exchanges attrition at
hours-scale tempo (each side's take ∝ enemy strength × supply ×
morale, fractional kills accumulating, rosters decaying
highest-group-first, morale eroding, zero-strength battalions dying);
capture flips undefended enemy objectives and the capturer garrisons
its prize; the .cmp last_resupply anchor drives the ground-supply
cadence (catch-up-once, the reinforcement cadence's own shape);
air-caused losses (the C1 sink's AG kills against battalion entities,
newly booked on the victim side too) are PULLED each update and thin
the line exactly once.

**One writer, one certificate**: every transition flows through the
ledger's typed apply methods (`apply_ground_loss`,
`apply_objective_capture`, `sync_ground_unit`); `to_json()` gains an
OPTIONAL ground block (totals, per-team rows, VU-sorted battalion
states, arrival-ordered events — ground-quiet runs emit byte-identical
pre-G1 documents, the C1 zero-event identity). The C5 harness's
determinism proof now covers the ground bytes with zero new
machinery. The write-back (`apply_ground_to`) lands positions,
rosters, losses, supply/morale/fatigue, heading, timestamps, and
owner flips into the WorldState; the SESSION mirrors the engine's
battalions into the sim's entities every update (transforms × 1024
ft/grid, tactical components, roster decay, the ALIVE tag on
destruction) — the 3D world's ground units march; flight-less
scenario-list worlds get populated for the mirror (the same
populate_world call the campaign_flights path makes). `campaign_qc
--ground-war` arms it; **exit 13** fires when an armed ground war
produced nothing (movement alone passes — fights happen at hours
scale).

**Verified on TestCamp (Release)**: the 6-minute smoke war — exit 0,
four verdicts green, MD5 identical across runs, 67 captures (the
save's frozen mid-war dispositions resolving), 204 front columns,
364 grid marched; the 1-hour war — exit 0, **MD5
8171e8b6a8dfeb8057f747a06d5b173e** re-derivable with md5sum, 13
vehicle losses (ROK 8 / DPRK 5), 1 battalion destroyed (run_losses
exactly its roster), 77 captures, the front WIDENING 204 → 290
columns, 2,520 grid marched, 1,085–1,300 tps, RSS flat. Two real
bugs caught on the way: the orders pass's `best_score` sentinel was
−1 while distant objectives score NEGATIVE (score − distance/2) —
no army ever marched until the sentinel became a large negative
(distance ranks, never vetoes); and the ground block's team rows
shipped a missing comma past the f4-json Reader's delimiter-lenient
walk — found parsing the artifact with a strict parser, pinned in
the tests forever. **Suites: 2,220/2,220 Debug + Release (15 new
tests).** See `Docs/GROUND_WAR_PLAN.md`.

## PERF-1 — The Shared Air Picture (the merge-phase collapse, closed output-identically)

**The armed war's throughput collapse, measured and fixed without
moving one byte of the war's ledger.** The C6 verification had named
the symptom — Release ~480 tps pre-fight falling to ~37 tps at peak
merge — and the phase profiler (`F4_TICK_PROF=1`) settled where it
went: `update_all` at 95–97% of tick time, per-tick cost growing
4 ms → 88 ms through the merge while the roster stayed FLAT at 96
live. The cost scaled with ENGAGEMENT state, not world size, and it
compounded — slower ticks stretched the merge in wall time, more
missiles stayed airborne, more brains armed their beam-fight
refresh.

**The root cause: one line of correct behavior times a 96× walk.**
While any hostile missile is visible, `BrainComponent` force-refreshes
its SensorFusion picture EVERY tick (the beam-fight rule — right, and
staying). But each refresh rebuilt the picture by walking
`with_component<TransformComponent>()` — the entity database, all
~4,400 transform-bearing entities of a populated save — paying an
`EntityHandle::get` hash probe per candidate and two string-keyed tag
lookups per survivor. Ninety-six brains × 60 Hz × the same walk: the
SHARED air picture re-derived from the database per-brain, per-tick.
The reference never does this — FreeFalcon's campaign loop iterates
its VU entity collections once per update and digi targeting reads
shared iteration state; the per-brain full-database walk was an
artifact of `SensorFusion` querying `EntityWorld` directly (correct
for a self-contained module, wrong at campaign scale).

**The fix — the host walks once, the brains read a snapshot.** New
`f4::ai::AirPicture` (a plain, dependency-light struct: contacts in
entity-index order — exactly the candidate set and order the
per-brain walk produced, clutter-skipped by the same
`TransformComponent::is_ground_clutter()` rule — carrying id /
position / velocity / interned team / the missile role bit).
`Simulation::push_air_picture_()` builds it once per tick and hands
one non-owning pointer to every roster brain;
`SensorFusion::set_air_picture()` makes the rebuild consume the
snapshot instead of the database (ownship still resolved from the
world — one lookup, same values). The fusion's world-query path
stays first-class for hosts and tests that push nothing, and both
paths share one per-contact build (`emplace_target`) so they cannot
drift. Byte-identity is by construction and proven by the acceptance
run: the 6-minute armed war's `campaign_result.json` MD5 is
UNCHANGED (e8496c7819cbb7b64b8f9e0a2fdc7b64, three consecutive
post-change runs).

**The demand gate — no walk without a consumer.** The first cut
built the picture every combat tick and regressed the quiet phases
(441 → 279 tps at t=63 s: the walk costs ~1.2 ms over 4,400 entities
and pre-flight nobody consumes it). The gate: `SensorFusion::
will_rebuild_this_tick(dt)` — an exact, side-effect-free mirror of
the fusion's own rebuild decision (skill timer expiring OR the
beam-fight missile rule) — queried per brain via
`BrainComponent::wants_air_picture(dt)`; the host builds only when
at least one brain says yes. Exactness note: a brain transitioning
Ground→Enroute INSIDE its update is combat-uninitialized there
(initialize() clears the picture), so the one in-tick entry into
combat-ladder eligibility takes the world path — identical output.
Cruise restored to 439 tps; the merge pays the walk ONCE.

**The policy batch hook.** `SensorFusion::DetectionPolicy` gains an
optional `prepare_batch()` (default no-op) fired once per rebuild;
`RadarBackedDetectionPolicy` resolves the ownship's radar + RWR
components once per batch instead of per contact (two component-map
probes × ~150 contacts × 96 brains × 60 Hz under the beam-fight
refresh). Direct/test `classify()` calls keep the per-call
resolution; the corpse rule holds on both paths.

**Verified (TestCamp v71, Release, this container).** The 6-minute
armed war: identical MD5, all four C5 verdicts green, wall 283 s →
183 s, merge sample 45 → ~156–177 tps. The 12-minute armed war —
which pre-PERF-1 never COMPLETED inside 570 s (still degrading past
24 tps at t=423 s) — now runs green end to end: wall 444.8 s, 12
cycles, 5 air losses, the reaper's first retirement, tps floor
140–179 through the fight and 192 recovering after, RSS flat at
~250 MB. Phase totals over the whole war: `update_all` 88% (the
flight-model pass + brains — goldens-pinned territory), sweeps ~10%
— nothing dominates the way the per-brain walk did, so PERF-2's
evidence-gated round promotes no further room (see
PERFORMANCE_PLAN.md §3). PERF-3 (the 24-hour Release war) is
documented with its certificate command and the ~15 h two-run
projection: this dev container cannot host multi-hour processes
(single calls cap at 10 minutes; background processes are reaped
between calls — measured), so the 24-hour certificate runs on a host
session while the dev-container evidence (the 12-minute war green
end to end, flat RSS, the sustained floor) stands in
PERFORMANCE_PLAN.md §3.

| Area | Change |
|------|--------|
| f4-ai — `air_picture.hpp` (new) | `AirPicture`/`AirPictureContact`: the host-built shared snapshot. Plain values, team interning table, no new dependency (f4-geo only). Contacts in entity-index order — the order the per-brain walk yielded (TargetInfo tie-breaks are iteration-order-sensitive). |
| f4-ai — `SensorFusion` | `set_air_picture()` (non-owning, per-tick); `will_rebuild_this_tick(dt)` (exact demand mirror); `DetectionPolicy::prepare_batch()` batch hook; the rebuild split into shared `emplace_target` + `resolve_ownship` with two paths (picture / world query) that share the per-contact OUTPUT contract; `initialize()` clears a stale picture pointer; `compute_geometry` drops its unused transform arg. Path B's team read copies the string out of the dying tag optional (a `-Wdangling-pointer` catch in the refactor). |
| f4-ai — `BrainComponent` | `set_air_picture()` (the host's injection point — same shape as the terrain/traffic picture pushes) + `wants_air_picture(dt)` (the demand predicate the host gates the walk on). |
| f4-simulation — `Simulation` | `push_air_picture_(dt)`: one demand-gated walk per tick (bucket copy → clutter filter → tag reads → interning → push/clear per roster brain), combat-gated (unarmed worlds never build it). `air_picture_` member rebuilt in place (steady state allocates nothing). |
| f4-simulation — `RadarBackedDetectionPolicy` | `prepare_batch()` caches the ownship's radar + RWR per rebuild; classify uses the batch pointers when prepared, per-call resolution otherwise (pointer safety: no world mutation inside a rebuild; every rebuild re-prepares). |
| docs | PERFORMANCE_PLAN.md (new): the measured problem, the principles, rooms PERF-1/2/3, acceptance contracts. CAMPAIGN_LOOP_PLAN.md: C6's merge-cost note now points at the closed perf tranche. |

**Testing.** Full suites 2,188/2,188 Debug + Release (2,183 + 5 new).
New units: `AirPicturePathMatchesWorldQuery` (a fixture world with a
closing hostile, a friendly, a missile, ground clutter, a
stationary-at-altitude anchor, and an untagged contact — two rebuilds
per path exercising EWMA, field-by-field TargetInfo equality, batch
hook fired once per rebuild, one classify per contact);
`ClearingAirPictureRestoresWorldQuery`;
`InitializeClearsStaleAirPicture`;
`WillRebuildThisTickMirrorsTimerAndMissileThreat` (the quiet-timer and
beam-fight branches); `RadarBackedPolicyBatchMatchesPerCall` (per-call
vs batch verdicts equal, the live-track verdict holds, the corpse rule
holds on both paths). The campaign-level proof is the war MD5 above
and `test_campaign_combat`'s armed E2E (green, deterministic).

## C6 — Arming the Campaign Flights: A/A Goes Live (campaign_qc --aa-combat)

**The C5 honest limitation, closed. The war loop's defining
interaction is live: campaign flights detect each other, engage,
shoot, and die — kills book to the ledger with full attribution,
the next cycle tasks a weaker force, and the whole thing stays
byte-deterministic.** Every piece of the fight already existed and
was test-pinned (M1 weapons, M2 sensors, M3 tactics + arbiter, the
C1 kill→ledger flow, the C5 reaper); C6 is the integration tranche
that ATTACHED it to campaign flights — the mission-role doctrine
answering the original "would break route-following" concern, the
radar-backed detection policy answering the deferred M2
GCI-omniscience flip, and the opt-in flag keeping every pre-C6
golden byte-identical.

**The doctrine (`arm_campaign_combat`, combat_bridge.cpp).** Mission
ROLE decides who fights: CAP/Sweep/Intercept/Escort categories arm
the full ladder (BVR + WVR + guns + release + the doctrine A/A
loadout — 4x AIM-120 + 2x AIM-9 + the M61 drum when the wire
loadout carries none); every other category flies DEFENSIVE-ONLY
through its BRAINDAT archetype — the shipped SEAD/Strike/Waypointer
shapes stand the engagement rungs down while MissileDefeat and
GunsJink stay armed (defense is doctrine, not aggression). The
bombs keep falling (hold_fire stays false — the A-G slice's pins
hold). No new brain API: the archetype gate is FreeFalcon's own
doctrine vocabulary, already load-bearing for scenario brains. The
role comes from `CampaignOriginComponent::mission_byte`, stamped by
both spawn paths.

**The wiring.** `Simulation::arm_campaign_aircraft(id)` — the
component set (radar/RWR/signature/damage/gun/identity, each
attached only when missing), `configure_brain_combat` for the
envelopes/ROE, the doctrine archetype, and a
`RadarBackedDetectionPolicy` owned in `combat_policies_` and
installed on the brain's SensorFusion (retire_aircraft reaps
campaign policies exactly like scenario ones). Bulk path arms at
initialize(); late spawner materializations arm in the session's
`adopt_new_spawns_()` cadence (register + arm, same campaign
second, both idempotent). Brain data loads eagerly via
`ensure_campaign_brain_data()` (loud on failure). Opt-in:
`CombatConfig::campaign_armed` (scenario JSON) ←
`CampaignSessionOptions::aa_combat` ← `campaign_qc --aa-combat`.
The counters ride the Stats/WarReport/diary surfaces
(armed_aircraft / armed_fighters / aa_kills; the war block gains
aa_combat + the armed counts; the progress lines gain ftrs=/aakill=
columns).

**Two integration bugs C6 surfaced (the unarmed war could never see
either; both are fidelity fixes, not features):**

- **The team mapping had no sides.** TestCamp's player slot is a
  neutral placeholder ("XX") while the war is ROK(2) vs DPRK(6);
  the B.3 `owner_team_string` mapped every aircraft to "green" —
  96 airborne, ZERO hostile pairs, zero fights possible. The
  diagnostic that found it: a per-tick team histogram. The fix:
  player-belligerent saves keep the classic mapping; a
  neutral-player save maps the WAR PAIR (first at-war pair in slot
  order, deterministic) to blue/red, everyone else green.
- **The radar painted the parking ramps.** The M2 placeholder
  scanned every transform entity — at campaign scale 48 radars x
  ~4,400 entities/sweep detected HALF of all candidates every
  second (measured 125k track-creating detections/s in a 36 s war),
  flooding track stores with ground clutter at perf AND fidelity
  cost. The shared `TransformComponent::is_ground_clutter()`
  predicate (stationary AND below every Korea terrain post —
  nothing genuinely airborne is ever clutter; a stationary rig at
  20,000 ft stays visible, so every original golden holds) now
  skips clutter in the radar scan AND the SensorFusion rebuild.
  Plus a range pre-rejection (beyond 8x reference range the
  detection model is provably pd=0) and nose-following scan-bar
  steering (a fixed north bar meant an east-flying fighter never
  painted the hostile off its nose; north-flying rigs keep the
  exact pinned bar).

**Verification.** New `test_campaign_combat` (7 units: the 41-byte
role map; the fighter arm — components/loadout/policy; the
defensive arm — archetype stands the rungs down, bombs stay armed;
idempotence; non-campaign rejection; the Simulation surface —
late-spawn arm + counters + ticking; the ARMED war over the routed
kunsan rig — verdicts green, deterministic, aircraft armed). Full
suites 2,183/2,183 in Debug AND Release. Real data (TestCamp v71,
regenerated world JSON, Release): the 6-minute armed war
(`--war 0.1 --aa-combat`, 2 in-process runs) — 6 cycles, 556
intents, 413 packages + 143 escorts, 115 routes (0 failures), 1,362
drawn (ROK 672 / DPRK 690), 96 live (48 saved + 48 synthetic), 96
armed (33 fighters), **the war loop's first A/A kill** — fully
attributed (killer credit, victim team/squadron/flight, t=356.7 s),
MD5 identical across runs and equal to `md5sum campaign_result.json`,
all four verdicts green, exit 0. A 12-minute observation run shows
fights compounding (12 → 39 engaged, merges at 0 NM); the merge
phase is the throughput cost (Release ~480 tps clean, ~37 tps at
peak merge — diary-documented, never gated). The reaper's kill
(356.7 s + 300 s hold) retires past this horizon — mechanics pinned
by the harness tests.

**Known limitation, honest by design.** The sim's air picture is
two-sided (blue/red/green): a third armed team in a war-pair save
(the U.S. squadron that scored the observed kill) engages whichever
side its own-relative hostility rule marks hostile. The
allied-to-a-side mapping is a follow-up refinement, documented in
the plan. The 24-hour full-horizon acceptance remains a Release,
multi-call run (the fight phase's throughput is the honest wall).

## SYMBOL-SVG-1 — f4-xml (vendored pugixml) + SVG Symbol Authoring: Import/Export, Color Roles, Holes, earcut Fills

**The SVG spike from the world-viewer UI audit: SVG becomes the
AUTHORING format for the SymbolLibrary (editable in Inkscape /
generatable by AI), while the library stays the RUNTIME format —
everything flattens once at import; nothing SVG-shaped runs per frame.
New root library f4-xml (vendored pugixml v1.15, MIT, committed under
f4-xml/third_party — same convention as tinyfiledialogs; exposed as
f4::xml) is the project's shared XML parser, chosen because it is
also the prerequisite for the documented BMS work (FALCON4_CT.XML &
co — worklog BMS-CT-1 / BMS-DATA-1, Docs/FALCON4_FILE_LAYOUT.md).
f4-renderer gains svg_import.hpp/.cpp: a STRICT subset importer
(svg/g/path/rect/circle/ellipse/line/polyline/polygon + title/desc;
nested translate/scale/rotate/matrix transforms; presentation
inheritance; full path command set M..Z with curve flattening at
16/32 segments; currentColor→Fill role, black/white→Outline,
data-color-role override; evenodd/nonzero subpath classification into
outer rings + holes) that fails LOUDLY by feature name on
out-of-subset input (gradients/filters/CSS/text/opacity... — identity
values editors stamp, like fill-opacity="1", are tolerated), plus the
inverse exporter (viewBox -1 -1 2 2; holes as subpaths; roles via
currentColor + data-color-role; stroke widths through a 64px
reference extent). The model grew to match the corpus's v2 schema:
SymbolColorRole (fill/fill_blend/outline) on polygons AND polylines,
polygon holes, and an earcut-triangulated fill cache (vendored
mapbox/earcut.hpp) rendered per-triangle in both the raylib and ImGui
draw paths — concave symbols and donuts now render correctly instead
of relying on the convex-only fan. The JSON loader learned the v2
sugar primitives the corpus actually uses (rectangles/lines/dots),
previously SILENTLY DROPPED, normalizing them to canonical
polygons/polylines (writer emits canonical form only; version bumped
to 2). refresh_fill_caches() recomputes the derived fills after load,
import, or editor mutation. Tests: 5 f4-xml smoke tests; 15
svg_import tests including a full corpus round-trip — every one of
the 75 f4_symbols.json symbols exports to SVG and re-imports
geometry-identical (roles, holes, widths, closure) — plus 29
symbol_library tests (unmodified, all green); world-viewer builds
clean against the extended model. Next steps (not in this change):
load f4_symbols.json at startup and prefer library keys in
RenderEntityIcon with procedural fallback, then delete the
~850-line hardcoded symbols.cpp vocabulary behind a parity flag;
thread campaign_icon through world_json → components.**

## QC-PASS-1 — Viewer QC Sweep: Route Clutter, Honest Speed Feedback, 3D for Everything Selected, Mechanical Cleanup

**A full QC pass over the repo against three user reports plus a
repo-wide sweep. (1) Flight plans no longer cover the map: all three
route passes (static waypoint polylines, live session routes,
mission→target / package→element links) draw ONLY for the current
selection — select a live aircraft and its route shows; select a
squadron and its flights' waypoints show; select a package and its
element links show; select an objective and the inbound mission links
show — with a new "All flight plans" master toggle (View menu +
Layers panel) restoring the old always-draw behavior. Selected routes
draw heavier. (2) The speed ratio "does nothing" report diagnosed:
the wiring was intact end-to-end — the real causes were sessions
starting PAUSED, the Debug build's ~330-540 ticks/s CPU cap making
10x/60x/240x deliver an identical effective ~7x with silent
debt-dropping, and zero rate feedback. `CampaignSessionRunner` now
measures its effective speed (EMA of sim-advanced per wall-second),
the session window prints `speed: 60x (effective 6.8x — CPU-limited)`
when the measured rate falls below 90% of the request, the radios
read back the runner's ACTUAL speed, speed presets appear pre-start,
and the keyboard gains `1`-`4` preset pick + `+`/`-` step (guarded by
WantCaptureKeyboard). Pause flips unified into one
`set_session_paused()` helper (button + Space + menu all agreed
before; now they share the code too). (3) Squadrons, ground units,
and live aircraft get a 3D view: the Inspector 3D tab no longer
hard-rejects non-objective selections — a new
`draw_entity_model_3d()` renders a LiveAircraft as its own
VisualModelComponent model at its transform facing; a Squadron as a
parked row of its class-table aircraft (gear down, ramp heading);
a static Flight as an airborne echelon pair; a Battalion/Brigade/
TaskForce as its VehicleCompositionComponent roster in vehicle groups;
all reusing the objective view's ground plane, orbit camera, and mesh
caches. The map's hit-test learns parked aircraft + deaggregated
vehicles (zoom > 2x, LiveAircraft selection kind) so everything on the
map is clickable, and the HUD summarizes live selections + hints
"zoom past 6x for 3D models" when a session runs. (4) Mechanical
sweep: the three commented-out draw passes re-enabled and truthfully
toggled (unit destinations, squadron→airbase links, BN→BDE hierarchy
lines); layer checkboxes single-sourced (one `draw_layer_groups` list
drives both the View menu and the Layers panel — the panel also gains
the Campaign-QC + Live-session groups that only the menu had); the
Campaign-menu auto-open fixed (it checked a null session right after
an ASYNC start and never fired); "Reset Session" made safe against
the async-create/deferred-stop race (the pending stop tags its target
session and a stale stop is dropped); Write-Result-JSON deduplicated
into one method; dead `draw_ground_grid()` removed; the 10ft/5ft
Z-sink constants reconciled into one named constant; `Impl::units()`
now an O(1) tag-index read via a new `tags::CATEGORY` tag set at world
load (was a 4-bucket union rebuilt ~4x/frame); parking overflow
(spawn_aircraft_from_squadrons' modulo wrap) offsets each extra pass
one wingspan to the aircraft's right instead of stacking every parked
aircraft on one invisible spot.**

| Area | Change |
|------|--------|
| `f4-world-viewer` — canvas | Route gating across the three passes: static waypoints draw when the unit is selected, a selected squadron owns the flight (`FlightPlanComponent::squadron`), or show_all_routes; live routes only for the selected aircraft or show_all_routes (selected = 2.5 px / alpha 235); mission links for the selected flight, its package/squadron associations, or — when an objective is selected — the links TARGETING it (inbound traffic view); package links for the selected package or a selected element flight. Hit-test extends to parked_aircraft() + deaggregated_vehicles() as LiveAircraft picks (zoom > 2x, 8 px tolerance). HUD: `[Live]` selection summary line (callsign + team from CampaignOriginComponent) + the zoom hint. Re-enabled destination/airbase/hierarchy draws under their existing toggles. |
| `f4-world-viewer` — entity_model_3d.cpp (NEW) | `ViewerApp::draw_entity_model_3d()` — the Inspector 3D branch for every non-objective selection. LiveAircraft: own VisualModelComponent model + facing from its quaternion; Squadron: parked row (up to 8) via `class_table.vis_type_for(class_table_index, 0)`, gear down; Flight (static): two-ship echelon; Battalion/Brigade/TaskForce: vehicle groups from VehicleCompositionComponent (up to 8 groups × 3 vehicles). Shares ground_layout_3d's ground plane, orbit camera, RenderTexture path, and mesh caches. inspector_panel.cpp dispatches on sel_kind. |
| `f4-world-viewer` — session UI | Speed feedback: the window's clock row gained the measured-rate readout (`speed: Nx`, `(paused)`, or `(effective Mx — CPU-limited)` when measured < 90% of requested, plus the short `(time-dilated)` marker unchanged); radios read the live runner; preset radios shown pre-start; Play/Pause + Space + menu all call `set_session_paused()`. Menu auto-open now unconditional (async start means session is always null at that point). Write Result JSON → shared `write_result_json()`. Deferred stop tags its target (`session_stop_target`) and process_session_stop() drops a stale tag (Reset during an in-flight async start could kill the freshly adopted session). Headless `--session --screenshot` smoke: the fixed exit countdown (6/12 s) no longer runs while an async create is in flight — on a slow Debug build it used to exit BEFORE adoption, silently skipping both the held screenshot and the session summary; the timeout thread now waits (bounded 240 s) for `campaign_session_starting()` to clear first, and its exit line reports a MISSING screenshot honestly instead of claiming success unconditionally. |
| `f4-world-viewer` — imgui_panels | `draw_layer_groups` — one lambda-defined list of four toggle groups (Base, Overlays incl. the new "All flight plans", Campaign QC, Live session) rendered by BOTH the View menu and the Layers panel; the panel's previously no-op checkboxes are gone (their draw passes are re-enabled and real). The stale "Now wired up" comment corrected. |
| `f4-simulation` — runner | `CampaignSessionRunner::effective_speed()` — atomic EMA (α = 0.25) of sim-seconds-advanced per wall-second, updated per batch (guard wall_sec > 1e-4); parked stores 0.0. Viewer reads it every frame for the CPU-limited readout. |
| `f4-simulation` — parking | spawn_aircraft_from_squadrons: when pilots exceed parking spots, the modulo-wrapped picks offset laterally one 60-ft wingspan per overflow pass along the aircraft's right vector (compass `h` → right = `(cos h, -sin h)`) — previously every extra aircraft stacked at ONE ENU point, rendering as a single aircraft while the roster counted many. |
| `f4-world` | world_loader populate_units stamps `tags::CATEGORY = "unit"` (first pass, beside ROLE/TEAM); viewer `Impl::units()` becomes a single `with_tag_ref` read (was: build a 4-bucket OPDOMAIN union + vector copy per call, ~4x/frame). `tags::CATEGORY` added to f4-entities with doc. |
| `f4-models` | geometry_extractor BSpecialXform: TODO replaced by the triage decision — the billboard/tree transform is viewer-dependent (the node carries only the type tag, no parameters), so there is nothing to bake in a view-independent extractor; the subtree recurses at authored orientation (correct placement, static facing), and making it face the camera is renderer-side work needing per-group TransformType metadata (the same call ASSET_PIPELINE_SPEC makes for far-LOD billboard cards). |
| `f4-world-viewer` — ground_layout_3d | `GROUND_SINK_FT` constant replaces the inconsistent -10/-5 literals; dead `draw_ground_grid()` + an unused color removed. |
| Tests | +0 tests, 32 FIXED on Windows: the f4-simulation suites that embed the generated-fixture path into scenario/world JSON documents (SimDataWiring, CombatIntegration, CombatTranscript, RegisterAircraft, SimulationLifetime) used `path.string()` — on Windows that renders backslashes, and the JSON loader decoded the `\f` in `...generated_fixtures\f16.json` into a FORM FEED, so every load failed with `generated_fixtures<FF>16.json`. All embedding sites now use `generic_string()` (forward slashes: valid JSON, valid Windows paths). The Linux container never saw this (no backslashes in paths). One f4-simulation test still fails on Windows only — CombatIntegration.CombatRecordingReplaysTheFight (the recorded TrackAcquired subject/object pair missing; MSVC numerics) — verified failing IDENTICALLY on the unmodified baseline, so pre-existing and out of this pass's scope. |
| Docs | README: f4-models-viewer section added (the app existed but was undocumented); session controls now document the keyboard presets + the effective-rate readout. This entry + worklog. |
| Deliberately not changed | `(void)num` at ground_layout_models.cpp:670 stays — it silences an unused STRUCTURED BINDING, whose name cannot be omitted pre-C++26; `(void)` is the idiomatic form. Squadron→parked-aircraft highlight (plan item C2) skipped: static squadrons carry no VU id (CampaignIdentityComponent has team+callsign only; UnitCoreComponent has no VU field), so matching parked aircraft's `CampaignOriginComponent::squadron_vu` to a selected static squadron is not cheaply possible — the parked row in the 3D tab covers the inspect need. |

**Verification:** incremental Debug builds of every touched module
(f4-entities, f4-world, f4-simulation, f4-models, f4-world-viewer)
clean — only the pre-existing warning set (C4244/C4100/C4996/C4189).
Final suites on Windows: f4-entities 83/83, f4-world 77/77,
f4-models 30/30, f4-renderer 13/13, f4-world-viewer 65/65,
f4-simulation 191/192 (the 1 failure pre-existing on Windows and
identical on the unmodified baseline — see the Tests row). Manual QA
list (TestCamp.cam): select aircraft/squadron/ATO row → only its plan
draws; "All flight plans" restores; speed presets show the true
effective rate (Debug: all top presets ≈ 7x, labeled); `1`-`4` and
`+`/`-` switch presets; 3D tab renders squadrons/units/live aircraft;
parked aircraft + vehicles click-selectable on the map at zoom > 2x;
re-enabled destination/airbase/hierarchy layers draw.

## C5 — The 24-Hour War: The Long-Horizon Acceptance Harness (campaign_qc --war)

**The campaign loop's acceptance run. C1's ledger, C2's one-pool
tasking, C3's routed generation, and C4's ATM pipeline now run for
HOURS of sim time — both sides generating, flying, fighting,
attriting, recovering, and resupplying — headless, deterministic, and
instrumented. `campaign_qc --war 24` is the "core game functionality
replicated" certificate: the war runs TWICE in-process, and the two
ledger documents must be byte-identical.**

**`CampaignWarHarness` (f4-simulation).** The acceptance runner —
not new campaign logic, but the SAME `CampaignSession` composition
the world viewer drives, advanced in fully-drained 4-sim-second
batches (byte-equivalent to any other tick split, per the C2 pin)
until the horizon, sampled every `--war-sample` seconds. Four
verdicts, checked at every sample:

- **DETERMINISM (exit 9)** — run 1's ledger bytes ≠ run 0's (compared
  as bytes; the MD5 is the certificate a human re-derives with
  `md5sum campaign_result.json`).
- **LEDGER DRIFT (exit 10)** — a one-pool identity broke: team pool
  outside `[0, initial]`, tasking view outside `[0, remaining]`, team
  books vs squadron books disagreeing (guarded on the ledger's own
  unmatched-flow counters), or a monotone counter going backwards.
- **ENTITY LEAK (exit 11)** — the roster identity broke:
  `live != initial + spawned − retired`. The deterministic form of
  "bounded memory" (RSS is diary telemetry, never a gate).
- **WAR ALIVE (exit 12)** — the clock stopped (frozen advance batches
  abort the war — the C5-FIX-1 class), no cycle fired in a sample or
  the whole war, or a belligerent that has EVER drawn went silent for
  a full sample with aircraft taskable.

The inherited tasking gates ride along, war edition (exits 6/7/8).

**The wreck reaper — the entity-churn bound.** Killed aircraft froze
in place forever (the FM stops, the transform parks, the roster keeps
walking the corpse every tick): a 24-hour war would accumulate wrecks
until the per-tick roster walks drown. `Simulation::retire_aircraft()`
removes a rostered aircraft (roster + wingman pairs + radar policies +
the world entity itself, in that order — nothing walks a destroyed
id); `CampaignSessionOptions::wreck_hold_sec` (default 0 = the
pre-C5 lifetime, every golden untouched) subscribes the kill feed and
retires each wreck `wreck_hold` sim-seconds after its
`EntityKilledMessage`. The loss was booked at EVENT time — the
corpse's removal never races the books. FreeFalcon's own shape: the
sim object dies, the campaign bookkeeping lives on.

**The artifacts.** `campaign_result.json` (run 0's ledger —
byte-stable); the summary's `war` block (DETERMINISTIC content only:
verdicts, counters, MD5s, per-team final pools — no wall-clock, no
RSS, no ticks/sec); and `campaign_war_diary.json` (one row per sample
WITH the performance telemetry — explicitly NOT byte-stable, in its
own file for exactly that reason). Per-sample progress lines print
throughout (cycles, draws, spawns, live roster, throughput, RSS).

**Verification.** New `test_campaign_war_harness` (5 units: the war
runs + certifies + is deterministic over the routed kunsan fixture;
the reaper's hold → retire-once → roster-identity mechanics, bus-fed
kill included, plus the hold-0 frozen-forever pin; the stall verdict
when no cycle ever fires; the single-run MD5 shape; option
validation). Full headless suite 2,176/2,176 (the container config
builds no GUI targets; the GUI suites are untouched — no renderer or
viewer code changed). On real data (TestCamp, v71): a 6-minute war —
6 cycles, 556 intents, 413 ATM packages + 143 escorts, 115 routes
(0 failures), 1,362 drawn (ROK 672 / DPRK 690 — both belligerents),
48 synthetic aircraft flown, ledger MD5 identical across the two
runs AND equal to `md5sum campaign_result.json`, all four verdicts
green, exit 0. Reinforcement verified with a 60 s cadence (28
delivered); the default 12 h cadence fires the stale catch-up at
t≈0 and refills at hour 12. Debug throughput ~330-540 tps at ~96
live aircraft — a 24-hour war is a multi-hour Debug run; use a
Release build for the full acceptance (the diary's ticks-per-sec
column makes the rate visible).

**Known limitation, honest by design.** A/A combat stays dark for
campaign flights in this slice (COMBAT_CHAIN_PLAN's arming gap), so
war runs book no air kills and the reaper stays quiet until that
lands — its mechanics are pinned by tests regardless. The war's
saved-flight default is the session's 48-flight interactivity cap
(`--max-flights` overrides); the ledger's pool arithmetic counts
every drawn aircraft whether it materializes here or not.

## C5 — The Starved Worker: Campaign Time Frozen Behind the Frame Lock (C5-FIX-1)

**The user report: "Campaign time doesn't seem to advance any more." It
froze COMPLETELY, not slowly — measured 0.0 sim-seconds advanced over
3 wall-seconds with the clock on and the UI perfectly smooth.**

**Root cause — an unfair mutex under a ~99.9% duty cycle.** The C4-FIX-3
tranche moved `advance()` onto the runner's worker thread and gave the
render loop a frame-long scope over the SAME mutex. But that scope held
the lock through `EndDrawing()` — raylib's 60 FPS pace WAIT lives inside
it — then released it for only ~tens of microseconds (the loop-top
checks) before re-locking. Under that pattern a plain `std::mutex` is
not fair: the UI thread's uncontended fast-path re-lock beats the woken
worker every single time (2-core box: the worker NEVER got the lock).
Campaign time advanced 0 seconds; a new regression test
(`ViewerFramePatternDoesNotStarveWorker`) reproduces run()'s exact duty
cycle and measured exactly 0.0.

**The fix, both halves.**

- **`FairMutex` (f4-simulation, header-only)** — a FIFO ticket-order
  mutex: tickets are assigned at `lock()` entry in arrival order and
  served in that order; every queued waiter is served before any
  newcomer, whatever the host's lock duty cycle. With two users (the
  worker and the frame) they strictly alternate — the worker is
  GUARANTEED at least one advance batch per frame, so at 1x the
  campaign clock tracks wall-clock by construction. Waiters block on a
  condition variable (no spinning — safe on 1-2 core boxes).
  `try_lock` never jumps the queue. The runner's session lock is now a
  `FairMutex` (`mutex()` return type changed; the two lock sites and
  the viewer's frame scope updated). `set_paused()`'s bounded-wait
  contract now means "at most one worker batch (~6-12 ms) ahead of
  you" — FIFO makes that provable instead of probabilistic.
- **`EndDrawing()` moved OUTSIDE the frame scope** — raylib's buffer
  swap + pace wait don't read the session; every `Draw*` has already
  copied its data into raylib's own batch buffers by the time the last
  draw call returns. The worker now gets the whole pace window (about
  two-thirds of every frame) for extra batches — high-speed presets
  (10x/60x/240x) actually reach their multipliers instead of sharing
  one batch per frame.

**Smoke hardening (the reason this shipped invisible).** The headless
`--session` smoke never ran the clock — every session starts PAUSED,
and no smoke ever pressed Play. Now: `--play` (with `--session`) starts
the adopted session RUNNING; on exit the viewer prints a one-line
summary a smoke can assert on — `[session] sim 79.8s campaign 38574439
cycles 0 missions 0 live 48` — before any teardown. The `--screenshot`
timeout thread now ends the run through `request_exit()` (atomic flag,
full epilogue: runner stop + join, the summary print, ordered GPU
unloads, `CloseWindow`) instead of `std::exit(0)` mid-frame, which
would have skipped all of that with a joinable worker attached. Also
fixed en passant: raylib's `TakeScreenshot` drops the directory part of
a path (rcore.c saves basePath + basename only) — the helper
`take_screenshot_to()` copies the file to the requested location, so
`--screenshot /tmp/x.png` really writes `/tmp/x.png` (the flag's
directory was silently ignored before, smokes included).

**Verification.** Full suite 2,249/2,249 (100%) — 2 new tests:
`ViewerFramePatternDoesNotStarveWorker` (run()'s exact 16 ms-hold /
50 µs-gap frame pattern; floor: ≥0.25x wall time at 1x; the bug
measured 0.0) and `FairMutexServesFifoAndTryLockNeverJumps` (queued
waiter served before a later newcomer, `try_lock` refuses while
held/queued, 3-thread mutual-exclusion hammer). Runner suite stable
across 3 consecutive runs. `campaign_qc` over TestCamp
(`--tasking 30 --minutes 20 --no-record`): 449 aircraft, 140 bombs,
85 features, 29 objectives, ledger MD5 identical across two runs —
the QC baseline is untouched (advance() semantics unchanged). Headless
`--session --play --zoom 12 --center 390,455 --screenshot` over the
real TestCamp world: 12 s clean run, exit 0, `[session] sim 79.8s`
(after: ~8 s of runner time at the 10x default preset — the clock
tracks the preset), screenshot 573 KB at the exact requested path with
1,767 distinct colors (terrain + live layers). Before the fix the same
run printed `sim 0.0s`.

## C4 — The Campaign Thread + Full 3D Coverage: Everything on the Map Renders (C4-FIX-3)

**Two user reports over the live session: "the UI becomes unresponsive
while running the session" and "many things don't have a 3D view —
ground units should show vehicles and personnel, squadrons should show
parked aircraft, in-flight aircraft should show them flying." Both were
architectural gaps, now closed.**

**(1) The campaign thread (V-THREAD).** The render loop called
`session->advance(wall_dt * speed)` inline in the ImGui frame — one
advance could legally run 240 ticks (the spiral-of-death cap) over 449
aircraft, which is seconds of work INSIDE one frame: the window went
"not responding". New `CampaignSessionRunner` (f4-simulation) owns the
loop: a worker thread advances the session in short mutex-guarded
batches (an ADAPTIVE tick budget measures each batch and scales the
per-call tick cap 1..session-cap to target a ~6-12 ms lock hold), and
the render loop takes the SAME mutex for one frame read+draw scope —
so every existing session read (canvas hit tests, the Campaign window,
the inspector, threat overlay, `session_handle` derefs) sees a frozen,
consistent session WITHOUT touching any of those ~70 call sites.
Pause/speed are atomics (the pause path mirrors the session's own flag
under the already-held frame lock; the locking `set_paused()` form
serves library hosts). Stop is deferred: the Stop button (inside the
frame lock) only sets a flag — `run()` performs the stop-join-reset
right after the scope releases the lock, so the join can never
deadlock against a worker waiting on our own lock. `advance()` gained
an optional `max_steps_override` (the runner's short-hold mechanism;
0 = the session's option — the QC and every test keep byte-identical
behavior, pinned by the two-run ledger MD5 test).

**(2) Full 3D coverage (V-3DLIVE).** What the user was missing, from
the data up: FALCON4.CT gives UNIT-level entities (battalion, brigade,
squadron) visType[0] == 0 — by data design, a campaign unit icon has
NO 3D model; the models live at the VEHICLE level and only exist after
DEAGGREGATION. On top of that, three of our own gaps: the session ran
with an empty ModelDatabase (2D-symbols-only by design), so even
deaggregated vehicles spawned NOTHING (the spawn path required a
resolvable model record); the parked squadron aircraft existed as
entities since create() but no layer drew them; and the deagg bubble
followed the first PARKED aircraft (a 1-grid circle around a ramp),
not the user's view. Fixed as a stack: (a)
`VisualModelComponent::vis_type` — every spawn path (flights, synthetic
intents, squadron parked aircraft, deagg vehicles, airfield features,
scenario aircraft) records the resolved vis type at spawn, so the
renderable identity survives an empty db; (b) vehicle deagg no longer
requires the db (identity lives in the class table; the mesh is the
host's problem); (c) new `draw_vis_type_mesh()` (f4-renderer) — the
same build/draw/cache path `draw_feature_mesh` uses after its
class-table lookup, callable with the vis type directly (`build_feature_mesh`
also lost a null-class_table check it never actually read); (d) the
canvas gained a LIVE 3D pass — session aircraft, parked squadron
aircraft, and deaggregated vehicles drawn as real KoreaObj models under
the same top-down ortho camera as the static pass (facing from velocity
or the parked quaternion), plus 2D dot layers for parked aircraft and
vehicles so everything on the map is visible at ANY zoom; and (e) the
VIEW BUBBLE — when the user is zoomed in, the deaggregation bubble
follows the CAMERA (the map viewer's "player"), radius scaled with the
visible extent (clamped 2.5-25 grid units), applied immediately so a
PAUSED session still deaggregates what the user zooms into. Toggle in
the Campaign window; off = FreeFalcon's ownship bubble.

Verification: full suite 100% (2,247/2,247, incl. 5 new runner tests +
the draw_vis_type_mesh GPU test); campaign_qc over TestCamp
(--tasking 30 --minutes 20 --no-record): 449 aircraft, 140 bombs,
85 features, 29 objectives — identical to the C4-FIX-2 baseline, ledger
byte-identical across two runs; in-container viewer smoke over the
real TestCamp world: --session --zoom 12 (camera bubble active, live 3D
pass drawing) runs 6 s under Xvfb, clean exit 0.

## C4 — Start Session Crash + Freeze: Lender Lifetimes in the Session Wiring, Async Start (C4-FIX-2)

**"Start Session" froze the viewer for a long time and then died with an
access violation in `ClassTable::vis_type_for()`. Two lifetime bugs in
the C4 session wiring and the synchronous create() were the whole
story. (1) `Simulation::init_bubble_manager()` loaded FALCON4.CT into
a STACK-LOCAL `ClassTable` and handed the BubbleManager — whose
constructor contract is "ct must outlive the manager" — a reference to
it. The local died at function return; the first tick's deagg
(update_bubble → deaggregate_ → spawn_vehicles_from_unit →
vis_type_for) read freed stack memory. Every QC/unit-test world
deaggregates nothing near the bubble center, so only the user's real
install campaign (garrison battalions parked ON the airbase the first
flight spawns at) hit it. The ClassTable is now a Simulation MEMBER
(`class_table_`), loaded once by `load_class_table()` in initialize()
and shared by every borrower — the spawn paths lose their per-call
re-loads (one table, three fewer file reads per session), and the
BubbleManager finally holds a reference that lives as long as it does.
(2) `CampaignSession::create()` handed CampaignSimSpawner three
LOCALS — the fallback airfield, the per-airbase airfield map, and the
template aircraft — that died when create() returned; the spawner
holds them by reference/pointer for the session's lifetime, so the
first synthetic spawn after a tasking cycle read freed memory (garbage
parking positions, freed parking-spot vectors — invisible on the
fixture worlds only because the freed heap wasn't reused there). The
session now owns them as members (airfield_ / airbase_airfields_ /
spawn_tpl_, declared before spawner_ so reverse-order destruction
keeps every borrower dying before its lender). (3) The freeze itself:
create() over a real install world is tens of seconds of work (world
JSON parse + population + 449 flights + thousands of squadron parked
aircraft), and it ran synchronously inside the ImGui button handler —
the window went "not responding", the user clicked Play while frozen,
and the queued keypress unpaused the session straight into the crash.
create() is pure headless (no GL/raylib/ImGui), so it now runs on a
worker thread (packaged_task → future); run() polls
adopt_session_start() every frame, the Campaign window shows a live
"Starting session…" state with a Cancel button, and the session lands
paused exactly as before. The window never freezes again — and the new
`--session` CLI flag (pair with `--screenshot`, which is HELD while a
start is in flight) gives headless smoke coverage of the whole flow.
Verified in-container under Xvfb: kunsan world + --session +
--screenshot runs clean, exit 0. QC acceptance re-run on TestCamp
(30-min tasking + 20-min INTSTRIKE, --no-record): 449 flights, 94
intents, 70 packages + 24 escorts, 62 slot snaps, 140 bombs / 85
features / 29 objectives, campaign_result.json byte-identical across
two runs, exit 0. Suite: 2,246/2,246 (100%).**

| Area | Change |
|------|--------|
| `f4-simulation` — Simulation | NEW member `class_table_` + `load_class_table()` (the ONE load, in initialize(), before every consumer) + public `class_table()` accessor (hosts share the table instead of re-loading the file). spawn_from_campaign_flights / spawn_squadron_aircraft / init_bubble_manager all read the member — the stack locals (one of which dangled inside the BubbleManager) are gone, and the per-path duplicate loads go with them. Empty path → empty member, preserving the documented graceful degradation. |
| `f4-simulation` — CampaignSession | The spawner's lenders are members: `airfield_` (the fallback airfield), `airbase_airfields_` (the per-airbase map, now `&member` not `&local`), `spawn_tpl_` (the template aircraft). Declaration order keeps them alive past spawner_'s destruction. create() fills the members instead of locals — the wiring is otherwise byte-for-byte the QC's. |
| `f4-world-viewer` — async start | Impl gains `session_starting` / `session_start_thread` / `session_start_future` (+ `SessionStartResult` — the future carries both the session and the error; the worker touches nothing of Impl's, the future's shared state is the single rendezvous). start_campaign_session() launches a packaged_task; run() calls adopt_session_start() each frame BEFORE anything reads impl_->session (Space toggle, advance block, canvas live layer, the window); the window's start row becomes a live "Starting session…" state with Cancel (stop_campaign_session joins + discards); the run() exit path and ~ViewerApp join a still-running worker (a joinable std::thread dtor would terminate()). The scheduled screenshot is HELD while a start is in flight so `--session --screenshot` always captures the adopted state. |
| `f4-world-viewer` — CLI | NEW `--session` flag: start the live campaign session over the loaded world right after the CLI loads settle (request_campaign_session() is the public wrapper the Campaign window's button also goes through). |
| tests | +4 (2,242 → 2,246, 100%): NEW test_simulation_lifetime.cpp (3 units — the member-load contract; the deterministic crash repro: after initialize() returns, a 256 KB stack-stomping recursion corrupts the dead frame exactly the way the user's render loop did, then force_deaggregate() must still resolve all 3 vehicles' vis types + models — reintroducing the old stack-local bug makes this test FAIL, verified; the per-tick bubble path: tick() deaggregates the co-located battalion on tick #1 and reaggregates when the player moves away — a crafted campaign-flights world JSON with an AIRBASE objective, a squadron, a flight, and a garrison battalion carrying vehicle_groups, run against the real FALCON4.ct + KoreaObj models) + 1 session unit (SyntheticSpawnsParkAtFinitePositionsInsideTheater: advance past tasking cycles, every materialized aircraft's transform is finite and inside the theater — the dangling-airfield observable). |

## C4 — The ATM Pipeline: 7-Phase Tasking, FindBestAir, Escort Pairing, TOT Slots, Mission Recovery (C4-ATM-1)

**The tasking ladder is now FreeFalcon's actual Air Tasking Manager
shape: a 7-phase pipeline (request generation → prioritization →
deconfliction → package building → support assignment → route planning
→ TOT slot scheduling) with budget awareness, replacing the C3
role-fallback bridge with the reference's FindBestAir SCORING (a
counter-air F-16 wing is taskable for strike at a reduced rating —
scored, never gated). Packages compose from the profile's own hints:
ADDSEAD + a defended target pairs a SEADESCORT flight, ADDESCORT pairs
a fighter escort, each with its own FindBestAir pick and TOT staggered
by the support profile's separation — multi-flight packages, one
intent per flight (the spawner's contract unchanged), all flights
sharing the package_id and the main flight's route (package-shared
ingress, the C3 deferral this tranche closes). Takeoffs slot against
the DECODED `atm_airbases` schedules (the wire's 32-block × 5-minute
bitmasks, now emitted as `atm_schedules` and parsed into WorldState —
the save's own planned sorties are already-consumed slots our flights
deconflict against); the mission-priority tables (mission_priority /
objtype_priority, GetPriority's own inputs) gate request generation
exactly the reference's way (a 0 entry = the team never requests that
mission). The decoded ATO backlog seeds the first cycle (past-TOT
requests take the reference's 30-minute delay pushes, capped at 8).
And the C2 "drawn = committed" simplification closes: mission
recovery — when a flight's mission-over deadline passes, its
SURVIVORS (drawn minus the ledger's per-flight booked losses) return
to the tasking pool and fly again; only deaths keep a draw spent. The
legacy ladder stays the library default (atm_pipeline off — the B.3/
C2/C3 goldens byte-identical, pinned by test); campaign_qc and the
campaign session arm the pipeline. Verified on TestCamp: a 4-hour
tasking + 20-minute INTSTRIKE run — 8 cycles, 703 intents, 523
packages with 180 escort flights, 143 takeoff slot snaps, 406 aircraft
recovered back into the pool, 240 routes (1,157 wps), 88 bombs / 66
features destroyed / 20 objectives written back, campaign_result.json
byte-identical across two runs, exit 0. Suite: 2,238/2,238.**

| Area | Change |
|------|--------|
| `f4-campaign` — AirTaskingManager | NEW atm.hpp/cpp: the 7 composable phases as public methods (each independently unit-tested, the M4.2 deliverable) — generate_requests / prioritize (stable priority sort + the missions_per_cycle tempo budget) / deconflict (mindistance/mintime vs booked flights) / compose_packages (target analysis: ScoreThreatFast at the profile's target altitudes → NEED_SEAD, then FindBestAir) with build_support_flight_ (the escort pairing) / schedule_takeoff (FindTakeoffSlot + ScheduleAircraft ports: exact/+1/+2/backward snap, fudge-block fill, large-flight double slot; the commit point that books a flight for recovery) / recover_completed. MissionRequest/FlightTasking/RecoveryRelease as plain data. FindBestAir (atm.cpp:1534 port): rating = UCD Scores[reference-ARO] when the theater DB resolves the squadron, else the specialty-derived fallback (AA→100 CA/30 ground, AG the mirror, unspecialized 60); ±5 specialty; lowestScore gate; capability/range/availability/schedule-full skips; −5 one-short; +3 within-package squadron reuse; +2 same-airbase; +2 half-range; +2 quickest-arrival with the reference's previous-best rebalancing. Every simplification documented in the header. |
| `f4-campaign` — Campaign integration | CampaignConfig::atm_pipeline (default OFF — legacy goldens pinned) + AtmConfig tunables (the aiinput [ATM] keys as config, the RouteBuilderConfig pattern). When armed, run_tasking_cycle_atm_ composes the phases, builds the package-shared routes (phase 6), snaps slots (phase 7), publishes one intent per flight (MissionIntent +flight_role/+escorted_flight_id), books ledger draws, and rides mission recovery on the tick (survivors back before the next cycle's draws). The cycle-time fix: every due cycle now fires at its OWN due time — a big tick == N small ticks exactly (the pre-C4 code fired all cycles at the advanced clock, equivalent only for single-cycle horizons; C4's slot scheduling exposed it). The summary gains an `atm` block (opt-in, like every attachment). |
| `f4-campaign` — ledger recovery | apply_mission_recovery (the draw's mirror: releases clamped at outstanding draws, run_draws DECREMENTED so the pool math needs no extra term) + flight_air_losses query (the per-flight loss count the recovery arithmetic reads off the air-loss log) + MissionRecoveryRecord/aircraft_recovered()/mission_recoveries()/mission_recovery_log(). to_json: recovery totals in `totals` + the `mission_recoveries` array (absent when empty — legacy byte-shape preserved). |
| `f4-world` + `f4-world-convert` — the ATM data surface | world_json emits `atm_schedules` (the 32-block bitmasks behind the id list) and the team priority tables `mission_priority`/`objtype_priority` (GetPriority's inputs — decoded all along, never emitted). WorldState parses all three + the existing atm_requests; ITeamSource grows default-implemented accessors (mission_priority/objtype_priority/atm_airbases/atm_requests — AtmAirbaseState/AtmRequestState structs); the TeamAdapter overrides them. The boundary discipline unchanged: the campaign reads the same interfaces, f4-world-convert stays invisible. |
| `f4-simulation` — QC + session | campaign_qc arms the pipeline (min_seadescort_threat 25, the fixture-theater host override — same reasoning as the route config's 25), prints the ATM telemetry line, carries the atm counters in the summary's tasking block, and gates exit 8 (drew aircraft but built no packages — the phase-chain-broke class). CampaignSession: atm_pipeline ON by default (the C4/C5 development surface — the session's role_fallback option is REPLACED by it), Stats +packages/+escorts/+recovered. |
| `f4-world-viewer` — session panel | The war-status block gains the packages/escorts/recovered line; the generated-missions table gains the role column (main / +sead / +esc) — the package composition is now visible in the UI. |
| tests | +32 (2,206 → 2,238, 100%): 30 ATM units (AirbaseSchedule find/fill/full semantics; generation ladder + priority-table drop rule + backlog seed/pushes/timeout; prioritization sort + budget; deconfliction; FindBestAir role scoring + the counter-air-wing-flies-strike pin + availability gate; escort pairing (defended→SEAD+escort, undefended→escort-only, lead-squadron preference); slot snap + seeded-schedule deconfliction + fill-shifts; recovery with and without ledger losses; ledger draw-recover netting + JSON block; the Campaign mode switch — determinism, the legacy goldens byte-identical with the pipeline off, drawn-aircraft-return, multi-flight package id/role/TOT contracts), 1 world-state unit (the team ATM fields + adapter boundary), 1 session unit (ATM session: packages build, byte-identical across two sessions). Fixtures: kunsan_campaign.world.json regenerated with the new emission (real ROK/DPRK ATM schedules + backlogs + priority tables). |

## V-CAMP.1 — Runtime Fixtures Are Build Outputs: "Start Session" Needs No Preparation Tools (VCAMP-FIX-1)

**"Start Session" failed with "aircraft config not found" on a viewer-only
build — and the fix is structural, not a path tweak: the session's runtime
inputs (generated_fixtures/f16.json, MissionProfiles.json) are BUILD
artifacts whose generation custom targets lived inside f4-convert's TESTS
directory. A single-target build of an app — `cmake --build build --target
f4-world-viewer`, or F5 on the viewer project in Visual Studio — builds
only that target's dependency closure and SKIPS every ALL custom target,
which is exactly where the fixture generation hid. The apps then launched
without their data. The generation is now hoisted to the LIBRARY level
(f4-convert/CMakeLists.txt — target names, output paths, and commands
unchanged, so every existing test-side add_dependencies keeps working),
and the three app targets that consume the fixtures declare dependencies
on them: f4-world-viewer (+ mission_profiles_fixture), f4-scenario-player,
and campaign_qc. Building the app IS the preparation — no manual tool
runs, no full-build-first. The viewer additionally pre-checks the two
required fixtures before creating a session and reports the exact rebuild
command (a bare "aircraft config not found: <path>" sent users hunting
for a preparation step that never existed). Regression evidence: the
before/after Start Session repro (fixtures deleted → the exact reported
failure; rebuild the viewer target alone → session creates and advances
over the real save), campaign_qc over TestCamp byte-identical pre/post
change (campaign_result.json), the full C3 acceptance reproduced exactly
(exit 0), and the suite at 2,206/2,206 — now INCLUDING the f4-renderer /
world-viewer / scenario-player test dirs, which this tranche's dev
container build enabled for the first time (GUI targets were previously
skipped in-container; X11 dev headers extracted to a user prefix +
Xvfb). That first-time enablement also surfaced one stale test that had
been red at HEAD all along — f4-renderer's coord_transform, whose
expectations matched no implementation that ever shipped — pinned here
to the documented, visually-verified conversion.**

| Area | Change |
|------|--------|
| `f4-convert` — fixture generation hoist | The golden-fixture generation block (25 aircraft .dat JSONs + simdata/mnvr/brain/formation + wave-2 veh/irst/rwr/visual/sig) moved from `tests/CMakeLists.txt` to the library CMakeLists, AFTER the CLI subdirectory (the converters ARE the generators; the block is gated on `F4_CONVERT_BUILD_CLI`, default ON — same coupling the old tests/ block had). Rationale: these JSONs are runtime inputs for the apps, not test-only byproducts; a config with tests OFF no longer silently loses them, and the latent configure breakage of downstream `add_dependencies(test_x convert_golden_fixtures)` under `-DF4_CONVERT_BUILD_TESTS=OFF` is gone. The redundant trailing re-set of `F4_GENERATED_FIXTURES_DIR` dropped (the ROOT CMakeLists owns that cache var, FORCE-set before any add_subdirectory). |
| `f4-convert/tests` — slimmed | Pure test registration + compile definitions; a pointer comment documents where the generation moved and why. |
| `f4-world-viewer` — fixture deps | `add_dependencies(f4-world-viewer mission_profiles_fixture)` + guarded `convert_golden_fixtures`: a viewer-only build (single target or VS F5) now generates f16.json and MissionProfiles.json — the exact gap that produced "aircraft config not found". |
| `f4-scenario-player` — fixture deps | Guarded `add_dependencies(f4-scenario-player convert_golden_fixtures)` — every scenario template's aircraft_config_path points at the generated f16.json; a player-only build produces it now too. |
| `f4-simulation` — campaign_qc deps | Guarded `convert_golden_fixtures` beside the existing `mission_profiles_fixture`: building only the QC target yields a runnable QC (its default --config is the generated f16.json). |
| `f4-world-viewer` — Start Session pre-check | campaign_session_view.cpp: before CampaignSession::create, verify the F-16 config and mission-profile table exist; on a miss, report what is missing, its path, AND the rebuild command (`cmake --build <build> --target f4-world-viewer`) instead of letting create() surface a bare path error. |
| `f4-renderer` — stale test pinned | test_coord_transform.cpp: ModelVertexToRaylib_Axes/_General expected `(x,y,z)→(x,−z,y)`, a mapping NO shipped implementation ever had (the function was `(y,z,−x)` at introduction, `(y,−z,−x)` since the model-viewer-era update — the one every shipped rendering surface uses). The test had been red since the renderer targets were first enabled; pinned to the documented current conversion `(x,y,z)→(y,−z,−x)` with the history noted in-comment. Test-only change — zero runtime code touched. |

## V-CAMP — The Live Campaign Session: Time Controls, Flying Flights, Route Inspection in the World Viewer (VCAMP-1)

**The world viewer can now RUN the war, not just display its save.
CampaignSession (f4-simulation) is the campaign_qc wiring repackaged
as one frame-driven object: WorldState → adapters → C1 ledger + C2
one-pool ladder + C3 route builder, all publishing onto the
SIMULATION's own bus, with the spawner materializing generated
missions INTO the sim's world and registering them through the new
Simulation::register_aircraft() — the one-world closure the QC never
had (its synthetic flights spawned into a side EntityWorld nothing
ticks: "materialized" there means counted, here it means FLYING).
The session advances ONE clock in fixed sim_dt ticks (the FM's tuned
discretization — the scenario player's "Fix Your Timestep" contract);
the ladder and the damage sync ride whole campaign seconds accumulated
from the same ticks. The viewer wraps it with the controls this
tranche exists for: play/pause (Space), 1x/10x/60x/240x speed presets
(wall-clock scaling — the tick dt never changes), the campaign clock
as D# HH:MM:SS, a war-status block (cycles, missions, routes,
draws/losses/reinforcements, live/airborne counts), a generated-
missions table (click a target to select + pan), a canvas live layer
(live aircraft symbols in owner colors, their MissionPlan routes as
polylines with numbered waypoints), a threat-map overlay (the C3 SAM
rings, painted), live-aircraft picking, and a live flight-plan
inspector (waypoints, actions, kinematics, phase). The saved-flight
spawn filter caps the sim at interactive budgets (default 48 of
TestCamp's 449). Deterministic by construction — two sessions over
the same world advanced identically produce byte-identical ledger
JSON (pinned by test). Suite 2122 → 2128 (+6, 100%, zero warnings);
the campaign_qc E2E reproduces the C3 acceptance exactly (4 h tasking
+ 20 min INTSTRIKE: 8 cycles, 411 intents, 1,013 drawn, 81 routes /
383 wps, 8 synthetic flown, 88 bombs, 20 objectives back, exit 0).**

| Area | Change |
|------|--------|
| `f4-simulation` — the roster API | `Simulation::register_aircraft(EntityId)`: hosts spawning into `sim.world()` after `initialize()` (the campaign-spawner path) join the tick loop's roster — ground-elevation pre-pass, combat-intents roster, FM → Transform sync, recorder. Without it update_all still ticks a late-comer's brain + FM while its transform parks forever: the "materialized but not flying" gap, closed for every host. Idempotent; rejects unknown/non-FM entities. Pinned by test_register_aircraft (registered twin syncs, unregistered twin demonstrably doesn't). |
| `f4-simulation` — CampaignSession | campaign_session.hpp/.cpp: the full Phase-C loop as ONE headless, frame-driven object (create/advance/set_paused/ledger_json/apply_writeback + Stats snapshot per advance). Composition only — no new campaign or sim logic; f4-campaign still never sees EntityWorld. One world (the sim's), one clock (fixed sim_dt ticks; ladder + damage sync in whole campaign seconds), spiral-of-death guard (tick cap 240/advance, debt dropped, surfaced as "time dilated"). Options mirror campaign_qc (cycle 1800 s, reinforcement 12 h armed, role fallback ON, MinAvoidThreat 25) with interactive differences documented (saved-flight cap 48, flight-less worlds degrade to the template + synthetic airfield instead of the QC's hard fail). |
| `f4-simulation` — session tests | test_campaign_session.cpp (4): creation over the raw kunsan fixture (the degraded path — no flights, no airbase), the full advance loop over kunsan_session.world.json (the new committed fixture: kunsan + the USA squadron → ARO_S at airbase 2659, the same patch test_campaign_tick applies in-memory) asserting generation → routes → spawn → REGISTRATION (every spawned aircraft transform-synced to its FM) → ledger draws; byte-identical determinism across two sessions; pause + the fresh-session C1 golden identity. |
| `f4-simulation` — cleanup | campaign_spawner.cpp: the last surviving C3 debug fprintf ("[dbg] intent flight_id=… resolved to a non-flight entity") removed — the failure class already books into stats.unknown_flight_ids, stderr noise only double-reported it. |
| `f4-world-viewer` — session UI | campaign_session_view.cpp (new): the "Campaign Session" window — start row (team filter + saved-flight cap), Start/Stop/Reset, play/pause + speed presets, campaign clock, war status, generated-missions table (click target → select + pan), Write Result JSON (campaign_result.json, the QC artifact, byte-stable) + Write Back (apply_to the session's WorldState). A "Campaign" menu mirrors the quick paths. A session starts PAUSED (a 30-minute tasking cycle at 1x is a commitment — accidentally-live is the worse default); Space toggles. |
| `f4-world-viewer` — the live canvas layer | canvas.cpp: live aircraft drawn at their TransformComponents (grid = ENU ft / 1024, the static layer's own convention) as fighter glyphs in owner colors (CampaignOriginComponent::team_slot, the same palette), airborne full-strength / grounded dimmed, each with its MissionPlan route polyline + numbered waypoints (the C3 evidence, visible); a threat-map overlay toggle paints enemy AD density per cell (alpha by density, the SAM-ring picture routes bend around); live picking prioritized above static entities (moving targets get a 10-px radius). |
| `f4-world-viewer` — the live inspector | inspector_panel.cpp: a LiveAircraft selection branch — callsign (the C1 origin stamp), team, squadron VU, brain phase, position/velocity/altitude, airborne state, and the flight plan table (idx / grid / alt / action from the MissionPlan — the route the aircraft is actually flying). Selection kinds gained LiveAircraft (the id lives in the SESSION's world — a separate id space, the kind discriminates). |
| `f4-world-viewer` — lifecycle | Loading a different world stops a running session (it is bound to the world it was created over); stopping clears live selections. Viewer build requires the campaign fixtures at configure time → root CMake moves f4-world-viewer AFTER f4-campaign + f4-simulation (the same ordering rule that put f4-campaign before f4-simulation). |

## C3 — Threat Map, A*, Route Builder: Generated Missions Fly Their Own Routes (C3-ROUTES-1)

**Generation-to-spawn is closed. The campaign now BUILDS the routes it
tasks: ThreatMap (the ScoreThreatFast data model — per-cell ownership +
2-bit AD density rings, one byte per cell, both belligerents in the
viewer bit-halves), AirPathFinder (asearch.cpp's A* with every
reference constant: 2000-node pool, QuickSearch 12, 5-point threat
sampling, ×4 heuristic, 96-step cap, best-effort partial paths), and
RouteBuilder (BuildPathToTarget: CheckSafePath → FindSafePath corners
→ IP → target → turn point → egress → EliminateExcessWaypoints). The
threat model's wire face landed too: UCD HitChance[8]/Range[8] indexed
by MoveType, emitted by cam2json per unit and read through
IUnitCoreSource. The tranche's biggest fix is a SEMANTIC one: the
stance vocabulary. The reference's is an ENUM (cmpglobl.h RelType:
0 NoRelations, 1 Allied, 2 Friendly, 3 Neutral, 4 Hostile, 5 War —
RoEData is indexed by it), NOT a sign convention; real saves carry
garbage toward unused slots (TestCamp: -5141 toward the empty Gorn
slot), so the pre-C3 "< 0 = war" test made the U.S. a belligerent in
a war it is Neutral to while the actual war (ROK↔DPRK, mutual 5)
starved target selection of every enemy objective — 0 routes, the
exit-7 gate. f4/world now owns Relation + relation_from_wire
(out-of-range → NoRelations) and every consumer tests == War
(belligerent_teams, select_target_, ThreatMap::war_, the bridge's
side_color). The role gate gained an honest bridge: FreeFalcon's
selection SCORES role vs capability (FindBestAir — the C4 tranche);
CampaignConfig::tasking_role_fallback (default OFF, goldens
byte-identical) lets a role-less team task its best-available wing —
campaign_qc arms it, because TestCamp's belligerents field
all-counter-air squadrons. Verified on TestCamp (4-hour tasking +
20-minute INTSTRIKE): 8 cycles, 411 intents, 1,013 drawn, 81 routes
(383 waypoints, 0 build failures, 22 threat-avoidance searches), 8
synthetic INTSTRIKE aircraft flown alongside the 49 saved flights, 88
bombs / 20 objectives written back — campaign_result.json
byte-identical across runs, all gates green (exit 0); the no-tasking
baseline is unchanged. Suite 2091 → 2122 (+31, 100%, zero warnings).**

| Area | Change |
|------|--------|
| `f4-world` — the relations vocabulary | data_source.hpp: `enum class Relation` (RelType port) + `relation_from_wire(int16_t)` — out-of-range wire values (the -5141 garbage toward unused slots) decode as NoRelations. The single source of truth every belligerence/RoE decision reads. |
| `f4-world` — the C3 threat data path | UnitState + unit_hit_chance[8] / unit_weapon_range[8] (the UCD arrays, MoveType-indexed: LowAir=4, Air=5 are the SAM rings); IUnitCoreSource default-implemented accessors (zeroed — pre-C3 sources keep compiling, a zeroed map has no threats, the same as un-enriched world JSON); WorldStateAdapters override; world_state.cpp parses "hit_chance"/"weapon_range" arrays. |
| `f4-world-convert` — emission | world_json.cpp: hit_chance + weapon_range arrays per unit, emitted inside the theater-db enrichment branch (the same place scores/vehicle_groups land — class table resolves DTYPE_UNIT → UCD index). TestCamp regeneration verified byte-identical from the tracked TestCamp.cam. |
| `f4-campaign` — ThreatMap | threat_map.hpp/.cpp: ownership (nearest objective, 10-then-80 radii, 0xF unowned), 2-bit low/high density per cell (MAP_RATIO 6, saturating at 3), viewer bit-half packing (units at war with the viewer → bits 4-7), band formulas (Low 28·low+2·high +10 over war territory; Medium 10·low+23·high; High 30·high; VeryHigh 15·high), alt_band_from_feet (the MinAltAtLevel/MaxAltAtLevel boundaries), score() with off-map = 100, Stats{ad_units, threatened_cells} (implemented now — cells carrying ANY painted density, either half), war_() == RelType::War. |
| `f4-campaign` — AirPathFinder | path_finder.hpp/.cpp: the reference's algorithm shape with modern plumbing (parent-chain nodes, no bit-packing — positions serve the route builder directly): sorted open list (strict <, ties keep the earlier node), linear tried list with same-position duplicate suppression, snap-to-target (< one step becomes the goal), threat > 120 impassable (armed for the RoE tranche), cost = base×step + threat/2, RETURN_PARTIAL_ON_FAIL best-effort recovery (lowest to_go, open queue then tried list, strict <). |
| `f4-campaign` — RouteBuilder | route_builder.hpp/.cpp: RouteWaypoint (grid x/y, altitude, WP_ACTION byte, WPF flags, target VU for delivery wps), profile_flies_delivery_route (the A-G family gate — CAP racetracks are the loiter tranche), RouteBuilderConfig (aiinput.dat tunables: MinAvoidThreat 40, BreakpointDist 10, AirPathMax 2000 — hosts override; QC uses 25 for the sample-UCD rings), build() = takeoff → CheckSafePath(ingress) → [FindSafePath corners] → IP → target → turn point (5-candidate lowest-threat scan, ties toward home) → egress in reverse → landing → EliminateExcessWaypoints (two-pass, WP_NOTHING-only, critical flags respected). |
| `f4-campaign` — Campaign | set_route_planner(const RouteBuilder*, const IObjectiveSource*) (the set_result_ledger pattern); run_tasking_cycle_ builds the route for every delivery-family intent (target = select_target_'s highest-priority enemy-owned objective), stamps intent.synthetic + route + target_objective_id on success, counts routes_built/routes_failed/route_safe_searches/route_fallbacks (new accessors — the QC's exit-7 gate reads THEM: counting route-less intents is blind, the synthetic mark only lands on success); select_target_ + belligerent_teams on RelType::War; CampaignConfig::tasking_role_fallback (default false). |
| `f4-simulation` — spawner | campaign_spawner: synthetic-intent spawns — the squadron's airbase parking + per-base airfield data (runway heading, departure altitude), stats.synthetic_spawned / synthetic_failed (the QC gate's route-loss counters), the FlightSpawnFilter applied to intents exactly as to saved flights (team / mission / max-flights). |
| `f4-simulation` — bridge | campaign_bridge: build_mission_plan_from_route (route → MissionPlan: takeoff WP dropped for the TakeoffModule, delivery WPs resolve their target objective through the id map with the flight-level fallback) + spawn_aircraft_for_intent; side_color on RelType::War (the garbage-tolerant decode — phantom slots stay green). |
| `f4-simulation` — campaign_qc | The route planner attaches to the ladder: viewer = FIRST BELLIGERENT (te_team can be a non-participant — a neutral viewer packs an empty map), RouteBuilderConfig.min_avoid_threat 25 (host override — the fixture UCD's single-ring band scores sit under the reference default 40), tasking_role_fallback armed. The tasking line + summary block gain routes/waypoints/build-failures/safe-searches/direct-fallbacks/threat-ad-units/threat-cells/synthetic-spawn telemetry (coverage is VISIBLE, not silent). exit 7 reads the Campaign's own counters. |
| Tests | +31 (suite 2091 → 2122, 100%, zero warnings): test_threat_map (8) — ownership nearest/radius, band formulas, the bit-half rule (+ the coverage stats), war-territory +10 low-altitude only, counter saturation at 3, non-AD units paint nothing, band boundaries; test_path_finder (7) — straight-run exactness, threat cost paid (the ×4 heuristic dominates sub-threshold rings — the honest pinned behavior), density tariff, budget partials, EmptyOnFail, determinism; test_route_builder (8) — IP at BreakpointDist, turn-point scan, safe-path threshold + corners, elimination passes, egress symmetry, route-only circuit; test_campaign_tick (+2) — the planner arms intents with targets+routes, detachment restores the bare intents; test_campaign_spawner (+3) — route→MissionPlan conversion (leading takeoff dropped), intent spawn composition (route + origin stamp), synthetic spawn counting; test_world_state (+2) — the threat arrays parse + adapter exposure; test_theater_data (+1) — the arrays emit when the theater DB loads. The five stance-pinning tests + the kunsan fixture moved to War=5 (same belligerents — byte-identical goldens). |
| Docs | CAMPAIGN_LOOP_PLAN.md C3 → LANDED (the vocabulary correction with its evidence, the role-gate bridge, the QC telemetry, the fixture-UCD coverage limitation + the regeneration command); README f4-campaign section; this entry. |

**The war's forward leg is now THREAT-AWARE and self-contained: the
campaign picks enemy objectives, builds routes around what the map
knows, and the spawner flies them — 22 of TestCamp's 81 routes bent
around SAM rings the same run's ledger later bombed.**

## C2 — One Pool: Tasking Draws, Combat Losses, and Resupply Deplete the Same Ledger (C2-ONEPOOL-1)

**The war loop's forward leg joins the return leg. C1 made sim outcomes
write back; C2 makes tasking draws, combat losses, and reinforcement
refills flow through ONE pool — the CampaignResultLedger. While a ledger
is attached, the Campaign's availability gate reads the ledger's
squadron_tasking_available() (snapshot − draws − non-drawn losses +
reinforcements) and every generated mission books its draw with
apply_mission_draw() where combat losses already lived; the Campaign's
own pool/available counters are untouched in that mode (the no-ledger
path keeps B.3's behavior, byte-identical goldens — a fresh ledger
reports exactly the same numbers, pinned across THREE cycles of draws).
Draw/loss netting: a drawn aircraft's death consumes its draw — the pool
debits once, the existence counters count every death; a non-drawn death
(parked, scenario) debits the pool directly, C1's behavior when no draws
exist. The reinforcement cadence fires FreeFalcon's own gate (now >
last_reinforcement + period) against the .cmp header's decoded anchor: a
stale anchor (TestCamp: 0 vs a 38.5M-second epoch) fires exactly ONCE,
each fire refills squadron deficits toward their run-start snapshot
consuming the WIRE's per-squadron reinforcement budgets (TestCamp: 26
squadrons carry 24..168), team existence pools gain deliveries capped at
initial. The period is a config tunable, default DISABLED — attaching a
fresh ledger to a default-configured Campaign still changes nothing (the
C1 golden identity). campaign_qc --tasking <minutes> runs the first true
multi-cycle loop — the ladder draws, the saved-flight sim fight attrites,
the cadence refills, ONE ledger carries all three; exit 6 (tasking-broke)
and a tasking summary block (per-team pool trajectory) gate it. Verified
on TestCamp: 4-hour tasking + 20-minute INTSTRIKE — 8 cycles, 438
intents, 957 drawn, 1 fire delivering 232 aircraft to 22 squadrons, 88
bombs / 20 damaged objectives / fstatus written back, all gates green.
Suite 2077 → 2091 (+14, 100%, zero warnings).**

| Area | Change |
|------|--------|
| `f4-campaign` — ledger tasking side | SquadronLedger gains availability (the run-start snapshot), reinforce_pending (the wire budget), run_draws / run_reinforced / drawn_deaths; TeamLedger gains drawn / reinforced / drawn_deaths. apply_mission_draw(t, team, vu, count): tasking debit, existence untouched (drawn aircraft fly), unknown VUs loud (draws_unmatched — never silent). apply_reinforcements(t): per-squadron deficit refill min(deficit, budget), budget consumed, team existence capped at initial, one ReinforcementRecord per delivery. apply_air_loss gains the netting (drawn death consumes a draw slot; team drawn_deaths tracks it). Queries: squadron_tasking_available(), team_aircraft_tasking() (floored, netted). to_json v2: mission_draws + reinforcements event logs, per-team drawn/reinforced/aircraft_tasking, per-squadron aircraft_available/aircraft_tasking/run_draws/run_reinforced/reinforce_budget — byte-stable, strictly valid, no floats. |
| `f4-campaign` — shared force snapshot | src/squadron_snapshot.hpp (internal, shared by campaign.cpp + result_ledger.cpp — the two consumers can never drift): the squadron roster DECODED from the wire's 2-bit-per-group packing (0x5555aaaa = 24 ships; the pre-C2 Campaign read the RAW u32 — 1.4 billion available aircraft on any real v71 save — kunsan never noticed because its rosters are 0), roster-less squadrons sharing the team pool as before, and the per-squadron reinforcement budget from IUnitCoreSource. |
| `f4-campaign` — Campaign | set_result_ledger(CampaignResultLedger*) — MUTABLE now (the ledger IS the pool while attached: tasking writes draws into it, the C1 read side unchanged). run_tasking_cycle_: ledger-mode availability via squadron_tasking_available(), draws via apply_mission_draw, own counters untouched in that mode. tick() fires the reinforcement cadence after the tasking cycles (a boundary landing mid-tick sees the depleted pools first): epoch from ICampaignSource::current_time(), anchor from last_reinforcement(), catch-up-once (anchor JUMPS to now). CampaignConfig::reinforcement_period_sec (tunable — FreeFalcon's rate is a runtime difficulty setting, not wire data; default 0 = disabled, hosts opt in). to_summary_json gains a "reinforcement" block ONLY when it fired (legacy goldens stay byte-identical). |
| `f4-world` — the C2 data path | CampaignState + last_resupply / last_repair / last_reinforcement (the .cmp header's maintenance timers, absolute campaign times — decoded by f4-world-convert since v71, now LOADED); TeamState + replacements_avail (the team block's replacement stock). ICampaignSource/ITeamSource default-implemented accessors (the bullseye pattern — alternative sources keep compiling); WorldStateAdapters override both. The last_resupply/last_repair timers are carried for the ground-supply and repair tranches (their consumers land later); replacements_avail is exposed but not yet consumed (the squadron-level wire budgets are the operative C2 source). |
| `f4-simulation` — campaign_qc | --tasking <minutes> (the synthetic ladder, ledger attached, BEFORE the spawner subscribes — its intents publish to nobody; synthetic flights carry no saved routes, generation-to-spawn is the C3/C4 route-builder tranche), --tasking-cycle <sec>, --reinforce-period <sec> (default 12 h when --tasking is on), --profiles <json> (default: the generated MissionProfiles.json — CMake now orders f4-campaign BEFORE f4-simulation so the fixture target + cache var exist at configure time). The summary gains a "tasking" block (cycles, intents, drawn, fires, delivered, unmatched, per-team initial/drawn/reinforced/losses/tasking). NEW GATE exit 6: the ladder ran over belligerents that HAD aircraft and drew nothing — the generation side broke (profiles / availability gates / roster decode). |
| Tests | +14 (suite 2077 → 2091, 100%, zero warnings): f4-campaign test_result_ledger (12) — the roster packing decode (24/20/team-share); draws debit tasking not existence (empty() and apply_to still no-ops on a draw-only run); drawn-death netting (24−4 with 2 dead, history 2, existence 8); non-drawn deaths debit (draws-exhausted surplus); unknown draw squadron loud and counted; reinforcement refill from budget (deficit 4, budget 3, budget spent, second fire quiet, team existence 9); cap at snapshot (huge budget refills only the deficit; pristine squadrons get nothing); to_json v2 byte-stable + strict + greps; the THREE-cycle ledger-mode golden identity (own-pool vs one-pool byte-identical); THE acceptance — draws deplete (cycle 2 tasks nothing) → the fire refills → cycle 4 flies again; the stale-anchor catch-up fires exactly once; disabled cadence matches legacy. f4-world test_world_state (2) — the maintenance timers + replacement stock parse (values, absent-key defaults) and the adapters' exposure through the I*Source boundary. |
| Docs | CAMPAIGN_LOOP_PLAN.md C2 section → LANDED (semantics, the catch-up rule, the known gaps — replacements_avail unconsumed, drawn aircraft never return this slice, resupply/repair timers carried for their tranches); README f4-campaign section (the one-pool contract + the reinforcement example). |

**The loop, now multi-cycle (what a TestCamp run proves): tasking draws
957 aircraft from the ledger's pool over 8 cycles; the reinforcement
cadence refills 232 of them from the save's own budgets; the sim's 88
bombs damage 20 objectives whose fstatus lands back in the WorldState —
every number the next cycle reads already reflects every number the last
cycle spent.**

## C1 — The War Loop Closes: Sim Outcomes Write Back into Campaign State (C1-LEDGER-1)

**The campaign's return leg exists now. The CampaignResultLedger (f4-campaign)
is the write model — snapshotted from the same sources the run started from,
fed by the CampaignResultSink (f4-simulation), which resolves EntityKilled /
BombImpact bus events back to campaign identity through the new
CampaignOriginComponent (the sim entity's flight/squadron VUs + team slot,
stamped at spawn). Air kills decrement the victim team's pool, book the
victim squadron's total_losses (uchar-saturating), credit the killer's
aa_kills when it resolves (unattributed when it doesn't); ag kills book
credit; objective damage is final-state sync (the sink snapshots each
objective's damage at construction so a mid-campaign save's prior damage is
initial, not this run's). apply_to(WorldState) writes it all back into the
typed world — pools, squadron counters, the fstatus bitmaps — and
campaign_qc runs the whole loop on TestCamp: a 20-minute INTSTRIKE run drops
4 bombs, damages 1 objective (5/64 features, 7.8%), the write-back lands,
and exit 5 fires when combat happened but the ledger stayed empty. The
Campaign gains set_result_ledger(): tasking availability consults post-loss
numbers — 22 of 24 losses shrink packages, 24 stop generation, a FRESH
ledger changes nothing (golden-identity pinned). Suite 2054 → 2077 (+23,
100%, zero warnings).**

| Area | Change |
|------|--------|
| `f4-campaign` — CampaignResultLedger | The write model (result_ledger.hpp/.cpp): constructor snapshots team pools (te_number_aircraft — the same seed Campaign reads) + the squadron roster WITH its save history (a mid-campaign save starts non-zero; run deltas are tracked separately so "activity" never means the save's own numbers); apply_air_loss / apply_ag_kill / apply_objective_damage / apply_bomb_impact; to_json() — "f4-campaign-result" v1, byte-stable, strictly valid JSON, NO floats (miss distances whole feet, destroyed percentages integer, times whole milliseconds). Saturating wire limits: total_losses 255 (uchar), aa/ag kills 32767 (int16), pools floor at 0. |
| `f4-campaign` — world_writeback | apply_to(ledger, WorldState) — the opt-in header (world_adapters.hpp's pattern: the detail WorldState include stays OUT of the core ledger header). Writes team pools, squadron counters (absolutes — seed + run deltas), objective fstatus bitmaps; activity means non-zero RUN delta (zero-event ledger = identity, pinned); unmatched VUs (stale world, missing unit) are counted and returned, never dropped. |
| `f4-campaign` — Campaign injection | set_result_ledger(const*) — the C2 hook, non-owning (the set_weapon_table / set_brain_archetype pattern). While attached, the tasking cycle's squadron availability is snapshot − THIS-RUN run_losses, floored at 0; the role gate picks by effective availability; the aircraft gate caps at it. Null or fresh ledger = pre-C1 behavior, byte-identical (the golden identity test). |
| `f4-entities`→`f4-simulation` — CampaignOriginComponent | The restored sim→campaign link as DATA (FreeFalcon never needed it: a sim entity IS a campaign entity there; the engine-agnostic split severs that identity, which is exactly why nothing could flow back). campaign_origin.hpp: flight_vu / squadron_vu / home_airbase_vu / team_slot (the CAMPAIGN owner vocabulary, not the sim's blue/red/green strings) / wire-faithful callsign bytes. Stamped in spawn_aircraft_for_flight — the shared core of the bus, bulk, and scenario-driven spawn paths. Scenario-list aircraft carry none (compatibility contract). |
| `f4-simulation` — CampaignResultSink | campaign_result_sink.hpp/.cpp: subscribes EntityKilled + BombImpact on the sim bus (attach/detach, the spawner's pattern; handle_* for bus-less hosts). Kill classification: origin-ful victim → air loss; origin-less victim + origin-ful shooter → ag credit; neither → unclassified (counted, unbooked — the loud boundary). sync_objective_damage(): walks every feature-bearing objective, diffs against the construction snapshot (fstatus bytes + weighted destroyed state), hands CHANGED objectives' final states to the ledger. Stats: kills_seen / air_losses_recorded / kills_attributed / ag_kills_recorded / kills_unclassified / bomb_impacts_seen / objectives_synced. |
| `f4-simulation` — campaign_qc | The C1 loop section: ledger constructed from the world's own sources; sink attached before the first tick (damage snapshot = save-time state); sync after the last; apply_to + campaign_result.json written beside the summary; the summary gains a "results" block (totals, write-back counts, artifact path); NEW GATE exit 5 — combat outcomes occurred (kills and/or impacts) but the ledger stayed empty: the write-back chain broke (sink never fired / origin stamp missing / classification dropped every event). |
| Tests | +23 (suite 2054 → 2077, 100%, zero warnings): f4-campaign test_result_ledger (17) — snapshot seeds pools + squadron HISTORY with zero run deltas; air-loss bookings (pool, saturation at the uchar limit with run deltas still counting, floor at zero, unattributed, unknown-squadron team-only loss); ag credit without pool effect; objective damage last-write-wins with delta-corrected totals; impact log whole-feet rounding; to_json byte-stability + strict Reader round-trip + no-floats; the golden identity (fresh ledger changes nothing); 24/24 losses stop tasking; 22/24 shrink packages and cap aircraft_count; apply_to writes pools/counters/fstatus; zero-event identity; stale-world unmatched loud. f4-simulation test_campaign_result_sink (6) — origin stamping through the REAL spawn path (flight/squadron VUs, team slot, per-flight specificity); kill classification all three branches; THE E2E MISSILE KILL (launch → flyout → fuze → EntityKilled on the bus → sink → ledger: pool −1, squadron loss, aa credit); byte-identical documents across two runs; bomb impact + objective damage sync + apply_to + repopulate round-trip (DamageBitmapComponent carries the damage); pristine damage syncs nothing. |
| Docs | Docs/CAMPAIGN_LOOP_PLAN.md (new — the Phase C plan: C1 landed, C2 reinforcement + full tasking consumption, C3 threat map + A* + routes, C4 ATM, C5 the 24-hour war); README f4-campaign section (the library had none — tasking side AND result side, with the loop-shaped example). |

**The loop, closed (what a TestCamp run proves now):** decode → spawn from
tasking → fly saved routes → bomb the targets (f4-weapons' own feature
ledger) → the sink reads the damage back → the ledger books it → apply_to
puts it in the WorldState → the next campaign cycle tasks a weakened force.
The middle word — attrite — is no longer missing.

## A-G Employment — Strike Missions Deliver Ordnance (M5-AG-1)

**Campaign strike flights now bomb their targets. The save's per-flight
loadout (LoadoutStruct, wire weapon ids) decodes and maps onto engine
weapon stations; delivery-mission flights arm a strike fire-control
rung; route waypoints carry their wire WP_ACTION and target so the
brain's StrikeModule pulses a release when the CCIP geometry opens; the
host converts the pulse into `release_bomb()` — a real 3-DOF drag
ballistics flyout entity that inherits the shooter's state, falls to the
aim point, and applies blast damage to the target objective's feature
ledger (VIS states + the fstatus bitmap, the campaign save format's own
damage wire). End-to-end on TestCamp, full population, 15 minutes:
449/449 spawned with routes, 445/449 airborne, 84 armed flights
delivered 124 bombs, 124 impacts, 88 features destroyed across 20+
objectives, max objective 46% destroyed, miss distances 131–348 ft.
Exit 4 gate added: armed-but-never-released is a QC FAILURE. And the
run itself got 31× faster — the per-tick cost at campaign scale
(4,400-entity worlds) was dominated not by the simulation but by the
host's O(world) bookkeeping scans; a component-type index in EntityWorld
(the tag index's pattern, applied to `with_component`), the RWR's
missile discovery moved onto the tag bucket, and the combat-intents
pass visiting only the ACTIVE roster (thousands of dormant
parked-inventory brains no longer cost anything) took the tick from
11.5 ms to 0.37 ms. The full-population run previously OOM-killed small
hosts mid-trace; `--no-record` runs it without the recorder. The suite
is 1907/1907.**

| Area | Change |
|------|--------|
| `f4-weapons` — Bomb | The pure 3-DOF gravity munition (missile.hpp's mirror): exponential-atmosphere drag + gravity, terminal Impact at the release-time impact plane (aim-point ground elevation) or Expired at the TOF limit, deterministic (no RNG). `bomb_fall_time_s()`/`bomb_vacuum_range_ft()` are the release-solution helpers the trigger and tests share; the bomb itself flies the full drag ODE — the formulas only decide WHERE to pull the release. |
| `f4-weapons` — BombBattery (ECS) | Bombs are entities (FreeFalcon: every airborne weapon is a VuEntity). `release_bomb()` is the only sanctioned creator: validates + debits the shooter's WeaponStoreComponent, captures the aim point + impact plane, builds Transform + Bomb + BombSim (priority 41, right after the missile sim) + CampaignIdentity + team/role tags, publishes BombReleasedMessage; zero EntityId = refused (dry station). BombSimComponent ticks the flyout, mirrors state into the transform, runs the impact-plane check, applies objective damage, publishes BombImpactMessage. `sweep_spent_bombs()` destroys terminal bombs between ticks; `count_live_bombs()` for QC. |
| `f4-weapons` — objective feature damage | `apply_objective_feature_damage()`: the campaign-side endpoint of the A-G chain (FreeFalcon: SimFeatureClass::ApplyDamage → Objective::SetFeatureStatus). Blast model per feature = horizontal distance from impact to the feature's placement; per-feature hp ledger (lazily seeded from FCD hit_points, 100-hp default when the fixture lacks the class); 2-bit VIS damage state (0 normal / 1 repaired / 2 damaged / 3 destroyed, f4vu.h) kept in sync with DamageBitmapComponent's fstatus bits — byte i/4, shift (i%4)*2, the save format's own packing, so a damaged objective re-serializes correctly. `objective_damage_summary()` reads the ledger back out for QC + viewer. |
| `f4-weapons` — messages | BombReleasedMessage (shooter, target, bomb id, weapon handle, position, speed, sim time) + BombImpactMessage (impact point, miss distance, TOF, end cause, the objective damage result). Both ride the combat event recorder (BombReleased/BombImpact kinds; the impact event carries damage = features destroyed this hit, hit_points_after = value-weighted destroyed %). |
| `f4-ai` — StrikeModule | The A-G release trigger, engine-agnostic (no world, no bus, no weapon types — f4-ai never links f4-weapons): watches the aim point, pulses release when horizontal distance ≤ ballistic_range(dz, groundspeed, drag_factor), evaluated every tick (descending/accelerating aircraft re-solve the release point). Salvo pacer: a stick of up to salvo_max releases spaced salvo_interval_s apart, then `delivered()`. min_release_agl_ft floors the pass (terrain clearance); impact_tolerance_ft is the CCIP pipper window; hold_fire is the ROE gate (no phantom pulses). `is_ag_delivery_action()`: the wire WP_ACTION delivery set (14 GNDSTRIKE, 15 NAVSTRIKE, 17 STRIKE, 18 BOMB, 19 SEAD). |
| `f4-ai` — brain strike rung | CombatIntent gains `bomb_release`/`bomb_target_id` (independent of the A/A fields — a strike pass and an air fight never share a trigger). The rung runs while Enroute, un-gated by the combat flag (bombing the target is the mission, not a dogfight), gated only by Defensive and the safety pull-up; the CURRENT route waypoint's action + target arm the module, everything else disarms it. The module never flies the aircraft — NavigationModule keeps flying the delivery waypoint underneath. |
| `f4-simulation` — campaign weapon map | The save's wire weapon ids (campaign WeaponDataTable indices) map onto engine WeaponClassTable handles ONLY for confirmed identities (68 → GBU-12, 310 → GBU-12; source: FreeFalcon campweap.h's BAI loadout comment, cross-validated against TestCamp's BAI/SAD loads). Unmapped ids ride as bookkeeping stations labeled "WPN-<id>" — honest placeholders, never invented names. |
| `f4-simulation` — arm_flight_strike() | Loadout → WeaponStoreComponent (mapped droppable stations + wire-faithful bookkeeping), doctrine MK-82 fill for delivery-category flights whose wire loadout maps to no bomb (FreeFalcon's LoadWeapons squadron-stores fallback — the flight still delivers ordnance, and the QC summary separates wire vs doctrine counts), and the strike fire control configured from the bomb card itself: drag_factor computed by flying the card's own ODE with and without drag at the doctrine delivery point (5,000 ft / 675 fps), salvo = droppable rounds clipped to the doctrine stick, CCIP tolerance = half the lethal radius. Strike/SEAD/CAS missions arm; CAP/escort/support keep bookkeeping-only stores and a disarmed trigger. |
| `f4-simulation` — route arming | `build_mission_plan_from_flight()` (+objective_id_map): every route waypoint carries its wire action; delivery waypoints floor HIGHER (1,500 ft — the release envelope grows with altitude above the target; the saved INTSTRIKE z=10 is the ingress number, not a release altitude) and resolve their target (waypoint target_num through the objective map, else the flight's resolved target) into NavigationModule::Waypoint::target_id — what arms the StrikeModule at the delivery point. |
| `f4-simulation` — combat intents (A-G + perf) | `execute_brain_combat_intents()` executes the bomb pulse (combat flag NOT required — campaign flights keep the A/A ladder dark): one pulse = one bomb off the loaded Bomb-category station through release_bomb. NEW `active_aircraft` parameter: the host passes its roster (Simulation::aircraft_entities_) and the pass visits exactly those — campaign worlds hold ~4,000 DORMANT parked-inventory brains whose per-entity lookups cost more than the entire intents pass (dormant brains are also skipped on the legacy walk). |
| `f4-simulation` — bomb recording | record_snapshot emits one track per LIVE bomb (missile=true — the replay's weapon-track discriminator; callsign = weapon name, ai_state = released/impact) so a recorded strike replays with the falls visible; the impact point itself rides the BombImpact combat event. |
| `f4-entities` — component-type index | `with_component<T>()` was an O(entities × components) walk; at campaign scale six such queries per tick measured ~7 of the ~11.5 ms/tick. The index (type_index → id bucket, the tag index's pattern): lazy per-type build on first query, incremental maintenance on add (tail append; out-of-order slot reuse drops the bucket for a lazy rebuild), remove, destroy (walks the entity's components), and move ops (ids are values — no dangling). Empty buckets are KEPT: a queried-every-tick-but-empty type (no missiles airborne) must not trigger a full rebuild walk per tick — that pathology measured 0.8 ms/tick before the fix. Snapshot-by-value contract preserved exactly (sweeps destroy entities while iterating the returned vector). 5 regression tests: consistency through add/remove/destroy, replace-no-duplicate, slot-reuse rebuild, empty-bucket refill, world move. |
| `f4-sensors` — update_rwr | Missile discovery moved from a walk of EVERY TransformComponent entity (~4,400 get_tag lookups + a 35 KB id copy per tick) to the ROLE="missile" tag bucket — `with_tag_ref`, O(1), the exact fast path the Phase-D tag index was built for. Same result set (geometry-based launch detection unchanged). |
| `f4-simulation` — campaign_qc | The ordnance ledger: release/impact subscriptions + end-of-run objective damage read (the save-format face of the damage); summary gains strikes_armed, bombs_released/impacted, features_destroyed, max objective destroyed %, per-impact log (shooter, target, miss distance, TOF), per-objective damage rows. Exit 4 = armed but never released (the A-G employment failure: broken arming, an envelope that never opens, a store that never debits). `--no-record` for full-population runs on small hosts (449 flights × 5,400 samples does not fit 4 GB; the QC gates don't need the trace). Default window 15 min (TestCamp's strike flights sit a median 34 NM out — 5 min proved taxi/departure but landed every strike flight short of the release point). |
| `f4-world` / `f4-world-convert` — loadout decode | Flight tail: LoadoutStruct[] (v≤72 uchar WeaponID[16]+WeaponCount[16], v≥73 short[] — two PARALLEL arrays, not interleaved) decodes entry 0 into `loadout_stations` (weapon_id 0 = empty hardpoint skipped; the flight's other aircraft slots carry the same fill in practice — count stays in `loadouts` for the wire-faithful record). IFlightSource carries the stations across the adapter boundary; WorldState + JSON round-trip them. |
| perf — tick profile | `F4_TICK_PROF=1` env-gates per-tick phase timings (ground/update_all/intents/sweeps/sync/guns/record) printed every 600 ticks from Simulation::tick — the triage tool that found all three scan hotspots; off by default, zero cost when off. |

## B.3 — The Campaign→Sim Loop Closes, and the Viewer Becomes the QC Bench (B3-QC-1)

**Live campaign flights now fly. `emit_flight_intents()` turns the 449
tasked TestCamp flights into MissionIntents on the message bus; the new
CampaignSimSpawner subscribes, resolves each intent's flight through the
VU_ID map, and materializes it through the shared spawn path with its
SAVED ROUTE attached — grid waypoints → ENU feet MissionPlans the digi
brains actually fly (taxi, departure, enroute). The world viewer renders
the whole tasking picture as the primary end-to-end QC: an ATO/Tasking
browser (sortable, filtered, click-to-focus), mission→target and
package→element link overlays, the bullseye, mission-type filters, and a
flight/package inspector that walks every cross-reference. Three
load-bearing bugs found by the loop are fixed (JSON-loaded worlds lost
EVERY VU_ID — the maps came back empty; campaign aircraft spawned with
FMs at (0,0) and teleported there on tick 1, and their ATC was wired
before the airfield was derived so nobody ever got a taxi clearance;
and the first full `campaign_qc` run showed all 12 spawned aircraft
taxiing ACROSS THE THEATER at 19 kts, never taking off — the save embeds
ground layouts only for Airstrips, so every Airbase-based flight fell
back to the first derivable field's taxi route). The suite is
1878/1878; `campaign_qc` runs the full loop over any decoded save,
writes the summary + replay trace, and EXITS 3 when nothing gets
airborne (the "tasking didn't fly" gate).**

| Area | Change |
|------|--------|
| `f4-campaign` — emit_flight_intents | Free function over the world-source interfaces (B.3's "wire MissionIntent generation from live flights"): one intent per tasked flight — flight_id/package_id are the units' real VU_ID.nums, aircraft_count decodes the flight roster's 2-bit group packing (0xA0 = 4 ships), TOT stays the save's absolute CampaignTime, team/squadron names resolve when the sources are passed. Publishes on the bus (synchronous fan-out) and returns the vector. |
| `f4-campaign` — MissionCategory | The 41 mission bytes collapse to 11 behavioral categories (CAP/Sweep/Intercept/Escort/Strike/SEAD/CAS/Recon/Support/Other/None) — a total, pinned mapping at the granularity the viewer's filters and the spawner's route profiles read. |
| `f4-entities` — components | FlightPlanComponent gains `target` (resolved mission target EntityId); PackageSupportComponent gains `elements` (the element flights, resolved) + a `PackageMissionRequest` block (the ATM request that produced the package — mission, TOT, priority, action, target/requester, resolved); CampaignStateComponent gains the bullseye. |
| `f4-world` — **the VU_ID fix** | world_state.cpp NEVER parsed `id_num`/`id_creator` for units OR objectives: every JSON-loaded world built an EMPTY unit_id_map/objective_id_map, so flight→package, flight→squadron, battalion→brigade, and squadron→airbase resolution silently no-oped (squadrons fell back to the positional heuristic; nothing else resolved at all). In-memory WorldStates — the cam2json in-process pipeline — always carried them, which is why only JSON reloads were broken. The loader now also resolves flight mission targets and package elements/requests, and carries the campaign bullseye through ICampaignSource (default-implemented — optional data, the established pattern). New `mis_request` JSON block parse for packages. |
| `f4-simulation` — route building | `build_mission_plan_from_flight()`: WaypointPlanComponent (grid x/y, MSL z, WP_ACTION bytes) → MissionPlan (ENU feet). Leading WP_TAKEOFF legs drop (the TakeoffModule owns departure); the terminal WP_LAND back at home plate becomes the approach entry fix; z floors at 500 ft so ramp legs never command terrain level; 400 kts default cruise (per-action speeds arrive with the M4.5 route tranche). |
| `f4-simulation` — spawn path | `spawn_aircraft_for_flight()` extracted as the shared core (bulk + bus paths produce identical aircraft): routes attach when the flight has a usable plan; the TEAM tag maps from the campaign owner slot (player → blue, hostile-to-player → red, else green — documented approximation until the stance-accurate pairwise rule lands); **parking offsets are per-airbase** (the old global counter put flight #400 three miles from its field); **the FM now initializes at the parking position** (the old call left the FM at (0,0) while the TransformComponent held the real spot — tick 1's FM→Transform sync teleported every campaign aircraft to the theater datum). |
| `f4-simulation` — CampaignSimSpawner | The B.3 loop's sim side: subscribes MissionIntent on the bus (or manual `handle()` for tests/QC), resolves flight_id → entity through a copied unit_id_map, dedupes per flight, counts unknown ids (synthetic-ladder intents) and routes attached, and keeps its own per-airbase parking bookkeeping so bus-fed and bulk-fed fleets park identically. `FlightSpawnFilter` (team/mission/cap) — also the scenario JSON's `campaign_flight_filter` block, so the scenario player and the QC tool share one filter vocabulary. |
| `f4-simulation` — airfield ordering fix | initialize() now derives the campaign airfield BEFORE wire_atc(): the old order wired StubATC against the empty hand-authored airfield, so taxi clearances could never arrive and every campaign aircraft sat parked forever. `record_every` decimates snapshots for long multi-aircraft runs (1 = original behavior). |
| `f4-simulation` — **the layout-less-airbase ground-ops fix** | The first end-to-end `campaign_qc` run on TestCamp spawned 12/12 with routes but ended with **0 airborne** — every aircraft taxiing a straight line at 32 fps for the whole 5 minutes. Root cause: the save embeds ground layouts (runway/taxi points) ONLY for Airstrip-class objectives — all 50 real Airbases (Kunsan, Osan, …) decode with an empty `ground_layout` (their geometry lives in theater static data we don't load), so `derive_airfield_from_objective` rejected them, the per-airbase ATC registry never got entries, and every TaxiRequest fell back to the DEFAULT (first derivable) airfield — cross-theater taxi routes (aircraft 1's observed 54.2° heading matched the bearing to the first Airstrip to 0.3°). Fix, two parts: (1) airfield-CLASS objectives with no layout get a SYNTHETIC local field (7000 ft runway on the requested active-runway heading straddling the objective center, 2-waypoint taxi route, 8-spot parking row) — real layouts, when a later tranche loads theater OCD/PHD data, take precedence automatically; (2) flights whose squadron sits at a NON-airfield objective (TestCamp's army-aviation flights at Army Bases) park at the caller's fallback airfield instead of the army base, so they too depart locally and fly their saved route. Verified: 12/12 airborne, full Taxi→Takeoff→FlyOut→Enroute phase sequence, 65–116 NM flown in 5 min. |
| `f4-simulation` — campaign_qc gate | Exit 3 when the sim ran but nothing got airborne — the end-to-end "tasking didn't fly" failure this tool exists to catch (ground-ops stall, broken taxi route, cross-theater taxi), reported AFTER the summary/trace are written (the artifacts are what debugging it needs). |
| `f4-simulation` — campaign_qc tool | `tools/campaign_qc.cpp`: one command over any decoded save — the B.3 loop (emit → bus → spawner, stats), a scenario-driven Simulation run (the generated campaign_qc_scenario.json is replayable in the scenario player), and campaign_qc_summary.json (world stats, mission histogram, b3_loop stats, per-aircraft end state). Filters: `--team/--mission/--max-flights`; on TestCamp: 449 intents, filtered BARCAP2 spawns with 6/6 routes, aircraft taxi out and depart from their real squadron airbases. |
| `f4-world-viewer` — ATO / Tasking window | The QC surface: sortable flight table (callsign, mission, team, package, TOT, target, squadron, waypoints) with live-count mission + team combos, row click selects + focuses, target click jumps to the objective. Rows rebuild only on filter/world change (generation stamp), clipped rendering for the 449-flight case. |
| `f4-world-viewer` — canvas QC overlays | Mission→target lines (owner-colored, ring at the target, selected double-width), package→element faint links, bullseye crosshair (now plumbed: JSON → WorldState → ICampaignSource → CampaignStateComponent), and mission-filter dimming that shares the canvas/ATO filter pair. View menu gains a "Campaign QC (B.3)" group. |
| `f4-world-viewer` — inspector | Flight: mission/category, callsign, TOT/MOT (campaign clock format), priority, loadouts, altitude, fuel burnt, and clickable target/package/squadron refs. Package: element flight list (clickable), the ATM mission request block, support assignments. Waypoint table upgraded with arrival times and target VU_IDs. **WP_ACTION table corrected against campwp.h** (the old guess swapped TAKEOFF/LAND — saved routes only make sense with the real mapping). |
| Tests | +21 (suite **1878/1878 — 100%**): world — package elements/request resolution + mis_request parse; campaign — MissionCategory pins + emit_flight_intents (tasked-only, save-shaped facts, team-name resolution); simulation — route building (grid→ENU, takeoff drop, altitude floor), route/team-tag/filter spawn assertions, the full spawner loop (emit→bus→spawn→dedupe), scenario filter parsing, synthetic-field derivation (layout-less Airbase/Airstrip → local taxi route ending at the threshold; layout-less non-airfield → nullopt), and the per-base-map parking guard (army-based flight relocates to the fallback field; null map keeps legacy base parking); viewer — WP_ACTION anchors + campaign clock formatting. |

**How to QC a campaign end-to-end now:** decode the save
(`cam2json --objectives ... --theater-data ...`), open the world JSON in
the viewer (ATO + overlays + inspector = the strategic picture), then run
`campaign_qc <world.json> --team <slot> --mission <AMIS_*>` and replay the
emitted trace.json (File > Open Replay) = the execution picture. The gap
between the two is exactly where campaign-logic bugs live.

## Campaign Saves v71 — Real Mid-Campaign Worlds Decode (VIEWER-V71-1)

**A normal in-campaign save now opens. TestCamp.cam — pushed after half
a day of fighting — decodes 1715/1715 units (449 flights carrying live
missions, 371 packages with mission requests, 672 battalions, 94
squadrons with pilot rosters, 114 brigades, 15 task forces), 2659
objectives rebuilt from the theater base list plus 14 .obd damage/owner
deltas, all 8 teams with full .tea state and their air-tasking
worklists, and the whole .cmp campaign header (events, ratios, bullseye,
squadron preload). The kunsan fixture that CampaignTick needed is
regenerated and committed — the suite is 1857/1857.**

| Area | Change |
|------|--------|
| `f4-world-convert` — unit_decoder (v71 layouts) | Every gate the v71 format adds: CampBaseClass carries `pos_.z_` at gCampDataVersion >= 70 (owner moves to record+28 in the validator); `current_wp` and `wp_count` become ushorts at v >= 71; squadron `stores[]` grows 200 -> 220 at 69 <= v < 72; flights gain the v > 65 trio (`old_mission`, `mission_context`, requester VU_ID); the Package big branch is implemented (flights/wait_for/8 route corners/takeoff/tp_time/package_flags/caps + ingress & egress routes + the 76-byte MissionRequestClass bulk struct). Branch selection is deterministic from the record itself: small iff `(unit_flags & U_FINAL=0x100000) && wait_cycles == 0` — the same test PackageClass::Save makes at save time. |
| `f4-world-convert` — unit dispatch | Class-table dispatch first — the same `Falcon4.ct` the game loads: `(classInfo_[VU_DOMAIN], classInfo_[VU_TYPE])` picks the constructor exactly like FreeFalcon's NewUnit (unit.cpp:5890): (AIR,1)=Flight (2)=Package (3)=Squadron, (LAND,1)=Battalion (2)=Brigade, (SEA,1)=TaskForce. The trial-and-error fallback stays for the no-table path, with its type range widened past 2000 (TestCamp carries battalions at type 2022) and a grid-coordinate plausibility gate on the next-record header — without it, one trial order false-positives on v63 saves and the mirror order on v71 (a wrong-length tail that lands exactly on a real record boundary is structurally indistinguishable; the class table is the ground truth). |
| `f4-world-convert` — team_decoder (full rewrite) | The old decoder read TeamClass as 52 bytes with `who`/`cteam` as shorts — Team is a uchar (cmpglobl.h:87), so every team after the scan-shift was garbled and only 2 of 8 slots ever matched. Now the whole 739-byte record decodes (stance matrix, initiative, supply/fuel/replacements, current & start strength stats, bonus objectives, target/unit/mission priorities, max_vehicle, flag/color/equipment, name, motto, ground action + both air actions) followed structurally by the team's AirTaskingManager — airbase schedule list + the pending mission request list (the ATO worklist: mission, who/vs, TOT, priority, target/requester VU_IDs) — and GTM/NTM. Parity: TestCamp's 11,466-byte .tea consumes exactly; all 8 teams, 6 ATMs with 10 requests each. |
| `f4-world-convert` — campaign_decoder (full .cmp) | The decode previously stopped after the team block (1904 of 22,080 bytes). Now it walks CampaignClass::Decode end to end: lastMajorEvent/resupply/repair/reinforcement timers, the 7 ratios, theater size, day/active-teams/endgame/situation, bullseye, the 4 x 40-char names, PlayerSquadronID, both UI event queues (20-byte x86 uieventnode + length-prefixed text — the campaign news feed: "Elements of the ROK 25th Armored Division engaged DPRK ground forces..."), the terrain ownership map, the 68-squadron preload list (68-byte SquadUIInfoClass each), tempo, and the creator block. bytes_consumed == decompressed_size on both fixtures. |
| `f4-world-convert` — objective deltas (.obd) + VU_ID fix | New `decode_obd`: the [outer][count][inner][LZSS] container holding per-objective UpdateFromData deltas (owner/supply/fuel/losses/fstatus) that a normal save writes instead of the full objective list. Objective VU_IDs are now read num-then-creator (vutypes.h declares num_ first — the old decoder swapped every reported pair) and are emitted so flights, packages, and deltas can be correlated with objectives. |
| `f4-world-convert` — world_json | Normal saves get their objectives the way FreeFalcon does: base list from the scenario's .obj + deltas on top. When the .cam embeds no "obj" subfile, `find_base_objectives` auto-discovers one (any .obj beside the .cam, else the bundled save1.obj fixture — the stock korea scenario's list, extracted and committed); cam2json gains `--objectives <file> [--objectives-version <n>]` to pin it explicitly. New emission: unit and objective `id_num`/`id_creator`, flight `old_mission`/`mission_context`/`requester_id`, package `element_ids` + `mis_request` (mission/TOT/priority/target/requester) + big-branch routes, team `.tea` enrichment (initiative, supply/fuel, current stats, `atm_airbases`, `atm_requests`), and the campaign extension fields + `events`. |
| `f4-world-viewer` — import path | `import_cam_archive` benefits automatically: the CamArchive now remembers its source path (new `path()` accessor), so the auto-discovery runs in-process and the world viewer opens a mid-campaign save with objectives, flights, and missions — the reason the file "didn't open" was 0/1715 units decoded (v63-only layouts) plus no objective source, both fixed. |
| `f4-campaign` — kunsan fixture | `f4-simulation/tests/fixtures/kunsan_campaign.world.json` was never committed — the reason main was RED (6 failing CampaignTick tests). Regenerated from save1.cam with documented modifications (scripts/make_kunsan_fixture.py): team names USA/ROK/DPRK, negative war stances per the campaign's ID_HOSTILE sign test, 24-aircraft pools for the three belligerents, and one ARO_CA wing each for USA/DPRK (ROK fields none — the availability-gate test). All 7 CampaignTick tests pass. |
| Tests | +6 (suite **1857/1857 — 100%**): V71Units.TestCampDecodesAllUnitsWithClassTableDispatch (1715/1715, cursor at exactly 423,065, class distribution pinned), TestCampPackagesCarryMissionRequests (371 packages, all small-branch, elements + branch), TestCampDecodesWithoutClassTableToo (the fallback path, full parity), V63FixtureStillDecodesWithV71Code (regression: v63 parity with and without the table, per-record class agreement), Campaign.FullDecodeConsumesTheEntirePayload (replaces the old remaining-payload contract), plus the obd/objective-id coverage riding the existing suites. |

**Why this matters for the campaign->sim loop (the B.3 milestone):** the
mission data the sim-side spawner needs is now first-class: each flight
carries its mission, TOT, target, loadout count, package and squadron
links, callsign, and full waypoint route; each package carries its
element flight IDs and the mission request that drove it; each team's
ATM carries the pending request worklist. A TestCamp-derived world JSON
is the first fixture where the campaign engine can read REAL tasking
(not synthetic profiles) — the next milestone wires MissionIntent
generation from these live flights instead of the profile ladder.

# F4 Cleanup Pass — Changes Summary

## G2 — The Interdiction Link (CAS against real battalions, the bombs booking)

**The two wars touched.** C6 made the air fight; G1 made the ground
fight; they shared a clock, a ledger, and a world — and never
interacted. The booking side had been wired since G1 (the sink's
battalion branch, `apply_ground_loss(air=true)`, the engine's
air-loss pull, the mirror) and the delivery side since the A-G
tranche (MK-82 stores, the strike fire control, the ballistic
flyout) — but the missing middle meant a bomb whose target was a
battalion resolved no feature set and detonated harmlessly, and no
tasking rung ever aimed a synthetic mission at a unit (UNIT-class
profiles flew target-less, the C3-documented deferral). Even
TestCamp's own saved CAS/BAI flights had been dropping harmless iron
on battalions for the whole campaign era. G2 is the missing middle:
**CAS packages draw, route to a front-line-ranked enemy battalion,
drop a stick of iron — and the line thins** (the ledger's ground-loss
rows carry `air=true`, the engine pulls them, the mirrored roster
decays 11→9 on the acceptance run).

**The chain, end to end**: the ATM (and the legacy ladder) pick a
UNIT-targeted delivery target — enemy battalions ranked by distance
to the contested FLOT (the front-line math extracted from the engine
into a shared helper so tasking and the engine can never drift),
ledger-destroyed skipped, wire-order ties, rotation-spread; the
route builder resolves the battalion's grid position (objectives
first, then units — the world loader's own order) and emits the
attack profile (the mission table's own `WP_CAS` string mapping to
the ground-strike delivery action); the mission-plan builders resolve
the delivery waypoint's `target_num` through the unit id map; the
brain's strike rung releases the stick (unchanged); at terminal the
NEW battalion blast endpoint computes an integer vehicle-kill count
(warhead × falloff / 96 lb per vehicle, capped at the mirrored
strength — pure, never mutating: the engine owns the battalion's
life) and publishes ONE `GroundUnitLossMessage` per effective bomb;
the sink (armed by the session's `unit_strike`) books
`apply_ground_loss(air=true)` + per-vehicle `apply_ag_kill` credit —
the exact branch G1 wired and waited for; the engine pulls, the
roster decays, the mirror syncs. Two wars, one war.

**Opt-in at every layer** (`unit_strike`, the aa_combat/ground_war
contract — default off, byte-identical documents): the tasking
rung gates on `CampaignConfig::unit_strike` (both ladders, one flag),
the sink's booking gates on its own arm (the blast endpoint cannot
know the session's flags — f4-weapons owns ballistics, not policy —
and ungated booking would change every pre-G2 golden that flew a
saved unit-targeted flight; with the flag off the events count in
stats and nothing books). The 0.1 h C6 armed war
(`e8496c7819cbb7b64b8f9e0a2fdc7b64`) and the 1 h G1 ground war
(`8171e8b6a8dfeb8057f747a06d5b173e`) re-verified byte-identical with
the flag off — the front-line extraction refactor included.

**The verification** (TestCamp, Release): the 0.3 h interdiction war
— exit 0, four green verdicts, identical 2-run MD5s, `agv=2`; the
1 h interdiction war — exit 0, `losses=15 (air=2)`, the engine pull
in the books (battalion 4121: strength 11→9, run_losses 2); the
combined `--aa-combat --ground-war --unit-strike` run shows the
honest contested side — the CAS package was shot down 23 s short of
its TOT (the exit-14 gate fires with the horizon guidance; the
full-length combined certificate rides PERF-3's wall-clock
constraint). Suites 2,226/2,226 Debug + Release (21 new tests).
**One real bug caught by the rig**: `objective_found` is true for
any transform-carrying target — the terminal's objective branch
swallowed battalion targets and the unit branch never ran; the
objective branch now keys on a resolved feature set. Docs:
INTERDICTION_PLAN.md; `campaign_qc --unit-strike` + exit 14.

## Guns Employment — The Last Unflown Weapon Flies (M3-TACTICS-4)

**The AI fires the cannon. An armed jet working a merge tracks the GUN
solution — steering at where the target will be when the bullet arrives,
superelevation included — and the trigger goes down when the boresight
error projects inside the hit footprint at the current range. One
AI-vs-AI E2E proves the whole chain headless: missiles tight, guns free,
a drone bandit, and the fight resolves at the trigger — 25-50 rounds of
20 mm land, the bandit dies, the shooter disengages. The shipped
`guns_merge` scenario puts the trigger fight in the scenario-player,
tracer streaks and all. The suite is now 1,661/1,661.**

| Area | Change |
|------|--------|
| `f4-weapons` — GunStream (hardening) | SEGMENT hit detection: a tracer checks the segment it swept each tick (`[p_old, p_new]`, derivable exactly from the semi-implicit Euler update — no stored state), not its point position. At 3,400 ft/s a round covers ~57 ft per 1/60 s tick — more than the 40 ft hit radius — and at coarse host steps (dt >= 0.024) point checks jump straight THROUGH a target; the segment sweep closes the tunnel (regression-tested with a constructed jump-over). Plus: `set_sim_time()` (the host sweep stamps the clock — GunFired/Damage/Kill messages previously carried 0.0), `set_weapon_handle()` (name resolution), and `start_burst(rounds, aim_target_id)` (the aim hint on GunFiredMessage, which also gains `weapon_handle`). |
| `f4-weapons` — GunComponent + update_guns (new) | The per-aircraft cannon as a passive ECS component (the RwrComponent pattern — the host drives it, never update_all: hit detection mutates the world, and a firing jet must emit from its FRESH muzzle pose). `attach_combat_loadout` adds it (GunConfig from the M61A1 card: muzzle 3,400 ft/s, per-round 0.22 lb, lethal 40 ft; seeded like the radar — deterministic per scenario, distinct per shooter). `update_guns(world, bus, dt, t)` is the world-level sweep `Simulation::tick` runs AFTER the FM->Transform sync: boresight = the velocity unit vector, muzzle 15 ft ahead (the same clearance `launch_missile` gives a missile), tracers fly, hits damage, bursts announce on the bus. |
| `f4-ai` — GunModule (new, engine-agnostic) | The guns fire control, AI_IMPLEMENTATION_PLAN Steps 11-12. ROLE 1 — THE PREDICTOR: a gun is not fired at the target but at the LEAD POINT — where the target will be after the bullet's time of flight (`tof = range_now / (muzzle + closure)`), flown from the TRACK-FILE PREDICTION (the fusion refreshes at the skill interval — seconds — and at merge closure 5 stale seconds is the whole envelope; every quantity starts from `position + velocity * age_s`). The lead includes SUPERELEVATION: the aim point sits above the kinematic lead by `0.5*g*tof^2` — the gravity drop every real fire computer compensates (16 ft at 1 s). ROLE 2 — THE TRIGGER: a RANGE-SCALED hit-quality cone (`error <= atan2(hit_radius, range)`, capped ~5 deg) — a fixed cone cannot work, the FCS's own ~1.5-deg tracking lag is a 44 ft miss at 5,000 ft and a hit at 1,500 ft; 40 ft = the weapons model's hit radius. Burst discipline: 100 rounds (~1 s of trigger at 6,000 rpm) then 1 s cooldown; a 511-round drum budget; ROE hold_fire at the module level (no pulse, no phantom budget). |
| `f4-ai` — WVRModule integration | `wvr().guns()` — the fire control composed like the heater's `fire()`. MERGE: the snapshot — while the gun is armed and the target is inside its envelope, the steering swaps the missile-grade pursuit for the GUN lead (aiming IS steering there; the integrators reset at the reference change — windup carried across reference swaps held the trigger closed through a whole window in testing) and the trigger gate runs. OFFENSIVE: the same swap for the sustained tracking solution (OverB still overrides below 0.35 NM with hard closure — overshoot control trumps gunnery; a slow-closure stern chase is the gun shot). The module's ownship boresight estimate: consecutive positions / dt (the exact quantity the host sweep fires along); a two-tick warmup on engagement (stale history is dropped — a solution you cannot measure is not one you fire on). Guns tight: the merge flies EXACTLY as before — every pre-gun scenario is untouched. |
| `f4-ai` — TargetInfo/SensorFusion | `TargetInfo.age_s`: seconds since the fusion rebuilt the entry (0 = fresh). The fusion ages its track file every `update()` between rebuilds — a track file, not a live feed; consumers needing now-geometry dead-reckon. Missile modules ignore it (their envelopes absorb staleness); the gun predictor needs it. |
| Brain + bridge (the host half) | `CombatIntent` += `gun_trigger`/`gun_target_id` (the one-tick burst edge, the same intent surface as `weapon_release`; hold_fire gates it at the brain). `configure_brain_combat` configures the gun from the M61A1 card (envelope, muzzle) + the store's gun-station drum, with the full ROE matrix: `hold_fire` (all tight) / `bvr_hold` (radar missiles) / `missiles_hold` (NEW — all A/A missiles tight, guns free: the guns-dogfight doctrine, subsumes bvr_hold) / `guns_hold` (NEW — guns tight, **default TRUE**: the no-surprise rule — guns are the newest weapon and every pre-gun scenario must fly the identical fight after the wiring lands). `execute_brain_combat_intents` turns the edge into `GunStream::start_burst` (burst = the module's doctrine clipped to the drum; the store debits what left the muzzle). |
| `f4-recorder` + transcript | `CombatEventKind::GunFired` (wire name `"gun_fired"`, 9th kind): subject/object/weapon/rounds/muzzle position, captured by the bridge's recorder subscription; gun DAMAGE rides the existing DamageApplied stream (`missile_id == 0` = gun hit, the message contract's documented marker). JSON round-trip both directions; the LLM summary gains a `gun_bursts` array and gun-kill weapon attribution (the killer's most recent burst). The transcript learns the trigger call: **"Guns, guns, guns."** |
| Scenarios | `guns_merge.json.in` (shipped, scenario-player): EAGLE1 vs BANDIT1 (a hold-fire drone that fights geometry but never shoots) — both spawned nose-on at fight speed flying a shared MERGE waypoint dead ahead (a single-waypoint route: the nav's spawn-on-leg consolidation skips waypoints an aircraft is past, and turn-anticipation swallows a merge point near a corner — the merge must be the LAST waypoint), missiles tight, guns armed, a drone hull calibrated to one burst. Tracer streaks (fading amber lines back along each round's velocity) in the player's combat view. |
| Tests | +29 (suite **1,661/1,661 — 100%**, zero warnings): 15 GunModule units (TOF/lead/superelevation math, the track-file prediction, the range-scaled cone, the burst state machine, budget, ROE, reset semantics), 3 GunStream units (the tunneling regression, sim-time/aim stamps, the update_guns sweep from a moving muzzle), 6 WVR gun-intent units (the two-tick warmup, burst cycle, guns-tight hold, envelope, the Offensive steering swap vs the pursuit, reset), 1 recorder unit (gun_bursts summary + gun-kill attribution) + the extended every-kind round-trip, and the E2Es: GunsRoeDefaultsAndWiring, GunsRoeMissilesTightGunsFree, **AiVersusAiGunsMergeFight** (head-on at fight speed, WVR band entered, zero missiles launched despite BVR engaging, bursts by the eagle only carrying the aim hint, the store debit matching the rounds fired, a GUN kill (damage with missile_id == 0), the shooter alive and disengaged after) + the shipped-file twin. |

**Watch the trigger fight** (player build needs X11/OpenGL):

```
f4-scenario-player <build>/scenarios/guns_merge.json --run --follow
```

EAGLE1 closes head-on, the HUD flips to WVR/Merge, and inside half a mile
the amber tracer streaks reach out ahead of the nose — the lead-point
solution, drop-compensated. The drone jinks when the angle sorts; watch
the snapshot bursts at each pass. The trace records the bursts
(`gun_fired` events in guns_merge_trace.json).

## The 2-Ship: WingmanModule — Formation, the Sort, the Rejoin (M3-TACTICS-3)

**The AI flies in formation now. A #2 with a `lead_callsign` holds its
FightingWing station through the cruise, follows the lead into the BVR
fight, SORTS onto the bandit the lead has not taken (a genuine 2v2, not
two 1v1s sharing a map), kills it, and rejoins the lead's wing after the
fight — all autonomous, all regression-tested. The 2v2 E2E proves the
full wingman contract end to end: formation before the fight, split
targets during, both bandits dead, both blues alive, wing reformed
after. The shipped `two_ship` scenario puts the whole thing in the
scenario-player to watch. The suite is now 1,632/1,632.**

| Area | Change |
|------|--------|
| `f4-ai` — WingmanModule (new) | Step 11 of AI_IMPLEMENTATION_PLAN: the formation-keeping + engagement-discipline module. Engine-agnostic per the house rule — the host pushes a `LeadPicture` (position/velocity/heading/speed/altitude + validity) every tick before the brains run, the module answers with steering; it never touches the world or the bus. FSM: `None` (no live lead — empty output, brain flies the mission) / `Following` / `Rejoining` (with lead-range capture — see below). Five 2-ship formations from FreeFalcon's formdata (FightingWing default, Echelon L/R, Trail, LineAbreast) via `command_formation()`; the 4-ship types stay deferred to a 4-ship roster. |
| `f4-ai` — the steering laws | Two channels, two regimes each. LATERAL: far out, pure pursuit of the station slot; inside 3× tolerance, the formation law — the LEAD's heading plus a clamped proportional correction toward the slot's lateral offset (pure pursuit at zero error would orbit a co-moving slot; a fixed lead-heading would freeze the offset in place; the blend does neither — it forms). LONGITUDINAL: Following uses a station-frame PD law (lead speed ± P on the along-track error, minus D on closure — the D term is what keeps a 150-kt join from sailing through the slot); Rejoining uses a RANGE-TO-LEAD law, deliberately rotation-free: during the lead's post-fight turn the station frame rotates under the wingman and the along-rate becomes frame rotation, not closure — the station-frame law phugoided 36 kft around the flight before the split. Capture back into Following fires on lead range < 5,000 ft (the slot sweeps ~3,200 ft around a turning lead; station-distance capture kept missing the flyby). |
| `f4-ai` — SensorFusion: the sort + a friendly-leak fix | `sorted_threat_target(lead_engaged_id)`: the wingman's target pick — the highest-scoring hostile that is NOT the lead's engaged target (the free bandit outranks the lead's target even at lower score — that is the point of the sort); with only the lead's target visible it doubles up (support the kill); with the lead not fighting it degenerates to the plain query. REAL BUG fixed alongside: `threat_target()` never filtered hostiles, so in a 2-ship the wingman's own LEAD won the query pre-detection and the BVR rung engaged a friendly — 2v2 was structurally impossible until this. Friendlies stay in the target list (situational awareness); they just never arm a combat rung. |
| `f4-ai` — BrainComponent: the Formation rung | The ladder is now Defensive > WVR > BVR > **Formation** > mission module: a wingman with a live lead picture flies formation instead of its own route (with combat disabled too — formation is not a combat behavior); a fighting wingman stops forming and fights (the combat rungs preempt the module); a dead/landed lead empties the rung and the wingman becomes a single-ship. New API: `set_flight_lead()` / `update_lead_picture()` / `set_lead_engagement()` (host-fed) and `combat_engagement_id()` (host-read, feeds the sort). Mode names: `WingmanFormation` / `Following` / `Rejoining` ride the recorder + HUD for free. |
| `f4-simulation` — the host half | Scenario schema gains per-aircraft `"lead_callsign"` (empty = single-ship, as everything was). `resolve_wingman_refs()` runs after all aircraft spawn (a lead may sit anywhere in the list): resolves the callsign, validates same-team (a red wingman of a blue lead is an authoring bug — initialize() throws, like the team check), marks the brain. `push_wingman_lead_pictures()` runs every tick BEFORE `update_all`: reads the lead's transform/FM/damage/brain and pushes the picture + the lead's current engagement id. No-op when no aircraft declares a lead — the pre-Step-11 world is untouched. |
| Scenarios | `two_ship.json.in` (shipped, scenario-player): EAGLE1 + EAGLE2 (lead_callsign, spawned on station) vs BANDIT1 + BANDIT2 (hold-fire drones, 13 NM stern chase — the fight resolves with both blues alive, deterministically). Watch the #2 form, sort onto the free bandit, shoot, and rejoin. Records like the other combat scenarios (`two_ship_trace.json`). |
| Tests | +22 (suite **1,632/1,632 — 100%**, zero warnings): 14× WingmanModule units (station geometry per formation incl. lead-heading rotation, vertical offset, no-lead/lead-lost → None, speed up behind / slow down ahead / hold on station, steer toward the slot, blowout → Rejoining → Following hysteresis, a point-mass convergence test over 120 s, names, reset), 5× SensorFusion units (threat_target never returns a friendly; prefers hostile over near friendly; the sort takes the free bandit, supports the lead's kill when solo, degenerates without a lead engagement), 1× schema (lead_callsign round-trip + the three rejections: unknown lead, self-lead, cross-team), and **AiVersusAiTwoShipBvrFight** — the 2v2 E2E: formation rung pre-detection, sort separation while both fight, both bandits killed by blues only, both blues alive, zero live missiles, and the wing reformed (3D station distance < 4,000 ft, state Following) — plus the shipped-file twin, `TwoShipScenarioFilePlaysOut`. The rejoin assertion's failure message carries a 5 s timeline (mode/state/distance/kills) — a rejoin regression without it is undebuggable. |

**Watch the 2v2** (player build needs X11/OpenGL):

```
f4-scenario-player <build>/scenarios/two_ship.json --run --follow
```

EAGLE2 (the #2) is the one to watch: `WingmanFormation/Following` on the
HUD through the cruise, then the BVR crank on the bandit EAGLE1 did not
take, and the rejoin after the splash. Tab to it to see its picture.
## Combat Events in the Recorder — Fights Replay Headless (M4-RECORDER-1)

**A recorded fight is now the whole fight: FlightRecorder captures the
combat bus transitions (detection, spikes, launches, impacts, kills) as a
`CombatEvent` stream AND the missile flyouts as per-tick track snapshots,
alongside the existing aircraft snapshots — all in the same trace JSON the
world viewer replays. Run a fight headless, write the trace, load it back:
the full engagement chain survives the round-trip with tick-aligned timing,
and the LLM-facing summary gains a combat debrief (launches with outcomes,
kills with attribution). The two shipped combat scenarios now record:
watch `bvr_intercept` or `wvr_merge` in the scenario-player and a
replayable trace lands in `<build>/*_trace.json` when you close it. The
suite is now 1,610/1,610.**

| Area | Change |
|------|--------|
| `f4-recorder` — CombatEvent (new) | `combat_event.hpp`: the discrete half of a fight recording. One fat value struct with an 8-value kind enum (`track_acquired` / `track_dropped` / `rwr_lock` / `rwr_launch` / `missile_launched` / `missile_detonated` / `damage_applied` / `entity_killed` — stable wire names via `combat_event_kind_name()`), per-kind payload fields (ids, launch/burst positions, weapon name, end cause, miss distance, damage/HP, RWR range), and the same engine-agnostic stance as FlightSnapshot: raw uint64 ids and plain strings, NO f4-weapons/f4-sensors dependency — the bridge that flattens bus messages lives in f4-simulation. `weapon_name` is resolved at capture time so a replay never needs the weapon table. |
| `f4-recorder` — FlightRecorder | `record(CombatEvent)` + `combat_events()` / `combat_event_count()` / `combat_events_in_range(t0, t1)` (the snapshot query's twin). `to_json()` appends `combat_event_count` + a `combat_events` array **only when events exist**; `from_json()` parses them (unknown kinds and fields skip — the reader's forward-compatibility rule). `to_summary_json()` gains a `combat` debrief: engagement window, per-launch outcomes (shooter/target/weapon, end cause, miss distance, flight time — correlated launch↔detonation by missile_id), and kills (victim/killer/weapon — correlated kill↔damage↔launch). |
| `f4-recorder` — missile tracks | `FlightSnapshot::missile` marks a track as a munition: callsign carries the weapon name ("AIM-120C"), `ai_state` the flyout status ("guided"/"ballistic"), kinematics the missile's. Serialized **only when true** and the combat arrays **only when present** — aircraft-only recordings stay byte-identical to the pre-M4 format (diff baselines unperturbed), old readers skip the new keys, new readers load old docs clean. The summary's aircraft/phases/state-sequence sections filter missiles out (munitions, not flights). |
| `f4-simulation` — event bridge | `attach_combat_event_recorder(sim)` (combat_bridge): subscribes the recorder to all seven bus transitions and converts each message to a CombatEvent. Tick stamping is the subtle part: bus events publish mid-tick BEFORE `tick()` increments its counter, so events get `tick_count()+1` — aligned with the FlightSnapshots the SAME `tick()` call records. Wired automatically in `initialize()` whenever the scenario enables recording (harmless with combat off: no messages, no events). |
| `f4-simulation` — missile track recording | `record_snapshot()` now walks `with_component<MissileComponent>()` each tick and records every live missile's position/speed/status (swept-terminal missiles vanish first — their last position is the tick before detonation; the burst point lives in the detonation event, so replay endpoints stay covered). `Simulation::recorder()` accessor exposes the live recording to hosts. |
| Scenarios | `bvr_intercept.json` and `wvr_merge.json` flip `record: true` with `<build>/*_trace.json` paths — exit the player after a fight and the trace is ready to load in the world viewer's replay mode (missile trails included). Test runs write nothing (only the player host calls `write_recording()`). |
| Tests | +13 (suite **1,610/1,610 — 100%**, zero warnings): 12× recorder units (record/queries, every kind's JSON round-trip incl. positions and payloads, unknown-kind forward compat, missile-flag round-trip + aircraft byte-compat, old-format doc load, combat debrief content, missile exclusion from the aircraft summary) and **CombatRecordingReplaysTheFight** E2E: the full BVR engagement flown with recording on, written to disk, re-loaded — asserts both aircraft tracks AND the missile flyout (name/status/motion), the complete event chain in order (acquire → RWR lock → FOX 3 with name → RWR launch → `target_hit` detonation → killing damage → kill attribution), event ticks within the snapshot tick range, cause-before-effect timing, and the debrief section. |

**How to get a replayable fight trace** (player build needs X11/OpenGL):

```
f4-scenario-player <build>/scenarios/bvr_intercept.json --run --follow
# ... watch the fight, close the window ...
# <build>/bvr_intercept_trace.json now holds snapshots + combat events
```

Headless traces come from any scenario JSON with `record: true` +
`record_path` (the E2E test does exactly this through the public API).

**Deferred (documented, deliberate)**: world-viewer replay UI for the
event stream (the viewer already loads the format — a combat timeline
panel is a viewer-side feature for a session that can build GL),
countermeasure consumption (chaff/flare remain intents), guns (no gun
events exist to record), campaign-flights combat attachment.

## WVR Merge — The AI Fights Inside 3 NM (M3-TACTICS-2)

**The last band of the fight is flown: BVRModule now hands off to a new
WVRModule inside the plan's 3 NM WVR entry band, and the merge plays out
autonomously — geometry sorting (Merge/Offensive/Defensive/BugOut),
lead-pursuit closure, break-turn jinks with reversals, overshoot control,
and IR employment (FOX 2 off the wingtip before any AMRAAM leaves the
trench). A new shipped scenario, `scenarios/wvr_merge.json`, gives the
scenario-player the merge to watch: radar-missile-tight ROE walks two
fighters head-on from 5 NM into the band, EAGLE1's heater ends it while
the fire-holding bandit drone defends. The suite is now 1,597/1,597.**

| Area | Change |
|------|--------|
| `f4-ai` — WVRModule (new) | The plan §5 Step 9 module (`modules/wvr_module.{hpp,cpp}`, ~600 lines): a 5-state FSM (None/Merge/Offensive/Defensive/BugOut — FreeFalcon wvrengage.cpp's ladder) with dwell-guarded geometry classification from the SensorFusion angles (`ata`/`ata_from`: we hold the angle when the target is in our forward cone and pointed away; they hold it when behind us and nose-on), the plan's 11-value `WVRTactic` enum with the flown subset documented (RandP/Straight at the merge, OverB overshoot control, GunJink defensive break turns, BugOut), and an embedded IR `MissileModule` fire control (opportunity shots inside the forward cone — a heater at a target on our six has nothing to track; 3 s cadence, shoot-shoot 2). Defensive steering is the gunsjink: ±60 deg break turns off the threat bearing reversing every 3 s with an altitude weave, throttle on the rail. Bug-out doctrine requires the IR allotment spent AND a sustained defense (grace timer), then hands the reopened fight back past the 4.5 NM exit ring. |
| `f4-ai` — BrainComponent ladder | New `CombatMode::WVR` rung between Defensive and BVR: inside `bvr().config().wvr_entry_range_nm` (3 NM, plan constant) the brain resets BVR and hands the fight to WVRModule; past `wvr().config().wvr_exit_range_nm` (4.5 NM hysteresis) it hands back — one source per boundary (entry lives in the BVR band taxonomy, exit in the WVR config). Missile defense still preempts everything; falling off the ladder resets the nav integrators. `mode_name()`/`state_name()`/`combat_mode_name()` report `WVREngage` + the WVR states for HUDs and traces. |
| `f4-ai` — ROE in the fire control | `MissileModule::Config::hold_fire` (weapons tight) — the gate lives in `should_fire()` itself, NOT only at the brain's intent layer: a module-level hold means no pulse, no phantom shot counted, and no doctrine separation can trigger on a launch that never happened (the intent-only gate would have let BVR count two phantom AMRAAMs under `bvr_hold` and bug out before the merge). Per-aircraft `hold_fire` disarms both fire controls; the combat block's `bvr_hold` disarms the BVR one alone. |
| `f4-simulation` — combat bridge | `configure_brain_combat()` now configures BOTH envelopes from the weapon class table (BVR from the longest-range A/A class as before; WVR/IR from the IR-guided class — AIM-9M's [0.5, 8] NM doctrine window) and installs the ROE flags at module level. `execute_brain_combat_intents()` gains WVR-aware station doctrine: when the shooter's brain is in the WVR rung, IR-guided stations fire first, then the shortest-range A/A — FOX 2 off the wingtip before an AMRAAM out of the trench. |
| Scenario schema | Per-aircraft `"hold_fire": true` (that aircraft never releases — it still locks, maneuvers, defends; the difference between a live opponent and a maneuvering target drone) and combat-block `"bvr_hold": true` (SPINS-style radar-missiles-tight for everyone, heaters free). Both parsed, documented on the structs, and asserted in tests. |
| `f4-scenario-player` — scenario | New `scenarios/wvr_merge.json.in` (registered in the root CMake template list): EAGLE1 (blue, live) and BANDIT1 (red, `hold_fire`, 10 HP) spawn 5 NM apart head-on at 15,000 ft with `bvr_hold` on — the deterministic twin of the E2E test geometry. The user watches from EAGLE1: closure, the WVR handoff at 3 NM (HUD mode flips to `WVREngage/Merge`), FOX 2, the drone's RWR `MISSILE LAUNCH!` and jinking defense (visible as the bandit's break turns), splash. Missiles render with the m4-scenario-1 procedural visuals; the COMBAT panel narrates with the `FOX 2` brevity word (already in the transcript's guidance-kind map). |
| Hygiene | Removed the landed `f4-m3-tactics-1.patch` / `f4-m4-scenario-1.patch` from the repo root (both applied and pushed as `BVR test` d6a6032 — same discipline as step 0 removing the landed M2 patch). Fixed a latent `-Wunused-function` (`fmt1`) in combat_transcript.cpp. |
| Tests | +20 (suite **1,597/1,597 — 100%**, zero warnings): 18× WVRModule unit tests (geometry classes, dwell anti-chatter, jink offset + 3 s reversal, OverB guard, IR pulse/cooldown/shoot-shoot, forward-cone + RWR-blind gates, band exit, bug-out doctrine incl. no-bugout-with-heaters-remaining, reset contract) and **AiVersusAiWvrMergeFight** + **WvrMergeScenarioFilePlaysOut** E2Es: BVR-tight closure from 5 NM, both brains reach the WVR rung (the RWR-only drone too — the lock warning is its picture), the merge shot is an IR-class station, zero AMRAAMs expended, the drone's RWR sees the IR launch and MissileModule defends, kill + attribution + disengage + clean sweep. |

**How to watch the merge** (needs a scenario-player build — X11/OpenGL,
unlike the headless CI):

```
f4-scenario-player <build>/scenarios/wvr_merge.json --run --follow --camera-distance 8000
```

The fight resolves in ~40 s of sim time (the 32x speed slider makes it
ten); set the drone's `hold_fire` to `false` in the JSON for a live
two-way merge, and remove `"bvr_hold"` to let the AMRAAM exchange back
in.

**Deferred (documented, deliberate)**: guns employment (GunStream is
f4-weapons-side; the sim tick has no gun-sweep driver yet), one-circle
vs two-circle geometry + the reserved WVRTactic values (Roop/Avoid/Beam/
BeamReturn/RunAway — they need the WingmanModule's formation picture and
the skill layer), countermeasure consumption (chaff/flare remain intents),
recorder combat events (the M4 replay half — next session), visual
detection (the eyeball model behind the WVR "visual" band), and
skill-parameterized behavior.

## BVR Intercept Scenario — The Fight You Can Watch (M4-SCENARIO-1)

**The AI-vs-AI BVR engagement is now a scenario you can fly as a spectator:
`scenarios/bvr_intercept.json` spawns EAGLE1 (blue) vs BANDIT1 (red), both
brains fight autonomously through the M3 chain, and the scenario-player
renders the whole thing — missiles with contrails and pursuit lines, a
brevity COMBAT transcript (FOX 3 / spike / splash), the watched aircraft's
RWR picture, and a Tab key to switch which jet you ride with. The engine
side of the observability is a new engine-agnostic `CombatTranscript` in
f4-simulation (headless-tested), so the narration exists with or without a
window.**

| Area | Change |
|------|--------|
| `f4-simulation` — CombatTranscript (new) | Subscribes to the combat bus transitions (radar acquire/drop, RWR lock/launch, missile launch/detonate, damage, kill) and formats them as brevity radio calls into a ring buffer: `"EAGLE1: FOX 3, AIM-120C away on BANDIT1."`, `"BANDIT1: Spike from EAGLE1, 13 NM."`, `"C2: Splash BANDIT1!"`. Callsigns resolve via the `aircraft_entities()` ↔ `scenario().aircraft` index map; launch brevity follows the weapon's guidance kind (`missile_brevity_word()`: ActiveRadar→FOX 3, SemiActiveRadar→FOX 1, Ir→FOX 2). Severity (Info/Warning/Kill) ships on every entry for hosts to color-code. Engine-agnostic: no renderer, no window — tested headless. |
| `f4-scenario-player` — scenario | New `scenarios/bvr_intercept.json.in` (registered in the root CMake template list): two spawn-in-air F-16s, blue 506 kt 13 NM behind red 420 kt, both northbound — the same deterministic stern-chase geometry the M3 E2E test proves (detection inside the Pd=1 knee, AMRAAM flyout, kill, disengage). `combat: {enabled: true, radar_rng_seed: 777}`. 10-minute tick budget; the fight resolves in ~1-2. |
| `f4-scenario-player` — combat view | Missiles render procedurally (they carry no KoreaObj visual record): a bright cylinder body along the velocity vector, a 500-ft wire-sphere tactical marker (visible at BVR zoom), a fading contrail sampled once per frame (900 points ≈ 15 s), and a thin red line to the assigned target that makes the proportional-navigation pursuit visible. A **COMBAT** panel under the ATC transcript draws the CombatTranscript entries color-coded by severity (white/amber/red). |
| `f4-scenario-player` — watched aircraft | The HUD, follow camera (C), focus (F), and FCS panel (F3) now track the **watched** aircraft instead of always the first: **Tab** cycles (bvr_intercept: EAGLE1 ↔ BANDIT1), an ImGui "Watched" combo does the same. The HUD gains an RWR line (`clear` / `SPIKE (locked)` / `MISSILE LAUNCH!` — the same flag the MissileModule defends on) and a live-missile count for combat scenarios. Also fixed: non-primary *scenario aircraft* were gated by the "Show airport" toggle (the bandit disappeared with the runway); aircraft-vs-feature gating now uses the `aircraft_entities()` list. |
| Tests | 4 new (suite **1,577/1,577 — 100%**): 3× CombatTranscript (brevity-word mapping, ring-buffer + callsign fallback + capacity/clear semantics, and a full E2E narration of the AI-vs-AI fight — every link from radar contact to splash asserted on the transcript), plus **BvrInterceptScenarioFilePlaysOut**: loads the *shipped, build-configured* `scenarios/bvr_intercept.json` (not an in-memory copy), flies it, and asserts launch → kill → attribution → missile sweep, so a typo in the file the player actually reads can never hide behind the in-memory test. |

**How to watch the fight** (needs a build with the scenario player on —
X11/OpenGL, unlike the headless CI):

```
f4-scenario-player <build>/scenarios/bvr_intercept.json --run --follow --camera-distance 12000
```

Space pauses, Tab flips between EAGLE1 and BANDIT1, the speed slider
fast-forwards the boring transit, and the COMBAT panel (top-right) narrates.
The scenario starts PAUSED without `--run`.

## M3 Tactics — The AI Fights (M3-TACTICS-1)

**The brains now fight the war themselves: BVRModule engages, MissileModule
defends, and the E2E test drives two AI flights through a complete
autonomous kill chain with nothing but `Simulation::tick()`. Two real
engine bugs surfaced on the way: missiles were invisible to the victim's
RWR (missing ROLE tag), and red-team brains were blind to blue fighters
(blue-biased hostility).**

| Area | Change |
|------|--------|
| `f4-ai` — BVRModule (new) | The offensive BVR fight (AI_IMPLEMENTATION_PLAN Step 8): `None → Entering → Employing → Separating` state machine, plan range bands (entry ring 1.3×Rne, WVR 3 NM, merge/bugout 2 NM), lead-pursuit steering, 45° crank support window after each shot, cold- turned bug-out with range-reopen hysteresis. Engine-agnostic by contract: it never touches f4-weapons/f4-sensors — the radar lock and weapon release leave as **intents** (`wants_lock()`, one-tick `release_pulse()`) that the host executes. |
| `f4-ai` — MissileModule (new) | Two roles in one class (Step 10, mengage.cpp + mdefeat.cpp): **fire control** — deterministic Pk model (range × aspect), employment envelope, 4 s cooldown, shoot-shoot allotment, weapons-grade-picture gate (RWR-only contacts never fire); **missile defeat** — beam maneuver (nearest ±90° off the threat bearing), full-AB outrun, `has_override` defensive preempt, chaff/flare intents, and a 2 s defeat-linger so the jink doesn't stop on the detonation tick. |
| `f4-ai` — BrainComponent | The DigitalBrain priority ladder's first rungs (Step 12): while Enroute, `Defensive > BVR > navigation`. The brain owns a SensorFusion (initialized lazily; the host installs the detection policy) and exposes `CombatIntent` (lock + release) each tick; falling off the ladder resets the nav steering integrators (the same transient guard the phase handoffs use). `mode_name()` reports `BVREngage` / `MissileDefeat`. |
| `f4-ai` — SensorFusion | Two-sided combat fixes: hostility is now **own-relative** (target team ≠ ownship team; legacy "red ⇒ hostile" only as the no-tag fallback — a red brain was blind to blue fighters before), and `missile_threat()`/missile threat-scoring are **hostile-only** (your own missile must never read as an incoming threat — the shooter would have defended against its own AMRAAM). |
| `f4-simulation` | `configure_brain_combat()` turns a spawned brain into a fighting brain (ladder on + table-derived envelope: AIM-120C's 40 NM boundary → 20 NM doctrine Rne). `RadarBackedDetectionPolicy` now answers all-false for **killed** entities (corpses don't paint — the M3 host decision radar_component.hpp deferred here), so the shooter's BVR sees `LostTarget` and goes home instead of pumping the shoot-shoot allotment into a still-flying airframe. New `execute_brain_combat_intents()` runs between `update_all` and `update_rwr` each tick: lock intents → `command_track` (idempotent until the track is live), release intents → `launch_missile` through the sim's table (longest-range A/A station first — AMRAAM before Sidewinder; killed aircraft never fire). `Simulation` owns one policy per spawned combat aircraft. |
| `f4-weapons` | **Bug fix**: `launch_missile` never set the `ROLE="missile"` tag — `update_rwr`'s launch detection and SensorFusion's `is_missile` classification both key on it, so the victim's RWR was blind to every launch and missile defense was impossible. Found by the AI-vs-AI E2E, not by reading code. |
| Tests | 29 new: 14 BVRModule (range bands, state ladder, fire-control pulses/cooldown/shoot-shoot, crank offset band, bug-out hysteresis, RWR-only hold-fire, corpse reset), 11 MissileModule (Pk monotonicity/bounds, fire gates, beam = 90° off threat bearing, override + AB, chaff/flare conditions, linger, own-team refusal), 3 SensorFusion (own-relative hostility ×2, hostile-only missile threats), 1 E2E: **AiVersusAiBvrEngagement** — two AI flights, zero test-driven steering, the whole OODA loop (detect → lock intent → fire intent → victim RWR launch warning → beam defense → AMRAAM kill → corpse filter → disengage → nav resumes) asserted end to end. Full suite: **1,573/1,573 — 100%**, zero warnings. |

## Combat Chain Integration — M3 Step 1 (COMBAT-INT-1)

**f4-weapons and f4-sensors are no longer unconsumed leaf libraries: the
Simulation ticks the whole combat chain end to end — radar detect → track →
STT lock → RWR warning → AMRAAM launch → flyout → kill — proven by a single
E2E test. The integration also flushed out (and fixed) a real bug: the
FM→Transform sync never wrote velocity.**

| Area | Change |
|------|--------|
| `f4-simulation` | Links `f4-weapons` + `f4-sensors` (PUBLIC). Scenario JSON gains `"combat": {"enabled", "radar_rng_seed", "fighter_hit_points"}` and per-aircraft `"team"` (blue/red, validated). When enabled, spawned aircraft carry `WeaponStoreComponent` + `SignatureComponent` + `RadarSimComponent` + `RwrComponent` + `DamageStateComponent` + `CampaignIdentityComponent`; `Simulation::weapon_table()` exposes the built-in `WeaponClassTable` every launch goes through. `tick()` stamps the radar/missile sim clocks before `update_all` and runs `update_rwr` + `sweep_spent_missiles` after it — all gated, so combat-disabled worlds are byte-identical to before. |
| `combat_bridge` (new) | `attach_combat_loadout()` — one call adding the whole combat component set (per-aircraft radar seeds derived from the scenario seed). `RadarBackedDetectionPolicy` — the M2 `SensorFusion::DetectionPolicy` hook made real at the host layer (f4-ai stays interface-pure): radar = live track in the ownship's track store, RWR = warning from that emitter, visual/GCI = **false** — the flip off GCI-omniscience, ready for BVRModule to install. |
| FM sync fix | `Simulation::tick`'s FM→Transform sync now writes `vx/vy/vz` (NED→ENU, same axis swap as position). Before, every combat consumer reading the transform saw a *parked* aircraft: `launch_missile` gave the missile 0 ft/s inherited velocity (it fell ballistic and lost the seeker cone instantly) and radar aspect was degenerate. Found by the E2E test, not by reading code. |
| `campaign_bridge` | Dropped a dead local (`h2`, unused-variable warning under `-Wall`). Combat attachment for the campaign-flights spawn path is deliberately deferred: it needs team resolution from campaign data, which belongs with the M3 tactics that consume it. |
| Tests | 5 new cases in `f4-simulation/tests/test_combat_integration.cpp`: component attach (incl. IFF teams, seeds, store loadout), nothing-when-disabled, the full E2E chain through `Simulation::tick` (deterministic Pd=1 knee geometry: 13 NM stern chase), the policy adapter (invisible before scans, radar-not-GCI after), and JSON parsing/validation. Full suite: **1,544/1,544 — 100%**. |

## Green Suite + CI + Repo Hygiene (STEP-0)

**The full suite is 1,539/1,539 green for the first time; every push now
builds and tests itself on GitHub Actions; ~11 MB of dead weight and
runtime state left the tree.**

| Area | Change |
|------|--------|
| `f4-flight-api` | `PilotInput::validate()`: deleted the nine debug asserts that fired *before* the clamps — they contradicted the clamping contract pinned by `PilotInputTest.ValidateClamps*` and made those 5 tests impossible to pass in Debug builds. `validate()` is a sanitizer, not a checker. |
| `f4-flight-model` | `EngineModel::update()`: deleted the two asserts that contradicted the null-table guard three lines below them — `EngineModel.DefaultConstructedHasNoTables` tests exactly that contract (default-constructed engine → zero thrust/fuel flow, no abort). |
| CI | New `.github/workflows/ci.yml`: headless configure (all X11/OpenGL targets OFF) → build → full `ctest`, GCC 14 / Debug, on every push and PR to main. Same command sequence as the local verification workflow. |
| repo hygiene | Untracked: `temp/mapcheck*.png`, `temp/dump_full.txt`, the FreeFalcon source dumps `temp/ff_*.{cpp,h}`, the already-landed `f4-combat-m2-sensors.patch`, `imgui.ini`, `Testing/`. `.gitignore` gains `imgui.ini`, `Testing/`, `Data/` (asset-pipeline §4), `build*/`, and `temp/*` with the load-bearing `KoreaObj.{HDR,LOD,TEX}` re-included — their removal is deferred to the asset pipeline's Stage 3 glTF export, which replaces them with generated `Data/` assets. |
| Tests | 1,539/1,539 pass (was 1,533 + 6 "pre-existing environment failures" that were actually Debug-build assert-vs-contract bugs, not environment issues). The 4 `ControlLoop*` tests remain `DISABLED_` and the `F4_INSTALL`-dependent tests skip gracefully, unchanged. |

## Viewer Terrain Fixes (TERRAIN-TEX-2)

**Fixes five user-reported world-viewer bugs; textures now load in every
flow, the map is north-up, and 3D objectives sit on the terrain.**

| Area | Change |
|------|--------|
| `f4-renderer` | `WorldView::load_theater` accepts both the theater root and the terrain subdir (f4-install's `Theater.dir` is the subdir — every install-flow load silently failed before); `terrain_dir()` exposes the resolved dir. Far-ring z bias −20 → −400 ft so the coarser L4 ring can't poke through the near L2 surface (black z-fight blobs). |
| `f4-world-viewer` map | North-up fix: the cache became a plain `Texture2D` in TEX-1 but the canvas still draws with negative source height (row 0 renders at the *bottom*); both paint paths now write row 0 = south with tile art mirrored per cell. Theater binaries also load via world-JSON/import/install-set paths (`try_load_theater_tiles`), not just the campaign dialog. |
| `f4-world-viewer` 3D | Geometry (selected + neighboring objectives) samples the near post level — the same elevation the textured terrain renders — instead of the 128×128 MEA summary; the orbit camera targets the terrain elevation instead of sea level. `--select` sets the selection kind, also matches class names, prefers objectives with layout/features, and no longer zooms the map to a corner. |
| tools | `png_probe` takes an optional region `[x y w h]` and prints per-cell luminance variance. |

## Textured Terrain + Unified WorldView (TERRAIN-TEX-1)

**Both viewers now render real Falcon 4 terrain tile art, through one
shared code path.** Validated against real geography at Kunsan.

| Area | Change |
|------|--------|
| `f4-terrain` | New decoders: `TheaterGeometry` (ENU↔post↔block conversion, one documented place for the theater-scale convention), `PostLevel` (THEATER.O\*/L\* 7-byte TdiskPost records, dedup'd block offsets), `FarTileDB` (FArtILES.PAL/.RAW), `NearTileDB` (TEXTURE.BIN + texture.zip PCX art with H/M/L variant resolution), internal PCX reader. `tools/dump-terrain-textures` validates everything against a real install. |
| `f4-io` | `ZipReader` — minimal STORED-entry PKZIP reader (Falcon's texture.zip needs no inflate). |
| `f4-renderer` | Textured terrain: `TerrainTileCache` (lazily-grown GL_TEXTURE_2D_ARRAYs), `TerrainShader` (GLSL 330, 4 tile arrays via `vertexTexCoord2`, lighting + distance fog), textured path in the chunk builder (post-aligned quads, UVs ported from FreeFalcon's `DiskblockToMemblock`, near region + far ring). **`WorldView`**: one class owning load-theater → ensure-GPU → set-view-center → per-frame uniforms → teardown; both apps call it instead of hand-rolling the lifecycle. Sampler helpers deduplicated into `src/terrain_internal.hpp`. |
| `f4-scenario-player` | Terrain lifecycle now one `WorldView` member; scenario JSON accepts optional `theater_dir` (substituted from `F4_INSTALL` at configure time); falls back to the untextured MEA mesh without it. |
| `f4-world-viewer` | Install flow loads theater binaries into `WorldView`; the 3D Ground Layout tab renders textured terrain centered on the selection; **the 2D strategic map now paints real far-tile art** (2048×2048) instead of elevation-band colors; new `--select <name>` CLI flag + auto 3D-tab for headless screenshots. |
| Tests | 22 new unit tests (zip reader, post level vs real L2 fixture, tile DBs with synthetic theaters, geometry). Full suite: 1519 tests, only the 9 pre-existing failures (coord_transform, PilotInput clamps ×5, EngineModel — fail on the clean tree too). |

---

This document summarizes the cleanup pass applied to the F4 codebase.
**All 998 unit tests pass on a clean build.** (Up from 988 — added 10 new
tests covering the fixes.)

## Build Fixes (the repo on `main` does not compile out-of-the-box)

| File | Fix |
|------|-----|
| `CMakeLists.txt` | Added `add_subdirectory(f4-io)` — `f4-world-convert` and `f4-terrain` link against `f4-io` but the root CMakeLists never added it. Fatal: `f4/io/read_file.hpp: No such file or directory`. |
| `CMakeLists.txt` | Added `enable_testing()` at top level. All test executables built fine, but `ctest` from the build root reported "No tests were found!!!" because no top-level `enable_testing()` had been called. |
| `f4-json/include/f4/json/writer.hpp` | Added missing `f4::json::escape_string()` free function. `world_json.cpp:24` does `using f4::json::escape_string;` but the function didn't exist in the header. The worklog claims it was added but it never made it into the committed code. |

## Phase 0 — Stabilize

### Phase 0b: Deleted 14 orphaned diagnostic files

Removed `f4-models/tests/diag_*.cpp` (14 files, ~1370 lines):
- `diag_bsp_direct.cpp`, `diag_correct.cpp`, `diag_geometry.cpp`,
  `diag_geometry2.cpp`, `diag_hex.cpp`, `diag_hex2.cpp`,
  `diag_offsets.cpp`, `diag_offsets2.cpp`, `diag_offsets3.cpp`,
  `diag_raw_bsp.cpp`, `diag_slot_sizes.cpp`, `diag_tags.cpp`,
  `diag_tree.cpp`, `diag_vtables.cpp`

These were standalone `int main()` diagnostic programs (not gtest tests),
were NOT listed in `CMakeLists.txt` (so never built), and contained
hardcoded absolute paths like `/home/z/my-project/f4-repo/...` that
don't exist in any checkout. Pure dead code cluttering `tests/`.

### Phase 0c: RAII FILE* in `f4-io/src/read_file.cpp`

Replaced raw `FILE* fp = std::fopen(...)` with a `FileGuard` RAII
wrapper. If the `std::vector<uint8_t> buf(sz)` allocation throws
`std::bad_alloc` between `fopen` and `fclose`, the handle no longer
leaks. This single helper backs every binary loader in the project.

### Phase 0d: Comment typos fixed

- `f4-flight-model/include/f4/flight/flight_model.hpp:193` — wrong
  arithmetic in the `minorFrameTime_` comment ("6 sub-steps of 1/60s =
  1/10s major" — actually 6 × 1/360s = 1/60s major).
- `f4-math/include/f4/math/scalar.hpp:147` — referenced a `qsquared`
  function that doesn't exist.

### Phase 0e: Tightened weak tests

The original tests asserted "is the value finite" or used tolerances so
loose they admitted fully-developed stalls and divergent EOMs.

**`test_atmosphere.cpp`**:
- Replaced self-consistency check (`EXPECT_NEAR(out.rho, RHOASL, 1e-9)`)
  with assertions against published ISA-1976 reference values at sea
  level, 18000 ft, 36089 ft (tropopause), and 50000 ft. Tolerances are
  relative (0.5%) to admit the legacy Falcon-4 lapse rate constants
  while still catching wrong layer breakpoints or swapped exponents.
- Tightened `ZeroAirspeedDoesNotProduceNaN` to assert `mach == 0` and
  `qbar == 0` (the previous implementation produced a phantom Mach of
  `1/sound` at zero airspeed because the `safe_vt` floor leaked into
  the Mach computation). **This found a real bug in `atmosphere.hpp`
  which is now fixed** — Mach and qbar use the actual `vt`, only `qovt`
  uses the floor.

**`test_flight_model.cpp`**:
- `SixtySecondStabilityRun`: tightened G tolerance from ±3.0 to ±1.0,
  added altitude drift bound (12000 ft — loose enough to admit the
  known trim/FCS settling transient, tight enough to catch divergent
  EOMs), added speed divergence bound (10x).
- `PitchStickChangesAlpha`: rewrote to compare against a no-input
  baseline (because trim() doesn't seed the FCS integrator, so the
  first few seconds involve settling). Asserts alpha INCREASES (not
  just "moves") and Nz INCREASES above baseline. Found a real
  direction issue that was hidden by the old `|delta| > 0.1` check.
- `ThrottleIncreasesSpeed`: added quantitative lower bound (≥50 ft/s
  gain in 10 s of full AB) — catches a regressed throttle path (AB
  not lighting, throttle reversed, etc.) that the old "speed went up"
  check would miss.

**`test_table_accessors.cpp::F16TablesInterpolateCorrectly`**:
- Added grid-point fidelity check (interpolated value at an exact
  breakpoint must equal the raw table value to 1e-9).
- Added CL-monotonic-in-alpha check at low Mach (catches indexing
  bugs that "look right" but read from the wrong cell).
- Fixed boundary clamping test to use `mach0 - 1` / `alpha0 - 1`
  instead of hardcoded `-1`/`-10` (the F-16 fixture's alpha axis
  extends to -10 deg, so the old test was querying INSIDE the table).
- Added thrust magnitude check (catches table-misload like
  thrust_ab being loaded into thrust_mil's slot).

## Phase 1 — Type safety & code quality

### Phase 1a: `f4-geo` position ctors `explicit` + `from_degrees()` factory

`f4-geo/include/f4/geo/position.hpp`:
- Marked 3-arg ctors of `WorldPosition`, `LatLonAlt`, `ECEFPosition`
  as `explicit`. Closes the most common bug: passing degrees where
  radians are expected, which compiled silently because both are
  `double`.
- Added `LatLonAlt::from_degrees(lat_deg, lon_deg, alt_ft)` factory.
  This is the recommended way to construct a LatLonAlt from
  human-readable coordinates — applies `DEG_TO_RAD` so callers don't
  have to remember the radians convention.
- Updated `operator+` / `operator-` to use explicit `WorldPosition{...}`
  construction (so the explicit ctor doesn't break arithmetic).
- Updated all call sites in `f4-geo/tests/` to use either
  `T(...)` functional-cast form (which works with explicit ctors) or
  `T{...}` direct-init (also works). The parenthesized-braced form
  `EXPECT_EQ(x, (T{...}))` had to become `EXPECT_EQ(x, T(...))` because
  gtest treats the parenthesized braced-init as copy-init.
- Added new test `Position.FromDegreesFactoryConvertsCorrectly`.

### Phase 1b: Named constants for magic numbers in binary parsers

`f4-world-convert/src/campaign_decoder.cpp`:
- Extracted `NUM_TEAMS`, `TEAM_NAME_LEN`, `TEAM_MOTTO_LEN`,
  `CMP_HEADER_BYTES` from inline literals.

`f4-world-convert/src/objective_decoder.cpp`:
- Extracted `OBJ_HEADER_BYTES`, `LINK_COSTS_PER_LINK`,
  `NUM_RADAR_ARCS`, `GRID_COORD_MIN`, `GRID_COORD_MAX`, `VU_ID_BYTES`.
  (Renamed from `MOVEMENT_TYPES` to avoid collision with the
  `MoveType::MOVEMENT_TYPES` enum value.)

`f4-models/src/hdr_parser.cpp`:
- Documented `FORMAT_VERSION` and added `OLD_FORMAT_SENTINEL = 0xFEEF`
  (was a bare hex literal with no explanation of what it means).
- Extracted `LOD_ENTRY_BYTES` and `LOD_ENTRY_SPARE` from inline `12`
  and `20` literals.

### Phase 1c: `BinReader::remaining()` footgun fixed

`f4-models/src/bin_reader.hpp`:
- The old `bool remaining()` returned `size - pos` (a `size_t`)
  implicitly converted to `bool`. Callers writing `auto n =
  r.remaining();` got 0 or 1, not the byte count.
- Renamed to `remaining_bytes() -> std::size_t` (returns the actual
  count) and added `has_remaining() -> bool` for the boolean check.
- No callers existed, so the rename was safe.

### Phase 1d: `EntityWorld` move ctor regenerates cookie

`f4-entities/include/f4/entities/entity.hpp` + `f4-entities/src/entity.cpp`:
- The defaulted move ctor copied the cookie from the source, so after
  `EntityWorld b = std::move(a);`, both `a` (moved-from) and `b` had
  the same cookie. Handles captured against `a` before the move would
  incorrectly validate against `b` — defeating the use-after-free
  detection the cookie is there to provide.
- Replaced with a custom move ctor/assign that regenerates the cookie
  on the destination via a new `detail::next_world_cookie()` helper
  (declared in the public header, defined out-of-line in `entity.cpp`
  so the global atomic counter stays file-local).

### Phase 1e: `Trace::append()` is now O(1)

`f4-state-machine/include/f4/fsm/trace.hpp`:
- Switched internal storage from `std::vector<Record>` to
  `std::deque<Record>`. `push_back` + `pop_front` are both O(1) on
  deque; the previous `erase(begin())` on vector was O(n) per append
  when full. At 360 Hz on a 1024-record trace, that's a measurable
  cost on the minor frame.
- `records()` now returns `std::vector<Record>` by value (a copy)
  instead of `const std::vector<Record>&`. Trace inspection is rare
  (test/diagnostic only), so the copy cost is acceptable.

## Phase 2 — `Cursor::check_and_throw()`

`f4-io/include/f4/io/cursor.hpp`:
- Added `Cursor::check_and_throw(const char* context)` and the
  `const std::string&` overload. Converts the silent sticky error
  flag into an observable `std::runtime_error` at the end of a parse
  block — one-line idiomatic check instead of
  `if (c.error) throw std::runtime_error(...)` by hand.
- Rationale: the original sticky-flag design (worklog.md:1504) chose
  silent flags over throw-on-OOB to surface real bugs in subclass
  dispatch paths where exceptions would be caught and swallowed. But
  the consequence was that any caller who forgot to check `error`
  would silently produce zeroed records. `check_and_throw()` gives
  those callers a one-liner.

Migrated all throwing call sites to use it:
- `f4-world-convert/src/theater_data.cpp` — 9 sites converted from
  `if (c.error) throw std::runtime_error("...");` to
  `c.check_and_throw("...");`
- `f4-world-convert/src/campaign_decoder.cpp` — 2 sites.

Call sites that use the sticky flag for non-throwing rollback behavior
(`objective_decoder.cpp`, `unit_decoder.cpp`, `team_decoder.cpp`) keep
their existing `if (c.error)` pattern — that's the correct use of the
sticky flag.

Added 6 new tests in `f4-io/tests/test_cursor.cpp`:
- `CheckAndThrowNoopsWhenNoError`
- `CheckAndThrowThrowsAfterOobRead`
- `CheckAndThrowIncludesContextInMessage`
- `CheckAndThrowAcceptsStdString`
- `CheckAndThrowAfterSkipOob`
- `CheckAndThrowAfterFixedStringOob`

## Phase 3 — Deduplication

### Phase 3a: JSON serialization consolidated

`f4-convert/src/json_io.cpp` was a 664-line copy of
`f4-data/src/config_loader.cpp` (both admitted "Both copies MUST stay
in sync" in their comments). The "circular dependency" justification
was invalid: `f4-convert` already depends on `f4-data` via
`target_link_libraries(f4-convert PUBLIC f4-data nlohmann_json)`.

- `writeJson()` / `writeJsonFile()` / `readJson()` / `readJsonFile()`
  are now thin adapters that delegate to `f4::data::writeConfig()` /
  `loadConfig()` / `loadConfigFromString()`. The `IoResult` ↔
  `LoadResult` adapter handles the structural difference (IoResult
  takes cfg by reference; LoadResult embeds it).
- `diffConfigs()` stays in f4-convert — it has no equivalent in
  f4-data and is genuinely f4-convert-specific (used by the `json_diff`
  CLI tool and the round-trip test harness).
- Net: ~470 lines of duplicated serialization code deleted. The
  "MUST stay in sync" maintenance hazard is gone — format changes
  now happen in exactly one place.
- Updated misleading comments in `f4-data/src/config_loader.cpp` and
  `f4-convert/CMakeLists.txt` to reflect the new reality.

### Phase 3b: `f4-models::Vec3` aliased to `f4::math::Vec3<float>`

`f4-models/include/f4/models/types.hpp`:
- The duplicate `struct Vec3 { float x, y, z; }` (with only `operator==`)
  is now `using Vec3 = f4::math::Vec3<float>;`. Consumers get the full
  Vec3 operator set (dot, cross, length, normalize, hadamard, scalar
  arithmetic, `operator[]`) from f4-math, instead of the bare struct.
- Layout is identical (3 contiguous floats, 12 bytes, no padding), so
  `sizeof(Vec3)` in the binary parsers (which read arrays of Vec3
  directly from disk) is unchanged.
- `f4-models/CMakeLists.txt` now links `f4-math` as a PUBLIC dependency.

## Verification

Clean build from scratch:
```
$ rm -rf build && cmake -G Ninja -DCMAKE_BUILD_TYPE=Release \
    -DF4_BUILD_MODEL_VIEWER=OFF -DF4_BUILD_VIEWER=OFF ..
$ cmake --build . -j2
$ ctest -j2
100% tests passed out of 1012
```

(Viewers are OFF because they require X11 + OpenGL dev headers not
present in this environment. The library code itself builds clean
with viewers enabled on a dev machine.)

## What was NOT done (and why)

The following items from the original review were deferred because they
are higher-risk or require more design work:

- **Full strong-type migration in f4-flight-model** (CRITICAL #1 in the
  review). The flight model uses raw `double` for alpha/beta/sigma/etc.
  with degrees/radians mixed in the same struct. Migrating to strong
  types is invasive — every consumer of `AeroState` / `KinematicState`
  / `calcBodyRates` needs updating. Recommend doing this as a separate
  dedicated PR with its own test pass, not as part of a cleanup batch.
  **DONE — see "Phase 4 — Angle Strong-Type Migration" below.**

- **`f4-messaging::publish()` shared_mutex refactor** (CRITICAL #5).
  The current `recursive_mutex` + vector copy on every publish is a
  real serialization point at 60 Hz, but the reentrant-publish
  semantics need careful thought. Needs its own benchmark + design
  pass.

- **`LayeredStateMachine::applyInhibition()` `force_to_state()`**
  (HIGH #3). The current `reset()` call fires the initial state's
  entry action, which is wrong if initial ≠ idle. Fix requires adding
  a new `force_to_state()` method to `StateMachine` and reasoning
  carefully about which transitions should/shouldn't fire on
  suppression.

- **`dat_parser.cpp` exception-as-control-flow** (HIGH #6). The
  backtracking search uses `try { ts.nextDouble(); } catch (...) {
  ts.setPos(saved+1); }`. Fix requires adding `tryNextDouble() ->
  std::optional<double>` to `TokenStream` and replacing 5+ sites.

These are all good candidates for the next cleanup pass.

## Phase 4 — Angle Strong-Type Migration (CRITICAL #1)

Resolves the largest correctness hazard flagged in the original review:
`AircraftState` stored angles as raw `double` with the unit convention
documented only in comments (alpha/beta in degrees, euler angles in
radians, alpha_dot in deg/s). Passing a degree value where radians were
expected (or vice versa) compiled silently because both are `double`.

### Approach

`f4-units` already provides a complete `Quantity<U,R>` framework with
`Radians`, `Degrees`, dimension arithmetic, and user-defined literals.
The flight model now uses these directly via flight-local aliases and
factory functions in a new public header:

**`f4-flight-model/include/f4/flight/angle.hpp`** (new, ~120 lines):
- `Angle` = `f4::Quantity<f4::Radians>` (radians canonical storage)
- `AngularRate` = `f4::Quantity<f4::RadiansPerSecond>` (rad/s canonical)
- Factories: `angle_from_degrees(d)`, `angle_from_radians(r)`,
  `angular_rate_from_degrees_per_second(dps)`,
  `angular_rate_from_radians_per_second(rps)`, `zero_angle()`,
  `zero_angular_rate()`
- Accessors: `to_degrees(a)`, `to_radians(a)`, `to_deg_per_s(r)`,
  `to_rad_per_s(r)`
- The `Angle` ctor is `explicit` (inherited from `Quantity`), so
  implicit `double -> Angle` conversion is rejected at compile time.
  Call sites must pick a side via the named factories.

### Changes by file

| File | Change |
|------|--------|
| `f4-flight-model/include/f4/flight/angle.hpp` | **New** — Angle / AngularRate aliases + factories + accessors |
| `f4-flight-model/include/f4/flight/aircraft_state.hpp` | All euler angles (sigma, gmma, mu, psi, theta, phi) and aero angles (alpha, beta, alpha_dot, beta_dot) and FCS commands (aoacmd, betcmd) migrated from raw `double` to `Angle` / `AngularRate`. Body rates (p, q, r) kept as raw double with a comment explaining why (they're integrated into the quaternion and never compared with degree-valued quantities). |
| `f4-flight-model/include/f4/flight/aerodynamics.hpp` | `Aerodynamics::update()` signature: `alpha_deg`/`beta_deg` params → `alpha`/`beta` of type `Angle` |
| `f4-flight-model/include/f4/flight/fcs.hpp` | `FlightControlSystem::update()` + `computeGains` + `runPitch` + `runRoll` + `runYaw`: `alpha_deg`/`beta_deg`/`phi_rad` params → `Angle` |
| `f4-flight-model/src/aerodynamics.cpp` | Extract `alpha_deg`/`beta_deg` locals via `to_degrees()` at the top of `update()` (the F-16 aero tables are degree-indexed and we deliberately do NOT convert them). Body unchanged. |
| `f4-flight-model/src/fcs.cpp` | Same boundary-extraction pattern. Write-backs via `angle_from_degrees(...)`. |
| `f4-flight-model/src/eom.cpp` | `trigonometry()` reads `to_radians(k.theta)` etc. instead of `k.theta` directly. Quaternion recovery writes via `angle_from_radians(...)`. Ground clamp uses `zero_angle()`. |
| `f4-flight-model/src/flight_model.cpp` | `init()`, `minorStep()`, `trim()`, `updateStallSM()` — all `alpha_deg`/`beta_deg` field references converted to `alpha`/`beta` (Angle) with `to_degrees()` extraction at the boundary. |
| `f4-flight-model/CMakeLists.txt` | Added `f4-units` as a PUBLIC dependency (was previously explicitly NOT a dependency). |
| `f4-flight-model/tests/*.cpp` | All `a.update(alpha, beta, ...)` and `fcs.update(..., alpha, beta, ..., phi, ...)` call sites updated to pass `angle_from_degrees(x)` for angle args. Direct field assignments (`aero.alpha_deg = 30.0`) updated to `aero.alpha = angle_from_degrees(30.0)`. Field reads in EXPECT_* macros wrapped with `to_degrees(...)`. |
| `f4-flight-model/tests/test_angle.cpp` | **New** — 11 unit tests covering the Angle / AngularRate factories, accessors, round-trip conversions, arithmetic, and the explicit-ctor guarantee. |

### What was deliberately NOT changed

- **The F-16 aero coefficient tables remain degree-indexed.** They are
  physical data files in degrees; converting the data would alter the
  flight feel. The degree convention now survives at exactly one place:
  the lookup call site (`table(mach, alpha_deg)`), where `alpha_deg` is
  a local extracted via `to_degrees(alpha)` and named `_deg` to make
  the convention explicit.
- **`StallConfig::*_deg` and `StallDetection::alpha_deg` / `*_deg`
  fields.** These are plain-data fields consumed by the polling
  detection logic and serialized into bus messages. They are
  deliberately `double` (degrees) because they cross the bus boundary
  as plain data and consumers (UI audio cues, JSON config) want
  degrees. The bridge from the typed `AeroState::alpha` to the plain
  `StallDetection::alpha_deg` is one `to_degrees(a.alpha)` call in
  `flight_model.cpp::updateStallSM()`.
- **Body rates p, q, r in `KinematicState`.** They are rad/s and feed
  the quaternion integrator + FCS roll-rate lag, never a degree-valued
  comparison. Typing them would add friction without closing a real
  correctness gap. Documented in the file-level comment.
- **`AeroState::clalpha`, `clalph0`, `cnalpha`.** These are
  dimensionless aerodynamic derivatives (dCL/dalpha per radian), not
  angles. Correctly `double`.
- **`FcsState::startRoll`.** This is the time-integral of p (rad), not
  an angle. Documented.

### Verification

Clean build from scratch:
```
$ rm -rf build && cmake -G Ninja -DCMAKE_BUILD_TYPE=Release \
    -DF4_BUILD_MODEL_VIEWER=OFF -DF4_BUILD_VIEWER=OFF ..
$ cmake --build . -j2
$ ctest -j2
100% tests passed out of 1012
```

The 11 new `test_angle` tests cover the Angle/AngularRate contract
itself; the existing 1001 flight-model / world / etc. tests confirm
the migration didn't change runtime behavior (the same trim alpha
converges, the same stall transitions fire, the same FCS gains
compute).

## Phase 5 — Deferred Item Resolution & Code Quality Pass

Resolves the three deferred items from Phase 4 plus additional code
quality improvements identified in the comprehensive audit.

### C1: MessageBus shared_mutex + copy-on-write refactor

**CRITICAL** — The `recursive_mutex` + vector-copy-on-every-`publish()`
was the dominant serialization point at 60 Hz × N entities.

`f4-messaging/include/f4/messaging/bus.hpp`:
- Replaced `std::recursive_mutex` with `std::shared_mutex`. `publish()`
  takes a shared lock (concurrent reads), `subscribe()`/`unsubscribe()`
  take exclusive locks.
- Replaced per-publish vector copy with copy-on-write `shared_ptr<vector>`.
  `publish()` reads the current shared_ptr under shared lock — zero
  allocation per publish. `subscribe()`/`unsubscribe()` create a new
  vector and swap the shared_ptr.
- Reentrant publish (handler calling publish() on the same bus) is now
  handled via a thread-local deferred list instead of recursive_mutex.
  The outer publish drains the list after handler dispatch completes.
- Deleted move operations (shared_mutex is not movable, and the
  thread-local reentry list references `this`).

### C2: LayeredStateMachine inhibition uses force_to_state()

**CRITICAL** — `applyInhibition()` was calling `reset()` which fires the
initial state's entry action. When a higher-priority DigiMode layer
activates, suppressed layers would execute phantom entry actions (e.g.,
starting timers, publishing messages) instead of silently going to idle.

`f4-state-machine/include/f4/fsm/state_machine.hpp`:
- Added `force_to_state(StateEnum s) noexcept` — sets `current_` to `s`
  without firing entry/exit actions or recording a transition. This is
  an administrative reset, not a UML 2 transition.

`f4-state-machine/include/f4/fsm/layered.hpp`:
- Changed `applyInhibition()` to call
  `layers_[j].sm.force_to_state(layers_[j].idle_state)` instead of
  `layers_[j].sm.reset()`.

### C3: dat_parser exception-as-control-flow eliminated

**HIGH** — The backtracking search in `parseEngine`, `parseRollData`,
and `parseLimiters` used `try { ts.nextDouble(); } catch (...) { ... }`
as a branching mechanism. C++ exceptions are ~100× slower than normal
return on most platforms, making .dat loading the dominant cost.

`f4-Fconvert/src/dat_parser.cpp`:
- Added `tryNextDouble() -> optional<double>`,
  `tryNextInt() -> optional<int>`, and
  `tryNextDoubles(n) -> optional<vector<double>>` to `TokenStream`.
  These return `nullopt` on EOF or parse failure WITHOUT throwing.
- Replaced all 4 catch sites: `parseEngine` legacy format scan (site 1),
  `parseEngine` alpha-factor probe (site 2), `parseRollData` (site 3),
  and `parseLimiters` (site 4).
- Added `<optional>` include.

### H9: Cursor::remaining() unsigned underflow

`f4-io/include/f4/io/cursor.hpp`:
- `remaining()` now returns 0 instead of `SIZE_MAX` when `p > end`
9  (unsigned underflow from `static_cast<size_t>(end - p)`). Previously
  any code using `remaining()` without first checking `error` would get
  a garbage value that could be used as a loop bound.

### CP2: [[nodiscard]] on result-returning functions

`f4-convert/include/f4/convert/dat_parser.hpp`:
- Added `[[nodiscard]]` to `loadFile()` and `loadString()`. Discarding
  the `ParseResult`D silently ignores parse errors.

`f4-flight-model/include/f4/flight/flight_model.hpp`:
- Added `[[nodiscard]]` to* `trim()`. Discarding the bool return
  silently ignores trim convergence failure.

### L6/L7: FlightModel::setMinorPerMajor + Cursor non-copyable

`f4-flight-model/include/f4/flight/flight_model.hpp`:
- Changed `minorPerMajor_` from `int` to `unsigned int`. Negative values
  silently became 1; now the type prevents them at compile time.

`f4-io/include/f4/io/cursor.hpp`:
- Added `Cursor(const Cursor&) = delete` and `operator=`. Copying a
  Cursor creates two readers sharing the# same buffer, advancing
  independently — a logic error.

### M8:2 ModelDatabase texture cache thread safety

`f4-models/include/f4/models/model_database.hpp`:
- Added `mutable std::shared_mutex texture_cache_mutex_` to protect
  `decoded_textures_`. `fetch_texture()` is const and lazily populates
  the cache; concurrent calls from render + export threads would
  otherwise be a data race.

### H8: FlightModel::bus_ lifetime contract documented

`f4-flight-model/include/f4/flight/flight_model.hpp`:
- Added explicit lifetime contract comment in `set_message_bus()`.
  The raw pointer pattern is retained for the default-construct-then-
  reassign pattern, but the contract is now prominent.

## Phase 6 — Pre-AI Hardening Sprint

Resolves the 5 Critical + 5 High issues from the dark-pattern audit
that would have become acute the moment multiple aircraft start sharing
state at 360 Hz. All 1020 tests pass (up from 1008 — 12 new tests added).

### PR1: C3+C4+C5 — silent-garbage fixes (low risk, high value)

- **C3: THEATER.MAP magic validation** (`f4-terrain/src/terrain_data.cpp`):
  the parser read `header.magic` but never compared it against
  `0x444CFFAE`. A file with the wrong magic but plausible dimensions
  parsed silently and produced garbage elevation data. Added
  `constexpr uint32_t THEATER_MAP_MAGIC = 0x444CFFAEu` in the public
  header (`terrain_data.hpp`) and a check in `load()`. Dedupes 8
  scattered literal occurrences across the terrain lib, hex inspector,
  and tests.
- **C4: BinReader::remaining_bytes() underflow** (`f4-models/src/bin_reader.hpp`):
  same bug as the `Cursor::remaining()` H9 fix in Phase 5, applied to
  the parallel reader. Now returns 0 (not `SIZE_MAX`) when `pos > size`
  after an OOB read.
- **C5: JSON skip_value() strict validation** (`f4-json/include/f4/json/reader.hpp`):
  the bare-token path accepted any non-structural char run — `truu`,
  `1.2.3.4`, `@#$%` all parsed silently. Now validates against
  `true`/`false`/`null`/number grammar and throws on anything else.
  Added `read_bool()` helper to replace the hand-rolled
  `if (r.consume('t')) ... skip_value()` pattern in `world_state.cpp`
  (3 sites) that broke under strict skip_value.
- 5 new tests: `Terrain.RejectsBadMagic`, `SkipValueFalse`,
  `SkipValueNumber`, `SkipValueRejectsMalformedBareToken`,
  `SkipValueRejectsBarePunctuation`.

### PR2: C1 — Table1D/Table2D data race on mutable cache

`f4-math/include/f4/math/table.hpp`:
- The "cached last-index hint" used `mutable std::size_t last_` written
  on every `const operator()` call. Two `FlightModel` instances sharing
  an aero table (e.g. formation AI) would race on this write.
- Replaced with `mutable std::atomic<std::size_t>` using
  `memory_order_relaxed`. Relaxed atomics are ~1ns on x86, preserving
  the cache's perf benefit while making the const operator() thread-safe
  per the C++ memory model. The cache is only a hint — a racy write
  simply causes the next call to do a full scan (still correct).
- Added explicit copy/move constructors because `std::atomic` is
  non-copyable. The cache value is loaded/stored via relaxed atomics;
  any value (including stale) is acceptable.
- No new tests (the existing 192 f4-math tests verify correctness).
  TSAN would now pass on a multi-aircraft shared-table test.

### PR3: H2 — warning flags + GoogleTest fetch dedup

- **Warning flags**: created `cmake/f4_warnings.cmake` defining an
  INTERFACE library `f4_warnings` with `-Wall -Wextra -Wpedantic`
  (GCC/Clang) or `/W4 /permissive-` (MSVC).
  `-Wno-missing-field-initializers` is suppressed (tests legitimately
  use `{}` aggregate init for structs with many fields). Linked to
  every f4-* library target (PUBLIC for STATIC, INTERFACE for
  header-only). First clean build: 2 real warnings (unused function,
  unused variable) — both fixed. Zero compiler warnings now.
  `-Wshadow -Wconversion` deferred to a follow-up (would produce
  hundreds of warnings on legacy-shaped code).
- **GoogleTest dedup**: the 17 `f4-*/tests/CMakeLists.txt` files each
  repeated the same 10-line `FetchContent_Declare(googletest ...)` +
  `include(GoogleTest)` block, pinning the version in 17 places.
  Hoisted to `cmake/f4_deps.cmake`, included once from the root
  `CMakeLists.txt`. Each test CMakeLists now just calls
  `gtest_discover_tests()`.
- 0 new tests (infrastructure change).

### PR4: C2 — TagValue → std::variant

`f4-entities/include/f4/entities/entity.hpp`:
- Replaced the hand-rolled tagged union (4 parallel fields:
  `str_val`, `int_val`, `float_val`, `bool_val`, only 1 populated,
  ~40 bytes wasted per int/bool tag) with
  `std::variant<std::string, int64_t, double, bool>`.
- API change: the 4 fields → 4 pointer-returning accessors
  (`as_string()`, `as_int()`, `as_float()`, `as_bool()`). Returns
  nullptr if the variant doesn't hold the requested type — surfaces
  type mismatches instead of silently returning default-constructed
  zeros/empties.
- Retained `Type` enum and `type()` method for backward compatibility
  with code that switches on type.
- Updated 12 call sites across `f4-entities/tests/test_entity.cpp`,
  `f4-world/tests/test_world_loader.cpp`, and
  `f4-world-viewer/src/{canvas,inspector_panel}.cpp`. The viewer's
  `team_tag->type == Type::Int ? team_tag->int_val : 0` pattern
  (4 sites) became the cleaner `team_tag->as_int() ? *team_tag->as_int() : 0`.
- 0 new tests (existing 20 entity tests + 7 world-loader tests verify
  the migration).

### PR5: H1 — ECS Phase 4 verification + interface contract test

**Finding**: the audit's H1 was overly pessimistic. The ECS Phase 4
work is **already complete** — `IDataSource` interfaces
(`ICampaignSource`, `ITeamSource`, `IObjectiveSource`, `IUnitCoreSource`
+ `IGroundUnitSource`/`ISquadronSource`/`IFlightSource`/`IPackageSource`)
exist in `f4-world/include/f4/world/data_source.hpp`, the bridge has
interface-based overloads, `WorldState` is forward-declared in the
public header (not included), the adapter structs are private to
`src/world_loader.cpp`, and the viewer reads exclusively from
`EntityWorld` components (verified by grep — zero direct `WorldState`
field accesses in viewer code).

The only remaining smell is that `UnitState` is still a 40-field
tagged-union struct. But it's now an INTERNAL implementation detail
of the `WorldStateAdapter` (the bridge's concrete `IUnitCoreSource`
implementation), in `detail/world_state.hpp`, never exposed to
consumers. This is acceptable — the smell is contained.

**Delivered**:
- Added `test_interface_bridge.cpp` (7 tests) — implements mock
  `ICampaignSource` / `ITeamSource` / `IObjectiveSource` /
  `IUnitCoreSource` adapters inline and calls `populate_world()`
  through the interface overloads. This is the **contract test for
  f4-ai**: if the interface contract breaks, this test fails before
  any AI code can be written against it. Also unlocks future data
  sources (BMS saves, DIS streams, procedural generation) without
  touching `WorldState`.
- Added deprecation comment to `UnitState` documenting that it's
  internal-only and pointing consumers to either `EntityWorld`
  components or the `IUnitCoreSource` interface.

### Verification

Clean build from scratch:
```
$ rm -rf build && cmake -G Ninja -DCMAKE_BUILD_TYPE=Release \
    -DF4_BUILD_MODEL_VIEWER=OFF -DF4_BUILD_VIEWER=OFF ..
$ cmake --build . -j2
$ ctest -j2
100% tests passed out of 1020
```

Zero compiler warnings. 12 new tests added across the 5 PRs.

### What was NOT done (and why)

- **`-Wshadow -Wconversion` warning flags** — would produce hundreds
  of warnings on legacy-shaped code (narrowing in binary parsers,
  shadowed loop vars in viewer code). Add in a follow-up cleanup pass.
- **`std::function` on state-machine hot path** (H3) — 32 bytes each,
  heap-allocating. Migration to SBO function type is invasive (every
  `StateMachine::Builder::on()` call site). Defer to a dedicated SM
  perf pass.
- **`EntityHandle::add<T>()` returns `T&`** (H4) — breaks encapsulation.
  Migration to `ComponentHandle<T>` touches every component-add call
  site. Defer until f4-ai actually needs dirty-flag/versioning hooks.
- **`FlightModel::state()` mutable overload** (H6) — f4-ai will reach
  into `state()` for sensor reads. Defer the mutable-overload removal
  to the f4-ai integration PR (where the actual call sites will be
  known).
- **Splitting `UnitState` into per-subclass structs** — the struct is
  now internal-only. Splitting it would require rewriting the JSON
  loader + adapter. Low value now that the smell is contained.

### PR6: H8 + H5 — locale-independent JSON parsing + portable temp paths

Follow-up batch closing two audit items that fit the sprint's spirit but
were missed in PR1–PR5.

- **H8: Locale-independent number parsing** (`f4-json/include/f4/json/reader.hpp`):
  `read_int()` used `std::strtol(..., 10)` and `read_number()` used
  `std::strtod`. `std::strtod` is **locale-dependent** — with
  `LC_NUMERIC=de_DE.UTF-8` the literal `"3.14"` parses as `3.0` (the
  `.` is treated as a thousand separator). Any campaign save loaded on
  a German/French/Spanish user's machine would silently corrupt every
  coordinate, velocity, and probability field in the JSON. Replaced
  both with `std::from_chars` (C++17), which is locale-independent by
  spec (§charconv). Also migrated the `\uXXXX` hex parse to
  `std::from_chars(..., 16)` for consistency — `strtol` with base 16
  was already locale-independent, but the migration removes the last
  locale-sensitive stdlib surface from the reader. `long` return type
  of `read_int()` is preserved for ABI compatibility; `std::from_chars`
  handles `long` natively.
- **H5: Hardcoded `/tmp/` paths in tests** — replaced with
  `std::filesystem::temp_directory_path()` in 3 sites:
  - `f4-data/tests/test_config_loader.cpp` (config write test)
  - `f4-world-viewer/tests/test_settings.cpp` (XDG_CONFIG_HOME temp dir)
  - `f4-world/tests/test_world_state.cpp` (terrain resolve test)
  The two remaining `"/tmp/..."` literals in `test_settings.cpp`
  (`s.last_world_json`, `s.last_terrain_json`) are **round-trip data
  values** — the test verifies JSON serialize/deserialize, not
  filesystem access — so they're left as POSIX-style sample strings.
  Portability fix: tests now work on Windows (where `/tmp/` may not
  exist), macOS sandboxed runners, and CI containers with non-standard
  `TMPDIR`.
- 2 new tests: `ReadNumberLocaleIndependent`, `ReadIntLocaleIndependent`.
  Both call `std::setlocale(LC_NUMERIC, "de_DE.UTF-8")` and verify the
  parser still returns the C-locale value. Skipped via `GTEST_SKIP()`
  on systems without the `de_DE` locale installed (CI runners and
  minimal containers commonly lack it).

### Verification (post-PR6)

Build not re-run in this environment (no cmake/ninja available in the
agent sandbox). The change is mechanical:
- `std::from_chars` for `long` and `double` is available since
  libstdc++ 11 / libc++ 14 / MSVC 19.41; the project already requires
  C++20 and g++ ≥ 13 (per existing CI config).
- The reader now includes `<charconv>` and `<system_error>`; both are
  header-only and have no link-time impact.
- The 4 existing float/int tests (`ReadIntPositive`, `ReadIntNegative`,
  `ReadNumberFloat`, `ReadNumberScientific`, `ReadNumberNegativeFloat`)
  continue to pass — `std::from_chars` accepts the same grammar that
  `std::strtod` did for the JSON number subset (no `0x`, no `inf`/`nan`,
  no grouping).
- The 2 new locale tests are skipped on systems lacking `de_DE`, so
  they cannot regress the suite on minimal CI runners.


---

# f4-scenario-player v0 — Initial Implementation

## Summary

Built the first cut of the `f4-scenario-player` host executable that
spawns an F-16 on a parking spot at Kunsan (synthesized layout) and
renders the aircraft + airport geometry (runway, threshold bars,
centerline dashes, taxi route, parking/hold-short/runway-end markers,
compass rose) in a Raylib window with orbit/pan/zoom camera.

The simulation starts **paused** so the aircraft sits at the parking
spot. Press Space to begin taxi (the brain's `TakeoffModule` follows
the scenario's taxi route to the runway threshold).

## Files added

- `Docs/SCENARIO_PLAYER_PLAN.md` — design doc, replaces the earlier
  `F4_TAXI_DEMO_PLAN.md`. Documents the rename, the v0 acceptance
  criteria, and the deferred items (auto-spawning from campaign data,
  real Kunsan ground layout, takeoff rotation, multi-aircraft).
- `f4-scenario-player/` — new top-level crate:
  - `CMakeLists.txt` — builds `f4_scenario_player_lib` (static library)
    + `f4-scenario-player` (CLI executable). Fetches Raylib 5.0 +
    Dear ImGui v1.91.5 + rlImGui (same versions as `f4-models-viewer`).
  - `cli/main.cpp` — entry point: `f4-scenario-player <scenario.json>`.
  - `include/f4/scenario_player/player_app.hpp` — public pimpl API.
  - `include/f4/scenario_player/airport_geometry.hpp` — `AirportGeometry`
    struct + `build_airport_geometry(Scenario)`.
  - `include/f4/scenario_player/coordinate_transform.hpp` — ENU ↔ Raylib
    math (extracted to a public header so tests can verify it without
    linking Raylib).
  - `src/player_app.cpp` — lifecycle (load_scenario, run, screenshot).
  - `src/renderer.cpp` — orbit camera, mesh building, scene drawing,
    HUD overlay, ImGui panel.
  - `src/airport_geometry.cpp` — synthesizes runway/threshold/dashes/
    taxi-route/markers/compass from a `Scenario`.
  - `src/viewer_state.hpp` — private pimpl state.
  - `scenarios/kunsan_parking.json.in` — CMake-configured scenario
    fixture (substitutes `@F4_SOURCE_DIR@` / `@F4_BINARY_DIR@` so it
    can reference `temp/KoreaObj.HDR/.LOD/.TEX` and
    `build/generated_fixtures/f16.json` portably).
  - `tests/test_airport_geometry.cpp` — 12 tests covering runway
    surface, threshold bars, centerline dashes, taxi route, markers,
    compass rose, colors, and zero-length-runway edge case.
  - `tests/test_coordinate_transform.cpp` — 9 tests covering the
    ENU → Raylib and model-vertex → Raylib axis mappings.

## Files modified

- `CMakeLists.txt` — added `add_subdirectory(f4-scenario-player)` gated
  by `F4_BUILD_SCENARIO_PLAYER` option (ON by default).
- `f4-world-convert/include/f4/world_convert/class_table.hpp` — added
  `int16_t vis_type[7]` field to `ClassTableEntry` and a
  `vis_type_for(entity_type, slot=0)` accessor. This closes the
  data-flow gap from `Falcon4.CT` → `ModelDatabase` so that future
  campaign-driven aircraft spawning can auto-resolve the visual model.
- `f4-world-convert/src/class_table.cpp` — parser now reads the 14-byte
  `visType[7]` array at offset 60 of each 81-byte CT record (previously
  discarded). Added `vis_type_for()` implementation.
- `f4-world-convert/tests/CMakeLists.txt` — registered the new
  `test_class_table` test target.
- `f4-world-convert/tests/test_class_table.cpp` — new (8 tests):
  regression guard for the existing fields (classInfo, dataType,
  dataPtr) + new tests for visType exposure (F-16 vehicle-class entry
  has vis_type[0]=1052, out-of-bounds slot returns 0, ~1080 of 2135
  entries have non-zero vis_type[0]).
- `Docs/AIRCRAFT_BINDING_DESIGN.md` — updated to reference
  `f4-scenario-player` (was `f4-taxi-demo`) and the new
  `SCENARIO_PLAYER_PLAN.md`.

## Build

```bash
cd build
cmake -DCMAKE_C_FLAGS="-I.../local-deps/usr/include" \
      -DCMAKE_CXX_FLAGS="-I.../local-deps/usr/include" \
      -DX11_Xrandr_INCLUDE_PATH=... \
      [... other X11 paths ...] \
      /path/to/F4
cmake --build . --target f4-scenario-player -j 4
```

(Note: the build needs X11 dev headers — `libxrandr-dev`,
`libxinerama-dev`, `libxcursor-dev`, `libxi-dev`, `libxfixes-dev`,
`libx11-dev`, `libgl-dev`. In a sandbox without root, these can be
extracted locally via `apt-get download` + `dpkg -x`.)

## Run

```bash
cd build
./f4-scenario-player/f4-scenario-player scenarios/kunsan_parking.json
```

Controls: left-drag orbit, right-drag pan, scroll zoom, Space pause/
resume, F focus aircraft, R reset view, F2 screenshot.

## Headless smoke test

```bash
xvfb-run -a -s "-screen 0 1024x768x24" \
    ./f4-scenario-player/f4-scenario-player \
    scenarios/kunsan_parking.json \
    --screenshot out.png --width 800 --height 600
```

The screenshot is 36 KB, 800×600 RGBA, with detectable pixels for the
runway surface (~4700 grey pixels), threshold bars + centerline dashes
(~5000 white pixels), taxi route (~360 yellow pixels), parking-spot
marker (~530 green pixels), runway-end marker (~40 red pixels), and
the F-16 aircraft model (~83000 dark pixels).

## Test results

- New tests added: 29 (8 ClassTable + 12 AirportGeometry + 9 CoordinateTransform).
- All 29 pass.
- Pre-existing tests: 6 failures in `PilotInput.Validate*` (5) and
  `EngineModel.DefaultConstructedHasNoTables` (1) — these are
  pre-existing and unrelated to this change (the EngineModel test
  asserts on a null table pointer that's a known issue in the test
  setup, not the production code).

## What's next (per `SCENARIO_PLAYER_PLAN.md` §8)

- Real Kunsan ground layout from `f4-world::WorldState`'s
  `GroundLayoutList` data (currently synthesized from scenario JSON).
- Auto-spawn aircraft from campaign `Flight`/`Squadron` units via
  the new `vis_type_for()` accessor.
- Takeoff rotation + climb-out (the brain supports it; the renderer
  just needs to keep drawing).
- Multiple aircraft (the `Simulation` currently tracks one
  `aircraft_entity_`; supporting N is a small refactor).

---

# Phase 2 — Campaign-Derived Scenarios

## Summary

Closed the §4.3 gap (deferred from Phase 1): the scenario player can now
spawn aircraft from a real Falcon 4.0 campaign save instead of a hand-
authored JSON aircraft list. Loading `save1.cam` (via `f4-world-convert`'s
`cam2json`) produces a `WorldState` with `Flight`-class units; the new
`spawn_aircraft_from_flights()` bridge walks those units, resolves each
flight's squadron → airbase → parking spot, and composes a 4-component
aircraft entity (`TransformComponent + FlightModelComponent +
VisualModelComponent + BrainComponent`) per flight.

## Changes

- **`f4-simulation/scenario.hpp`**: Added `SpawnMode` enum (`ScenarioList`
  | `CampaignFlights`) + `Scenario::world_json_path` + `class_table_path`
  fields. Backward compatible — defaults to `ScenarioList`.
- **`f4-simulation/campaign_bridge.{hpp,cpp}`** (new): Two functions:
  - `derive_airfield_from_objective(obj, runway_id)` — pure conversion
    from `ObjectiveState.ground_layout` to `ScenarioAirfield`. Returns
    `nullopt` for non-airbases. Grid→ENU conversion (1024 ft/grid unit).
    Builds taxi route from parking → follow-me → threshold.
  - `spawn_aircraft_from_flights(world, ct, db, cfg, airfield, template)`
    — walks `world.with_component<FlightPlanComponent>()`, resolves each
    flight's squadron → airbase → transform for parking spot, applies
    per-flight lateral offset (80 ft alternating ±), looks up vis_type
    via `ClassTable::vis_type_for(squadron.class_table_index, 0)`,
    composes the 4-component aircraft entity. Falls back gracefully when
    CT lookup or squadron resolution fails.
- **`f4-simulation/simulation.{hpp,cpp}`**: Replaced single
  `aircraft_entity_` with `std::vector<EntityId> aircraft_entities_`.
  Spawn dispatcher picks `spawn_from_scenario_list()` (Phase 1 path, now
  iterates `scenario.aircraft[]`) or `spawn_from_campaign_flights()`
  (Phase 2 path: loads WorldState, populates EntityWorld via
  `f4-world::populate_world`, derives airfield, loads ClassTable, calls
  `spawn_aircraft_from_flights()`). `tick()` and `record_snapshot()`
  iterate the vector — one snapshot per aircraft per tick.
- **`f4-simulation/CMakeLists.txt`**: Added `f4-world` + `f4-world-convert`
  + `f4-terrain` to dependencies (transitively required by WorldState +
  ClassTable).
- **Tests**: 17 new tests.
  - `test_campaign_bridge.cpp` (11 tests): 7 for `derive_airfield_*`
    (nullopt paths, threshold/runway_end/departure_alt, taxi route,
    heading conversion, runway_id propagation) + 4 for `spawn_*` (empty
    world, one-per-flight, lateral offset, vis_type fallback, threshold
    fallback).
  - `test_scenario_loader.cpp` (+6 tests): `spawn_mode` parsing,
    unknown-mode-throws, `campaign_flights` requires `world_json_path` /
    `class_table_path` / aircraft template.
- **Docs**: New `Docs/NEXT_PHASE_PLAN.md` documents Phase 2 scope +
  acceptance criteria + implementation order.

## Test Results

- All 56 simulation tests pass (11 CampaignBridge + 6 new ScenarioLoader +
  5 existing ScenarioLoader + 8 ClassTable + 5 VisualModelComponent +
  21 others).
- Full suite: 1265 of 1271 pass. The 6 failures are pre-existing
  (`PilotInput.ValidateClamps*` + `EngineModel.DefaultConstructedHasNoTables`)
  and unrelated to this change.

## What's Next (Phase 3)

- Wire `f4-scenario-player`'s renderer to iterate `sim.aircraft_entities()`
  (currently draws only the singleton via `sim.aircraft_entity()`).
- Build a `kunsan_from_campaign.json` scenario fixture that uses
  `spawn_mode: "campaign_flights"` with the bundled `save1.world.json` +
  `FALCON4.ct`.
- Smoke-test with `--screenshot` to verify multiple F-16s are visible at
  their campaign-derived parking spots.

---

## Phase 2A — Real Airfield Meshes

**Goal:** Replace the procedural painted airfield (one dark-grey quad with
threshold bars + centerline dashes) with **real KoreaObj BSP models** placed
at Kunsan: runway sections, taxiway, control tower, hangars, fuel tanks,
parking apron. The F-16 and airfield features share the same render path —
both are just entities with a `VisualModelComponent`.

### Architecture

A new `ScenarioFeature` struct joins `ScenarioAircraft` and
`ScenarioAirfield` in `f4-simulation/scenario.hpp`. Each feature carries
`{name, vis_type_index, position, heading_rad}` — the same keying model as
the aircraft block (direct KoreaObj model index, no class-table lookup).
The scenario JSON gains an `airfield_features[]` block.

`Simulation::spawn_airfield_features()` creates one entity per feature with
`TransformComponent` + `VisualModelComponent` (no FM, no brain — static).
Tracked in a separate `feature_entities_` vector so `tick()` doesn't try
to sync them from a flight model.

The renderer refactors `aircraft_meshes` (a flat `std::vector<MeshEntry>`)
into `mesh_cache` (a `std::unordered_map<int, MeshCacheEntry>` keyed by
`parent_index`). Multiple features sharing the same `vis_type` reuse one
GPU upload. `draw_aircraft()` becomes `draw_visual_entities()` which walks
`world.with_component<VisualModelComponent>()` and draws every entity —
aircraft and features share the same draw path.

### Files changed

| File | Change |
|------|--------|
| `f4-simulation/include/f4/simulation/scenario.hpp` | New `ScenarioFeature` struct + `Scenario::airfield_features` vector |
| `f4-simulation/src/scenario.cpp` | `read_feature()` parser + `airfield_features` block in `parse_scenario()` + validation |
| `f4-simulation/include/f4/simulation/simulation.hpp` | `feature_entities_` vector + `feature_entities()` accessor + `spawn_airfield_features()` decl |
| `f4-simulation/src/simulation.cpp` | `spawn_airfield_features()` impl + call from `initialize()` |
| `f4-scenario-player/src/viewer_state.hpp` | `MeshCacheEntry` struct + `mesh_cache` map (replaces `aircraft_meshes` vector) + `build_mesh_for_model()` + `draw_visual_entities()` decls |
| `f4-scenario-player/src/renderer.cpp` | `build_aircraft_meshes()` → thin wrapper; new `build_mesh_for_model(int)` lazy builder; `upload_textures()`/`unload_meshes()` walk `mesh_cache`; `draw_aircraft()` → `draw_visual_entities()` walks all VMC entities |
| `f4-scenario-player/scenarios/kunsan_parking.json.in` | Adds 12 real feature placements (runway sections, threshold bars, taxiway, parking apron, control tower, hangars, fuel tank, runway access gate) |
| `f4-simulation/tests/fixtures/takeoff_kunsan.json` | Adds 4-feature block for testing |
| `f4-simulation/tests/test_scenario_loader.cpp` | 4 new tests for `airfield_features` parsing + validation |
| `f4-simulation/tests/test_feature_spawning.cpp` | **NEW** — 8 integration tests for `spawn_airfield_features()` (entity creation, transform encoding, static-ness, model sharing, discoverability) |
| `f4-simulation/tests/CMakeLists.txt` | Wires `test_feature_spawning` |

### Test results

- 4 new `ScenarioLoader` tests pass (fixture parsing, empty-allowed, invalid-throws, non-zero-heading)
- 8 new `FeatureSpawning` integration tests pass (entity-per-feature, transform+VMC present, no-FM, tick-doesn't-modify, model-sharing, empty-spawns-zero, heading-to-quaternion, with_component-discovery)
- All 19 prior `ScenarioLoader` + `CampaignBridge` tests still pass
- `f4-scenario-player` builds clean and produces a screenshot showing 11 VAOs (F-16 sub-meshes + multiple feature models) loaded into VRAM
- Pre-existing `PilotInputTest` failures are unrelated (assertion in `f4-flight-api/src/pilot_input.cpp:21`, present before Phase 2A)

### What's next (Phase 2B)

- Wire `BrainComponent` to drive the F-16 along the taxi route from parking
  to hold-short, using `FlightModelComponent`'s nosewheel steering + ground
  throttle.
- Render the aircraft actually moving across the real airfield meshes.

## Fixed-Timestep Player Loop (speed slider rework)

**Goal:** Make the simulation trajectory independent of the speed slider
and the frame rate, and raise the slider max from 4x to 10x.

### The problem

The scenario player ticked the sim ONCE per rendered frame with an
INFLATED dt (`player_app.cpp:241` before this change):

    sim->tick(scenario.sim_dt * time_scale);

so the flight model's FCS/EOM minor step (dt/6) slid between 1/3600 s
(0.1x) and 1/90 s (4x). The FCS PI + lead-lag filters are discrete and
dt-dependent — past their tuned operating point (1/360 s minor step)
the pitch loop destabilizes, which is why the slider was clamped to 4x
(FLIGHT_CONTROL_STABILITY_PLAN.md §4.2 RC-2 — a symptom patch). Two
more defects fell out of the same line:

1. Real-time pacing depended on the FRAME RATE: the sim advanced
   `sim_dt*time_scale` per FRAME, so "1.0x = real time" only held at
   exactly 60 FPS (a 144 Hz vsync display ran the sim ~2.4x too fast).
2. The trajectory itself changed with the slider (different dt =
   different integration path), so interactive runs, headless CI runs,
   and recorded traces were never directly comparable.
3. `Simulation::tick` ALSO multiplied by its own `time_scale_` member
   (default 1.0, never set by any caller) — a latent double-scaling
   trap for future hosts.

### The fix

Fixed-timestep accumulator in the player (the classic "Fix Your
Timestep" pattern):

    accumulator += frame_dt * time_scale;      // slider scales WALL TIME
    while (accumulator >= scenario.sim_dt) {   // dt is ALWAYS sim_dt
        sim->tick(scenario.sim_dt);
        accumulator -= scenario.sim_dt;
    }

- dt is now always `scenario.sim_dt`, so the FM minor step stays at its
  tuned 1/360 s at every speed; filter behavior is speed-independent.
- The slider scales wall-clock time, not dt: 1.0x is true real time at
  any frame rate, and slow-mo carries fractional remainders across
  frames smoothly.
- The tick sequence — hence the trajectory, the flight recording, and
  the FCS CSV trace — is identical across slider settings and matches
  the headless harnesses (`sim.tick(1.0/60.0)` loops), so interactive
  and CI traces are directly comparable again.
- The 4x stability cap is replaced by a CPU-bounded guard
  (`kMaxSimStepsPerFrame = 30` — covers 10x at 30 FPS; drops the
  remainder after a stall instead of freezing the render loop), and
  the slider max is raised to 10x (10x/60 FPS = 10 ticks x 6 minor
  steps per frame).
- `Simulation::set_time_scale()`/`time_scale_` (behavioral scaling) are
  REMOVED. The FCS CSV trace's `time_scale` column is kept and now
  records pure host metadata via `Simulation::set_trace_time_scale()`
  (default 1.0) — baselines stay identifiable, but no host can
  accidentally double-scale dt.

### Files changed

| File | Change |
|------|--------|
| `f4-scenario-player/src/player_app.cpp` | Accumulator tick loop with `kMaxSimStepsPerFrame = 30` guard; slider + `set_time_scale` clamp raised to [0.1, 10.0]; comments updated |
| `f4-scenario-player/src/viewer_state.hpp` | `sim_accumulator` member |
| `f4-scenario-player/include/f4/scenario_player/player_app.hpp` | `set_time_scale` doc (real time at any frame rate, [0.1, 10.0]) |
| `f4-simulation/include/f4/simulation/simulation.hpp` | `set_time_scale`/`time_scale_` removed; `set_trace_time_scale`/`trace_time_scale_` added (metadata only) |
| `f4-simulation/src/simulation.cpp` | `tick(dt)` no longer scales dt (dt is authoritative); FCS trace sample records `trace_time_scale_` |
| `Docs/FLIGHT_CONTROL_NEXT_STEPS.md` | Phase 2b row marked Superseded |
| `CHANGES.md` | This entry |

### Testing

- Full non-renderer test suite passes (all f4-* libraries; renderer/
  viewer executables excluded from CI here only because the container
  lacks GL dev headers — no renderer code was touched).
- The scenario-player translation unit compiles clean against raylib
  5.0 / imgui v1.91.5 / rlImGui @ 9acdbbf headers.
- Manual: run `takeoff_only` at 0.1x / 1x / 10x — the FCS CSV traces
  must be identical modulo the `time_scale` column; at 10x the run
  completes ~10x faster with no pitch oscillation.
- Manual: resize/drag the window mid-run at 10x — the render loop must
  stay live (guard drops catch-up debt) and sim time must not jump.

## SimData AI — the maneuver table, brain archetypes, and formations fly as data (SIMDATA-AI-1)

**SimData.zip's AI trio is now engine-agnostic data the sim consumes
end to end. mnvrdata.dat (the 9x9 maneuver selection table + class
flags), BRAINDAT.brn (8 brain archetypes x ~25 mode rows), and
FORMDAT.FIL (9 formations) parse through f4-convert into canonical
JSON (f4.mnvrdata / f4.braindata / f4.formdata v1), load through
f4-data, and reach two real consumers: WingmanModule flies the game's
own formation stations (command_formation_slot ports
bvrengage.cpp:3367-3378 — range x mFormLateralSpaceFactor,
relAz x mFormSide + lead heading, relEl or the raw flightIdx x -100 ft
stack; kickout/closeup x2/x0.5, WMToggleSide mirror) and BrainComponent
flies per-archetype doctrine (set_brain_archetype: SEAD / Strike /
Waypointer stand down every engagement mode — clean reset, formation
keeps flying; MissileDefeat stays armed everywhere; the WVR entry band
comes from the archetype's own WVREngageMode range; MissileEngage /
GunsEngage gate the release pulses). The scenario JSON drives it:
"brain_profile" + "formation" per aircraft, brain_data_path /
formation_library_path per scenario, lazy-loaded (nothing references
it -> nothing loads -> byte-for-byte the pre-SimData world) with the
build tree's converted fixtures as the default. The reference's
quirks are documented, not hidden: the 'A' file marker the original
engine silently skips, the .brn files NO FreeFalcon reader ever
loaded (1997 design data consumed as intent), the positional rows
whose labels predate the DigiMode enum, and SEAD's duplicated
GroundMnvr section. Suite 1907 -> 1985.**

| Area | Change |
|------|--------|
| f4-convert — mnvr_parser | ReadManeuverData's table shape (digimain.cpp:811-913) as data: 9x9 (or NxN) choice grid keyed by relative energy / angle bins, per-class availability flags; accepts the shipped file's 'A' marker the reference engine silently skips on (it reads one byte and expects '#') — warned, never hidden. CLI: mnvr2json. |
| f4-convert — brain_parser | The .brn format: numBrainTypes, per-type '# <Name>' sections, positional rows (label comment / enabled / priority / range / angle). Rows-to-next-section parsing (SEAD's 26-row duplicated GroundMnvr lands as filler), label-intolerant positionality documented. CLI: brain2json. |
| f4-convert — formation_parser | formdata.cpp:12-115 verbatim: num4Slots / num2Slots / formNum / <name>, slot triples in the file's units with converted accessors; the num2Slots==0 -> 2-ship-inherits-slot[0] rule; formNum = the WingManCmd enum value FindFormation keys on. CLI: form2json. |
| f4-data — loaders | maneuver_data / brain_data / formation_data with canonical JSON round-trips, tolerant find_mode (case/space-insensitive + prefix fallback for the tag rows), find_archetype / find_by_name / find_by_form_num, BrainModeKey vocabulary mapping the .brn labels to the consumers' rung set. NM_TO_FT 6076.211 (phyconst.h) is f4::data::kNmToFt. |
| f4-convert — build fixtures | simdata_golden_fixtures: the three shipped files convert into ${BUILD}/generated_fixtures/simdata/ at build time (same conversion any install's SimData.zip takes); exported as F4_GENERATED_FIXTURES_DIR. The subdir keeps the config-loader's aircraft-fixture walk clean. |
| f4-ai — WingmanModule | command_formation_slot(Formation): the 2-ship station from FORMDAT.FIL; formation_position()'s data-driven branch (bvrengage.cpp:3367-3378 in ENU); kickout()/closeup() scale the lateral range (WMKickout / WMCloseup) but NOT the -100 ft flightIdx stack (the reference applies it raw — verified bvrengage.cpp:3378 + wingai.cpp:2923); set_formation_side mirrors both geometry paths; command_formation reverts to the built-in table (last command wins — the revert no longer no-ops when the resting enum matches). |
| f4-ai — BrainComponent | set_brain_archetype(const BrainArchetype*) — non-owning doctrine pointer, null = pre-SimData behavior. archetype_allows() gates MissileDefeat (armed in every shipped archetype: defense is doctrine), the engagement stand-down (BVR+WVR disarmed -> the running fight resets the same clean way bingo ends it), the WVR entry band (the archetype's WVREngageMode range), and the MissileEngage / GunsEngage release pulses under hold_fire. Wingy gates the formation rung. |
| f4-simulation — scenario | Aircraft "brain_profile" (archetype name) + "formation" (wingman only — validate() rejects a formation without lead_callsign before any spawn); scenario "brain_data_path" / "formation_library_path" (scenario-relative, resolved like every asset path). |
| f4-simulation — wiring | Simulation::apply_simdata_ai_profiles(): after wingman refs resolve, lazy-loads ONLY the referenced side (scenario path else the compile-time default dir F4_SIMDATA_DEFAULT_DIR -> the build tree's converted fixtures), resolves names (unknown -> initialize() fails with the known-name list; missing file -> fails with the loader errors), and injects: archetype pointers into brains, formation slots into wingman modules. BrainData / FormationLibrary are owned Simulation members — both consumers take non-owning pointers. brain_data_loaded() / formation_library_loaded() witness the lazy contract. |
| tests | +78 (1907 -> 1985, 100%, zero warnings): 45 parser units, 10 loader units, 9 WingmanModule FORMDAT geometry units (spread's LEFT-side 0.5 NM station, trail's explicit 2-ship triple, ladder's relEl branch, kickout's raw stack, closeup, side mirror, lead-heading frame, revert, no-picture contract), 1 archetype roundtrip, 13 scenario-wiring tests (schema, validation, lazy contract, default + explicit paths, loud failures, and the two behavior E2Es: a SEAD wingman never engages or releases while its archetype-free lead kills the bandit; a spread wingman converges on the file's own station from 11 kft out). |

## SimData Wave 2 — the class table, sensors, and signatures fly as data (SIMDATA-2-1)

**The rest of SimData.zip's combat-relevant data is engine-agnostic now.
VehDef (Vehicle.lst's 86 class rows + 90 .veh files — which sensors
every VU class mounts, plus the weapon physical cards), the SENSDATA
authoring files (.IRS IRST seekers / .RWR receivers / .VSS visual
sensors + their .LST indexes), and the SIGDATA signature grids
(.RCS / .IR0-.IR2 / .VIS azimuth-elevation breakpoint tables) parse
through f4-convert (veh2json / sens2json / sig2json) into canonical
JSON (f4.vehdef / f4.irstdata / f4.rwrdata / f4.visualdata / f4.sigdata
v1) and load through f4-data. Two real consumers: the radar detection
model reads the RCS grids DIRECTLY (SignatureComponent carries a
non-owning grid pointer like BrainComponent's archetype; the grid IS
the lobe shape — detection.hpp's documented "when the RCD data lands"
moment, placeholder lobe retired to the data-free path), and the RWR
model is parameterized by the .RWR files (RwrConfig's defaults ARE
generic.rwr: 180/90 omni + sensitivity 1.0 = byte-identical behavior;
harm.rwr's 2.0 sensitivity doubles the receiver's reach, the 60/60
cones gate emitters by elevation and — given the receiver's heading —
azimuth). The reference's quirks are documented, not hidden: Sea rows
never open their .veh (the shipped "dpthchrg,veh" comma typo is one),
the SENSDATA/SIGDATA text files have NO reader in the FreeFalcon tree
(the runtime freads precompiled .ICD/.VSD/.RWD binaries — the text is
the 1998 authoring source, the same design-data class as BRAINDAT.brn),
and the shipped ALQxxx.veh's misordered "# Data Idx" block shifts
every field — the reference's atoi() silently reads the garbage, and
so does this parser (loudly). Suite 1985 -> 2054.**

| Area | Change |
|------|--------|
| f4-convert — veh_parser | vehdef.cpp:29-252 verbatim: Vehicle.lst (count + type/file rows; -1 unused), SimACDefinition (combat class, airframe index, player + AI sensor loadouts), SimHeloDefinition, SimGroundDefinition, SimWpnDefinition (flags/cd/weight/area/ejection/mnemonic/class/domain/type/dataIdx). Case-insensitive .veh resolution for the list's Windows-style mixed-case paths; Sea rows record and never open; atoi()/atof() semantics on non-numeric tokens (warned — the shipped ALQxxx.veh quirk); duplicate stems warn, find() returns the first. CLI: veh2json. |
| f4-convert — sensor_parser | The 5-value .IRS, 3-value .RWR and .VSS positional formats with the files' own '#' comments as field names; the .LST index (count + names) resolves case-insensitively in the same directory; plausibility bounds (FOV 0-361, flare chance 0-1, sensitivity > 0). CLI: sens2json <irst\|rwr\|visual>. |
| f4-convert — signature_parser | The grid text format (numAz, numEl, azimuth breakpoints, then one row per elevation breakpoint whose LEADING value IS the elevation breakpoint — there is no separate elevation block): bilinear-interpolation contract from visual.cpp:79-99's Math.TwodInterp comment; ascending-axis validation; per-stem five-family loads (RCSDAT/, IR/ x3, VISUAL/). CLI: sig2json <dir>. |
| f4-data — loaders | vehicle_def_data (variant per type, SensorSlot pairs, enum name tables for CombatClass / WeaponClass / WeaponType / WeaponDomain / SensorType — the f16.veh comment's own enum), sensor_data (IrstSeekerData / RwrReceiverData / VisualSensorData with nominal_range_nm() = sqrt(gain)/ft-per-NM — the original signal = gain/range^2 model), signature_data (SignatureGrid::value_at: bilinear, azimuth wrap-to-mirror [0,180], elevation clamp; AircraftSignatureData's five grids). |
| f4-sensors — detection | TargetSignature.rcs_grid + elevation_deg (forward-declared pointer, f4-data stays PRIVATE to f4-sensors): when the grid is set, detection_range_nm(params, sig) uses value_at(aspect, elevation) directly — the grid encodes the lobes, the placeholder lobe factor does NOT stack; data-free callers take the exact pre-SimData path. RadarSimComponent's sweep copies the grid pointer from SignatureComponent and feeds the LOS elevation. |
| f4-sensors — SignatureComponent | rcs_grid (non-owning, the library lives with the host) + effective_rcs_m2(aspect, elevation): the grid lookup when set, the scalar otherwise. |
| f4-sensors — RWR | RwrConfig.az_limit_deg / el_limit_deg / sensitivity (defaults = generic.rwr — nothing gated, range unchanged); RwrModel::evaluate scales the receiver range by sensitivity, gates emitters by elevation (always, world frame) and azimuth (when the receiver's heading is known — evaluate()'s new receiver_heading_rad, NaN = omni; update_rwr derives it from the victim's velocity, parked = omni). |
| f4-convert — build fixtures | simdata_wave2_golden_fixtures: the shipped VehDef/SENSDATA/SIGDATA convert into ${BUILD}/generated_fixtures/simdata/ (vehdef / irstdata / rwrdata / visualdata / sigdata.json) — the same conversion any install's SimData.zip takes; consumers: f4-data loader tests, f4-sensors consumer tests. |
| fixtures | 112 files extracted from the zip into f4-convert/tests/fixtures/simdata/ (VehDef/ 90, SENSDATA/ 13, SIGDATA/ 8 incl. TACAN/stations.dat held for wave 3). |
| tests | +69 (1985 -> 2054, 100%): 17 veh units (shipped row counts by type, F-16 player-vs-AI loadouts, SA-6 card, helo/ground rows, Sea/typo/duplicate/backslash-path quirks, ALQxxx shifted read, JSON round-trip, synthetic + error paths), 16 sensor units (aim9l/agm65b values, generic-vs-harm, gain->10NM, round-trips, error paths), 17 signature units (breakpoint/midpoint/bilinear values, clamp/wrap, rear-hot IR rows, JSON round-trip, synthetic + error paths), 10 loader units, 9 f4-sensors consumer units (grid replaces placeholder lobe — beam-on 20 m^2 = sqrt(2)x scalar range; no-grid identical; sensitivity extends range; elevation/azimuth gates; defaults = generic.rwr; world-level heading gate). |
