# Changelog

One line per landed milestone, newest first. The verbose task reports this
replaces live in `Docs/history/changes-archive.md`; the raw session log in
`Docs/history/worklog.md`. Current design docs live in `Docs/` (see
`Docs/README.md` for the index).

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
