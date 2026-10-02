# World-Viewer Performance Analysis — Why the viewer burns CPU FreeFalcon never did

> **Status**: Round 1 LANDED (VIEWER-PERF-1 — PV-1, PV-2, PV-4,
> PV-5, and the UI honesty item; see CHANGELOG). PV-3 (the lock diet)
> is deliberately deferred until the new profiler measures the duty
> cycle on target hardware — §7 marks what landed and what didn't.
> Root causes were identified by a four-way code audit (frame/UI path,
> aircraft-view path, engine interaction path, FreeFalcon reference)
> with the load-bearing claims spot-verified in source. This document
> remains the tranche's plan of record.
>
> **Triggered by**: "While viewing campaigns in world-viewer we are
> generally cpu limited to time accelerations less than 10x and on
> occasion when viewing a specific aircraft we can get CPU limited to
> less than 1x. Try to figure out why we require so much more CPU than
> FreeFalcon."
>
> **Companions**: [FID_OPT_PLAN.md](FID_OPT_PLAN.md) (the headless tick
> budget — ALL landed), [AGGREGATE_CLOCK_PLAN.md](AGGREGATE_CLOCK_PLAN.md)
> (AGG-3, the authenticity clamp), [CAMP_HOST_PLAN.md](CAMP_HOST_PLAN.md)
> (the runner/contract architecture), the archived
> [PERFORMANCE_ANALYSIS.md](archive/PERFORMANCE_ANALYSIS.md) (the earlier
> "why are the steps slow" round — its tranche landed as FID/FID-OPT).

---

## 1. The headline

**The war is not the cost anymore.** The same engine, headless, sustains
116×–1,676× on the FID-OPT certificates (a 0.157 ms/tick campaign with
4,000+ visual entities — CAMP-OPT-1). The viewer delivers <10× because
of what sits BETWEEN the worker thread and the war, and between the war
and the screen. Five multiplicative costs, none of which FreeFalcon
ever paid:

1. **The whole UI frame runs under the session mutex.** Input handling,
   the contract-plane snapshot, the map canvas, every ImGui window —
   all inside the worker's `FairMutex` scope
   (`viewer_app.cpp:299-548`). The worker's only window is
   `EndDrawing()`'s pace wait. Every millisecond of draw cost is a
   millisecond the war does not get.
2. **The contract plane re-serializes the war to JSON and re-parses it
   every frame while stepping.** The `step_serial` gate
   (`campaign_session_view.cpp:448`) is a no-op at speed — the worker
   bumps the serial every batch, so `fetch_snapshot`
   (`campaign_queries.cpp:306-319`) runs its 4–5 query round trips
   (time, stats, flights, tasking, +threat) per frame: engine-side
   `f4::json::Writer` builds, client-side `Reader` walks, thousands of
   small allocations, all under the lock.
3. **The map's objectives pass is O(N²).** `objective_defended()`
   (`ground_war.hpp:571-581`) is a linear scan over all 2,659
   objectives, called once per objective per frame
   (`canvas.cpp:624-632`) — ~3.5M iterations/frame on the default
   view, before a single pixel is drawn.
4. **Camera/selection movement re-points the deaggregation bubble and
   runs the FULL tier chain synchronously inside the frame lock.** A
   followed jet re-fires `Focus` at up to the re-point threshold rate;
   each `submit` → `set_view_bubble` (`campaign_session.hpp:729-743`)
   walks the ground bubble (a by-value ~800-unit roster copy +
   per-unit handle/transform gets), runs `evaluate_tiers_()` over the
   whole fleet, materializes/retires aircraft, and calls
   `refresh_stats_()` — twice per re-point, on the UI thread.
5. **AGG-3 holds the feed at 1× while the viewed aircraft is
   deaggregated — by design.** A selected live aircraft anchors the
   bubble on itself (`viewer_app.cpp:445-451`); its flight deaggregates
   → `bubble_live ≥ 1` → the runner clamps the preset to
   `min(speed, 1.0)` (`campaign_client_runner.cpp:138-141`). The
   sub-1× residual the user sees is the CPU failing to feed even that
   1× stream while the frame lock and the churn (4) eat the worker's
   window.

