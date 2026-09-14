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
| `ARCHITECTURE PROPOSAL.md` | As-built architecture of the 34 libraries. *(Action: refresh from "Draft proposal" to as-built — the TOC still lists libraries that don't exist and omits ~19 that do.)* |
| `FALCON4_FILE_LAYOUT.md` | Living reference: every Falcon 4.0 / FreeFalcon on-disk file, what it contains, whether we parse it. |
| `FreeFalcon_Core_Systems_Reference.html` | Generated analysis of the upstream FreeFalcon codebase. Regenerable artifact — do not hand-edit. |
| `AIRCRAFT_BINDING_DESIGN.md` | Short design note on aircraft↔entity binding. Stable. |

## Active plans (the one doc per subsystem)

| Subsystem | Document | State |
|---|---|---|
| Flight control (longitudinal, control-theoretic) | `LONGITUDINAL_STABILITY_PLAN.md` | Active. P4.2 TECS margin campaign parked; describing-function work is next. |
| Landing / taxi / formation / AAR | `LANDING_PRECISION_FORMATION_AAR_PLAN.md` | Active. Tranche B (taxi-back) not started. |
| AAR redesign | `AAR_REDESIGN_PLAN.md` | Active. Supersedes Tranche D ScriptedTanker. |
| Campaign loop | `CAMPAIGN_LOOP_PLAN.md` | C1–C6, G1, G2 landed; doc retains the roadmap. |
| AI architecture | `AI_IMPLEMENTATION_PLAN.md` + `DIGI_AI_PHASE2_PLAN.md` | Implementation reference. Consolidate these two into one AI doc. |
| Data / no-binary runtime | `NO_BINARY_RUNTIME_PLAN.md` | Tranches 0a–0e landed. **This (`Docs/`) is the canonical copy — the root copy was a stale duplicate and is deleted.** |
| Asset pipeline | `ASSET_PIPELINE_SPEC.md` | Draft v1 — design agreed, pending implementation. |
| Save / write | `SAVE_WRITE_PLAN.md` | Foundation landed (binary save format + §6.1 emitter). |

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
