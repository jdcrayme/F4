# Combat Chain — M5a: The WVR / Guns Merge Harness + the Merge-Geometry Fixes

> **Status**: LANDED — implemented, verified, closed (see CHANGES.md for
> the tranche entry).
> **Prerequisite**: M4 landed (the BVR intercept acceptance harness —
> COMBAT_CHAIN_M4_PLAN.md). The combat chain's LAST un-certified
> surface was the inside-the-band fight.
> **Companion**: [COMBAT_CHAIN_PLAN.md](COMBAT_CHAIN_PLAN.md),
> [COMBAT_CHAIN_M4_PLAN.md](COMBAT_CHAIN_M4_PLAN.md),
> [AI_IMPLEMENTATION_PLAN.md](AI_IMPLEMENTATION_PLAN.md) (Steps 8–12),
> [POLE_DIAGNOSIS_RESULTS.md](POLE_DIAGNOSIS_RESULTS.md) (the balloon
> findings this tranche measured from the AI side).

---

## 1. Where we were

M4 certified the BVR intercept end to end and deliberately deferred three
surfaces (M4 §6): the WVR/guns E2E harness, the multi-flight harness, and
the A/G E2E. The WVR fight existed — `wvr_merge.json.in` and
`guns_merge.json.in` play out in the test suite (M3-TACTICS-2) — but two
of its integration tests were FAILING on main (the guns-merge pair, a
casualty of the flight-model stability work that changed the plant's
energy response), and no certified artifact existed: no verdicts, no
determinism certificate, no QC tool, no employment validation.

Verified by reading source and instrumenting the fight (not by trusting
plan documents):

| Capability | Status at M5a start |
|---|---|
| WVR fight E2E (heaters) | passing test, no harness |
| Guns fight E2E | **FAILING** (the guns-merge pair) — CA hijack + no gun solution |
| Multi-flight (2v2) | passing test (BVR scale), no WVR-scale harness |
| Band-transition evidence | none — the recorder only heard bus messages |

## 2. The fixes the harness forced (the guns-merge diagnosis)

Building the harness required the guns fight to actually complete; the
diagnosis chain produced five code fixes and one scenario
re-calibration, all doctrine-level and all measured:

1. **The commit band (`GunModule::in_commit_band`)** — the WVR merge
   commit (the collision-avoid exemption that hands the pass geometry to
   the weapons) previously reused the gun TRIGGER envelope, whose
   MINIMUM bound (0.08 NM) is trigger doctrine ("don't fire at a target
   past the pipper"), not geometry ownership. A head-on pass spends its
   most lethal second with the predicted range BELOW that minimum —
   measured: predicted range 467 ft at t+10.67 s, the commit dropped,
   CA broke the pass one tick later (the exemption lags one tick by
   design), the envelope never re-opened, and the fight never completed.
   The commit band is the envelope's OUTER edge only.
2. **The fight plane (Merge/Offensive altitude)** — the merge chased the
   target's LIVE altitude while the opponent's brain chased ours back:
   a two-brain positive feedback loop (each chasing the other's lagged
   climb response) that diverged 846 ft vertically through the merge.
   Merge and Offensive now hold the engage altitude captured at
   `engage()` — the engage() contract's own documented purpose ("the
   altitude the vertical game weaves around") — with the gun branch's
   lead-point tracking owning the residual aiming.
3. **Combat VS authority** — the WVR steering kept the nav-comfort VS
   tune (STAB-E1's 2,500 fpm cap + STAB-E29's 400 fpm/s slew) and could
   not command a descent that fights a spawn-energy balloon. A fight
   lives seconds; the CA break already flies 12,000 fpm with the slew
   off for exactly this reason. The WVR steering now does the same.
4. **MIL in the offensive states** — AB buys closure the head-on
   geometry already has, and the FCS's G-hold converts the excess thrust
   into climb (measured: the bandit's speed loop saturated at 1.5
   chasing the engage command and ballooned +2,400 fpm AGAINST its own
   descent command). Merge/Offensive fight at MIL; Defensive/BugOut keep
   AB (energy fights).
5. **The captured engage CAS** — `engage_speed_kts` is a CAP, not a
   chase target. Commanding 450 KCAS from a ~300-kt spawn state is an
   acceleration command, and the plant's closed-loop speed mode is
   marginally unstable (the pole program's default-tune finding) — the
   acceleration converts to a climb transient that scales with the
   energy gap (measured +2,400..+7,000 fpm across spawn calibrations).
   The merge now holds the CAS it arrived at, capped at doctrine.
6. **Merge STT refresh** — the gun fire control dead-reckons the track
   file on `age_s` (correct radar practice), but at merge closure a
   Veteran-interval (5 s) snapshot IS the entire gun envelope of error:
   a decelerating bandit's stale +195 ft/s climb rate dead-reckoned
   +977 ft of phantom altitude into the lead point and the hit-quality
   cone (1.1–1.7 deg) never closed (7–13 deg of error measured). While
   the WVR module owns the geometry (the merge commit), the fusion
   refreshes every tick — the same rule the missile-defense rung uses.
7. **The scenario re-calibration** — `guns_merge`'s spawn energy
   (760/720 fps) was tuned for the pre-P4.1 plant; on the current plant
   it guarantees a balloon (the pole program's domain). The scenario now
   spawns both jets at the plant's ~15,000-ft trim speed (532 fps ≈
   250 KCAS), the energy state the flight-model stability work
   certified. The doctrine under test is unchanged (head-on merge, CA
   exemption, gun employment, kill, disengage); the energy state is
   calibration, and it is the same calibration `wvr_merge` already flew.

## 3. The band transition as first-class evidence

The WVR fight's "engaged" rung is not a weapon event: a merge that
commits and never fires is a stalled fight at a DIFFERENT rung than one
that never detected. M4's proxying (launch/RwrLock events) cannot see it.

- `f4-recorder`: two new `CombatEventKind` values — `WvrEngaged` (11) /
  `WvrDisengaged` (12), wire names `wvr_engaged` / `wvr_disengaged`.
  Additive; old traces parse unchanged.
- **Parser bug fixed en route**: the kind-name round-trip loop walked
  kinds 0..8 only — `bomb_released` / `bomb_impact` silently degraded to
  the default kind on `from_json`. The loop now walks the full enum.
- `f4-simulation`: `Simulation::record_wvr_band_flips()` walks the active
  roster after the intents pass (recording only) and appends an event per
  combat-mode crossing. Subject = the aircraft; object = the engagement
  target (0 on disengage). The events make the band transitions
  replayable evidence for the harness's fight-alive gate.
- Documented finding: the tick-1 detection-policy handoff flickers the
  band (GCI-omniscient legacy rules on the first ladder tick, the
  radar-backed policy from tick 2 until its first scan completes) — the
  events record it honestly; the fight verdicts are handoff-insensitive.

## 4. The harness

`WvrMergeHarness` (f4-simulation) mirrors `BvrInterceptHarness` line for
line (create/execute/run_pass_/sample_/check_sample_/finalize_, the
4-second batch drain, the two-pass MD5 certificate, the diary/telemetry
split) and substitutes the WVR verdict contents:

| Verdict | M4 (BVR) | M5a (WVR) |
|---|---|---|
| deterministic | recorder bytes ×2 passes | same |
| engagement_completed | kill attributed via MissileLaunched | kill attributed via MissileLaunched **OR GunFired** |
| roster_bounded | live == initial + spawned − retired | same |
| fight_alive | detection + (launch OR RwrLock) | detection + **WvrEngaged** (band entry) |

The engagement window narrates the merge as a fight: `first_detect_s →
first_wvr_engage_s → first_gun_s / first_launch_s → first_kill_s →
first_wvr_disengage_s`, plus `last_wvr_engage_s` (re-attack count) and
the missile/gun shot splits. The failure diagnostics name the rung: no
detection / no band entry / no employment / no kill / attribution.

`wvr_merge_qc` (tools/) mirrors `bvr_intercept_qc`: the three artifacts
(`wvr_merge_result.json` — the byte-stable certificate;
`wvr_merge_summary.json` — deterministic content only;
`wvr_merge_diary.json` — telemetry) and the exit-code table:

| Exit | Meaning |
|---|---|
| 0 | all four verdicts green |
| 1 | usage / IO / load failure / harness abort (watchdog) |
| 2 | `combat.enabled == false` (the refusal — stable prefix contract) |
| 3 | fight_alive: no detection within horizon |
| 4 | fight_alive: detected but never entered the WVR band |
| 5 | engagement_completed violated (the rung named in the diagnostic) |
| 6 | roster_bounded violated (the sample named) |
| 9 | deterministic violated (the two MD5s name it; skipped at `--runs 1`) |

## 5. What does NOT change

- The component set, the binding design, the two-pass ECS tick, the bus
  contract — the harness is a driver, not a re-implementation.
- The M4 harness and its acceptance: `bvr_intercept_qc` re-run green
  (the MD5 certificate value moves with the fight's behavior — the
  recorder now carries band events and the merge flies the fixed
  doctrine — but the verdicts and the two-pass byte-identity hold).
- The flight-control pole program's scope: the balloon fixes here are
  steering-layer doctrine (the CA precedent), NOT FCS re-tuning; the
  P4.2 TECS margin campaign remains the structural fix for the
  default-tune cascade, and the pole CI gates still bound it.

## 6. Out of scope (deferred, deliberately)

- **The A/G E2E harness** (`ground_strike_qc`) — the other M4 §6
  deferral; the chain pieces are individually tested and the
  `--unit-strike` QC mode exists; the certified A/G rung is its own
  tranche.
- **Per-shot Pk telemetry** (`PkEvaluatedMessage`) — unchanged deferral.
- **Countermeasures as entities** — unchanged deferral.
- **The WVR verdicts' FreeFalcon employment-constant table** (the M4
  §5.3 analog: jink periods, merge entry timing) — the harness carries
  the structural gates; the employment-constant enforcement lands with
  the WVR tactics' fidelity pass (one-circle/two-circle needs the
  formation picture first).