And one asymmetry the certificates never paid: the viewer's session
runs the **ground war** (`campaign_session_view.cpp:206-212`, opt-in in
`campaign_qc.cpp:500` and OFF in the 116× certificate run), pays
**`refresh_stats_` per 1–2-tick batch** (the QC harness drained
240-tick `advance(4.0)` batches — one `refresh_stats_` per 240 ticks;
the viewer's worker at ≤10× carries 1–2 ticks per `step()`, so the
per-call stats walk fires 100×+ more often per sim-second), and has
**no `--accel-max-live` deagg ceiling** (`campaign_qc.cpp:492-493`
caps the certificate at 32; the viewer materializes as deep as the war
goes).

---

## 2. The two symptoms, decomposed

### 2.1 "Generally CPU limited to <10×"

The worker's ceiling is structural: FIFO alternation on the
`FairMutex` guarantees it **at least one batch per frame**
(`fair_mutex.hpp:22-28`), and its adaptive budget targets 6–12 ms
holds (`campaign_client_runner.cpp:22-23`). Its window is the
`EndDrawing()` pace wait (V-THREAD-2 released the lock for exactly
this). So:

```
sim CPU per wall second  ≈  (batch ms) / (draw ms + batch ms) × 1000 ms
```

Every millisecond the UI thread spends under the lock — the O(N²)
objectives pass, the JSON round trip, the minimap's second full
theater walk (`canvas.cpp:1780-1883`), the parked layer when zoomed
(`canvas.cpp:1397-1441`, ~4,000 entities), the ATO table's per-frame
re-sort (`campaign_session_view.cpp:1152-1211`), the 3D panels when a
tab is open — stretches the frame period and divides the worker's
duty cycle. On top of the duty-cycle loss, the per-sim-second cost in
the viewer's configuration carries the cert-unpaid terms (§1 tail):
`refresh_stats_` (`campaign_session.cpp:3404-3482` — intents walk,
ground stats, `aircraft_entities()` walked twice with per-aircraft
handle+FM gets, `count_bubble_live_` per call) at small-batch
granularity, the ground-war engine + entity mirror per campaign
second, and the Focus churn's structural-rebuild debt (~2 ms
spinner-roster rebuild per structural change, CAMP-OPT-1's own
number, plus the behavioral-cache sweep) landing on the ticks right
after every re-point. The compound lands under 10× on hardware in the
cert sandbox's league or below; the split needs the measurement (§8).

### 2.2 "Viewing a specific aircraft → <1×"

Three causes superimposed, in order of weight:

1. **The AGG-3 clamp (policy, not CPU).** Selecting a live aircraft
   anchors the deagg bubble on it with no zoom gate
   (`viewer_app.cpp:445-451`); the followed flight deaggregates and
   its live lead sits inside its own bubble → `bubble_live ≥ 1` → the
   feed holds at 1×. This is the DoCompressionLoop authenticity rule
   working as designed (the reference's
   `campaign.cpp:2394-2520`); the UI labels it ("1x — action in
   bubble", `campaign_session_view.cpp:730-733`) but the effective
   readout at `:783-786` still shows the number, and a number under
   1× reads as a bug.
2. **The 3D chase view's per-frame rebuilds (CPU, under the lock).**
   With a LiveAircraft selected and the 3D tab active:
   `objectives_within_radius(…, 50000)` walks all 2,659 objectives
   **every frame** and `build_airfield_geometry_3d` re-runs per
   nearby objective **every frame** (`entity_model_3d.cpp:748-797`),
   a full second render pass runs into the shared RenderTexture
   (`:808`), per-part rig recomposition issues one `DrawMesh` per
   part per frame (`scene_draw.cpp:153-206`), and every 20,000 ft of
   aircraft drift rebuilds the whole terrain chunk set inside the
   lock (`entity_model_3d.cpp:586-608` — at war speeds that is
   multiple rebuilds per minute).
3. **The churn spiral.** A moving anchor re-points `Focus` at up to
   the vis-extent/16 threshold rate; each re-point runs the §1-item-4
   chain synchronously (bubble walk + tier evaluation + aircraft
   materialization + double `refresh_stats_`), and every
   spawn/retire bumps the structural epoch → the worker's next ticks
   pay the roster/cache rebuilds, pushing batch holds past 12 ms →
   the budget halves → 1 tick per batch. Meanwhile the combat-deagg
   machinery (FID-5, armed by default) materializes the engagement
   around the followed aircraft with **no ceiling** — the deepest
   deagg state the certificates never ran uncapped. The 1× feed
   itself then can't drain: effective < 1×.

---

## 3. The frame anatomy — where a viewer frame goes

`ViewerApp::run()` (`viewer_app.cpp:197-556`), campaign session live:

| Phase | Site | Lock | Contents |
|---|---|---|---|
| loop top | `:199-272` | free | keys, adopt/poll, screenshot |
| **frame scope** | `:299-548` | **held** | `refresh_session_snapshot` → input → camera-bubble block → `BeginDrawing` → `draw_canvas` → `draw_imgui` |
| present | `:550` | free | `EndDrawing` pace wait — **the worker's window** |

Inside the frame scope, per frame (while stepping — the `step_serial`
gate never holds):

| Cost | Site | Scale |
|---|---|---|
| contract JSON round trip ×4–5 | `campaign_session_view.cpp:448-451` → `campaign_queries.cpp:306-319` | full tasking (hundreds of intents × 16 fields, 3 strings each) + flights + stats + time; +threat = 2×~29k ints serialized and re-parsed when the overlay is on (`threat_map.hpp:92` — ratio 6 over the Korea extent ≈ 171×171 cells) |
| objectives pass | `canvas.cpp:520-687` | 2,659 objectives × (4 component gets + `objective_defended` **linear scan** + 2 `pb_int` string-keyed lookups + icon + labels) ≈ **O(N²)** |
| `ato_targets` rebuild | `canvas.cpp:535-542` | whole tasking vector → set, per frame |
| units pass | `canvas.cpp:799-945` | ~600–800 units × 2 component gets before cull |
| live-aircraft layer | `canvas.cpp:1064-1243` | grows with the bubble (the deagg state itself) |
| parked layer (zoom > 2) | `canvas.cpp:1397-1441` | ~4,000 entities × handle+transform gets |
| minimap (default ON) | `canvas.cpp:1780-1883` | second unculled walk: all objectives + all units |
| threat overlay (opt-in) | `canvas.cpp:769-796` | ~29k cells, 2 `world_to_screen` per non-zero cell, 1 `DrawRectangleRec` per visible cell — no batching, no cached texture |
| ATO table | `campaign_session_view.cpp:1095-1360` | order vector + iota + `stable_sort` over hundreds of rows, per frame; per visible row: objective-name resolution (map find + handle + component get) |
| flights table | `campaign_session_view.cpp:865-1078` | per visible row: `unit_type_name` — **unmemoized** (map find + handle + component get + class-table walk) |
| event log (force-opened) | `campaign_session_view.cpp:339, 1442-1514` | up to 2,000 rows, **no clipper**, per-row time format + circle + text |
| event drain | `campaign_session_view.cpp:395-408` | per frame (cheap when empty; bursty at tasking cycles) |
| camera-bubble block | `viewer_app.cpp:439-519` | Focus submit per re-point → the §1-item-4 engine chain |
| 3D panels (tab open) | `entity_model_3d.cpp` / `ground_layout_3d.cpp` | the §2.2-item-2 cluster |

The inspector itself is **light** for a live aircraft — four component
polls and Text widgets (`inspector_panel.cpp:913-999`); no sensors,
no recorder traces, no per-frame graphs. The aircraft-view cost is the
3D scenery rebuild + the churn, not the inspector.

---

## 4. What FreeFalcon did instead

| Axis | FreeFalcon | F4 viewer (today) |
|---|---|---|
| UI reads campaign state | Direct in-memory VU entity reads; fog-of-war a precomputed per-team `Spotted` bit; no serialization between engine and view | 4–5 JSON encode/parse round trips per frame under the lock (the contract plane's byte-stable wire) |
| Locking | No UI/campaign mutex; narrow critical sections; `Sleep(1)` + the sim handshake paces the campaign thread | One `FairMutex` held for the whole input+query+draw phase; the worker's window is the present wait |
| Compression | One scaled clock (`vuxGameTime`); catch-up capped at 1 campaign-minute/iteration; de-rate halves the ratio when behind | Same shape post-AGG-0/1 (the AIMD governor IS the reference's rule) — but the feed then has to *drain through the UI's frame* |
| The expensive thing | Full 6-DOF exists only inside a 2,540 ft player-anchored bubble; AI in-bubble fly SuperSimple; combat never meets compression (DoCompressionLoop) | Same shape post-FID/AGG-3 — but the bubble is anchored on **camera/selection attention**, re-pointed at threshold rate, with the tier chain running synchronously in the frame scope, and no max-live ceiling |
| Per-cycle budgets | ATM `Task()` capped at 200 ms; A* capped (2,000 nodes, zero-alloc) | `refresh_stats_` per 1–2-tick step; ground-war passes the cert never ran; rebuild debt per structural change |
| Map draw | Cheap per-entity flag/field reads; LOD by symbology | O(N²) defended scan + per-frame set rebuilds + second full-theater minimap walk |

The irony to keep: F4's engine now HAS the reference's economics
(AGG-0..4 + FID + FID-OPT bought them, measured); the viewer wraps
them in the three multipliers the reference never had — the
serialization plane, the whole-frame lock, and the attention-anchored
bubble. FreeFalcon's campaign thread could not be slowed by the UI
because the UI was a lock-free reader; ours is a lock-holding client
that pays for every read twice (encode + parse).

---

## 5. Root causes — ranked by expected impact

### #1 — The whole-frame session lock (the duty-cycle ceiling)

- **Mechanism**: `viewer_app.cpp:299-548` holds the worker's mutex for
  input + snapshot + canvas + ImGui; the worker advances only in the
  present wait (and its FIFO-guaranteed batch per frame).
- **Multiplier**: the sim's CPU share is `batch/(draw+batch)`; a
  10 ms frame phase at a 6 ms batch = 37% duty — before any other
  cost. Every UI cost below feeds THIS.
- **Fix path (§7, PV-3)**: shrink the locked scope (snapshot once,
  release, draw from the snapshot) — the canvas/tables mostly already
  read cached `session_snap` rows; the render-plane seam
  (`session_handle` derefs in the layer loops) is what pins the lock
  today.

### #2 — The per-frame contract JSON round trip

- **Mechanism**: the `step_serial` gate is a no-op while stepping;
  `fetch_snapshot` re-encodes and re-parses the whole tasking/flights/
  stats/threat picture per frame (`campaign_queries.cpp:306-319`,
  engine side `campaign_session_host.cpp:220-403`).
- **Fix path**: throttle the refresh (every N serials / ~100 ms — the
  tables already re-derive from the snapshot, they just do it too
  often), reuse row storage, and cache the threat grid server-side
  (it is a pure function of the route builder's map — it only changes
  per tasking cycle).

### #3 — The O(N²) `objective_defended` map pass

- **Mechanism**: per-objective linear scan (`ground_war.hpp:571-581`
  × `canvas.cpp:624-632`), ~3.5M iterations/frame at default zoom.
- **Fix path**: the mirror already carries parallel
  `objectives_`/`defended_` vectors — one `vu → index` map (the
  `unit_vus_` pattern) makes it O(1) per query; rebuild the index
  when `defended_` is stamped. Pure win, no behavior change.

### #4 — Focus churn: the synchronous tier chain in the frame scope

- **Mechanism**: re-point threshold `vis/16` (a followed jet crosses
  it in seconds); each `Focus` runs `refresh_bubble` (a by-value
  ~800-entry bucket copy + per-unit handle/transform gets,
  `bubble_manager.cpp:38-82`) + `evaluate_tiers_` (fleet walk +
  ms-class aircraft materializations,
  `campaign_session.cpp:1330-1443, 2936+`) + `refresh_stats_` — on
  the UI thread, under the lock — and every spawn/retire buys
  ~ms-class roster/cache rebuild debt on the worker's next ticks.
- **Fix path**: rate-limit re-points (time- or radius-scaled
  threshold), move the immediate evaluation to the next advance
  boundary (the FID-4 handoff contract already re-evaluates per
  campaign pass), give the bubble walk an epoch-cached roster, and add
  the viewer-side max-live ceiling the QC has
  (`--accel-max-live`'s default 32).

### #5 — The 3D chase view's per-frame scenery rebuild

- **Mechanism**: `objectives_within_radius` + `build_airfield_geometry_3d`
  per objective per frame, uncached
  (`entity_model_3d.cpp:748-797`); terrain chunk-set rebuild per
  20,000 ft drift inside the lock (`:586-608`); full second render
  pass + per-part rig recomposition.
- **Fix path**: cache the geometry per objective id (structural-epoch
  key), amortize terrain rebuilds, consider a 30 Hz chase cadence.

### #6 — Viewer-config engine costs the certificates never paid

- `refresh_stats_` per 1–2-tick `step()` (QC: per 240) —
  `campaign_session.cpp:3404-3482, 877-999`;
- ground war ON (`campaign_session_view.cpp:206-212`; cert: OFF,
  opt-in `campaign_qc.cpp:500`);
- no deagg ceiling (cert: 32);
- `evaluate_combat_`'s convergence loop armed by default.
- **Fix path**: batch `refresh_stats_` behind the step serial (the
  stats query is already snapshot-cached client-side — the engine
  recomputes per call for nothing), and give the viewer the QC's
  ceiling knob.

### #7 — The long tail (each small, all unconditional)

Minimap double walk; parked layer handle churn; per-frame
`ato_targets` set; ATO re-sort per refresh; unmemoized
`unit_type_name` per flights-table row (the ATO table's memoization
pattern from ATO-SORT-1 applied to its sibling); event log without a
clipper; threat overlay drawn per cell with no batching. None of
these is the story alone; together they are the draw phase that #1
converts into lost sim throughput.

---

## 6. What does NOT change (and what is NOT the problem)

- **The tick itself.** 0.157 ms/tick with 4,000+ visual entities
  (CAMP-OPT-1); FM 3.7 µs/aircraft-tick, brain glue 2.9 µs
  (FID-OPT-3). The viewer's per-frame costs are 10–100× the per-tick
  cost of the war they observe.
- **The FID tier machinery, the AIMD governor, the honest clock.**
  All working; AGG-3's 1× hold is the reference's own authenticity
  rule doing its job — the fix is surfacing, not removal.
- **The inspector.** Light.
- **Events.** Bursty, small (~0.5 KB copies); not a term.

---

## 7. The fix path (round 1 landed as VIEWER-PERF-1)

- **PV-1 (measure first — the FID-OPT discipline)** — **LANDED**:
  `F4_FRAME_PROF=1` arms the frame profiler: run() stamps the frame
  phases (snapshot / input / focus / canvas / imgui / present) as
  EMAs, and the Frame Profiler window prints them beside the worker's
  batch composition (hold ms EMA, last ticks, budget, delivery scale,
  effective speed) and the duty-cycle estimate. Zero cost when off.
- **PV-2 (quick wins, no behavior change)** — **LANDED**: the
  defended index (#3: sorted vu → index, binary search, built once in
  the GroundWar constructor), the refresh throttle (#2: the snapshot
  fetch fires at most every 100 ms under a moving serial; command
  invalidations bypass; the supply cut-off cache rides the same
  cadence), the threat query's server-side JSON cache (the map is
  immutable — every re-encode produced identical bytes), the flights
  table's per-flight type memo, the Event Log's filtered-index +
  clipper, the ATO-mark set cached per snapshot identity, and the
  objectives pass's single vu read (was two string-keyed lookups).
  The threat OVERLAY's draw walk stays per-frame (opt-in layer,
  visibility-culled; texture batching is a later tail item).
- **PV-2b companion (not in the original ladder — landed)**: the AGG-3
  clamp state rides each step's RESULT now (StepResult::bubble_live ←
  the engine's per-batch count; the runner clamps off it; the UI's
  snapshot mirror is gone). The authenticity hold engages at the
  reference's own compression-loop cadence, decoupled from the query
  throttle.
- **PV-3 (the lock diet)** — **DEFERRED, deliberately**: hold the lock
  only for the snapshot + input hit-tests; draw from the cached
  snapshot + a per-frame copy-out of the render-plane fields the canvas
  layers need. The one structural change in the ladder — and exactly
  the kind the FID-OPT round taught us to MEASURE first. With PV-2's
  throttle removing the per-frame query round trip and PV-4's churn
  policy removing the synchronous tier chains, the duty-cycle
  arithmetic changed; the profiler's canvas/imgui-vs-duty split now
  decides whether PV-3 buys enough to justify the copy-out seam.
- **PV-4 (the churn policy)** — **LANDED except the epoch-cached bubble
  roster**: `refresh_stats_` is lazy (advance marks; the first reader
  walks — the per-1-2-tick-batch stats tax is gone), the viewer arms
  the Tier-B ceiling (`max_live_flights` = the QC's 32; the
  attention-driven Bubble trigger defers at the ceiling — the war's
  own ops/TOT/recovery windows and the Combat/Force triggers are never
  capped), and a selection-anchored re-point fires only after the
  anchor crosses half the bubble radius (the deagg set changes at the
  bubble's edge, not its center). The tier evaluation still runs
  synchronously in the Focus chain (bounded by the ceiling + the rate
  limit now); moving it wholly to the advance boundary stays a
  follow-up.
- **PV-5 (the 3D diet)** — **LANDED except cadence**: the per-objective
  airfield geometry is cached per world (built once; the draw toggles
  apply per frame), objectives_within_radius walks a flat position
  index captured once (objectives never move), and the terrain drift
  rule stands as-is (the existing 20,000 ft amortization). A 30 Hz
  chase cadence option remains a tail item.
- **UI honesty** — **LANDED**: the speed readout names the AGG-3 hold
  ("held at 1x — action in bubble") distinctly from CPU dilation, and
  names BOTH when the held 1x feed is itself CPU-starved.

Order going forward: run the profiler on target hardware (PV-1's
readout) → PV-3 only if the duty split says the draw phase still
starves the worker → the remaining tail (minimap double walk, parked
layer handle churn, threat-overlay texture, chase cadence) in
whatever order the numbers rank.

---

## 8. Bottom line

FreeFalcon ran 12× on a 1990s PC because its campaign thread owned a
core, its UI read shared memory for free, and its expensive physics
lived in a small bubble that compression never touched. F4's engine
now has all three properties (measured: 116–1,676× headless) — but
the viewer wraps it in a client that holds the engine's lock for the
whole frame, pays a JSON encode/parse tax on every read, scans the
theater quadratically to paint it, and re-points a deaggregation
bubble with the user's attention. The <10× general case is the
duty-cycle ceiling plus the cert-unpaid engine terms; the <1×
aircraft case is the AGG-3 authenticity clamp (by design) with a
CPU-starved 1× feed under it. The fix path is the reference's own
shape: make the UI a cheap, lock-light reader again.
