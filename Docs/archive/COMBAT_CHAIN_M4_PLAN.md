# Combat Chain — M4: End-to-End BVR Intercept + Replay

> **Status**: LANDED — implemented, verified, closed (Task 61; see CHANGES.md).
> M1 (f4-weapons), M2 (f4-sensors), M3 (f4-ai combat modules) are LANDED.
> M4 was the named next deliverable — the first end-to-end combat
> acceptance artifact — and is now earned: `test_bvr_intercept_harness`
> 8/8 (including the §5.3 FreeFalcon employment validation), the QC tool
> exits 0 with all four verdicts green + the two-pass MD5 certificate
> (and 2 on a non-combat scenario). Verification caught and fixed three
> rig defects (unsubstituted fixture placeholders in the synthetic
> scenarios; the non-combat refusal firing the wrong failure class; the
> no-waypoints fight that never reaches STT) — the record is in
> CHANGES.md Task 61.
> **Prerequisite**: M3 (BVRModule / WVRModule / MissileModule / WingmanModule
> delivered; `BrainComponent` is the M3-arbiter; `attach_combat_event_recorder`
> wired to all 11 combat bus message types).
> **Companion**: [Combat Chain Plan](COMBAT_CHAIN_PLAN.md) (M1–M3 landed; M4
> is §2's "M4 — Combat E2E scenario + validation"), [AI Implementation Plan
> §6](../AI_IMPLEMENTATION_PLAN.md) (the FreeFalcon validation targets M4 asserts
> against), [Campaign Loop Plan §5](../CAMPAIGN_LOOP_PLAN.md) (C5 — the war
> harness whose verdict/MD5/determinism contract M4 mirrors at scenario scale).

---

## 1. Where we are

The combat chain *runs* — but only the M3 integration test proves it
(`test_combat_integration.cpp::DetectTrackLockLaunchKillSweep`,
`AiVersusAiBvrEngagement`, `BvrInterceptScenarioFilePlaysOut`,
`CombatRecordingReplaysTheFight`). That test suite is the right precedent
but the wrong artifact: it is a unit-test binary, gated by ctest, with no
headless runner, no determinism certificate, no acceptance verdict, and no
replayable trace artifact a human can `md5sum`. The viewer can play a
combat scenario frame by frame, but the viewer is interactive — it cannot
produce a certificate.

| Capability | Status | Evidence |
|---|---|---|
| Headless combat scenario runner | ❌ none | `run_scenario.cpp` runs any scenario but produces no verdict, no MD5, no combat-event log; `campaign_qc` is campaign-only (no scenario-list spawn path) |
| Combat determinism certificate | ❌ none | The C5 `CampaignWarHarness` produces a ledger MD5; no equivalent exists for the recorder at scenario scale |
| Combat acceptance verdict | ❌ none | C5 has 4 gates (deterministic / ledger_consistent / entities_bounded / war_alive); combat has no gates — a BVR scenario can stall, fail to detect, fail to fire, fail to kill, and the test suite does not say which |
| Replayable combat trace | ✅ exists, underused | `FlightRecorder::to_json()` already serializes snapshots + combat_events; `attach_combat_event_recorder` is already wired; no tool ingests the trace as the acceptance certificate |
| Combat debrief summary | ✅ partial | `to_summary_json` emits a `combat` block (launches / gun_bursts / kills); no engagement-level window (detect → launch → kill timing, shot effectiveness) |
| Rendered BVR variant | ✅ exists | `f4-scenario-player` loads `bvr_intercept.json.in`, attaches `combat_log`, renders missiles; no headless "run + verdict + exit" path |
| FreeFalcon range validation | ✅ unit-level | `test_bvr_module.cpp` / `test_missile_module.cpp` pin range bands, Pk, cooldown, crank offset, beam geometry; no integration-level assertion that an end-to-end run fires at MAR, observes cooldown, respects shoot-shoot, and defeats |

M4 closes those gaps. It does **not** add new library surface — every
combat message type, every `CombatEventKind`, the recorder schema, the
bridge wiring, and the scenario JSON shape are already in place. M4 is a
**composition + verdict + tool** tranche: a harness class that mirrors
`CampaignWarHarness`, a CLI tool that mirrors `campaign_qc --war`, one
additive extension to `FlightRecorder::to_summary_json`, a `--harness`
flag on the scenario player, and the tests that pin them.

