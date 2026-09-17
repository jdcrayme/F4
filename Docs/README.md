# Docs Index

This is the **only** table of contents for `Docs/`. Every document is listed
exactly once with its classification. If a document is not on this list, it
does not belong in the repo.

**Document lifecycle rule:** every doc carries a status banner on line 3.
`Active` → it is the single current plan for its subsystem. `Reference` →
stable, maintained. `Archived` → historical record of landed work; moved to
`Docs/archive/`. A superseded doc must never keep influencing decisions —
its surviving content is folded into the doc that supersedes it.

---

## Core references (always current)

| Document | What it is |
|---|---|
| `ARCHITECTURE PROPOSAL.md` | As-built architecture — the §3 inventory lists every build target with its real CMake link edges (refreshed P5; §17's phases all landed). |
| `FALCON4_FILE_LAYOUT.md` | Living reference: every Falcon 4.0 / FreeFalcon on-disk file, what it contains, whether we parse it. |
| `FreeFalcon_Core_Systems_Reference.html` | Generated analysis of the upstream FreeFalcon codebase. Regenerable artifact — do not hand-edit. |
| `AIRCRAFT_BINDING_DESIGN.md` | Short design note on aircraft↔entity binding. Stable. |

## Active plans (the one doc per subsystem)

| Subsystem | Document | State |
|---|---|---|
| Flight control (longitudinal, control-theoretic) | `LONGITUDINAL_STABILITY_PLAN.md` | Active. P4.2 TECS margin campaign parked; describing-function work is next. |
| Landing / taxi / formation / AAR | `LANDING_PRECISION_FORMATION_AAR_PLAN.md` | Active. Tranche B (taxi-back) not started (PLT_PARK data). Tranche D/AAR LANDED (Task 57 + P5 closure). |
| Campaign loop | `CAMPAIGN_LOOP_PLAN.md` | C1–C6, G1, G2 landed; doc retains the roadmap. |
| Fidelity tiers (air agg/deagg) | `FIDELITY_TIERS_PLAN.md` | ALL LANDED (FID-1..6 + FID-VIEW-1): tiered sessions, the viewer's flights table, the `--accel` certificate with exits 15/16, the campaign view that shows the aggregate air picture + the tasking countdown, and FID-5's event-driven combat deagg (the aggregate contacts in the shared air picture, the commit/convergence triggers with the launch veto, the transient combat windows, and synthetic intents riding the tier machinery — the 20× tiered certificate sustained 58.1× vs the 25.3× full-fidelity baseline). |
| Fidelity tiers optimization tranche | `FID_OPT_PLAN.md` | FID-OPT-1 LANDED: the active-cache walk (dormant components leave the per-tick dispatch — 317 µs → 0.1 µs, the 60× preset GREEN) + the ScopedSubscriptions UAF fix the speed-up exposed. FID-OPT-2 LANDED: the concurrent-fight budget — the fusion refresh tiered by threat (imminent/distant/quiet) + the shared air picture's own 10 Hz walk cadence, both under the ≤100 ms staleness bound (walks 4.4× fewer; the 60× deep-horizon armed sustained gate now clears). FID-OPT-3 LANDED: the sensor-sweep budget — the radar scan walks pointers, not maps (with_component_ref + inline clutter/range pre-gates: 1,259 → 130 µs/scan, 9.7×; byte-identical detections) + the RWR sweep's licensed 6-tick cadence (8.6 → 1.9 s); the deep-horizon armed sustained rate 61× → 137×. The residual (the FM floor, the brain's glue) measured and named in the plan §5. |
| AI architecture | `AI_IMPLEMENTATION_PLAN.md` | As-built implementation reference (Steps 1–12 LANDED; Phase-2 doc folded in and archived — the FAC/AWACS brain and flight-lead behavior are the open Part-III chapters). |
| IR/visual sensors + countermeasures | `SENSORS_COUNTERMEASURES_PLAN.md` | LANDED (as-built): IrstComponent + VisualComponent (the SENSDATA cards + the SIGDATA IR/VIS grids driving real detection), the dispenser/decoy/seeker-seduction model (the MissileModule defeat intents get their consumption half; flare chances from the data's own seeker cards), and the golden-identity `combat.countermeasures` gate — pre-tranche fights byte-identical. SensorFusion fusion + ECM are the named next legs. |
| Campaign host (the engine contract) | `CAMP_HOST_PLAN.md` | Draft v1 — HOST-1/2/3 (contract, event stream + journal, the viewer as client), CMD-1/2 (the full v1 command surface: RoE, retask, abort, priority, the tick-exact replay) and ATM-1 (the ACTION tables, SWEEP lines, tanker waypoint, `action_filed` events) shipped; CAMP-INIT/SCALE/DOM pending. |
| ATM strategy layer | `ATM_STRATEGY_PLAN.md` | LANDED (as-built, P7): the loiter racetrack (RouteBuilder circuits + NavigationModule's station hold — the AI plan's deferred OnStation rung), CAP-family station targeting over ranked own objectives, FindSupportFlights (AWACS/tanker/ECM share-or-file with their own station routes + escorts), RequestEnemyMission (a strike's ADDBARCAP files a defender BARCAP for the enemy's next cycle), and the RoE carry (the wire roe_check byte rides the backlog → request → flight → intent → the post-arm fire-control gates). One flag (`strategy_layer`); pre-strategy runs byte-identical. The ACTION tables, campaign RoE doctrine, and the full-data scaling pass are the named next legs. |
| Data / no-binary runtime | `NO_BINARY_RUNTIME_PLAN.md` | Tranches 0a–0e landed. **This (`Docs/`) is the canonical copy — the root copy was a stale duplicate and is deleted.** |
| Asset pipeline | `ASSET_PIPELINE_SPEC.md` | Draft v1 — design agreed, pending implementation. |
| Save / write | `SAVE_WRITE_PLAN.md` | LANDED end to end — encoders + CampaignSaver + the `campaign_qc --save-write` host consumer; the TestCamp decode→run→fight→apply→save→decode round-trip verified (P5). |

## Design documents (as-built, the worked example)

| Subsystem | Document | Note |
|---|---|---|
| Flight control as-built | `FLIGHT_CONTROL.md` | **Merged from 9 superseded plan/result docs.** This is the template: after a saga completes, merge the surviving truth here and archive the trail. |

## Archive (`Docs/archive/` — read for history, never for current state)

| Document | Why archived |
|---|---|
| `FLIGHT_CONTROL_STABILITY_PLAN.md`, `FLIGHT_CONTROL_NEXT_STEPS.md`, `FLIGHT_CONTROL_POLE_DIAGNOSIS_PLAN.md`, `POLE_DIAGNOSIS_RESULTS.md`, `PLANT_IDENTIFICATION.md`, `LOOP_MARGIN_REPORT.md`, `BISECTION_RESULTS.md` | The flight-control diagnostic saga. Superseded by `FLIGHT_CONTROL.md`; their §-references live on in the CI gate descriptions there. |
| `COMBAT_CHAIN_PLAN.md`, `COMBAT_CHAIN_M4_PLAN.md`, `COMBAT_CHAIN_M5_PLAN.md` | M1–M5 all LANDED. Surviving acceptance criteria live in the test suite. |
| `GROUND_WAR_PLAN.md`, `INTERDICTION_PLAN.md` | LANDED (G1, G2). |
| `RENDERER_GLTF_REWIRE_PLAN.md` | LANDED (Task 59). |
| `ECS_DECOUPLING_PLAN.md` | Complete (Phases 1–4). |
| `PERFORMANCE_PLAN.md`, `PERFORMANCE_ANALYSIS.md` | PERF-1 landed, PERF-2 closed by evidence. Fold any open §items into `CAMPAIGN_LOOP_PLAN.md` before archiving. |
| `PHASE4_FINDINGS.md` | P4.1 landed. |
| `TEXTURE_PIPELINE_PROGRESS.md` | T1–T5 complete. |
| `SCENARIO_PLAYER_PLAN.md`, `NEXT_PHASE_PLAN.md` | Superseded lineage; campaign-derived scenarios landed. Verify no unlanded §items remain, then archive. |
| `MODEL_VIEWER_IMPLEMENTATION_PLAN.md` | The viewer shipped; doc predates the implementation. |
| `AAR_REDESIGN_PLAN.md` | LANDED (Task 57 — real tanker + 8-state SM) and its last open tuning item closed (P5 — `test_aar_e2e` reaches Departing/Done with fuel transferred). Surviving design truth lives in `refuel_module.cpp`'s header. |
| `DIGI_AI_PHASE2_PLAN.md` | LANDED — recorder, ATC protocol, takeoff/landing/refuel demos, viewer replay all shipped; folded into `AI_IMPLEMENTATION_PLAN.md`. |

## History

- `history/worklog.md` — raw agent session log (141 entries). **Never cited
  as a source of current truth**; when a doc needs a task reference, it links
  the landed change in `CHANGELOG.md` (repo root) or an as-built doc above.
- `history/changes-archive.md` — the former root `CHANGES.md` (91 task
  reports), retained verbatim.
- `../CHANGELOG.md` — the terse milestone log. New work appends **one line**
  here, not an essay.

## Conventions going forward

1. One subsystem = one active plan. Milestones become **sections**, not new
   files (`COMBAT_CHAIN_M4_PLAN.md` is the anti-pattern).
2. Completing a saga: write/refresh the as-built doc (see `FLIGHT_CONTROL.md`
   as the template), move the trail to `archive/`, update this index.
3. No new root-level markdown. Root has exactly: `README.md`, `CHANGELOG.md`,
   build/config files.
4. Task IDs are namespaced and unique (`AREA-NNN`). The duplicate
   `EXPOSE-2`/`INSTALL-1`/`Task 59/60` collisions in the old logs are why.
