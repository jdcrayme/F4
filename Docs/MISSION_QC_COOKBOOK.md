# Mission QC Cookbook — how to QC a mission type

Status: **as-built workflow** (the tooling it names is landed; §7's
showcase scenarios are the one future tranche). Companion script:
`scripts/qc_missions.py`. The artifact vocabulary here is
`campaign_qc`'s (`f4-simulation/tools/campaign_qc.cpp` — read its
header comment for the full flag + exit-code contract).

## 1. The question the viewer can't answer

Playing a campaign back in the world viewer shows you *everything* —
449 flights, 8 teams, links, packages, a moving map — which is exactly
why it shows you *nothing*. "A lot is happening" is not a verdict, and
a human staring at the map cannot tell a broken strike pipeline from a
busy one. The viewer is the last step of QC, not the QC itself.

The QC actually lives in three layers, and the codebase already ships
all three:

| Layer | Where | What it answers |
|---|---|---|
| **Gates** | `campaign_qc` exit codes, headless | "did the chain break?" — loud, binary, CI-able |
| **Ledgers** | `campaign_qc_summary.json`, `campaign_result.json` | "what happened, as numbers?" — per-chain counters |
| **Eyes** | world viewer: ATO/Tasking window, replay mode | "what did it look like?" — narrowing, guided by the first two |

The discipline: **run the headless verdict first, then open the viewer
to confirm what the verdict pointed at.** Never the other way around.

## 2. The pipeline in one block

```bash
# 1. save → world JSON (keep subfiles if you want the .cam round-trip)
build/f4-world-convert/cam2json TestCamp.cam testcamp.world.json \
    --preserve-subfiles

# 2. one mission type, headless, with a recording
build/f4-simulation/campaign_qc testcamp.world.json \
    --mission AMIS_BARCAP2 --minutes 30 --max-flights 4 \
    --class-table Data/Classes/falcon4.ct.json \
    --config build/generated_fixtures/f16.json \
    --profiles build/generated_campaign/MissionProfiles.json \
    --out-dir qc/barcap2

# 3. the viewer (confirm what layer 1–2 told you)
build/f4-world-viewer/f4-world-viewer testcamp.world.json
#   ATO/Tasking window → mission filter AMIS_BARCAP2 → click a row
#   (camera pans; inspector shows the plan)  ·  replay mode loads
#   qc/barcap2/trace.json (scrubber, per-aircraft trail, ai_state)
```

Artifacts land in `--out-dir`: `campaign_qc_summary.json` (the
ledgers), `campaign_result.json` (the C1 write-back ledger),
`campaign_qc_scenario.json` (what the sim actually got), and
`trace.json` (the FlightRecorder — the viewer replay's format; only
written without `--no-record`).

## 3. QC a mission type, step by step

**3.1 Pick the byte.** The C++ `--mission` filter is BYTE-EXACT — there
is no "CAP family" flag. The behavioral families live in
`f4-campaign/include/f4/campaign/mission_type.hpp` (41 wire names →
11 categories). Two traps: the stock war's CAP is `AMIS_BARCAP2`
(byte 2) — filtering `AMIS_BARCAP` matches zero flights and exits 2;
and the tanker *mission* the stock war actually files is `AMIS_TANK`
(byte 39, 78 flights), not `AMIS_TANKER` (27, absent). Read the save's
own histogram first — it is the first block of every summary
(`world.missions_by_type`) and what `scripts/qc_missions.py` uses to
expand categories to the bytes that exist.

**3.2 Run the filtered headless run** (block above). Keep the cap
small (`--max-flights 4`): the QC question is per-type behavior, not
throughput, and the wall-clock you pay is mostly world setup
(~1,700 units decode + populate) plus ticks.

**3.3 Read the verdict.** Exit 0 still deserves a look at the
summary's per-category numbers (§4); any nonzero exit is a gate from
the C++ header — the B.3 path's are:

| exit | gate | what it means |
|---|---|---|
| 2 | filter matched nothing | the type isn't in this world (or you typo'd the name) |
| 3 | ground ops stalled | aircraft spawned, none airborne at the horizon |
| 4 | A-G employment broke | strike flights armed, zero bombs released |
| 5 | silent result loss | combat happened, the ledger recorded nothing |
| 6–8 | ladder gates (C2/C3/C4) | generation drew nothing / no routes / no packages |

The war path (`--war`) adds 9–12 (determinism, ledger drift, entity
leak, stall), the ground arms 13–14, the accel certificate 15–16.