## 2. What M4 is

A headless BVR intercept scenario — two flights, detect → engage →
shoot-shoot → kill or defeat — that runs to a verdict. The verdict is
certified by an MD5 over the recorded trace, validated against FreeFalcon's
own employment constants (MAR, cooldown, shoot-shoot, beam-defeat), and
replayable from text alone.

```
bvr_intercept.json ──► BvrInterceptHarness ──► { result.json (the trace)
                            (Scenario)              (verdict + MD5)      summary.json (debrief)
                                                                         diary.json (telemetry) }
                            ▲                                              │
                            │                                              │
                  runs TWICE in-process ─────────────► compare trace bytes (determinism gate)
```

The harness composes `Simulation` directly (no `CampaignSession` — there
is no campaign at scenario-list scale), calls `sim.tick(scenario.sim_dt)`
in deterministic 4-second batches (mirroring C5's drain discipline), and
certifies `recorder->to_json()` across two passes.

## 3. What does NOT change

- **The `Scenario` schema and `CombatConfig`** — unchanged. The shipped
  `bvr_intercept.json.in` (`{"combat": {"enabled": true, "radar_rng_seed": 777}}`)
  is already the M4 fixture.
- **The two-pass ECS tick contract** — unchanged. The harness calls
  `sim.tick(dt)`; `Simulation::tick` runs `update_all` (brains ≥ 75, physics
  < 75), `execute_brain_combat_intents`, `update_rwr`, `sweep_spent_missiles`,
  `update_guns`, the FM→Transform sync, and `record_snapshot` in that order.
- **The combat event bridge** — unchanged. `attach_combat_event_recorder(sim)`
  is already called inside `Simulation::initialize` whenever
  `scenario.record == true`. The 11 CombatEventKinds (`TrackAcquired`,
  `TrackDropped`, `RwrLock`, `RwrLaunch`, `MissileLaunched`,
  `MissileDetonated`, `DamageApplied`, `EntityKilled`, `GunFired`,
  `BombReleased`, `BombImpact`) cover the entire A/A chain.
- **The `FlightSnapshot` / `CombatEvent` struct layouts** — unchanged. New
  fields are zero-defaulted and emitted conditionally (the existing
  byte-stable contracts: aircraft snapshots omit `"missile": true`; the
  `combat_events` block is omitted when empty; unknown kind names parse
  forward-compat).
- **The `f4-recorder` link surface** — unchanged. f4-recorder still links
  only f4-geo + f4-json. The `engagement_summary` block is computed from
  the already-serialized `combat_events_` vector — no new dependency.
- **The combat modules** — unchanged. `BVRModule` / `WVRModule` /
  `MissileModule` / `WingmanModule` / `GunModule` / `SensorFusion` /
  `BrainComponent` are the M3-arbiter; M4 exercises them, doesn't extend
  them. Tuning changes (if any tuning surfaces from the validation runs)
  land as `Config` field adjustments with provenance comments, not as new
  modules.

## 4. Work breakdown

| # | Item | Files | Notes |
|---|------|-------|-------|
| 1 | **`BvrInterceptHarness`** — the harness class | `f4-simulation/include/f4/simulation/bvr_intercept_harness.hpp` (new), `f4-simulation/src/bvr_intercept_harness.cpp` (new) | Mirrors `CampaignWarHarness`: `InterceptHarnessOptions` / `InterceptSample` / `InterceptVerdict` / `InterceptReport` / `BvrInterceptHarness`. Composes `Simulation` directly (no `CampaignSession`). Tick loop is `sim.tick(scenario.sim_dt)` in 4-sim-second batches (the same 240-tick drain discipline). MD5 over `recorder->to_json()` across two passes. |
| 2 | **The verdict gates (4)** | same header | (a) **deterministic** — recorder bytes identical across runs; (b) **engagement_completed** — at least one `EntityKilled` within the horizon with correct shooter/target attribution; (c) **roster_bounded** — `live == initial + spawned − retired` at every sample (no entity leak; the M4 roster is small — shooter + target + N missiles — but the gate catches MissileSimComponent leaks that `sweep_spent_missiles` would otherwise mask across ticks); (d) **fight_alive** — the shooter's brain reached at least `BVRState::Entering` and the bus carried at least one `RadarTrackAcquiredMessage` — a scenario where the AI never detects is a stalled fight, not a quiet one. |
| 3 | **`bvr_intercept_qc`** CLI tool | `f4-simulation/tools/bvr_intercept_qc.cpp` (new) | Mirrors `campaign_qc.cpp::run_war()`'s shape: parse args, `BvrInterceptHarness::create(opts)`, `execute(on_sample)`, write three artifacts (`bvr_intercept_result.json` = run 0's recorder JSON — the byte-stable certificate; `bvr_intercept_summary.json` = verdicts + counters + MD5, deterministic content only; `bvr_intercept_diary.json` = per-sample telemetry — wall_sec, ticks_per_sec, rss_kb — explicitly NOT byte-stable), exit-code table. |
| 4 | **Exit-code table** | same tool | `0` = all four verdicts green; `1` = usage / IO error / scenario-load failure; `2` = scenario has `combat.enabled == false` (the harness refuses to run a non-combat scenario — silent success would be the worst failure class); `3` = no detection within horizon (`fight_alive` violated, pre-engage); `4` = no launch within horizon (the brain detected but never fired — MAR/Pk gate mis-tuned); `5` = no kill within horizon (`engagement_completed` violated); `6` = entity leak (`roster_bounded` violated); `9` = non-deterministic (`deterministic` violated — the recorder bytes differ across runs). |
| 5 | **`engagement_summary` block** in `to_summary_json` | `f4-recorder/src/flight_recorder.cpp` (modify `to_summary_json`, ~30 LoC additive) | New block emitted ONLY when `combat_events_` is non-empty, AFTER the existing `combat` block. Fields: `first_detect_s` (earliest `TrackAcquired`), `first_launch_s` (earliest `MissileLaunched`), `first_kill_s` (earliest `EntityKilled`), `engagement_duration_s` (kill − detect), `shots_fired`, `shots_hit` (detonations with `end_cause == "target_hit"`), `shots_missed` (detonations with other causes), `weapon_effectiveness_pct`. Byte-stable for old recordings (the block is omitted when `combat_events_` is empty — the existing `OldFormatDocLoads` test pins this). |
| 6 | **`--harness` flag on `f4-scenario-player`** | `f4-scenario-player/cli/main.cpp` (modify), `f4-scenario-player/include/f4/scenario_player/player_app.hpp` (modify — add `run_harness(summary_out)`), `f4-scenario-player/src/player_app.cpp` (modify) | Sibling to `--screenshot`. Instead of running the render loop, builds `BvrInterceptHarness` over the loaded scenario, runs `execute()`, writes the summary JSON, prints the verdict, exits. The `--screenshot` flow already establishes the "load → run briefly → write artifact → exit" pattern (`player_app.cpp:101-105, 311-316, 444`); `--harness` substitutes the harness for the render loop. Same scenario JSON the headless `bvr_intercept_qc` consumes — the rendered variant is the *same* fight, watchable. |
| 7 | **MD5 helper extraction** | `f4-simulation/src/bvr_intercept_harness.cpp` (copy the self-contained `Md5` class verbatim from `campaign_war_harness.cpp:29-180`) | The C5 harness ships a self-contained MD5 (pinned by test vectors `md5("")` and `md5("abc")`). M4 needs the same primitive. Two options: (a) copy-paste (zero coupling, 150 LoC); (b) extract to `f4-simulation/src/internal/md5.hpp` and share. **Recommendation: copy-paste first** — the F4 codebase prefers duplication over premature sharing (see `f4-lzss` vs `f4-world-convert`'s decompressor, the JSON readers in `f4-world` / `f4-terrain` pre-`f4-json`). If a third consumer appears, extract then. |
| 8 | **Scenario template (optional, the QC variant)** | `f4-scenario-player/scenarios/bvr_intercept_harness_qc.json.in` (new, optional) | A copy of `bvr_intercept.json.in` tuned for the QC harness: explicit `record_every: 1` (every tick — the trace IS the certificate), `total_ticks: 18000` (5 min at 60 Hz — well past the AMRAAM's expected time-of-flight), `record_path: "@F4_BINARY_DIR@/bvr_intercept_qc_result.json"`. If the shipped `bvr_intercept.json.in` already produces a clean kill within the harness's default horizon, skip this — the existing template is the fixture. Decide at implementation time. |
| 9 | **Tests** | `f4-simulation/tests/test_bvr_intercept_harness.cpp` (new), `f4-recorder/tests/test_combat_events.cpp` (modify — add `SummaryEngagementSummaryForBvr`) | Mirror `test_campaign_war_harness.cpp`'s 6-test rig: (a) `RunsCertifiesAndIsDeterministic` — the shipped `bvr_intercept.json.in` plays out, kill happens, two-pass MD5 matches; (b) `EngagementCompletedVerdictFiresWhenKillHappens`; (c) `FightAliveVerdictFiresWhenAiNeverEngages` — a `hold_fire: true` scenario detects but never fires; (d) `SingleRunSkipsTheDeterminismProof` — `runs == 1` leaves the second MD5 empty and `deterministic` vacuously true; (e) `RejectsNonCombatScenario` — `combat.enabled == false` produces a harness error (not a verdict); (f) `EngagementSummaryBlockEmitted` — the recorder's `to_summary_json` carries the new block with correct timing. |
| 10 | **CMake wiring** | `f4-simulation/CMakeLists.txt` (modify — register `test_bvr_intercept_harness`, add `bvr_intercept_qc` tool target), `CMakeLists.txt` (root — add the tool to the `add_subdirectory` flow if needed; mirror how `campaign_qc` is wired) | The `bvr_intercept_qc` tool links `f4-simulation` + `f4-json` (for the summary writer) + `f4-scenario-player`'s scenario-loader path is NOT needed (it consumes a scenario JSON path directly via `f4::simulation::load_scenario`). |

## 5. Acceptance criteria for M4

The harness is "done" when ALL of the following hold:

1. **The shipped `bvr_intercept.json.in` plays out green through the harness.**
   `bvr_intercept_qc <build>/scenarios/bvr_intercept.json --horizon-sec 300
   --runs 2` exits 0 and writes a summary whose verdict block is all-true.

2. **The four verdicts are derived correctly:**
   - `deterministic == true` — the recorder's `to_json()` byte stream is
     identical across the two passes (compared as bytes, certified as MD5).
   - `engagement_completed == true` — at least one `EntityKilled` event
     with `subject_id == bandit_id` and `object_id == shooter_id`
     (attribution), within the horizon.
   - `roster_bounded == true` — at every sample, `live_entities ==
     initial_entities + spawned_entities − retired_entities`. Missiles
     count; `sweep_spent_missiles`'s removals are observed as retirements.
   - `fight_alive == true` — at least one `RadarTrackAcquired` event AND
     the shooter brain reached at least `BVRState::Entering` (read from
     the recorder's per-aircraft `ai_state` snapshots, or from the
     BrainComponent directly during the run).

3. **FreeFalcon range validation (AI_IMPLEMENTATION_PLAN §6):**
   - **MAR firing**: the first `MissileLaunched` event fires at a range
     ≤ `entry_range_nm` (26 NM for the default AIM-120C envelope —
     `1.3 × 20 NM`).
   - **Cooldown**: the second shot (if any) fires ≥ `fire_cooldown_sec`
     (4.0 s) after the first. The recorder's two `MissileLaunched` events
     carry `sim_time_s` stamps that differ by ≥ 4.0 s.
   - **Shoot-shoot doctrine**: at most `shoot_shoot_max_shots` (2) missiles
     per engagement per shooter. The recorder's `MissileLaunched` events
     with `subject_id == shooter_id` against the same target number ≤ 2.
   - **Crank geometry**: between the first shot and the second shot (or
     the kill), the shooter's heading diverges from the target bearing by
     `[30°, 60°]` (the `crank_offset_rad = 45°` ± tolerance). Read from
     the recorder's per-aircraft snapshots (`heading_rad` vs the
     target-bearing computed from positions).
   - **Beam-defeat geometry** (if the bandit defends): when the bandit's
     brain is in `CombatMode::Defensive`, its heading puts the incoming
     missile on the 3/9 line (±90° from the threat bearing, ±10°
     tolerance). Read from the recorder's `ai_mode == "Defensive"`
     snapshots.
   - **Kill attribution**: the `EntityKilled` event's `object_id` (killer)
     matches the `shooter_id` of the killing `MissileLaunched` event
     (correlated by `missile_id`).

4. **Replayability**: `FlightRecorder::load_json(result_path)` succeeds
   and reproduces the combat event timeline verbatim. The
   `engagement_summary` block in `summary.json` carries `first_detect_s`,
   `first_launch_s`, `first_kill_s`, `shots_fired`, `shots_hit`, and
   `weapon_effectiveness_pct` — all derived from the recorded events, no
   new state.

5. **The rendered variant works**: `f4-scenario-player --harness
   <build>/scenarios/bvr_intercept.json --summary-out <path>` loads the
   scenario, runs the harness headlessly (no GL context for the
   `--harness` path; the player app's harness mode skips window
   creation), writes the summary, prints the verdict, exits 0. The same
   scenario JSON the headless tool consumes — the rendered variant is
   the same fight, watchable in the viewer when `--harness` is absent.

6. **No new library surface**: `git diff --stat` shows changes only in
   `f4-simulation` (new harness + new tool + new test + CMake), the
   `f4-recorder` summary extension, and the `f4-scenario-player` CLI.
   No new `CombatEventKind`, no new `CombatEvent` field, no new bus
   message, no new `Scenario` field. (A new `CombatEventKind` is
   acceptable IF the validation surfaces a per-shot Pk-at-launch-time
   telemetry need — see §6, out of scope.)

## 6. Out of scope (deferred, deliberately)

- **A/G combat E2E** — M4 is A/A BVR. The A/G chain (bomb release →
  feature damage → objective sync) is already tested in `test_bomb.cpp`
  and `test_campaign_result_sink.cpp`; an A/G E2E harness is a separate
  tranche (M5 strike slice or a `ground_strike_qc` tool).
- **Multi-flight BVR (2v2, 4v4)** — M4 is 1v1. The 2v2 path is already
  tested in `AiVersusAiTwoShipBvrFight` and `TwoShipScenarioFilePlaysOut`;
  a multi-flight harness is a straightforward extension once the 1v1
  harness is pinned, but it's not the named M4 deliverable.
- **WVR / guns E2E harness** — M4 is BVR. The WVR (`wvr_merge.json.in`)
  and guns (`guns_merge.json.in`) scenarios already play out in the
  test suite; dedicated WVR/guns harnesses follow the same pattern but
  are separate tranches (the verdict gates differ — WVR cares about
  `WVRState` transitions, guns cares about `GunModule::burst_count`).
- **Per-shot Pk-at-launch telemetry** — would require a new bus message
  (`PkEvaluatedMessage` from `BVRModule` at launch time) + a new
  `CombatEventKind::PkEvaluated`. The pattern is fully established (see
  `GunFiredMessage` → `GunFired`); defer until the engagement_summary's
  `weapon_effectiveness_pct` (post-hoc, computed from hit/miss) is
  proven insufficient for tuning.
- **Countermeasures (chaff/flare) as entities** — the MissileModule
  already produces `should_chaff()` / `should_flare()` intents; the
  AI's defense flies. Countermeasures as physics entities (chaff clouds,
  flare heat sources) that the missile seeker must discriminate against
  is an M5+ fidelity tranche.
