# Mission Contracts — the per-mission verification layer (MC)

> **Status: Active plan.** MC-1 (the instrumentation joins) is the
> landing tranche; MC-2–MC-5 build the verifier, the core archetypes,
> and the matrix wiring on top. This layer is the prerequisite
> instrument panel for the TOT-pacing design pass (CAMPAIGN_LOOP_PLAN
> §7, AIRWAR-QC) — per-flight TOT attribution is an MC-1 deliverable.

## 1. The problem: a passing war that reads as noise

The QC stack answers "did the machinery run" at three layers — the C++
exit gates (generic, fast), the `qc_missions.py` matrix (per-type
AGGREGATE counters: routes_attached, airborne_at_end, armed/released),
and the viewer replay of `trace.json` (a per-tick firehose). Nothing
connects a flight's IDENTITY (callsign, mission, target, TOT, station)
to what it DID, and nothing evaluates the SHAPE of what it did. A
human watching a green war run sees "aircraft spawning, flying around,
disappearing" — and a regression in BARCAP station discipline is as
invisible as a working BARCAP.

The mission-contract layer is the missing judgment: a declarative
per-archetype definition of "doing it right", evaluated per flight
against the observable stream, emitted as a human-readable report card
that is also machine-diffable.

## 2. What it is (and is not)

- **Not a replacement** for the C++ exit gates (they stay the fast
  generic authority) or the viewer replay (it stays the autopsy tool).
- **Supersedes** `qc_missions.py`'s hand-coded aggregate expectations
  (the `verdict_for()` pass) — the matrix runner survives as the
  DRIVER, invoking the contract verifier instead.
- **Adds**: the single-mission generator (spec-driven), the contract
  verifier, the report card, and the instrumentation joins everything
  reads.

## 3. The contract taxonomy

Contracts are per-archetype templates; a spec BINDS one (this target,
this TOT, this weapon). Clauses are TYPED so the verifier is generic:

| Clause | Measures | Archetypes |
|---|---|---|
| TIMELINE | wheels-up window, phase order | all |
| PATH | waypoint crossings vs lateral tolerance | all routed |
| STATION | racetrack dwell duration | CAP family, AWACS/tanker/ECM |
| TOT | arrival-at-delivery delta vs appointed | strike family, INTERCEPT |
| EMPLOYMENT | releases/impacts vs aimpoint + class | strike/SEAD/CAS/BDA |
| INTERACTION | AAR contact+transfer; escort proximity; FAC handoff | packages |
| RECOVERY | full stop at home, or honest booked death | all |
| ECONOMY | fuel at recovery, ordnance spent == intents | all armed |

Every clause carries a tolerance band. Bands live in the contract data
(documented like the known-reds: a re-pin shows in the card history).

## 4. The spec format (the user-facing generator interface)

A spec declares INTENT (what the ATM would file), not spawn poses —
the cousin of the scenario JSON, reusing its vocabulary where possible:

```json
{
  "name": "strike_gbu12_with_sead",
  "primary": {
    "mission": "AMIS_STRIKE", "team": "blue", "squadron": "auto",
    "target": { "objective": "auto", "aimpoint": "auto" },
    "tot_s": 3600,
    "loadout": { "weapon": "GBU-12", "count": 4 }
  },
  "support": [
    { "mission": "AMIS_SEADSTRIKE", "weapon": "AGM-45",
      "constraint": "fires_before_primary_tot_minus_s: 300" },
    { "mission": "AMIS_ESCORT", "constraint": "within_ft_of_package: 6000" },
    { "mission": "AMIS_TANKER", "track": "auto" }
  ],
  "opposition": [
    { "type": "sam_battalion", "objective": "same" },
    { "type": "cap", "team": "red" }
  ],
  "run": { "tier": "Tiered", "seed": 7, "horizon_s": 7200 }
}
```

Three parameter families: the mission itself (type, target selection,
TOT, loadout/weapon), the supporting cast (each adds an INTERACTION
clause to the primary's contract), and opposition (a SEAD contract is
meaningless without an emitter; an escort contract needs a threat).
Specs live in `qc/mission_specs/*.json` — versioned, diffable, the
named-profile library (same pattern as the scenario templates).

Two spawn modes:
- **injected** — the MissionIntent is built directly from the spec
  (deterministic, no ATM variance; the unit shape).