**3.4 Autopsy a failure with the trace.** The gate tells you *which*
chain broke; the trace tells you *where*. It is per-sample,
per-aircraft, and the two fields that close most cases are `ai_state`
(the state machine's actual path) and `target_description` /
`target_position` (whether the AI was ever *given* anything to do).
Group by `entity_id` — `callsign` is currently empty in snapshots.
§5 is a complete worked example.

**3.5 Only now open the viewer — as the guided tour.** Load the same
world JSON, filter the ATO/Tasking window to the one mission type, and
the canvas follows the filter (mission-target links, dimming, the
bullseye). Click an ATO row: the camera pans to that flight and the
inspector shows its plan. Load the run's `trace.json` in replay mode to
scrub the actual flight with cross-track coloring. You are no longer
looking for "what happened" — you are confirming the specific behavior
the ledgers named. The ground arms of §6 get the same tour now: the
viewer draws the FLOT, per-objective supply state and capture markers,
streams the ground event feed, and the inspector carries a ground pane —
so a `--ground-war` run's ledgers can be confirmed visually, not only
numerically.

**3.6 Sweep the whole ATO with the matrix runner.**
`scripts/qc_missions.py` loops the per-byte runs, applies the
per-category expectations of §4 on top of the C++ gates, and writes
`qc_matrix.json` / `qc_matrix.md`:

```bash
# one representative per category present in the save (9 runs here)
python3 scripts/qc_missions.py testcamp.world.json --jobs 2

# a full family, with traces for the viewer
python3 scripts/qc_missions.py testcamp.world.json \
    --missions Strike,CAS --record

# the synthetic tasking ladder (the generation side, §6)
python3 scripts/qc_missions.py testcamp.world.json \
    --tasking 35 --tasking-cycle 300
```

## 4. What "good" looks like, per category

The C++ gates are generic on purpose; the per-type interpretation is
this table (and what the matrix runner encodes). Horizon defaults
assume `--minutes 30`; the 15-minute preset of the B.3 tranche was
tuned for short-hop slices, and strike TOTs routinely exceed it.

| category | must be true | notes |
|---|---|---|
| CAP (1–6, 36) | spawned ≥ 1 · routes == spawned · airborne at end | BARCAP orbits a station; GoAround/bounce excursions show in `ai_state` |
| Sweep (7), Intercept (8–9) | same as CAP | interecept verdicts need a threat in bubble range — see the WVR/BVR harnesses for the combat slice |
| Escort (10–11) | same as CAP | station keeping is relative to the principal; package links in the viewer |
| Strike (12–16, 24), SEAD (17) | same + `armed > 0 → released > 0` | the employment chain: arming → target → release. `armed == 0` is a loadout/weapon-table concern, note it, don't gate it |
| CAS (18–21, 23, 31) | same as Strike | honest horizon is 1 h+ (TOT windows); `--unit-strike`/exit 14 is the battalion-booking gate |
| Recon (22, 29, 30) | same as CAP | BDA/RECON over target; no ordnance expected |
| Support (25–28, 32–35, 39–40) | same as CAP | TANK/AWACS racetrack; AIRLIFT shuttles between bases |

Two horizon cautions the table can't encode: flights honor their saved
TOT (a 40-minute run can be entirely pre-push taxi + wait, which is
*correct*), and strike release needs the route's release point reached
(median 34 NM out in TestCamp) — a strike FAIL at 15 minutes is a
horizon suspect, at 40+ minutes it is a chain suspect. Distinguish
before filing.

## 5. Worked example: the strike employment gap

The matrix runner's first sweep over TestCamp caught a live one, and
the catch is the workflow's proof:

1. **Gate.** `--mission AMIS_INTSTRIKE --minutes 15 --max-flights 4`
   → exit 4: "4 strike flights armed with ordnance, 0 bombs
   released." Raising the horizon to 40 minutes changed nothing (and
   one flight had already recovered) — not a TOT/horizon artifact.
2. **Ledger.** Summary: `armed=4, released=0, impacts=0`; routes
   attached 4/4; airborne at end 4/4 then 3/4. Nothing else anomalous —
   the tasking and route chains were green.
3. **Trace autopsy.** Both flights: `Taxi → PrepToTakeRunway →
   Takeoff → FlyOut → ToWaypoint (NavigationMode)` for 10–13 minutes,
   then `PullingUp (GroundAvoid) → ProceedToFix (LandingMode) →
   GoAround/OnFinal → Rollout → Parked`. **`target_description` is
   empty for every sample of every flight.** The AI was never given a
   target; it flew the route, then went home.
4. **Control.** Same run with the synthetic tasking ladder on
   (`--tasking 35 --tasking-cycle 300`): ladder green (7 cycles, 644
   intents, 222 routes, 38 reinforced), and the synthetic strike
   flights fail *identically* — armed, no target, no release.