- **Real RCS tables** — LANDED as the Task 64 wiring tranche: the
  f4-sensors grid path (TargetSignature.rcs_grid / SignatureComponent
  .rcs_grid) is now FED — the scenario "combat" block takes
  signature_data_path + aircraft_signature_stems (name -> SIGDATA.LST
  stem), the Simulation owns the SignatureDataLibrary (brain-data
  pattern), and matching aircraft get the stem's azimuth/elevation RCS
  grid in place of the placeholder lobe model. Config-only join for now
  (campaign aircraft spawn from the scenario template, so the binding
  covers both paths); the VCD rcs_factor rides the world JSON
  (VehicleGroup.rcs_factor) as the future per-vehicle join data. Every
  pre-Task-64 scenario is byte-identical (no config = placeholder path).
- **FALCON4.WST parsing** — the built-in placeholder WeaponClassTable
  (AIM-9M/AIM-7M/AIM-120C/M61/Mk-82/GBU-12) is sufficient for M4. Real
  weapon class data via `f4-convert` is a Tier 1 follow-on.
- **DIS/networking** — the harness is single-process. f4-dis is a Tier 3
  follow-on; f4-geo was designed for it but the adapter is not part of M4.

## 7. Implementation order

1. **The `engagement_summary` block in `to_summary_json`** (item 5) — the
   smallest, most isolated piece; lands first; the
   `SummaryEngagementSummaryForBvr` test pins it before the harness
   consumes it.