- **tasked** — the ATM files it in an otherwise quiet war (the
  integration shape; also proves the planner's target/route choice).

## 5. The report card (the join-point artifact)

One page per flight, verdicts plus measured numbers, tick references
into the trace:

```
VIPER 31 — AMIS_STRIKE (blue, spec: strike_gbu12_with_sead)
  TIMELINE   PASS  wheels-up 12:01:40 (window -60/+120s), phases in order
  PATH       PASS  6/6 waypoints, max cross-track 380 ft (tol 2000)
  TOT        FAIL  -04:12 vs appointed 13:00:00 (tol ±300s)   <- tick 214k
  EMPLOYMENT PASS  4x GBU-12 released, 3 impacts, 2 within 300 ft of aim
  INTERACTION PASS SEAD fired at TOT-06:20; escort max range 4.1k ft
  RECOVERY   PASS  full stop Kunsan 13:24, fuel 2,100 lbs
```

Emitted as `mission_report.md` (human) + `mission_report.json`
(schema-versioned, diffable). Failed clauses cite the tick and the
literal replay command. Cards are the review artifact — a PR that
touches behavior diffs cards, the same discipline as ledger MD5s.

## 6. The workflow: instigate → read → replay → inspect

- **Instigate, three doors**: (1) automatic — `verify.cmd`'s smoke
  rotation writes cards to `qc/verify/mission/`, a red clause fails
  the verify; (2) one-shot CLI —
  `campaign_qc --mission-spec <file> --report <dir>` (prints the
  verdict table, non-zero exit on failed clauses); (3) interactive —
  a Mission Lab panel in the world viewer (pick a spec, launch, ride
  the bubble) — the one new UI surface, on the existing campaign
  session view chassis.
- **Read**: the console table, then the card.
- **Replay**: DETERMINISTIC by construction (same binary + world +
  seed = same run — "replay what CI saw" is exact, not a re-roll):
  `f4-world-viewer --replay <run>/trace.json`. MC-2 adds card-driven
  tick bookmarks.
- **Inspect ladder**: console verdict → `mission_report.md` → the
  JSON diff (compare mode: same spec at two commits) → the event
  journal + `campaign_result.json` → the trace firehose →
  `campaign_after.world.json` / the `.cam` write-back.

## 7. The depth ladder (regression fit)

Single missions are CHEAP (tiered runs sustain 60–100x compression —
a 60-min mission is <1 min wall clock), but dozens per change is
still a non-starter:

1. **Every change** (`verify.cmd`, stays fast): the injected-mode
   smoke specs for the core archetypes (BARCAP, STRIKE, ESCORT,
   TANKER; +3–5 min), cards diffed in review.
2. **Pre-release / nightly** (`--mission-matrix` opt-in): the full
   tasked-mode matrix over all filed types (~20–40 min) — this also
   produces the HONEST COVERAGE MAP (which of the 41 wire types have
   behavior vs vocabulary-only; the stubs become the tranche list).
3. **Investigation (on demand)**: any spec at Full fidelity, 1x,
   full recording.

Clause-red handling follows the known-reds pattern: fix it or list it
with an owner.

## 8. Tranches

- **MC-1 — the instrumentation joins — LANDED (2026-10-02).** As-built
  above plus two semantics the first real recordings forced: waypoint
  captures carry the CLOSEST APPROACH while the waypoint was the target
  (the capture-tick distance measures the NAV-B turn-anticipation lead —
  the stock BARCAP's anchor legitimately reads ~13,000 ft), and the
  splice's cursor jumps are excluded (sequential-advance rule — the FID-4
  splice would have booked a phantom 185,463-ft miss). The snapshot
  identity also carries the route's total station contract (the STATION
  clause's applicability flag: the stock saves' own BARCAPs carry no
  racetrack — it rides the strategy layer's filings) and the nav's leg
  cross-track (a dead field since the first snapshot format — now
  filled) and the internal fuel (also dead — now filled).
- **MC-2 — the verifier + report card — LANDED (2026-10-02).**
  `scripts/mission_verify.py` (stdlib, `--selftest` pins the evaluator's
  verdicts on crafted traces): archetype selection from the flight's
  AMIS name (cap/strike/support/escort/generic), the generic clause
  evaluator, `mission_report.md` + schema-versioned
  `mission_report.json`. Clause semantics settled by the first real
  cards: TIMELINE accepts the airborne-start arc (Approach-start
  scenarios); PATH is phase-scoped to Enroute and bands the
  STEADY-STATE tracking (the p90 of the window's final quarter) with
  the departure transient REPORTED not banded; STATION skips when no
  station was promised; TOT measures the delivery-waypoint capture vs
  the appointment; EMPLOYMENT counts releases (skip on a booked death);
  RECOVERY accepts Complete, the stopped-on-deck end state, or (later)
  a booked death. **The first surfaced findings** (the layer working as
  designed): (a) the splice-resumed departure diverges to ~21,300 ft
  off its first leg before converging (~4 min, self-healing — the
  departure geometry vs the T3 route-path projection; owner: a splice
  refinement tranche); (b) a transfer flight's enroute stub never held
  its leg (steady p90 8,947 ft — the degenerate-stub shape; same
  owner). Both cards carry them with numbers.
- **MC-2b — the isolation profile — LANDED (2026-10-02).**
  `campaign_qc --single-mission TYPE` (short form or AMIS name): the
  mission filter pinned, one aircraft, one seeded ladder cycle (35-min
  tick contains the 1800-s first cycle; the 3600-s second falls
  outside), saved-flight emission SKIPPED (a saved flight would consume
  the cap at t=0), and a 45-h recorded horizon. Deterministic
  force-file: the tool seeds a MissionRequest per belligerent (each
  defending its own first objective / striking the first enemy
  objective in wire order — per-team pick; the one-aircraft cap keeps a
  single flight) through the new `Campaign::seed_mission_request` ->
  `AirTaskingManager::seed_request` — the request rides the NORMAL
  pipeline (prioritize/deconflict/package/escorts/TOT slot); a team
  that cannot field the profile lands in the ATM's own unfilled
  counter. **First findings** (the generator immediately paid for
  itself): (a) TANKER cannot isolate on TestCamp — no tanker unit
  resolves (the coverage map's first row); (b) ATM-composed spawns
  of some squadrons hit unknown-flight-id in the PLAIN qc path (the C5
  war's session path spawns them — the synthetic=48 certificate — so
  the seam is the plain path's unit map; owner: the coverage/MC-4
  tranche). **RETRACTED (MC-3, the honesty rule)**: the original (a) —
  "the ATM strike flew STK1-3 with no target/TOT" — was a MISREAD: the
  isolation drew a save-package RECEIVER (its route carries WP_REFUEL —
  TestCamp's 158 receiver flights) and the STK1-3 aims are the EMPL-2
  AAR racetrack stack (refuel action, 45-min hold), not strike points;
  the flight held the stack and recovered when no tanker answered
  (TestCamp fields none) — correct degraded AAR, and the EMPLOYMENT
  clause now says so (`SKIP receiver route`). The genuine open
  employment question — a saved STRIKE flight with a delivery-action
  waypoint + target to isolate — waits for the tasked mode (MC-4) or a
  different world; the ATM's own strike route DOES stamp delivery
  waypoints (the RouteBuilder's C3 arming rule).
  `--mission-spec` (the full spec file: loadouts, the supporting cast,
  opposition) rides with MC-3's archetype contracts.

- **MC-3 — the core archetype contracts — OPEN; the employment
  semantics LANDED (2026-10-02).** The EMPLOYMENT clause now splits
  OPPORTUNITY from EXECUTION: the route's delivery-action waypoints
  (wp_action 14-19 on the capture events) are the opportunity; the
  BombReleased events are the execution; no opportunity = SKIP (the
  loadout/route-arming concern), opportunity-missed = FAIL. That split
  was forced by the isolation runs' finding of record: **the
  saved-flight strike routes carry their delivery waypoints with
  action 0 and NO target stamp** (measured on the single-mission
  STRIKE: aims STK1-3 with no tgt= suffix, captures with action 0/4/7 —
  the A-G route arming resolved targets for the harness path but not
  for the saved-flight path; owner: the EMPL tranche, which also owns
  the target/TOT carry to the intent that MC-2b named). The CAP
  engagement awareness, ESCORT proximity (needs the package_id join),
  and TANKER AAR clauses are designed (§3) and land with their
  exercisable runs (the escort/tanker isolation needs the MC-4 tasked
  mode or a tanker-carrying world). Also LANDED with the MC-3 opening
  (2026-10-02): **FIRST_ATTEMPT** (a go-around is a deviation — FAIL
  unless the contract's allow_goaround flag deliberately tests it;
  counted from the snapshot ai_state transitions, which the 6-s
  decimation cannot miss) and **DURATION** (sortie time wheels-up ->
  recovery inside the band — the honest landing-time expectation, since
  the saved wires' own times are the ATO planner's multi-day horizon).
  Retro-proven: the T4b-era BARCAP wingman's card reads
  `first_attempt FAIL 1 go-around(s)` while every current-build flight
  on disk passes both clauses (the BARCAP pair, the strike isolation,
  landing_only — sortie 38 min).
- **MC-3 — the core archetype contracts**: BARCAP (station +
  engagement + recovery), STRIKE (path + TOT + employment +
  recovery), ESCORT (proximity), TANKER (AAR interaction), and the
  first new BEHAVIOR tranche the matrix schedules (the honest
  coverage map names it — likely AIRLIFT, the logistics family whose
  ground-side consumer, DOM-2 supply, already exists; there is no
  paradrop anywhere in the stock wire we converted).
- **MC-4 — the matrix wiring — OPENING LANDED (2026-10-02).**
  `qc_missions.py --record` now runs the contract verifier on every
  run's trace (the `cards` column + per-flight failed-clause details in
  `qc_matrix.json`; `--no-verify` opts out; card FAILs surface but do
  not gate the matrix exit — the owned findings are card data until
  their tranches land). **The coverage map exists** (the TestCamp run:
  22 types, 86 flights carded, 51 s wall) — and its headline finding:

  **ROUTE HOLDING ON SAVED FLIGHTS (the dominant behavioral finding).**
  52 of 86 carded flights FAIL the PATH clause — steady-state leg
  cross-track above the 2,000-ft band on the SAVED routes (the strike
  isolation's flight never got under 17,409 ft). Not a clause artifact:
  saved routes are turn-SPARSE (0.07 captures/min median vs the 0.5/min
  calibration flight), so the p90 is genuine steady-state deviation.
  Downstream: 20 first-attempt FAILs (arrive off-leg -> unstable
  approach -> go-around) and 30 recovery FAILs that are the 30-min
  matrix horizon cutting missions mid-flight (a horizon artifact, not a
  defect — per-category horizons are the fix). Owner: a nav/flight-
  control route-holding tranche. Until it lands, the cards say what
  Falcon's campaign would have said: the flights go where the route
  points them only roughly.

  The rest of the map: the strike family's 8 types all gate-fail
  (exit 4) — **re-attributed by the arming tranche (2026-10-02)**: the
  saved-flight arming synthesis is LANDED (build_mission_plan_from_
  flight synthesizes the delivery waypoint — the target's position, the
  family's delivery action, the trigger key — before the terminal LAND,
  when the mission delivers, the route lacks the point, and the flight's
  own target VU resolves), but on TestCamp it never fires: **the
  generated save's delivery flights carry NO target VU at all** (both
  the INTSTRIKE and STRIKE isolations' flights read fp.target invalid).
  The strike-family FAILs are a FIXTURE DATA gap (the generator wrote
  no targets — owner: the fixture/INIT-1 tranche), not an arming-code
  gap; real saves whose ATO writes target VUs get the synthesized
  delivery point. ESCORT and OTHER book 0 airborne (the plain-path
  ground-ops crawl); the CAP/support/logistics families fly and
  recover. TANKER: no flights in this world (the known TestCamp gap).

- **MC-4 — the route-holding tranche — LANDED (2026-10-02).** The
  dominant finding's root causes, each measured on the coverage map's
  own traces:
  1. **The intercept clamp + the NAV-B2 damper's fixed point.** The
     flat 20-deg intercept clamp (plus the closing-rate damper whose
     equilibrium `corr = corr_p − g·sin(corr)` throttles a saturated
     intercept to ~12 deg) converged every departure-scale excursion at
     ~140 ft/s — the AWACS departure flew parallel-COMMANDED for 224 s
     (roll_cmd 0.00 at xte 22,700 ft) before crawling onto its line.
     Fix: the intercept limit is distance-scheduled (max_intercept_rad
     at or inside xte_gain_ft, ramping to max_intercept_far_rad 0.95 rad
     at 3×gain) and the damper fades out on the same schedule. Near-
     field law byte-identical (all NAV-B/NAV-B2 pins hold).
  2. **The departure itself.** The takeoff FlyOut climbed the whole
     2,500 ft on the runway heading and THEN reversed — the measured
     21,000-34,000 ft excursions are the bank-limited turn that follows.
     Fix: the brain seeds the FlyOut's departure course from the plan's
     first leg (FlyOut entry, gated: not tankers, not already-armed
     receivers — the AAR e2e's co-based join choreography owns those
     departures). BARCAP2's departure transient: 21,311 → 7,371 ft.
  3. **The receiver join-stack orbit was unflyable.** A 12k×6k
     racetrack (legs under one turn radius at ANY of the nav's speeds;
     at the 400-kt leg speed R ≈ 33,000 ft) held a waiting receiver at
     a permanent 8,000-20,000 ft offset — the SAD waiter's whole
     30-minute station cycled xte −20,284..−989. Fix: the orbit is a
     twelve-point circle (30-deg fly-through corners leave ~700-ft
     bows), R = 24,000 ft, the whole lap at the nav's 200-kt floor —
     the SAD/AIRLIFT/TANK waiters hold it.
  Matrix re-run (same 22 types, `qc/coverage_map_routehold12`):
  **PATH fails 52 → 13, PASS 22 → 55**; the BARCAP lead's PASS holds
  (392 ft; transient 7,371 ft). Residuals, honestly: the ~170-degree
  runway-vs-leg departures (the ONCALLCAS pair, deterministic) still
  peak ~22k — the turn cannot complete inside a 2,500-ft climb, and
  their 3-minute windows end mid-recovery; two co-based flights whose
  route collapses to a 30-second Enroute window beside their own orbit
  (SEADESCORT CS008-5, BDA CS004-5 — both PASSED at baseline on the old
  box's longer walk; a regression, counted); the AWACS departure-tail
  artifact (the metric's final quarter lands on the convergence because
  the flight settles left of the line). Owner of the remainder: the
  departure-procedure tranche (turn during the climb at a lower gate)
  and the per-category horizons.

- **INIT-1c — the saved ATO's TOT appointed; the tot clause gains its
  saved-path semantics (2026-10-02).** The tot clause's 43 SKIPs were
  "no TOT appointed": the bridge never carried the save's own
  time_on_target onto the plan. The session's TOT arithmetic
  (seconds_to_time_on_target: tot − (epoch + clock)) fixed the units —
  the save's times are ABSOLUTE campaign seconds and the appointment is
  campaign-relative. The plan build now appoints plan.tot_s = tot − now
  for delivery missions with a FUTURE appointment (the sim does not
  model the ATO push wait — flights spawn at their bases and fly at the
  run's start). The measured TestCamp distribution across the delivery
  families: **4 stale rows, 109 beyond the run, 0 in-horizon** — so
  eval_tot gained the matching scope rule (an appointment beyond the
  run scopes out as "+N h against a push wait the spawn does not
  model" instead of sitting unappointed), and the cards now name their
  own stale ATO rows ("+2121 h", "+626 h"). In-horizon appointments
  judge automatically — the clause lights up the day the world or ATO
  carries near-term TOTs. The push-wait model (flights holding for
  their appointed push time) remains the CAMPAIGN_LOOP_PLAN §7
  TOT-pacing tranche's to name.
- **INIT-1d — the employment clause closes: 31 PASS / 0 FAIL
  (2026-10-02).** The two SAD employment FAILs had distinct mechanisms,
  both instrumented and fixed:
  1. **CS079-1 — the delivery-first route.** Its saved route opens WITH
     the delivery point (strike → egress), and the handoff's
     resume_from(1) skipped the mission itself (the delivery waypoint
     "captured" by the handoff jump at 153,501 ft). The reduction now
     has its exception: a route whose first waypoint is a delivery
     action resumes at it (resume_from(0) no-ops — the cursor stays on
     the mission).
  2. **CS138-2 — the armed-stick capture race.** The delivery
     waypoint's speed-proportional capture (max(3000, 10*vcas) ≈ 4,020
     ft) sequenced it AT closest approach (measured: 4,013 ft) while
     the release gate wanted the aircraft inside its range (~9,400 ft)
     with alignment — the attack run dismantled before the stick fell.
     The brain now holds the delivery waypoint's capture while the
     stick is armed, the aim valid, and the stick unfallen
     (NavigationModule::hold_delivery_capture), and the attack-run law
     gained the along-track reversal it lacked: past the aim the leg's
     extension ran AWAY from the target forever (the armed flight
     chased the extension for its whole 120-minute run, closest
     approach 8,715 ft, inside release range the whole time — the same
     line-chase the landing module's T5 wrong-side pursuit solved).
     Held, the attack leg flies INTO the aim; the gate fires inside its
     envelope; the brain lifts the hold when the stick completes and
     the capture sequences the egress.
  Measured (qc/coverage_map_init1d): SAD 4/4 flights release; **the
  employment clause 31 PASS / 0 FAIL / 0 SKIP; the matrix totals 123
  releases / 123 impacts / 38 features destroyed; gate fails zero.**
  The matrix rows now carry the features counter (the aimpoint/
  feature-layer concern — OCASTRIKE/STRATBOMB impacts with 0 feature
  kills — is visible per type). The card residuals stand: PATH 45 on
  the long arcs, first_attempt 13 go-arounds, recovery 11
  world-geography tail.- **INIT-1e — the WP_SAD vocabulary collision found and gated
  (2026-10-02).** The PATH 45 characterization exposed the support
  families' regression: the wire's WP_SAD (16) is SHARED vocabulary —
  the BAI/STRATBOMB planners write it on delivery points AND the
  support planners write it on racetrack corners (the measured AWACS
  route: actions 1,16,16,7,7; TANK: 1,8,8,16,16,7,7). INIT-1b's
  unconditional WP_SAD addition armed the StrikeModule on the support
  flights' own orbit corners, and INIT-1d's armed-stick capture hold
  then PINNED them there — the AWACS flight held its first orbit corner
  for the whole run (hdgrw swinging ±180, cLat -74,222 at run end). Fix:
  MissionPlan carries `ag_delivery_mission` (stamped by the bridge from
  the mission byte's category — Strike/SEAD/CAS deliver; default true
  keeps the engine-agnostic harnesses delivering), and every
  delivery-action consumer gates on it: the strike arming, the
  armed-stick capture hold, and the delivery-first resume. Measured
  (qc/coverage_map_init1e): AWACS 4/4 recovered (was stranding),
  employment 31 PASS / 0 FAIL holds, releases hold at 123 / 123 / 38,
  gate fails zero, **PATH 45 → 43** (the support families' corner
  excursions gone; the remainder is the delivery-arc route-holding and
  the departure-turn families). The predicate lives at the single
  source of truth (mission_type.hpp: mission_is_ag_delivery).- **INIT-1f — the pre-turn slowdown; the PATH tail measured to its
  geometric floor (2026-10-03).** The PATH 43 characterization (the
  excursion intervals + the leg context per failing flight) split the
  population: the big-p90 flights (12-31k) all peak ON A TURN — the
  measured BARCAP2 recovery turn was flown at 402 kts (R ~30,000 ft,
  the excursion 30,700 ft). Root cause: the turn-slow gate fires on the
  heading error, which spikes only WHEN the turn starts — a jet at
  400+ kts cannot decelerate inside it. Fix: the turn's size is
  knowable on approach (the course change from the incoming leg to the
  outgoing one), so controls_for_waypoint commands the corner speed
  (250 kts) within the 60,000-ft deceleration gate. Measured: the peaks
  fell 35-55% (AWACS 28,030 -> 12,704; BARCAP2 29,072 -> 16,271;
  SWEEP 29,647 -> 19,133/8,272). The deeper measurement closes the
  family honestly: **the remaining excursion is the geometric floor** —
  a 120-deg course change at the slowest legal enroute speed (250 kts,
  R ~12,000 ft) deviates ~R from the new line by construction (the
  tangent arc's mid-turn deviation is R-scale); the 2,000-ft band is
  unreachable mid-turn. The turn-window semantics (the metric judging
  steady-state legs, not turns) is a metric-doctrine question for the
  maintainers; the teardrop/holding-entry refinement would shorten the
  chase but not the arc's R-scale deviation. Measured
  (qc/coverage_map_init1f): recovery 68/8 (the best yet — the slower
  corner turns also recover cleaner), employment 31/0, gate fails
  zero, wall ~660 s. PATH sits 33 PASS / 43 FAIL with the FAILs at
  their geometric floor pending the metric-doctrine decision.
- **INIT-1g — the release-accuracy tranche: the pipper gate restored,
  the two-stage release, the feature-aim co-location (2026-10-03).**
  The feature-kill gap (impacts with 0 features destroyed) traced to
  the release accuracy: the measured sticks landed 287-2,680 ft from
  the aimed feature against a ~144-ft single-hit envelope. Three
  layers landed:
  1. **The CCIP pipper gate restored** (impact_tolerance_ft 150 in the
     release condition): EMPL-1 parked it because the old LNAV/homing
     run-ins could not hold sub-0.9 deg — but the virtual attack leg
     (EMPL-1a) converges the release geometry in both axes
     exponentially into the aim, and the armed-stick hold (INIT-1d)
     flies the run into the target. The converged sticks now release
     at a 287-ft pipper and the gate fires on the converged pass.
  2. **The two-stage release** (release_holdover_ft): the pipper-first
     accuracy with the bounded cone-edge fallback — the approaches
     whose pipper never tightens release holdover-short instead of
     arming-no-release (the SAD/STRATBOMB/OCASTRIKE approaches hold
     ~1-2.5k; the 1,200-ft holdover measured the preemption problem —
     it fired before the pipper's own pass and wiped INTSTRIKE's 13
     kills; 300 keeps the pipper's pass first).
  3. **The feature-aim co-location**: the co-located delivery waypoint
     aims at the FEATURE the strike wiring aims at (the same
     resolve_feature_aim rule) — the nav's attack run, the release
     pipper, and the feature damage converge on the same ~150-ft point.
     Measured: INTSTRIKE features 7 -> 15, STRIKE 0 -> 5.
  The mid-turn release hold (the heading-rate bound, 4 deg/s) landed
  with it: the pipper's track estimate lags the true velocity mid-turn,
  and a turn-time release landed where the STALE track pointed (the
  measured OCASTRIKE stick: released on a turn-dip pipper, 12,161 ft
  wide). Measured (qc/coverage_map_init1g-era runs): INTSTRIKE 15
  features, STRIKE 5, releases hold on all types; OCASTRIKE/SAD/
  STRATBOMB still show 0-2 features — the scoped remainder:
  OCASTRIKE's converging sticks land 448-717 ft from the aimed feature
  (the release-solution residual vs the 144-ft envelope; the card's
  drag model vs the bomb sim at the 1,500-ft delivery — the
  drag-factor default is inert, arm_flight_strike overrides it), and
  the SAD/STRATBOMB targets are BATTALIONS (unit entities with no
  FeatureSet — their damage ledger is unit damage, not objective
  features; the nearest-objective fallback aims the stick at the
  battalion's parent objective but the impact books on the unit).
  Owner: the unit-damage ledger tranche + the release-accuracy
  refinement.- **INIT-1i — the MEASURED fall time: OCASTRIKE's converging sticks
  12,161 -> 316-450 ft (2026-10-03).** The CCIP accuracy tranche's
  decomposition (the along/cross split of the first impact): the
  cross-track was 47 ft — the attack run's lateral was perfect — and
  the ALONG miss was 12,161 ft: the bomb's actual travel was 13,768 ft
  while the module's vacuum model credited ~8,400. The bomb sim's drag
  limits the fall to the terminal velocity: 2,251 ft took 19.7 s vs
  the vacuum 12.1 s — the computed range (gs x vacuum-fall x drag) was
  1.63x short, the release fired at aim_dist ~1,600 ft, and the stick
  glided 12,000 ft past the aim. Fix: the arming flies the bomb sim
  ONCE at the delivery geometry (dz 1,500, 675 fps) and stamps
  `measured_fall_time_s` on the strike config; the module scales it by
  sqrt(dz/ref_dz) per release (the analytic model stays for the tests,
  default 0). Measured (qc/featkill/ocastrike9): the converging sticks
  12,161 -> 316-450 ft; INTSTRIKE holds 15 features. The scoped
  remainder: 8 of 16 OCASTRIKE bombs (two flights) still release
  10-12k wide on their second-pass alignment geometry — the CCIP
  accuracy chain's own tranche — and the SAD/STRATBOMB battalion
  targets have no FeatureSet (the unit-damage ledger tranche).- **INIT-1h — the drag factor parameterized by the delivery dz
  (2026-10-03).** bomb_drag_factor_for now computes its dragged/vacuum
  ratio at the delivery dz (the co-located points fly at ~1,500 ft, not
  the 5,000-ft reference). Measured: the impacts were byte-identical at
  both references — the Mk-82's drag is near-nil at 675 fps at these
  dz (the ratio ~1.0 either way); kept parameterized as the more
  correct form for cards whose drag is significant at low dz. The
  448-717-ft stick residual is therefore NOT the drag factor: it is the
  pipper model vs the bomb sim at the release (the CCIP accuracy
  chain's own tranche), plus the second-pass overflight sticks
  (10-12k) whose release geometry needs the second-pass analysis.- **INIT-1i follow-up — the per-flight armed arcs measured (2026-10-03,
  the [strike] probe, qc/init2).** The OCASTRIKE run's four attack runs,
  separated by id: three converge well (the pipper minima 426 / 1,978 /
  3,990; the sticks 316-450 / 448-717 ft) and one — the delivery-first
  flight 4294971682 — never converges: the pipper minimum 30,035, the
  dist bottoming at 34,931 ft then receding, the stick falling only via
  the holdover (10-12k wide). Its route resumes AT the co-located
  delivery point (INIT-1d's resume-0), the attack leg anchors at the
  spawn, and the approach geometry (the spawn heading vs the
  assembly-to-aim line) defeats the leg's cross-track convergence — the
  aircraft oscillated wide and receded without ever pointing at the
  aim. Owner: the CCIP accuracy chain's approach-geometry tranche (the
  delivery-first attack runs need the arrival heading handled — the
  resume-0 flight's attack leg anchors at the SPAWN, so the approach
  heading is the spawn heading). The other three flights + all the
  measured improvements hold: 16 releases / 16 impacts / 0 features on
  OCASTRIKE with the wide sticks from this flight; INTSTRIKE 15
  features, STRIKE 5.- **INIT-2 — the exponential-decay localizer intercept: the go-arounds
  13 -> 2 on the characterized population (2026-10-03).** The
  go-around characterization's fix landed: the far-field localizer
  intercept's cut is now PROPORTIONAL to the lateral offset — the lead
  grows as |xtrack| / tan(asin(k|xtrack|/V)) (the decay constant
  k = 0.035/s, the tau ~28 s) instead of the constant atan(1/2.5) ~
  22 deg the linear lead scaling produced. The constant cut crossed
  the course at a constant ~200 ft/s with the turn-back arc
  overshooting ~the turn radius: the measured oscillation +/-5-12k ft
  around the course, the establish gate (250 ft) unreachable through
  the oscillation. The exponential eases the cut as the offset closes
  (the 1,000-ft cut 8.7 deg = the old law's boundary value
  continuous; the 3,000-ft cut 6 deg): the lateral converges
  MONOTONICALLY (the measured BAI re-run: 1,095 -> 271 ft hugging the
  gate, no crossing overshoot), the establish fires at the crossing.
  Measured (qc/goaround/bai3 + qc/coverage_map_init2): the BAI
  go-arounds 2 (the first-attempt FAILs 2 on BAI; CS065-4's single
  go-around is an honest missed approach — the flight recovered on
  the second); the full matrix: first_attempt 13 -> 23 FAILs across
  MORE flights (the newly-recovering population's own honest
  go-arounds), PATH 43 -> 21 (the monotonic approach also cleans the
  PATH tails), recovery 68/8 holds, gate fails zero. The landing
  suite 32/32; the scenario trio exits 0; verify.cmd GREEN. The
  decay-k tuning (0.020-0.035 measured flyable) is the approach-
  quality tranche's own knob.- **INIT-2a — the battalion-target retarget: the feature-kill chain
  closes for the last objective-defended types (2026-10-03).** The
  feature-hp/warhead balance question DISSOLVED into a target-
  resolution finding: the OCASTRIKE/SAD/STRATBOMB delivery waypoints'
  target_nums resolve through the G2 UNIT map to battalion entities —
  transforms yes, FeatureSetComponent no (the runtime [plan] probe:
  NO FeatureSet on all four targets, in both the tool's and the sim's
  worlds; the [populate] probe: 2659 feature sets on the objective
  entities, ruling out the loader). The planner meant the objective
  the battalion defends: its feature grid (+/-375 ft, 250-ft spacing)
  sat all around the aimed battalion center, and every burst landed
  BETWEEN features — bombs on battalions book the (empty, elements: 0)
  unit-damage ledger, so nothing registered. Fix: when the resolved
  target carries no FeatureSet, the plan build retargets the delivery
  to the nearest feature-bearing objective within 15 NM and aims at
  its alive feature (the same resolve_feature_aim rule). Measured
  (qc/featkill/*2c + qc/coverage_map_init2b): OCASTRIKE 0 -> 5
  features, INTSTRIKE 16, STRIKE 2, STRATBOMB 1 — the matrix totals
  **features 49 -> 55**, gate fails zero, employment 31/0, recovery
  68/8, PATH 21. The two-point measured-range refinement (the linear
  interpolation between dz-1000/6000 probes) measured WORSE on kills
  (16 vs 23 across the five types — the release geometry is chaotic
  w.r.t. small range shifts) and was reverted to the single-point
  sqrt scaling with the evidence in the header. The [populate] probe
  stays (the world-shape instrumentation). The scoped remainder: SAD's
  stick geometry still lands 563-846 ft from the aimed feature (the
  pipper convergence on those approach geometries — the
  release-accuracy chain's honest tail), and the unit-damage ledger
  (elements: 0 in the fixture) is the fixture-generator tranche's.
- **INIT-2b — the ground-avoid stand-down for the committed delivery
  pass: the balloons stop throwing sticks 10-12k long (2026-10-03).**
  The delivery-first approach-geometry tranche opened on flight
  4294971682 (pipper minimum 30,035 ft) and closed on a DIFFERENT
  owner: the terrain-avoidance pull-up. The instrumented [strike]
  probe (now per-instance, carrying the virtual leg's course/xte/
  along, the flown wp z, and the steering cascade's vs_target/gamma/
  alt_err/theta_target) autopsied the OCASTRIKE releases: two sticks
  released CLEAN at the pipper crossing (dist ~5,000, pipper 440-729,
  impacts 313-453, features) while two released at a +11,500-fpm
  CLIMB with the pipper at 6,947 ft and landed 10,000-12,400 ft long,
  zero features. The chain: the bridge's 1,500-ft delivery floor rides
  EXACTLY at the ground-avoid module's MIN_ALTT (1,500 ft clearance)
  over the flat z=0 campaign world — at 1,593 ft with -820 fpm of sink
  the predicted clearance read 1,511 - 82 < 1,500 and the escape
  fired, preempting the nav (ground avoid preempts everything); the
  recovery's dz inflation grew the computed release range
  (4,500 -> 8,500 ft), the holdover fallback opened, and the stick
  fell during the pull-up. The two clean sticks released before
  reaching the floor and never tripped it (pred clearance 1,993 /
  1,649). The pipper itself never gated a release on ANY pass: it is
  the release-point error (dist - rng, mostly along-track) and it only
  crosses when the pass stays level at the floor — the holdover was
  the de-facto release authority. Fix: the committed delivery pass
  STANDS THE TERRAIN PULL-UP DOWN (set_delivery_stand_down, set while
  a delivery waypoint with a live target is current and the stick is
  unfallen — the EMPL-2b refuel doctrine: the pass is planned low
  flight; the release gate keeps its own 500-ft AGL floor). The
  delivery waypoints also now ride the AIM's elevation
  (kDeliveryAltAboveAimFt 1,500 — a no-op on the flat world, the right
  vocabulary for elevated aims; the synthesized delivery wp's +300
  stopgap retired). Measured (qc/init2b_geo/ocas8, deterministic):
  ALL FOUR OCASTRIKE sticks release level at 1,497-1,499 ft at the
  crossing (dist ~4,080 = rng 4,380), pipper 65-751 ft, impacts
  66-758 ft, 4 features; one first bomb gated at pipper 65 ft — the
  150-ft gate FIRES on the clean pass. Matrix (qc/coverage_map_init2b2
  vs init2b): **features 55 -> 57** (BAI 6 -> 10; INTSTRIKE -1,
  OCASTRIKE -1 on different targets), all rows PASS, releases/
  impacts hold at 123/123. The employment 31 -> 28 PASS delta is the
  TOT push-wait gap EXPOSED, not a regression: three flights (SAD
  x2, BAI x1) hold their TIMING stations for ATO appointments 409-
  2,755 h beyond the run and now correctly never employ — the
  balloons used to knock them off their timing holds into accidental
  zero-feature deliveries (their tot clauses already SKIPped
  "beyond the run"; the push-wait model stays the fixture-generator
  tranche's). The GA suite grows DeliveryPassStandDownNeverPulls
  (14/14); the seven contract suites green (29+17+31+32+5+7+22);
  verify.cmd GREEN. The scoped remainder: the delivery-hold flights'
  PATH verdicts (the orbit vs the 2,000-ft band — the metric-doctrine
  decision), SAD's stick residual (unchanged), and the second-pass
  geometry is OBSOLETE (the clean passes release first pass; the
  10-12k "second-pass" sticks were the balloons).
- **INIT-2f — the PATH arc-geometry classifier: the corner arcs
  leave the band; the oscillators dissolve (2026-10-04).** The
  "route-intercept oscillation family" scoped by INIT-2e dissolved
  under the trace data: the six "oscillators" (INTERCEPT x2,
  OCASTRIKE x3, JSTAR) had NO sustained excursions — their over-band
  samples were CORNER ARCS the fixed 75-s turn window truncated
  (the measured OCASTRIKE arc: the pre-turn slowdown to 262 kts, the
  turn -90 -> -176 deg, xte peaking 13,709 ft = the turn radius at
  corner speed, arc 100-225 s, the capture at the arc's START — the
  75-s window covered barely half and every arc's tail became a
  sub-30-s over-band island riding the p90). The fixed-window A/B
  (75/150/210 s) converted FAILs into SKIPs — route time drains, the
  metric loses its teeth; the until-settle window failed
  geometrically (the arc crosses the old line mid-turn, closing the
  window early). The classifier that works: SETTLE INTERVALS (>= 60 s
  of banded under-band) as the reset signal, and an over-band sample
  is ARC GEOMETRY (reported, not banded) when it lies within 240 s of
  the most recent progression capture with no settle between;
  everything else is an EXCURSION. The departure transient boundary
  moved to the first settle (the documented reported-not-banded
  doctrine, now exact — a 4-min splice transient no longer fragments
  into a fake block). Two block-rule repairs the fixtures forced: the
  merge gap is >60 s exclusive (a gap of exactly one unobserved
  minute is not evidence of settling), and a block extending to the
  window's end is "STILL OFF ITS ROUTE" (an empty post-establish set
  is not "holding 100%"). Measured (qc/coverage_map_init2d
  re-carded): **PATH 29/11/36 -> 36 PASS / 4 FAIL / 36 SKIP**, every
  PASS p90 <= band, every FAIL naming its mechanism — the four
  remaining are the delivery-hold wanderers (SAD x3, BAI/1681: the
  flights holding their TIMING/delivery orbits for beyond-the-run
  appointments, 2.4k-24k ft off the route legs for hours). Selftest
  GREEN (the wanderer fixture now exercises the exact-60-s merge and
  the end-of-window rules); verify.cmd GREEN. The scoped remainder:
  the delivery-hold orbit geometry (the 4 FAILs' owner), the phase
  machine's early Enroute exit, the TOT push-wait model, the
  unit-damage ledger + fixture-generator batch, the hp review.
- **INIT-2e — the PATH metric doctrine: establish-and-hold; the
  designed orbits leave the band (2026-10-04).** The delivery-hold
  PATH verdicts question resolved into a metric-doctrine rewrite
  backed by the time-shape data: every big p90 in the 24-FAIL
  population lived either in the first 10-20 minutes (the spawn/join
  transient — one flight measured 33,998 ft of p90 in its first
  10-min bucket and 1,050 ft for the next two hours) or INSIDE the
  designed hold orbit (the racetrack/TIMING circle lives 3k-30k ft
  off the route legs BY CONTRACT — 22,650 ft of "error" measured
  during a hold the flight entered on schedule). The clause now
  bands ONLY the route time: the Enroute window minus the
  turn-window union minus the station-hold union (interval math —
  the pointwise form let the loop laps' 75-s capture windows chain
  into a blanket that consumed whole windows), with three verdict
  mechanisms: a SHORT-WINDOW SKIP (under 600 s of banded route time
  cannot demonstrate establishment — the measured join transients
  run 8-20 min; the phase machine's early Enroute exit puts the
  station families' whole orbit outside the clause's reach, and the
  station clause owns that contract), an ESTABLISH-AND-HOLD PASS
  (sustained excursions end, the flight holds >= 50% of its route
  time at p90 <= 2,000 ft, the establish time and the hold minutes
  ride the detail), and two honest FAILs (the WHOLE-WINDOW OSCILLATOR
  — sub-30-s crossings never sustain but 10%+ of the samples ride
  15,873 ft: the far-field route-intercept family, a NEW scoped
  finding; and the RE-OSCILLATOR/WANDERER families). The block
  accounting closes coherently: every PASS reads p90 <= band (the
  30-s sustain rule drops the capture-tick spike islands; without it
  a 15,873-ft-p90 flight read "holds 100%"). Measured
  (qc/coverage_map_init2d re-carded in place): **PATH 24 FAIL -> 29
  PASS / 11 FAIL / 36 SKIP** — the 11 are six oscillators (INTERCEPT
  x2, OCASTRIKE x3, JSTAR: the nav route-intercept oscillation, a
  new scoped family distinct from the landing intercept INIT-2
  fixed), two re-oscillators (OTHER: establish then re-cross, post
  p90 19k), and the three delivery-hold wanderers (SAD/1682, BAI/1681
  with 50 sustained excursions). Selftest grows the
  establish-and-hold and wanderer cases (GREEN); verify.cmd GREEN.
  The scoped remainder: the route-intercept oscillation family (the
  nav-side twin of the INIT-2 landing fix), the delivery-hold orbit
  geometry, and the phase machine's early Enroute exit (the station
  families' route life is unjudged territory).
- **INIT-2d — the go-around decay-k retune 0.035 -> 0.050: the
  matrix first-attempt FAILs 25 -> 15 (2026-10-03).** All 25
  first-attempt FAILs were exactly ONE go-around each, spread
  map-wide (AIRLIFT 4, OCASTRIKE 3, ECM 3, ...). The [land-dbg] gate
  autopsy on the AIRLIFT population: the surviving go-arounds were
  the TAIL BAND, not the far-field oscillation the INIT-2 law fixed
  — the lateral converging MONOTONICALLY (775 -> 645 -> 533 -> 436
  -> 350 -> 271 ft against the 250-ft establish gate, heading and
  beam gates green) as the fix's along-track room ran out one 2-s
  sample short; the missed approach re-enters tight and the second
  attempt establishes in 72 s (the measured AIRLIFT trace). k banks
  the tail margin: the AIRLIFT recorded sweep (0.020 / 0.035 /
  0.050) measured 2/4, 3/4, 3/4 landings with 1682's first attempt
  clean only at 0.050; the matrix A/B: **first_attempt 25 -> 15
  FAILs** (ten flights' first attempt now establishes: AIRLIFT,
  AWACS, DEEPSTRIKE, ECM x2, OCASTRIKE, ONCALLCAS x2, STRIKE,
  SWEEP), recovery 66 -> 65 (one AIRLIFT flight lands but is still
  in its rollout at the 120-min horizon), every other clause total
  identical. The landing suite is k-agnostic (32/32 at both
  points); the nine suites green; verify.cmd GREEN. The 15
  remaining first-attempt FAILs are the single-honest-missed-
  approach tail plus the station-hold family's horizon boundary —
  the metric-doctrine tranche's.
- **INIT-2c — the cruise small-error bank drive: the pipper gate
  fires; SAD 0 -> 2, the matrix features 57 -> 63 (2026-10-03).**
  SAD's 563-846-ft stick residual decomposed with the [release]
  probe's new miss vector (the predicted impact and the track vector
  it rode in on): the impact landed +311 along / -290 CROSS of the
  track — the aircraft's track passed ~350 ft left of the aim and
  every bomb inherited it, the 150-ft pipper gate unreachable, the
  holdover landing 425-723 ft wide. The 1-Hz leg trace showed the
  constant +0.8-deg right-of-course with a 2.7-deg intercept
  COMMANDED: the steering's sub-5-deg heading deadband (the landing
  beam's wings-level decoupling — approach_aileron_threshold_rad
  0.087 commands wings-level + zero pedal below it) froze every
  cruise leg's residual cross-track at the roll-out offset; the
  attack leg's intercept law never flew. Fix: AirSteering gains
  small_error_bank_cap_rad (0 = the legacy deadband; the landing,
  combat, and refuel modules own separate instances and keep it) —
  the NAV module sets 0.10 rad (5.7 deg, ~0.4 deg/s at 450 kts): the
  same bank cascade as the intercept branch, capped so the altitude
  coupling stays inside the beam-ride tolerance. Two companions: the
  committed delivery pass also stands the FORMATION rung down
  (delivery_pass_live shared with the ground-avoid stand-down — a
  wingman forming through its own attack run flies the LEAD's track
  into its release), and the measured range model gains its
  calibration ceiling (max_delivery_dz_ft 6,000: the drag makes the
  true range sub-sqrt — ~5% at dz 6,000 but ~25% at dz 10,475 where
  the SAD air-spawn pass released 4,700 ft early; above the band the
  trigger disarms and the FCS descends into it across the re-flown
  pattern). Measured (qc/init2b_geo/sad5, deterministic): the wingman
  stick released at the crossing — first bomb pipper 27 ft, impact
  31 ft, FEATURE KILLED, the leg's xte converged 232 -> 49; the
  high pass descended 12,000 -> 3,994 ft into the band, impacts
  117-494, 1 feature. Matrix (qc/coverage_map_init2c vs init2b2):
  **features 57 -> 63** (STRATBOMB 1 -> 6, STRIKE 2 -> 8, SAD 0 -> 2,
  OCASTRIKE 4 -> 5, BAI 10 -> 11; INTSTRIKE 15 -> 11 and ONCALLCAS
  12 -> 7 gave some back on different target geometry — the
  documented release-point chaos), all other clause totals hold
  (first_attempt 23 -> 25 FAILs: two ONCALLCAS single honest
  go-arounds on the changed approach geometry; PATH one flip each
  way). The nine suites green (29+12+17+31+32+5+7+22+14); verify.cmd
  GREEN. The scoped remainder: the go-around population's decay-k
  sweep, the TOT push-wait model, the unit-damage ledger.
- **INIT-1i follow-up — the measured-range model; the kill threshold
  reached (2026-10-03).** The measured-range model's first scaling
  (the analytic-fall ratio) double-counted the drag and killed nothing
  (every stick short); the corrected pure-ballistic scaling
  (sqrt(dz/ref) x the speed ratio) restored INTSTRIKE 15 / STRIKE 2.
  The accuracy now 316-450 ft on the converging sticks (the analytic's
  448-717 improved) — but the kills still don't register for
  OCASTRIKE/SAD/STRATBOMB: the impacts land 66-450 ft from grid
  features against the ~144-ft lethal envelope with the feature hp
  consuming more than the residual damage delivers. The kill
  threshold/hp review (the warhead vs the feature hp at the 250-ft
  grid spacing) is the feature-layer tranche's own scoped question —
  the release accuracy is no longer the binding constraint. Also
  measured: the bomb sim's bombs do NOT decelerate horizontally (the
  travel 13,768 ft = gs x the fall exactly) — the drag acts on the
  fall only; the bomb_drag_factor_for ratio (dragged/vacuum = 1.63 at
  dz 2,251, clamped to 1.0) documents it.- **INIT-1i follow-up — the feature-kill mechanism NAMED: the holdover
  releases fire 2,500+ ft short (2026-10-03).** The feature-kill gap's
  remaining mechanism measured on the recorded SAD/OCASTRIKE runs: the
  holdover fallback (aim_dist < computed_range - release_holdover_ft)
  fires on the FIRST descent-crossing of the shrinking computed range —
  hundreds of feet-ms before the pipper's own pass can converge. The
  measured SAD stick: released at aim_dist ~13.4k, the bombs landing
  2,532-2,680 ft SHORT of the aim (the walk 2,532 -> 2,680 walking
  away), no feature inside the ~144-ft envelope. The OCASTRIKE sticks:
  the same signature (the 10-12k cluster). The pipper's own pass
  (aim_dist ~ rng, aligned) would land ON the aim — the holdover
  preempts it every approach. The scoped fix: the holdover must wait
  for the pipper's convergence window to pass (the pipper-first
  ordering with the holdover as the receding-pass last resort), not
  just the range crossing. The release-accuracy tranche owns it with
  the probes in place.- **INIT-1k — the go-around characterization: the IAP intercept's
  far-field oscillation (2026-10-03).** The 13 first-attempt FAILs
  instrumented (the [fix] probe in check_fix_reached + the [ptf]/
  [land-dbg] telemetry on the recorded BAI run). The measured
  CS146-4-style arc: the entry fix captures 3,399 ft wide of the
  centerline, the ProceedToFix delivers the aircraft to the fix area
  but 8-9.8k ft off the course laterally, and the IAP course intercept
  (the far-field localizer law) oscillates +/-5-12k ft around the
  course — the establish gate (250 ft lat, the settle/beam/hdg
  companions) unreachable through the oscillation. The one pass that
  crossed clean established, flew the final ON the glideslope (the
  [final] telemetry: err -18..-93, the beam 46-884, cleared 1
  throughout), touched down at along +620 / agl 28 — the landing works
  when the intercept settles. The 13 FAILs are the flights whose
  oscillation never crossed clean within the approach. Owner: the
  localizer-intercept tuning tranche (the STAB-E20 lead scaling at the
  large offsets: the measured cut ~25 deg, the convergence 200-220
  ft/s, the crossing overshoot ~the turn radius — the damping review).
  The [fix] probe stays as the approach-autopsy instrumentation.- **INIT-1j — the receding-pass holdover gate: measured, reverted
  (2026-10-03).** The feature-kill mechanism's first fix attempt: the
  holdover gated on the aim sitting BEHIND the aircraft's track (the
  receding pass — the pipper had its chance, the geometry never
  converged). Measured: INTSTRIKE's pipper kills returned (11
  features) but SAD and OCASTRIKE regressed to armed-no-release
  (exit 4) — their parallel-offset attack passes never put the aim
  behind the track, so the holdover never fired and the flights
  recovered armed. Reverted: the releases matter more than the
  last-resort accuracy (employment 31/0 holds). The feature-layer
  tranche's real lever is the release-solution accuracy (the pipper's
  ~316-450 ft residual vs the 144-ft lethal envelope) and the
  feature-hp/kill-threshold review — not the holdover's firing
  order.- **MC-4 — the PATH turn-window semantics landed; PATH 43 -> 23
  (2026-10-03).** The INIT-1f measurement (the turn excursions at their
  geometric floor) drove the metric semantics to match its own
  documented doctrine: PATH = STEADY-STATE leg tracking. eval_path now
  scopes the turn windows (the samples within 75 s of a waypoint
  capture, plus the departure's own first 90 s) as reported-not-banded
  geometry — the same treatment the departure transient already had —
  and runs the steady-state p90 on the rest. The turn-arc peaks ride
  the detail as diagnostics. The 75-s bound is measured (the corner
  turns: ~60 s of arc at the corner speed + the roll). The remaining 23
  FAILs are the post-turn convergence tails (the failing samples sit
  75-150 s post-capture, xte 4-12k median, converging at the legal
  far-field rate) — the route-holding refinement's honest remainder.
  The tangent-arc flyout (commanding the outgoing course as a heading
  after the capture — the geometric tangent-arc correction) was built
  and measured a wash-to-worse on the fail counts (the arc's rollout
  offset feeds the same convergence tails) and was reverted with the
  evidence. Suites green; verify.cmd GREEN; the selftest GREEN with the
  turn-window semantics.- **MC-4 — the ProceedToFix "stall" re-attributed; horizon margin
  +20 — LANDED (2026-10-02).** The horizons tranche's dominant finding
  (AIRLIFT flights spending 18 minutes in ProceedToFix without
  sequencing) was instrumented (a [fix] probe in check_fix_reached
  printing every capture-arm's inputs) and RE-ATTRIBUTED: it is not a
  capture-gate defect. The AIRLIFT station orbits sit near the mission
  objective — 400,000-500,000 ft from the recovery field — and the
  existing ProceedToFix law (the wrong-side pursuit of the fix, the
  past-fix sequencing arm, the course intercept) flies that pursuit
  transit at hdgrw ~130 deg before joining the course. At a 150-minute
  horizon **all four AIRLIFT flights recover** (a textbook final: on
  slope, -1,090 fpm, threshold crossing) — the 90-minute horizon was
  cutting a pursuit transit, not a deadlock. The standard FAR/AIM
  holding entries (direct/parallel/teardrop) were considered and
  deferred: the existing two-shape law is a crude direct-entry that
  CONVERGES given horizon; a teardrop would shorten the wrong-side
  chase (hdgrw pinned ~130 deg for 15+ minutes) but is a refinement,
  not correctness — revisit only if a deadlock survives the horizon
  fix. Landed: the horizon formula gains the approach-and-recovery
  margin (max_time + 20, clamped [30, 120]; the cap 90 -> 120).
  Measured (`qc/coverage_map_init1c`): recovery 56 -> 65 PASS, station
  11 PASS / 0 FAIL (fully green), gate fails still zero, wall 605 s.
  Remaining recovery FAILs (11) are the world-geography tail: stations
  whose pursuit still outlives 120 minutes, and OTHER's floor-30
  flights. The [fix] probe stays (the approach-autopsy
  instrumentation).
- **MC-4 — the per-profile horizons — LANDED (2026-10-02).** The
  recovery clause was measuring the horizon, not the behavior: 38 of 76
  carded flights "failed to recover" only because the flat 30-minute
  matrix horizon cut them mid-flight — and a 45-minute station contract
  could never complete inside it. `qc_missions.py` now derives each
  run's horizon from the mission's own MissionProfiles.json `max_time`
  (Falcon's planned sortie window), clamped to [--minutes, --max-horizon]
  (30/90; `--flat-minutes` restores the flat behavior), records the
  horizon per row, and re-scopes the 0-airborne verdict to the C++ exit-3
  gate's own two conditions (0 airborne AND none recovered — a horizon
  past the sorties' length lands every flight Complete on the deck, and
  that is the system working). Same 22 types
  (`qc/coverage_map_horizons`): **recovery FAILs 38 → 8**, duration
  SKIPs 14 → 0 (completed sorties now judged; all under the 120-min
  band). The longer windows made previously-hidden findings VISIBLE —
  the map's job:
  - **6 recovery FAILs are the new dominant finding: the approach
    chain stalls in ProceedToFix.** The AIRLIFT flight flies a clean
    88-minute arc (transit, the full 45-min station, egress — PATH p90
    354 ft) then spends its last 18 minutes chasing the approach entry
    fix without capturing it (arriving from the orbit-exit geometry).
    Owner: the approach/landing tranche.
  - 4 newly-visible first_attempt FAILs: one go-around each (ECM ×2,
    JSTAR, SWEEP) before a recovery — the horizon used to cut the
    approach before the attempt counted.
  - 2 newly-visible PATH FAILs: STRIKE CS002-4 and TANK CS145-3 fly
    long enough now that a late-route excursion lands in the final
    quarter (their 30-minute windows ended clean).
  Unchanged: the 9 strike-family exit-4 gates (the fixture target-VU
  gap — still the owner of the employment story), station SKIP/FAIL
  counts (the never-held population is the splice's anchor skip, not a
  horizon question).

- **INIT-1 — the employment chain is LIVE (2026-10-02): 8 of 9
  strike-family types deliver; exit-4 gates 9 → 1.** The arming
  tranche's finding of record ("the generated save's delivery flights
  carry NO target VU — a fixture data gap") was WRONG, and the [plan]
  probe (build_mission_plan_from_flight, F4_LAND_DEBUG) disproved it:
  the saved flights carry valid mission_targets, their STRIKE/17
  waypoints sit on the routes with matching target_nums, the loader's
  second pass resolves them, and the built plans carry the delivery
  point with its target. The gates had THREE real mechanisms:
  1. **The takeoff-handoff splice projected into the post-strike join
     stack.** The strike routes carry the rendezvous (the refuel leg)
     BEHIND the delivery point — strike, then rally — and the bridge's
     join stack sits on that rendezvous a few miles from the home
     field, so the global-nearest-leg scan landed the cursor inside the
     stack (measured resumes 11/19, 8/21, 10/22), past the STRIKE
     waypoint: the flight walked its waiting circle and went home
     without employing. Fix: a ground launch always begins its route at
     the beginning — the handoff resumes at route[1] (the projection's
     own CAMP-GATE-ROLL reduction); the projection stays the NAV-D1
     air-spawn site's tool, where an airborne materialization genuinely
     is mid-route.
  2. **WP_SAD (16) was missing from the engine's delivery-action set**
     — the wire's shoot-the-target point the BAI/STRATBOMB/SAD
     planners write, and route_builder.hpp's own delivery vocabulary
     has always included it. The flights flew over their delivery
     points without ever arming. Added to is_ag_delivery_action.
  3. **Unit-targeted delivery points (BAI) had no resolvable target**
     — the wire's target_num is a battalion VU that never spawns as a
     sim entity. The delivery waypoint still has a POSITION (the
     planner wrote it on the target box): the plan build now falls
     back to the nearest objective entity (within 15 nm).
  Measured (the 90-min horizon matrix, `qc/coverage_map_init1_final`):
  employment 0 verdicts → 22 PASS / 6 FAIL / 3 SKIP, with 8 types
  passing (INTSTRIKE/STRIKE/STSTRIKE/ONCALLCAS 16 releases each, BAI
  11, DEEPSTRIKE/SAD/STRATBOMB 4); released flights' bombs are now
  filtered from the cards (the recorder's own missile-track stamp) and
  the verifier's combat-event kind names fixed to the recorder's
  serialization (bomb_released/entity_killed — CamelCase never matched
  anything). Also newly honest: PATH FAILs rose to 46 — the strike
  flights now fly their FULL 90-minute arcs (delivery run, post-strike
  rally, the station orbit, egress), and the windows' final quarters
  land on those excursions — the route-holding lens applied to delivery
  geometry.

- **INIT-1b — OCASTRIKE solved; ZERO matrix gate fails (2026-10-02).**
  The [strike] probe (brain_component's strike block, F4_LAND_DEBUG)
  caught the last gate's mechanism: the flight sat at its SEAD waypoint
  with the module armed and the aim VALID — 227,000 ft away and
  receding. The planner's delivery marker is a steer point, and its
  target (a battalion) was a whole province from it: the SEAD marker at
  grid (428,496), the target at (398,236) — 266,000 ft. The nav flies
  to the marker (EMPL-1a's attack run and the capture are keyed on it)
  while the release gate is keyed on the target — the flight flew its
  point at 4,011 ft and went home with the stick unfallen, the target
  never approached. Fix: a delivery waypoint whose resolved target sits
  more than 30,000 ft away CO-LOCATES with it at plan build (the
  delivery altitude floor still applies). Measured: OCASTRIKE exit 0,
  16 releases / 16 impacts; **the full 22-type matrix runs with ZERO
  gate fails for the first time**, employment 26 PASS / 2 FAIL. The
  card residuals (PATH 45 on the long arcs, recovery 20, first_attempt
  9 go-arounds) are the delivery-geometry route-holding work and the
  approach chain — behavioral, no longer gate-level.
- **MC-4 — tasked mode + the matrix wiring (the remainder)**: the
  tasked runs (ATM packages, the session path) as card sources; the
  verify smoke rotation.- **MC-5 — the coverage map + the viewer Mission Lab.** The truth
  table over all 41 types; the interactive door.
 Flight
  identity on the trace snapshots (mission name, flight VU, TOT,
  target objective — threaded through MissionPlan from the campaign
  bridge); the mission EVENT stream (waypoint captures with action +
  cross-track, station enter/exit, brain phase changes) in the
  recording beside combat_events. Pure instrumentation, no behavior
  change; the events are detected in the Simulation layer (the join
  point that already reads the brain each tick), keeping f4-ai
  engine-agnostic. TOT attribution falls out: the delivery waypoint's
  capture event vs the plan's tot_s.
- **MC-2 — the verifier + report card + the injected generator**
  (`--single-mission` / `--mission-spec`). The clause evaluator, the
  two report formats, card-driven replay bookmarks.
- **MC-3 — the core archetype contracts**: BARCAP (station +
  engagement + recovery), STRIKE (path + TOT + employment +
  recovery), ESCORT (proximity), TANKER (AAR interaction), and the
  first new BEHAVIOR tranche the matrix schedules (the honest
  coverage map names it — likely AIRLIFT, the logistics family whose
  ground-side consumer, DOM-2 supply, already exists; there is no
  paradrop anywhere in the stock wire we converted).
- **MC-4 — tasked mode + the matrix wiring**: qc_missions.py drives
  the verifier; verify.cmd gains the smoke rotation + the opt-in
  matrix stage.
- **MC-5 — the coverage map + the viewer Mission Lab.** The truth
  table over all 41 types; the interactive door.

## 9. What does NOT change

- The C++ exit gates, the scenario template library, the
  stock-landing/war harnesses, the known-reds discipline.
- The brain/flight-control behavior — MC-1 is read-only joins; the
  first behavior change (if any) lands with MC-3's new archetypes,
  under its own gates.
- Determinism: joins read sim state in walk order; recorded events
  inherit the run's seed.

## 10. Known notes

- AIRLIFT (wire byte 33) is vocabulary-only today — no cargo/airdrop
  behavior exists anywhere in the converted stock data (the paradrop
  memory is Tactical Engagement/mod territory, not the campaign AI).
  Its contract starts red by design; that IS the coverage map working.
- The TOT-pacing design pass (CAMPAIGN_LOOP_PLAN §7) consumes MC-1's
  per-flight attribution — the two tranches are sequenced MC-1 →
  MC-2 → (TOT pass || MC-3).