5. **Sweep.** The full nine-representative matrix over TestCamp
   (15-minute horizon, 3 flights/type): every non-ordnance category
   green (BARCAP2, SWEEP, INTERCEPT, ESCORT, BDA, TANK, OTHER —
   spawned 3/3, routes 3/3, airborne 3/3), and BOTH A-G categories
   red the same way (INTSTRIKE and ONCALLCAS: armed 3, released 0,
   exit 4). The gap is the air-to-ground employment family, not one
   mission byte.
6. **Verdict.** Campaign tasking (saved *and* ladder-generated) drops
   the strike target on the campaign→sim boundary: the spawner attaches
   routes and arms the loadout (weapon table), but nothing converts the
   tasking's `mission_target` into the sim AI's ground-attack steering.
   The A-G chain itself is proven by `ground_strike_qc` (targets
   injected directly) — the gap is the target *propagation* leg. The
   save even carries the data (`mission_target: 4113` on the flight
   units); the spawn path just never hands it over.

That last sentence is what a QC verdict should look like: chain named,
leg isolated, evidence named, fix direction stated — none of it
visible on the viewer's map.

## 6. Supply and ground orders (the same loop, war-shaped)

The tasking ladder and the ground arms are the "show it working" mode
for the war's return leg — same discipline, longer horizons:

```bash
# the ladder: generation → routes → spawns → reinforcement, as numbers
campaign_qc testcamp.world.json --tasking 120 --tasking-cycle 1800 \
    --minutes 30 --max-flights 24
#   summary "tasking" block: cycles_fired, intents, drawn_aircraft,
#   routes_built/failed, synthetic_spawned, reinforce fires/deliveries,
#   per-team pool trajectory (the one-pool ledger)
#   gates 6/7/8: drew nothing / no routes / no packages

# the ground war + its supply chain (G1 + CAMP-DOM-2), exit 13 = "moved nothing"
campaign_qc testcamp.world.json --war 6 --ground-war \
    --objective-supply --repair-period 3600 --replacement-stock

# CAS booking real battalions (G2), exit 14 = "air produced nothing"
campaign_qc testcamp.world.json --war 6 --ground-war --unit-strike
```

Ladder trap the hard way: `--tasking 2` does nothing — the default
cycle is 1,800 s, so a 2-minute window fires zero cycles. The window
must exceed the cycle (and a `--tasking-cycle 300` window of 35 min
fires 7). The summary's `tasking.cycles_fired` is the first number to
check on any ladder run.

## 7. Curated per-type showcase scenarios (the next tranche)

The stock save covers only the mission types its war happened to file
(21 of 40 bytes in TestCamp — no TARCAP, no SEADSTRIKE, no ASHIP, no
SAR). The per-type idea from the plan that predates this cookbook is
still the right end state: **a tiny, deterministic showcase world per
mission type**, checked in as fixtures, where the expectation table of
§4 is known by construction:

* one squadron with the matching `specialty` (ARO byte), one airbase,
  one target objective at a fixed distance, one tasked flight of the
  type under test — a stage, not a war;
* the matrix runner points at a showcase world and the row's verdict
  is the mission's unit test at campaign scale;
* two build paths, in order of increasing fidelity:
  1. **Surgical edit of a real save** (available today):
     `cam2json --preserve-subfiles` → flip the flight's `mission` /
     `mission_target` / owner in the JSON → `json2cam --reencode-all
     --baseline` → verify with `cam2json` byte-identity on the
     untouched records (the SAVE_WRITE_PLAN round-trip contract);
  2. **A generator** (`campaign init` grows a `--showcase <type>`
     mode): synthesizes the world from the class table + profiles —
     the campinit CLI is the natural home.

Until the tranche lands, `--missions <family>` over the stock save
plus the §4 table is the honest substitute, and the absent-byte list
in each summary's `missions_by_type` tells you exactly what the stock
war can't exercise.

## 8. Known gaps (deliberate, documented)

* The C++ `--mission` filter is byte-exact and single — no family
  flag. The matrix runner loops bytes in Python rather than growing
  the C++ surface; revisit if a `--category` filter ever earns its
  place in `FlightSpawnFilter`.
* Strike employment is broken as described in §5 — currently THE
  highest-value catch on the board.
* Trace snapshots carry empty `callsign` (group by `entity_id`).
* The tasking ladder cannot be *asked* for a mission type — generation
  is strategic (profiles × situation), the filter only gates spawns.
  Showcase scenarios (§7) are the deterministic answer.
* `--war` gates (9–12) are whole-war verdicts, not per-type; a
  per-type war row would need the war harness to take the same
  `FlightSpawnFilter` the B.3 path does.