2. **The harness header** (`bvr_intercept_harness.hpp`, items 1–2) — the
   public contract. Mirror `campaign_war_harness.hpp` line-for-line in
   shape; substitute the verdict semantics.
3. **The harness implementation** (`bvr_intercept_harness.cpp`, items 1–2
   + item 7's MD5 copy) — compose `Simulation`, drain ticks in 4-s
   batches, sample the recorder, compute the four verdicts, certify the
   MD5. The implementation reuses `Simulation::tick`'s entire combat
   sequence — the harness is a driver, not a re-implementation.
4. **The harness tests** (item 9, f4-simulation side) — mirror
   `test_campaign_war_harness.cpp`'s 6-test rig. The
   `RunsCertifiesAndIsDeterministic` test uses the shipped
   `bvr_intercept.json.in` template (already a CMake-configured fixture
   under `${F4_SCENARIOS_DIR}`).
5. **The `bvr_intercept_qc` CLI tool** (items 3–4) — mirror
   `campaign_qc.cpp::run_war()`'s shape. Write the three artifacts. The
   exit-code table is the acceptance surface.
6. **The scenario-player `--harness` flag** (item 6) — sibling to
   `--screenshot`. The harness mode skips the GL context; the player app
   loads the scenario, builds the harness, runs it, writes the summary,
   exits.
7. **CMake wiring + the optional QC scenario template** (items 8, 10) —
   register the test, add the tool target, decide whether the shipped
   `bvr_intercept.json.in` needs a QC-tuned sibling.

## 8. The decision the harness forces

M4 is the first time the *brain* drives the launch end-to-end (the M3
test `DetectTrackLockLaunchKillSweep` hand-launches the missile through
`weapons::launch_missile`). The brain's BVR module must:

- Detect (radar track Established, via `RadarBackedDetectionPolicy`).
- Employ (`BVRState::Entering → Employing`, `wants_lock` true, STT lock
  commanded).
- Fire at MAR (`release_pulse` true, `compute_pk ≥ 0.5`, cooldown 0,
  `shots_fired < 2`).
- Crank (`BVRTactic::Crank`, heading offset 45° from target bearing, hold
  8 s).
- Optionally fire the second shot (shoot-shoot, after cooldown).
- Separate (`BVRState::Separating`, `desired_heading = target_bearing + π`,
  cold).
- Lose the target on kill (`RadarBackedDetectionPolicy` returns all-false
  for the corpse → `BVRState::Employing → None` via `LostTarget` — the
  M2 design that prevents shoot-shoot from pumping rounds into a
  still-flying corpse).

If any of those steps stalls — the brain detects but never fires (MAR/Pk
gate mis-tuned), fires but never cranks (tactic selection broken), cranks
but never separates (state machine stuck), or never loses the corpse
(`RadarBackedDetectionPolicy` corpse-paint bug) — the harness's
`fight_alive` or `engagement_completed` verdict fires, the exit code
names the failure class, and the summary's `engagement_summary` block
shows exactly where the chain stopped (no `first_launch_s`, no
`first_kill_s`, etc.).

That is the M4 contract: a fight that *runs* end-to-end with a verdict,
or a fight that *fails* with a named failure class. The current state —
"it works in the test suite" — is neither.

---

*This document is the M4 entry in the combat chain plan. M1, M2, M3 are
landed (see `COMBAT_CHAIN_PLAN.md`); M4 is the named next deliverable.
The implementation notes live in the harness header
(`bvr_intercept_harness.hpp`) and the tool source
(`bvr_intercept_qc.cpp`) — the same place every tranche records its
decisions.*
