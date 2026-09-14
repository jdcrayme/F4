# Changelog

One line per landed milestone, newest first. The verbose task reports this
replaces live in `Docs/history/changes-archive.md`; the raw session log in
`Docs/history/worklog.md`. Current design docs live in `Docs/` (see
`Docs/README.md` for the index).

## Fidelity tiers (most recent)

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
