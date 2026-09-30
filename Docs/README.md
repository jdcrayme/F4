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
| Aircraft animation | `AIRCRAFT_ANIMATION_PLAN.md` | Active. M0–M2 LANDED (f4-anim: rigs + channels). M3–M5 open. |
| Campaign loop | `CAMPAIGN_LOOP_PLAN.md` | C1–C6, G1, G2 landed; doc retains the roadmap. GROUND-OPS-1 (2026-09-29) landed the FM half of the §7 GROUND-OPS CRAWL finding — the ground regime's wheel equation + the first-liftoff-minute crawl gate in test_digi_mission; the campaign-context re-measure stays open for the QC env. |
| Mission QC (the sanity-check layer) | `MISSION_QC_COOKBOOK.md` | As-built workflow: the three QC layers (gates → ledgers → viewer), per-mission-type filtered `campaign_qc` runs, and `scripts/qc_missions.py` (the per-category matrix runner with expectations). §5's strike gap is RESOLVED (EMPL-1, the CAMP_EMPLOYMENT_PLAN's own doc); §7's showcase scenarios are the open tranche. |
| Campaign employment (the campaign→sim kill chain) | `CAMP_EMPLOYMENT_PLAN.md` | EMPL-1 LANDED (A-G release chain), EMPL-1a (impact precision), EMPL-1b (recorder intended-path fields), EMPL-2 LANDED (campaign-path AAR — the receiver joins, latches, and refuels from campaign-spawned flights, the fleet-scale on-save demo included; the stick aim-point element is EMPL-2d). EMPL-3 LANDED via QC-ANCHOR. |
| Fidelity tiers (air agg/deagg) | `FIDELITY_TIERS_PLAN.md` | ALL LANDED (FID-1..6 + FID-VIEW-1): tiered sessions, the viewer's flights table, the `--accel` certificate with exits 15/16, the campaign view that shows the aggregate air picture + the tasking countdown, and FID-5's event-driven combat deagg (the aggregate contacts in the shared air picture, the commit/convergence triggers with the launch veto, the transient combat windows, and synthetic intents riding the tier machinery — the 20× tiered certificate sustained 58.1× vs the 25.3× full-fidelity baseline). |
| Fidelity tiers optimization tranche | `FID_OPT_PLAN.md` | FID-OPT-1 LANDED: the active-cache walk (dormant components leave the per-tick dispatch — 317 µs → 0.1 µs, the 60× preset GREEN) + the ScopedSubscriptions UAF fix the speed-up exposed. FID-OPT-2 LANDED: the concurrent-fight budget — the fusion refresh tiered by threat (imminent/distant/quiet) + the shared air picture's own 10 Hz walk cadence, both under the ≤100 ms staleness bound (walks 4.4× fewer; the 60× deep-horizon armed sustained gate now clears). FID-OPT-3 LANDED: the sensor-sweep budget — the radar scan walks pointers, not maps (with_component_ref + inline clutter/range pre-gates: 1,259 → 130 µs/scan, 9.7×; byte-identical detections) + the RWR sweep's licensed 6-tick cadence (8.6 → 1.9 s); the deep-horizon armed sustained rate 61× → 137×. CAMP-OPT-1 LANDED (the post-OPT-3 regression repair): the p7 spinner pass's per-tick full-world walk (1.96 ms/tick = 97% of the tick; 60× delivered 7.3×) replaced by an epoch-keyed roster cache — tick 2.09 → 0.157 ms (13×), the 60× certificate sustains 116.1× zero dilation (plan §4a). The residual (the FM floor, the brain's glue) measured and named in the plan §5. |
| The aggregate clock (event-driven campaign layer) | `AGGREGATE_CLOCK_PLAN.md` | Active. The time-compression diagnosis (why campaigns were CPU-limited below ~20× while the reference runs 64×) + the migration from the fixed-dt whole-war driver to the reference's event-driven aggregate economics. AGG-0 LANDED (the FID-DEF-GOV patch: Tiered is the session default, the runner's AIMD delivery governor). AGG-1 LANDED (the catch-up clock: the campaign pass left the tick stream — one pass per advance() call, the drained whole seconds as ONE engine delta; events batch per pass, the deliberate §5 re-pin). AGG-2a LANDED (the transition-triggered publishers: the damage sync diffs only the objectives a transition marked — O(changes), the full walk kept as the end-of-run form — and the verdict emit computes only after the capture tail moved; the deterministic due-queue + stagger primitive landed in `f4-campaign/due_queue.hpp`; the δ1 threshold-deferral retired honestly — the tier heartbeat must not be deferred). AGG-2b (the SpatialIndex radar term + per-unit detection cadences — the due-queue's next consumer), AGG-3 (aggregate-first spawn policy), AGG-4 (fully lazy aggregate state), AGG-5 (optional campaign thread) open. |
| AI architecture | `AI_IMPLEMENTATION_PLAN.md` | As-built implementation reference (Steps 1–12 LANDED; Phase-2 doc folded in and archived). §15 Part III is the active design for the open chapters — the datalink tier (AWACS/GCI replacing the omniscient leg), the flight-lead command module, and the specialist support brains. Step 13 (the DatalinkTier) LANDED on the f4-ai side: `datalink_net.hpp` (the node geometry + the per-contact team mask) and `SensorFusion`'s optional net leg beside the `DetectionPolicy` (no net = the legacy omniscient leg, byte-identical), pinned by the 15-case `test_datalink_net.cpp` twin suite; the host picture walk and the gate plumbing ride the f4-simulation tranche. |
| IR/visual sensors + countermeasures | `SENSORS_COUNTERMEASURES_PLAN.md` | LANDED (as-built): IrstComponent + VisualComponent (the SENSDATA cards + the SIGDATA IR/VIS grids driving real detection), the dispenser/decoy/seeker-seduction model (the MissileModule defeat intents get their consumption half; flare chances from the data's own seeker cards), and the golden-identity `combat.countermeasures` gate — pre-tranche fights byte-identical. The fusion tranche LANDED (§8, each leg behind its own gate): `combat.passive_sensors` (the policy’s passive optical legs — a dead-radar fighter still fights; the armed 60x war holds the preset, the perf certificate), `combat.ecm` + the per-aircraft fit (the burn-through model + the RWR’s Jamming strobe), `combat.throttle_ir_power` (the FM’s throttle picks the IR band). ECM-DATA-1 (2026-09-29) landed the campaign fit data source: the converted tables' VCD/WCD jammer bits → `resolve_vehicle_ecm` → the spawn-stamped `EcmFitComponent` → the arm's double gate (fit + `ecm`) — the data decides who jams. §9’s remaining item is behavior (the AI's notching response), not data. |
| Campaign host (the engine contract) | `CAMP_HOST_PLAN.md` | Draft v1 — HOST-1/2/3 (contract, event stream + journal, the viewer as client), CMD-1/2 (the full v1 command surface: RoE, retask, abort, priority, the tick-exact replay), ATM-1 (the ACTION tables, SWEEP lines, tanker waypoint, `action_filed` events) INIT-1 (create-from-parameters: the scenario pack → fresh `.cam` via the Task-70 encoders, byte-identity by construction, the C5 24-hour harness passes on a generated war, the G1 two-pair bed) and SCALE-1 (the Tier-3 full-data pass: complete UCD/VCD/WCD tables as `f4.theater.tables/1` JSON via `cam2json --emit-tables`, the runtime `TheaterTables` reader, the VCD countermeasure supply chain, the gated pilot-skill flow, the uncapped-fleet `--max-flights 0` certificate knob) and DOM-1 (victory scoring: the books' projection — the `verdict` query, the `verdict` event family, the priority-weighted territorial census + ledger rows, band stalemate/advantage/decisive) and DOM-2 (supply depth: the per-objective pool — the `.tea` strategic stocks feeding held objectives' clamped supply/fuel, battalions drawing from the nearest own-held depot and cut off beyond the radius, the supply-gated `last_repair` feature-repair cadence, the strategic reserve refilling reinforcement budgets, the `objective_repaired` event family, the live `objectives` query) and DOM-3 (personnel: the reference's `AssignPilots()` — every filed flight draws its crew from the squadron's decoded pilot roster (front-third lead, backward wingmen, the cannot-crew pick gate), the per-role effectiveness table decaying 25% per assignment and re-pricing FindBestAir, the personnel books + the pilot event trio (`pilot_assigned`/`pilot_lost`/`pilot_recovered`), the `squadrons` query, the write-back's roster face) and DOM-4 (airbase scheduling: the reference's FindTakeoffSlot depth beyond FID's airfield-ops windows — the slot grid slides with the clock (a moving epoch, past blocks fall off, the 160-minute horizon stops silencing late filings), the pick gate applies the reference's own previous-block rule and books its denials, a scrubbed flight's still-future slot releases, a horizon refusal books instead of staying silent, the `slot_denied` event family + the `airfields` query serve the scheduling face, and the intents carry the scheduled takeoff so the sim's ops window arms against the SLOT) and DOM-5 (naval: the wrap-then-decide — the upstream NavalTaskingManager is a 15-byte flag shell, so the naval face maps onto the ATM pipeline's request vocabulary: the anti-ship family (AMIS_ASHIP) files at the enemy's task forces through the ranked pool (own-shore distance, wire-order ties), the targeted filings route like strikes and publish on the SAME mission_filed event, the per-target filing books ride the additive `taskforces` query, and the "how deep" record names what a deeper naval tranche would take) and DOM-6 (task-force movement: the naval GroundWar sibling — the wire's own dest_x/dest_y IS the order, the `NavalWar` engine walks every belligerent task force toward it at the sea family default speed (UCD enrichment overrides) in the ground move phase's exact fixed-point arithmetic (arrival snap, heading byte, sub-grid 1/256), no ledger books — the moved rows sync live into the WorldState the `taskforces` query serves, the save carries them, the additive `heading` tail rides the row, one flag `naval_movement`, QC exit 18) shipped. |
| ATM strategy layer | `ATM_STRATEGY_PLAN.md` | LANDED (as-built, P7): the loiter racetrack (RouteBuilder circuits + NavigationModule's station hold — the AI plan's deferred OnStation rung), CAP-family station targeting over ranked own objectives, FindSupportFlights (AWACS/tanker/ECM share-or-file with their own station routes + escorts), RequestEnemyMission (a strike's ADDBARCAP files a defender BARCAP for the enemy's next cycle), and the RoE carry (the wire roe_check byte rides the backlog → request → flight → intent → the post-arm fire-control gates). One flag (`strategy_layer`); pre-strategy runs byte-identical. The follow-on legs it named — the ACTION tables, the campaign RoE doctrine, and the full-data scaling pass — have since landed as CAMP-ATM-1, CAMP-CMD-1, and CAMP-SCALE-1 (see `CAMP_HOST_PLAN.md`). |
| Avionics logic (the engine-agnostic avionics layer) | `AVIONICS_PLAN.md` | Active — the tranche ladder (INS + steerpoints → the FCR page SM → the HUD view model → SMS/HSD/delivery cues). **AVIONICS-1 LANDED**: `f4-avionics` (header-only, runtime side) — the IAircraftState seam, the INS (pure-table alignment SM on f4-state-machine, the ground-clock align, the stored heading/altitude/position chain, the seeded splitmix64 drift walk keyed on the airframe's nav data — byte-identical per seed+stream cross-platform), steerpoint navigation + the HSI steering cue (BRA over f4-geo); 24 tests, label `f4-avionics`; the drift-zero twin compares equal to raw truth. AVIONICS-2–4 open. |
| Data / no-binary runtime | `NO_BINARY_RUNTIME_PLAN.md` | Tranches 0a–0e landed. **This (`Docs/`) is the canonical copy — the root copy was a stale duplicate and is deleted.** |
| Asset pipeline | `ASSET_PIPELINE_SPEC.md` | LANDED (as-built): `f4-assets` (the `@asset:` id derivation, SHA-256/FNV-1a fingerprints, the manifest reader — 58 tests) + `f4-import` (doctor, models/textures glTF emit — 46 tests), Data/manifest.json committed as the provenance + integrity contract, and `scripts/generate_manifest.py --check` as the fail-fast drift gate (CI runs it before the build). |
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
