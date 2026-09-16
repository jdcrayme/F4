# Changelog

One line per landed milestone, newest first. The verbose task reports this
replaces live in `Docs/history/changes-archive.md`; the raw session log in
`Docs/history/worklog.md`. Current design docs live in `Docs/` (see
`Docs/README.md` for the index).

## Fidelity tiers (most recent)

- **FID-OPT-2** — the concurrent-fight budget: the fusion-refresh
  tiering + the shared picture's own cadence (Docs/FID_OPT_PLAN.md
  §3): the optimization tranche's second item. MEASURED FIRST, and
  the measurement re-attributed the plan's own budget: the deep-
  horizon cost is NOT the per-brain fusion rebuild the §3 arithmetic
  had closed on (~3–6 µs each) but the SHARED AIR-PICTURE WALK it sat
  next to invisibly (~1.4–1.7 ms per walk, 20–40× the fusion term —
  `push_air_picture_` runs outside the `update_all` window the
  FID-OPT-1 sub-profile had split, so the walk never showed). The
  mechanism: the legacy GCI rule sees every missile in the theater,
  so the beam-fight rule ("a visible hostile missile refreshes every
  tick") pinned EVERY combat brain at 60 Hz for as long as any red
  missile was airborne anywhere, and any single brain forced the walk
  every fight tick. LANDED, one bound (≤100 ms staleness — the
  design's own latency license), two cadences: (1) the fusion refresh
  is TIERED BY THREAT — imminent (inside the fusion's own 50 NM RWR
  band) keeps the every-tick beam-fight refresh, a distant theater
  missile rides the new 6-tick (10 Hz) combat cadence, quiet brains
  keep the skill timer; `will_rebuild_this_tick` mirrors all three
  exactly; (2) the shared picture walk is ALSO cadence-gated — at
  most one walk per 6 ticks while any brain demands it, the LAST
  snapshot handed out between walks. FOUND AND FIXED EN ROUTE: the
  push-null invariant (the first cut handed `nullptr` on
  demanding-but-not-walking ticks, dropping every rebuilding brain
  onto its ~1 ms world-query path — rebuilds measured at 977 µs
  before the fix) and the walk-gate off-by-one (walks 7 ticks apart →
  exactly 6). Measured on real TestCamp: walks 95,862 → 21,642 (4.4×
  fewer) and walk time 131.6 s → 36.2 s across 6 sim-hours of armed
  war with the missile-laden brain-seconds IDENTICAL (671,744 →
  672,770 — the war's shape preserved); the 60× deep-horizon armed
  certificate's sustained rate 52.36× → **61.07× (the 60× sustained
  gate now clears)** and its worst sample 7.92× → 15.14×; the 20× 1-h
  cert 1324× GREEN (baseline 54.6×, unchanged within noise), the 60×
  1-h cert **1707×** GREEN, the 2-h armed cert 410× GREEN zero
  dilation, the 4-h armed soak crash-free through 187+ deaggs / 48
  A/A kills / the hour-4 tasking wave. The 2-h armed ledger MD5
  changed (`73a06efd…`) — the first OPT patch that does: the throttle
  shifts detection timing within the licensed bound; re-pinned as the
  golden. The residual deep-horizon dilation (60 dilated samples,
  worst 15.1×) is now NAMED: ~228 s of active-component work (radar
  sim scans, steering, FMs, RWR over the materialized set) — the
  FID-OPT-3 budget, measured, not designed. Tests: 5 fusion + 1
  integration; full suite 2,565 green (the two pre-existing tree
  failures unchanged).

- **FID-OPT-1** — the active-cache walk + the ScopedSubscriptions fix
  (Docs/FID_OPT_PLAN.md): the optimization tranche's first item, driven
  by the FID-6 certificate's own finding ("the session's fixed per-tick
  cost over the ~8,400-entity theater walk"). Landed: the DORMANT FLAG
  on `BehavioralComponentBase` (per-component, routed through the
  owning world so a transition between ticks is picked up without a
  manual invalidate) + the ACTIVE behavioral cache (one rebuild sweep
  fills both lists; `update_all` walks the non-dormant subset — the
  ~8,126-component walk was ~99% of tick time, of which ~8,000 were the
  parked squadron inventory's documented no-op updates paying two
  virtual priority() dispatches each per tick). BrainComponent and
  FlightModelComponent delegate their dormancy to the base; their
  in-update early-returns stay as defense in depth. Measured on real
  TestCamp: update_all ~317 µs → ~0.1 µs/tick with zero aircraft; the
  1 sim-hour armed war's tick work 80.4 s → 0.12 s; the 20× tiered
  certificate sustained 1472× (was 58.1×); **the 60× preset — the plan's
  named target since FID-6 — now passes GREEN at 1331×**; the
  FullFidelity baseline itself lifted 25.3× → 55.3× (the same parked
  mass was taxing it). The 2-h armed war's ledger MD5 is IDENTICAL to
  the pre-OPT run — the war's behavior is byte-identical, only the
  host got faster. FOUND AND FIXED EN ROUTE: the per-entity AI modules'
  bus subscriptions (Takeoff ×2 / Landing ×2 / Refuel ×8) were never
  unsubscribed — a destroyed aircraft's handlers stayed in the bus and
  the next live brain's TaxiRequest publish invoked handlers whose
  captured `this` was freed memory (ASAN: heap-use-after-free on the
  4-hour armed war at ~3 sim-hours; latent since the modules landed —
  pre-OPT the deep-horizon war dilated so hard almost nothing
  materialized). Fix: `ScopedSubscriptions` (f4-messaging RAII bundle;
  bind at initialize, unsubscribe on destruction) adopted by all three
  modules; `Simulation`'s member order swapped so the bus outlives the
  world at teardown. Six new f4-entities tests (dormant skip, the
  unpark transition, active order, idempotence, the campaign
  spawn/unpark pattern, the passive edge); the full suite green, the
  two pre-existing tree failures unchanged. FID-OPT-2 (the
  fusion-rebuild budget — the per-materialized-aircraft cost the
  walk's removal exposed: ~52 µs/tick at 3 aircraft, ~700 at 21) is
  measured and designed in the plan §3, deliberately not in this patch.

- **FID-5** — event-driven combat deagg (Docs/FIDELITY_TIERS_PLAN.md §4.5–4.6):
  the last open milestone of the phase, and the certificate's own lever.
  Landed: the AGGREGATE AIR PICTURE (f4-ai `AggregateContact` +
  `Simulation::set_air_picture_aggregates` — the session publishes the
  engine's airborne aggregates as coarse contacts with team strings and
  cruise velocity; the campaign-flight entities are excluded from the
  world walk so the feed is the single publisher), the COMMIT TRIGGER
  (a Tier-B fighter's engagement id matched against the published set
  deaggregates the contact's flight; the radar-backed policy's coarse
  aggregate rule makes them detectable; the launch veto eats releases
  aimed at the phantom id — no missile ever flies at a non-entity), the
  CONVERGENCE TRIGGER (two opposing aggregates whose predicted tracks
  land inside the 30-kft envelope at the 120-s lookahead deaggregate
  both — wire order, deterministic), the TRANSIENT COMBAT WINDOWS (a
  Combat deagg pins for `combat_window_sec`, then the standard reagg
  rules fold it), and SYNTHETIC INTENTS AS AGGREGATES (the session's
  MissionIntent subscription + the engine's `register_synthetic` +
  the spawner's deferral arm — the generated war rides the tier
  machinery instead of spawning straight to Tier-B; the intent spawn
  path gained the flight path's AirSpawnPose airborne override; the ops
  trigger gained the TOT arm so deliveries still happen). Plus the A/B
  divergence harness the FID-2 acceptance deferred
  (`test_aggregate_fm_divergence` — the FM led the measured leg by
  2.03×, the ratio and the fuel gap pinned). The certificate's second
  run on real TestCamp: 20× tiered GREEN at 58.1× sustained vs the
  25.3× FullFidelity baseline (~2.3× the pre-FID-5 tiered war); the
  2-h armed war ran 59.5× with the tier machinery cycling the generated
  missions (16 deaggs / 4 reaggs, identity green). 7 new session tests
  + the A/B harness; the full suite green and unchanged.

- **FID-VIEW-1** — the campaign view shows the war (Docs/FIDELITY_TIERS_PLAN.md):
  the viewer's Tiered default ran the war but drew none of it — the canvas
  live layer rendered only materialized aircraft, so every aggregate flight
  was invisible and a fresh session read as dead. Landed: the aggregate air
  picture (a pass over `flight_tiers()` — AGG translucent, HOME dimmed, LOST
  a gray cross, LIVE skipped for the materialized aircraft, team filter +
  cull + click-pick with the flights-table selection ring), the tasking
  countdown (`Campaign::seconds_to_next_cycle` → `Stats::next_tasking_sec` →
  a "next tasking cycle in MM:SS" war-status line — the ladder's first
  generated missions land a full 1800-s cycle in), and the viewer
  `--smoke-seconds <n>` long-window smoke (the 6/12 s default can never
  cross the cycle). Engines untouched; one new session stats test.
- **FID-6** — the acceleration certificate (Docs/FIDELITY_TIERS_PLAN.md):
  `campaign_qc --accel <x>` runs the war harness under the TIERED policy
  at an interactive preset and gates exit 15 (DILATION — a sample or the
  sustained pass below x×(1−tolerance)) and exit 16 (DEAGG CEILING — the
  deaggregated set over `--accel-max-live`) on top of the C5 set (6–14);
  `--accel-baseline` measures the same war at FullFidelity for the
  before/after. The C5 roster identity gained the tier term
  (`+ tier_deaggs` — the deagg spawn path bypasses the spawner's
  synthetic counter), the diary gained the FID columns (agg_live,
  tier counters, sim_rate, dilated), and the harness validates the new
  knobs. First TestCamp run: tiered war passes every C5 gate
  (deterministic — identical ledger MD5 at 20× and 60×; drift/leak/alive
  ok); 20× green exit 0; 60× honestly fires exit 15 (sustained 31.7×:
  the ~8.4k-entity theater walk caps the host at ~48× empty, the
  synthetic Tier-B mass drops it to ~25×; FullFidelity baseline 20.7×).
  5 new harness tests; the fidelity-tiers test rig's temp-dir race
  (ctest -jN) fixed.
- **FID-1..4** — air agg/deagg (Docs/FIDELITY_TIERS_PLAN.md): the tiered
  session runs the war the game's way — flights are campaign aggregates
  (`FlightAggregateEngine`: the save's own arrive/depart schedule or the
  cruise walk, per-aircraft fuel burn) until the camera bubble, an
  airfield-ops window, or a click deaggregates them (`AirSpawnPose`
  airborne handoff; lead roll-up fold-back; a lost aircraft folds the
  flight destroyed). Viewer: fidelity-tiers checkbox (Tiered by default),
  the flights table (click-to-select + D/R per row), the tier summary
  line. Full fidelity untouched and bit-identical; 18 new tests
  (`test_flight_aggregate` 11, `test_fidelity_tiers` 7).

## Environment & data (most recent)

- **73** — Weather v1 + day/night model: 3-state condition Markov chain (seeded, deterministic), solar twilight bands, visual detection scales with weather×daylight. Suite 2,513.
- **72** — Complete AuxAeroData record (all 443 fields) across the fleet.
- **71** — TowerATC (AI Tier 3): sequencing tower behind the stub interface.
- **70** — `.cam` re-encoder reaches byte-identity (write side).

## Flight control

- **63–66** — Pole-based diagnosis program: `diag_poles` (trim → Jacobian → eigenvalues), unstable aperiodic speed mode measured at 18 trims; mechanism = the G-hold law, refuting the back-side-of-drag-curve and alpha-bias hypotheses. P4.1 inner-loop correction (`kp05 = 1/K_nz`, real integrator) shrinks the mode 26×; STAB-P1 `alt_integral_gain` 1.2→0.6; FCS pitch speed damper implemented, **refuted by measurement**, kept default-off as negative evidence. `test_poles_envelope` CI gates (5) pin goldens.
- **69** — Three pre-existing E2E failures closed: shipped `korea.world.json` was invalid JSON (8,016 dangling keys), landing STAB-E47/E48/E49/E57–E62. Suite 2,435.
- **STAB-E series (~55 fixes)** — instrumented, trace-verified flight-control fixes; full `digi_full_mission` passes end to end.
- **DIGI-1/2, ALT-2…5** — airspeed-rotated gamma-hold law; NED→ENU quaternion fix; altitude-loop tranches.

## Campaign loop

- **C1–C6** — War loop closes: result ledger + write-back (C1), one pool for tasking/losses/resupply (C2), threat map + A* routes (C3), campaign thread + 7-phase ATM tasking + full 3D coverage (C4), 24-hour war acceptance harness + starved-worker fix (C5), A/A combat live (C6).
- **V-CAMP** — Live campaign session in the world viewer (time controls, flying flights, route inspection); runtime fixtures become build outputs.
- **G1/G2** — Ground war (battalion maneuver, front line, books) and the interdiction link (CAS against real battalions).
- **PERF-1** — Shared air picture: merge-phase collapse, closed output-identically at 3–4×.
- **62** — WorldState→JSON emitter; the closed save loop (§6.1).

## Combat chain

- **M1–M5** — f4-weapons core (M1), f4-sensors (M2), AI tactics: BVR→WVR merge, guns, 2-ship wingman (M3), BVR intercept acceptance harness + combat events in the recorder (M4), A/G employment + WVR/guns merge harness (M5, incl. Task 68).

## Data & no-binary runtime

- **57–60** — AAR redesign reconciliation; `@asset:` resolver with manifest hash verification (Tranche 0e.3); runtime glTF rewire, `f4-models` cut from `f4-simulation` (Tranche 0d renderer+simulation halves); viewer on-demand conversions into `Data/Temp`; CI repair (LZSS use-after-free).
- **Tranche 0a–0e** — JSON subset of `Data/` committed (runs anywhere without an F4 install); CMake boundary enforcement; TEX→PNG + glTF materials; `f4import` model/texture exporters.
- **SIMDATA waves 1–2** — maneuver table, brain archetypes, formations; class table, sensors, signatures fly as data.

## Platform & hygiene

- **SYMBOL-SVG-1** — `f4-xml` (vendored pugixml) + SVG symbol authoring (import/export, color roles, holes, earcut fills).
- **QC-PASS-1** — Viewer QC sweep: route clutter gating, honest speed feedback, 3D for selections, mechanical cleanup.
- **TERRAIN-TEX-1/2, VIEWER-V71-1, GLV3D series** — textured terrain, campaign-save v71 decode, 3D viewer pipeline.
- **ECS Phases 0–6** — stabilize; type safety; `Cursor::check_and_throw()`; dedup; angle strong-type migration (Phase 4); deferred-item resolution; pre-AI hardening.
- **STEP-0** — Green suite + CI + repo hygiene.

---
*Going forward: one line per landed task, appended at the top. Narration
belongs in the as-built doc for the subsystem, not here.*
