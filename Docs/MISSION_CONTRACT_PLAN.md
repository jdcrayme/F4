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
  (exit 4 — the saved-flight arming gap, the EMPL finding); ESCORT and
  OTHER book 0 airborne (the plain-path ground-ops crawl); the CAP/
  support/logistics families fly and recover. TANKER: no flights in
  this world (the known TestCamp gap).
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