## 7. Acceptance criteria (met)

1. The guns-merge pair is GREEN: `AiVersusAiGunsMergeFight` +
   `GunsMergeScenarioFilePlaysOut` pass; full suite 2423/2426 with only
   the 3 pre-existing flight-model failures (a strict subset of the
   Task-67 baseline — this tranche FIXES two of the known failures).
2. `test_gun_module` 18/18 (3 new commit-band tests: below-minimum
   hold, outer-edge agreement, prediction-not-snapshot).
3. `test_wvr_merge_harness` 7/7: the guns fight certified +
   deterministic; the WVR heater fight certified + deterministic; the
   non-combat refusal (exit-2 prefix); the never-in-band fight_alive
   diagnostic (exit-4 class); the `--runs 1` proof skip; the 2v2
   multi-flight acceptance (M4 §6's deferral closes); the MD5 digest
   shape.
4. `wvr_merge_qc` exit 0 on both shipped scenarios (`guns_merge.json`
   MD5 3d301d8e…, `wvr_merge.json` MD5 9c2bb944…), exit 2 on a
   non-combat scenario; `bvr_intercept_qc` re-run exit 0 (M4 unaffected).
5. The boundary verifier PASSES at configure (no new parser links on
   the runtime side).
6. The band events survive the recorder JSON round-trip with their
   kinds intact (the parse-loop fix is pinned by the harness test).

---

*This document closes the WVR/guns harness deferral of M4 §6. The
campaign-loop known-gaps section (CAMPAIGN_LOOP_PLAN §7) remains the
strategy-layer queue; the A/G harness is the next combat-chain tranche.*
