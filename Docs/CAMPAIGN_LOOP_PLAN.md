# Campaign Loop — Closing the War (Phase C)

> **Status**: Active plan. **C1 (the result ledger + sink + write-back),
> C2 (one pool — draws, netting, reinforcement), C3 (threat map +
> A* + route builder, generation-to-spawn), V-CAMP (the live
> campaign session — the viewer runs the war), C4 (the ATM
> pipeline — 7-phase tasking, FindBestAir, escort pairing, TOT
> slots, mission recovery), C5 (the 24-hour war — the
> long-horizon acceptance harness), C6 (arming the campaign
> flights — A/A goes live), G1 (the ground war — battalion
> maneuver, the front line, attrition, capture), and G2 (the
> interdiction link — CAS against real battalions, the bombs
> booking, the engine pulling) are LANDED** — the campaign loop is
> CLOSED end to end, both sides of it AND the diagonal between
> them. The rest of this document is the roadmap that got here
> (the known-gaps §7 is the forward queue).
> **Prerequisite**: B.3 landed (campaign→sim loop: intents → spawner →
> aircraft fly saved routes; `campaign_qc` gates the loop end to end).
> **Companion**: [Next Phase Plan](NEXT_PHASE_PLAN.md) (§B — the campaign
> slice), [Combat Chain Plan](COMBAT_CHAIN_PLAN.md) (the fight itself).

---

## 1. The problem: a one-way war

B.3 closed the campaign→sim direction: the save's tasking becomes
aircraft that taxi, depart, and fly their routes; `campaign_qc` proves
it with gates (exit 3 "nothing flew", exit 4 "armed but never employed").
The A-G slice even reads objective damage back for the summary — but as
a **report**, not as **state**.

The return leg did not exist. Nothing a run does — kills, bomb damage,
attrition — ever touched campaign state: the team aircraft pools the
tasking gates read, the squadron kill/loss counters the debrief reads,
the objective fstatus bitmaps the save format carries. Kill a squadron's
last aircraft and the next tasking cycle happily tasks it again. Drop
bombs on an airbase and the save never learns. That is a mission replay
engine, not a campaign: Falcon 4's core game loop is
**simulate → attrite → retask**, and the middle word was missing.

The root cause was structural: the sim entities and the campaign units
live in two worlds bridged at spawn, and the bridge was one-way. A kill
landed on EntityId 47 of the sim world, and nothing could say which
squadron just lost an aircraft — so no write-back could even be
expressed. (In FreeFalcon this problem does not exist: a sim entity IS
a campaign entity — `SimVehicleClass` derives from `FalconEntity`,
which carries its VU_ID and campaign object pointer. The engine-agnostic
split severs that identity deliberately; C1 restores it as data.)

## 2. C1 — the result ledger (LANDED)

Four pieces, in the layer each belongs to:

| Piece | Library | What it is |
|-------|---------|------------|
| `CampaignResultLedger` | f4-campaign | The write model: snapshot (team pools, squadron history) + typed apply methods + `to_json()` (byte-stable) + the C2 hook. NEVER sees EntityWorld — campaign identity in, numbers out. |
| `CampaignOriginComponent` | f4-simulation | The restored sim→campaign link, as data: flight/squadron/home-airbase VUs + team slot, stamped once at spawn by `spawn_aircraft_for_flight` (the shared core of every campaign spawn path). Scenario-list aircraft carry none — the compatibility contract. |
| `CampaignResultSink` | f4-simulation | Entity→identity resolution: subscribes `EntityKilledMessage`/`BombImpactMessage` on the sim bus, classifies (air loss / ag credit / unclassified), syncs objective damage final states, feeds the ledger. |
| `apply_to(WorldState&)` | f4-campaign (opt-in header) | The write-back: pools, squadron counters, objective fstatus into the typed WorldState — in-process, so decode → run → fight → apply → reload → the damage and the pools are there. Unmatched VUs are loud. |

Semantics (FreeFalcon-correspondence, all test-pinned):

- **Air kill**: victim team pool −1 (floor 0), victim squadron
  `total_losses` +1 (uchar-saturating, the wire's own limit), killer
  squadron `aa_kills` +1 (int16-saturating) when the killer resolves to
  a campaign aircraft; otherwise UNATTRIBUTED — loss booked, no credit.
- **AG kill** (origin-less victim, origin-ful shooter): `ag_kills`
  credit only; the victim's own ground ledger is the ground-war
  tranche's.
- **Objective damage**: FINAL-STATE SYNC — f4-weapons owns the live
  per-feature ledger (`FeatureSetComponent.feature_hp` +
  `DamageBitmapComponent.fstatus`); the sink snapshots each objective's
  damage state at construction (so a mid-campaign save's prior damage is
  initial, not this run's) and hands changed objectives' final states
  to the ledger. Last write wins; the entity is authoritative.
- **Tasking reacts** (`Campaign::set_result_ledger`): squadron
  availability minus THIS-RUN losses, floored at zero. A fresh ledger
  changes nothing (golden-identity pinned); 22 of 24 losses shrink
  packages; 24 of 24 stop generation.

`campaign_qc` runs it all and gates it: the summary gains a `results`
block, `campaign_result.json` is the durable artifact (strictly valid
JSON, byte-stable, no floats), and **exit 5** fires when combat outcomes
occurred but the ledger stayed empty — the "outcomes didn't write back"
failure class this tranche exists to kill.

Verified on real data (TestCamp, v71 decode): a 20-minute INTSTRIKE run
— 4 bombs released/impacted, 1 objective damaged (5/64 features, 7.8%),
fstatus bitmap written back into the WorldState, all gates green.

## 3. C2 — ONE POOL: tasking, combat, resupply deplete the same ledger (LANDED)

C1 left two accounting worlds: the Campaign tracked mission draws on its
OWN per-squadron counters (never visible to the write-back or the
artifacts), while the ledger tracked combat losses. A drawn-then-killed
aircraft debited both. C2 merges them — **the ledger IS the tasking
pool**:

- **Mission draws debit the ledger** (`apply_mission_draw`): while a
  ledger is attached, the tasking cycle's availability gate reads
  `squadron_tasking_available()` — snapshot − draws − non-drawn losses
  + reinforcements — and every generated mission books its draw where
  combat losses and resupply already live. The Campaign's own counters
  are untouched in this mode (the no-ledger path keeps them — B.3's
  behavior, byte-identical goldens; a fresh ledger reports exactly the
  same numbers, so the two modes agree until events land).
- **Draw/loss NETTING**: a drawn aircraft's death consumes its draw —
  the pool debits ONCE (the draw already removed it), while the
  existence counters (team pool, total_losses) count every death. A
  non-drawn death (parked, scenario) debits the pool directly — the
  C1 behavior when no draws exist, byte-for-byte.
- **Existence vs tasking views**: drawn aircraft still exist —
  `team_aircraft_remaining` moves only for deaths and reinforcements;
  `aircraft_tasking` subtracts outstanding draws. The write-back
  carries the existence view (tasking ≠ attrition); the QC artifact
  carries both.
- **The reinforcement cadence** (`apply_reinforcements`): anchored on
  the .cmp header's `last_reinforcement` (absolute campaign time,
  bridged through `ICampaignSource::current_time`), firing
  FreeFalcon's own gate — `now > last + period`. A stale anchor
  (TestCamp: 0 against a 38.5M-second epoch) fires exactly ONCE
  (the anchor jumps to now), not the years it is behind. Each fire
  refills every squadron's deficit toward its run-start snapshot,
  consuming the wire's own per-squadron `reinforcement` budget
  (TestCamp: 26 squadrons carry 24..168); team existence pools gain
  the deliveries capped at their initial value. The PERIOD is a
  config tunable (the reference's rate is a runtime difficulty
  setting, not wire data) — default DISABLED so a fresh ledger changes
  nothing until the host arms it (the C1 golden identity, preserved).
- **The roster decode fix**: the wire's u32 squadron roster is the
  same 2-bit-per-group packing flights and battalions use
  (0x5555aaaa = 24 ships). The pre-C2 Campaign read the RAW u32 —
  1.4 billion available aircraft on any real v71 save. One shared
  force snapshot (src/squadron_snapshot.hpp) now feeds BOTH the
  Campaign and the ledger — the numbers agree by construction, not
  by convention.

`campaign_qc --tasking <minutes>` runs the whole thing as one
multi-cycle loop on a real save: the synthetic ladder draws (ledger
attached), the saved-flight sim fight attrites, the cadence refills,
and ONE ledger carries all three — plus exit 6 (the tasking-broke
gate: the ladder drew nothing despite belligerent aircraft) and a
`tasking` summary block (per-team pool trajectory).

Verified on real data (TestCamp): a 4-hour `--tasking` run + the
20-minute INTSTRIKE sim — 8 cycles, 438 intents, 957 aircraft drawn,
1 reinforcement fire delivering 232 aircraft to 22 squadrons, 88
bombs / 20 damaged objectives / fstatus written back — all in one
ledger, all gates green.

## 4. C3 — threat map + pathfinder + route builder (LANDED)

The forward leg grew its brain: generated missions now fly THEIR OWN
routes. B.3 flights flew SAVED routes (the save's WaypointStruct list,
built by FreeFalcon's own ATM run); C2's synthetic ladder drew
aircraft for intents that published to nobody. C3 closes
generation→route→spawn: every generated strike-family mission carries
a real enemy objective and a threat-aware route airbase → target →
airbase, and the spawner materializes it the moment it publishes.

Four pieces, in the layer each belongs to:

| Piece | Library | What it is |
|-------|---------|------------|
| `ThreatMap` | f4-campaign | ScoreThreatFast's data half: per-cell ownership (nearest objective, 10-then-80 radii) + 2-bit low/high AD density rings (MAP_RATIO 6), one byte per cell, BOTH belligerents packed per the viewer bit-half rule. Pure function of the sources — same inputs, same cells. |
| `AirPathFinder` | f4-campaign | asearch.cpp's A*: 2000-node pool, 8 directions at QuickSearch 12, threat sampled at 5 points (max), cost = base×step + threat/2, heuristic ×4, snap-to-target, 96-step cap, best-effort partial paths on budget exhaustion. Every constant is the reference's. |
| `RouteBuilder` | f4-campaign | campwp/mission.cpp's BuildPathToTarget: CheckSafePath on the direct leg (TT_MAX over MAP_RATIO samples) → FindSafePath corners when over threshold → IP at BreakpointDist → target WP → 5-candidate turn point → egress in reverse → EliminateExcessWaypoints (two-pass, WP_NOTHING fillers only). |
| `IUnitCoreSource::unit_weapon_range/hit_chance` + `world_json` emission | f4-world / f4-world-convert | The UCD threat-model arrays (indexed by MoveType: Range[LowAir]/Range[Air] are the SAM rings, HitChance gates them) — the wire face the map paints from. cam2json emits them per unit when the class table resolves DTYPE_UNIT into the theater DB. |

**The stance vocabulary correction (the tranche's biggest fix).** The
pre-C3 code read stance entries as a sign convention ("< 0 = at war").
The reference's own vocabulary is an ENUM — cmpglobl.h `RelType`:
NoRelations 0, Allied 1, Friendly 2, Neutral 3, Hostile 4, War 5 —
and team.cpp indexes `RoEData[roe][stance]` with it directly. Real
saves carry garbage toward unused slots (TestCamp: every team's stance
toward the empty Gorn slot is -5141), so the sign test read WAR
against a phantom side: the U.S. became a belligerent in a war it is
Neutral to, while the actual war (ROK↔DPRK, mutual 5) starved target
selection of every enemy objective — `select_target_` returned 0, no
route ever built, the QC's exit-7 gate fired. Fix: `f4/world`
`Relation` + `relation_from_wire` (out-of-range → NoRelations — the
reference's RoE table gives a 0 column the same answers), and every
consumer now tests `== War`: `belligerent_teams`,
`select_target_`, `ThreatMap::war_`, the bridge's side_color. The
kunsan fixture and the five stance-pinning tests moved to 5 (same
belligerents, byte-identical goldens).

**The role gate.** FreeFalcon's squadron selection SCORES role match
and aircraft capability (a counter-air F-16 wing is taskable for
strike — FindBestAir, mission.cpp); our ladder hard-gated on the
squadron's ARO specialty. Corrected belligerents on TestCamp field
all-counter-air squadrons, so delivery-family missions never
generated — the routed war could not be exercised at all.
`CampaignConfig::tasking_role_fallback` (default OFF — the strict gate
is B.3/C2 behavior, goldens pinned byte-identical) lets a team with
no exact-role squadron task its best-available wing instead;
campaign_qc arms it as the honest bridge to C4's FindBestAir.

**campaign_qc --tasking** runs the whole chain and gates it: the
route planner attaches to the ladder (threat map built from the SAME
sources, viewed from the FIRST BELLIGERENT — te_team can be a
non-participant, and a neutral viewer packs an empty map), every
generated delivery mission carries its route, the spawner materializes
it, and the QC's summary carries the telemetry: routes built/failed,
safe-path searches, direct fallbacks, threat-map coverage (painted AD
units / threatened cells), synthetic spawns. Exit 7 fires when the
ladder drew aircraft with a planner attached but built no routes (or
materialized nothing) — and reads the CAMPAIGN'S own route counters
(counting route-less intents cannot see build failures: the synthetic
mark is stamped only on success, the lesson of the first blind gate).

Verified on real data (TestCamp, v71, regenerated world JSON — see
below): 4-hour tasking + 20-minute INTSTRIKE — 8 cycles, 411 intents,
1,013 aircraft drawn, **81 routes (383 waypoints, 0 build failures,
22 threat-avoidance searches)**, 8 synthetic INTSTRIKE aircraft
spawned and flown alongside the 49 saved flights, 88 bombs / 66
features destroyed / 20 objectives written back into the ledger —
campaign_result.json byte-identical across two runs, all gates green
(exit 0). The no-tasking baseline is unchanged (the C2 numbers,
exactly — the planner is an attachment, not a mode switch).

**Known limitation, visible by design**: the fixture theater DB
(Falcon4.UCD) is an 8-entry sample, so only the AD battalions whose
entity types resolve into it paint (TestCamp: 3 of the 247 subtype-1
battalions — 2,088 threatened cells from rings of 29/86 grid units).
The other 244 need the full theater's UCD (game data, not code). The
QC's threat stats make the coverage number visible instead of silent,
and the host `MinAvoidThreat` override (25, vs the reference default
40 from aiinput.dat) lets the sample's single-ring band scores
(30-33) shape routes. With the full UCD the same code paints every
ring and the default threshold stands.

TestCamp.world.json is regenerable on demand (gitignored; TestCamp.cam
is tracked):

```
cam2json TestCamp.cam TestCamp.world.json --theater korea \
  --terrain korea.terrain.json \
  --class-table f4-world-convert/tests/fixtures/FALCON4.ct \
  --theater-data f4-world-convert/tests/fixtures
```

## 5. The rest of the loop (C4–C5)

### V-CAMP — the viewer runs the war (LANDED, the C4/C5 development surface)
C4/C5 need eyes: packages forming, routes bending, the 24-hour war
adapting. `CampaignSession` (f4-simulation — engine-agnostic
orchestration, composition only) repackages the QC wiring as ONE
frame-driven object with the ONE-WORLD closure the QC never had: the
spawner materializes generated missions into the SIMULATION's world
and registers them through the new
`Simulation::register_aircraft()` — update_all alone ticks a
late-comer's FM while its transform parks forever; the roster is what
the sync loop walks. One clock: fixed sim_dt ticks, the ladder and
the damage sync in whole campaign seconds off the same ticks. The
world viewer drives it (Campaign menu / Session window): play/pause
(Space), 1x–240x presets (wall-clock scaling — the tick dt never
changes), the campaign clock at the save's epoch, the war-status
block, a generated-missions table, live aircraft + routes + the
threat-map overlay on the canvas, and live flight-plan inspection.
Deterministic by construction (byte-identical ledgers pinned by
test). The 24-hour RUN remains a headless harness (C5); the viewer is
where it gets WATCHED.

### C4 — the ATM pipeline (M4.2) + squadron selection (M4.6) (LANDED)
The 7 composable tasking phases with budget awareness, package
composition from profile hints, and `FindBestAir` scoring — which
REPLACES the C3 role-fallback bridge with the reference's actual
capability-scored selection. What shipped:

- **`AirTaskingManager`** (f4-campaign, atm.hpp/cpp): the phases as
  public, independently-testable methods — 1. request generation (the
  profile ladder + the decoded ATO backlog seed, past-TOT requests on
  the reference's 30-minute delay pushes capped at 8; GetPriority's
  deterministic subset scores each request from the team's own
  mission_priority/objtype_priority tables, both now decoded and
  emitted), 2. prioritization (stable priority sort + the
  missions_per_cycle tempo budget), 3. deconfliction
  (mindistance/mintime vs booked flights), 4. package building
  (ScoreThreatFast at the profile's target altitudes → NEED_SEAD,
  then FindBestAir), 5. support assignment (ADDSEAD + NEED_SEAD pairs
  a SEADESCORT flight, ADDESCORT pairs a fighter escort — each with
  its own FindBestAir pick and TOT staggered by the support profile's
  separation), 6. route planning (Campaign-side: the C3 builder, the
  main flight's route shared package-wide), 7. TOT slot scheduling
  (FindTakeoffSlot/ScheduleAircraft ports over the decoded
  `atm_schedules` bitmasks — the save's own planned sorties are
  already-consumed slots our flights deconflict against; the snapped
  TOT shifts by the snap delta and the flight is booked for
  recovery).
- **FindBestAir** (atm.cpp:1534's port): rating (UCD Scores[ref-ARO]
  when the theater DB resolves the squadron, else the
  specialty-derived fallback), ±5 specialty, the lowestScore gate,
  capability/availability/schedule-full skips, −5 one-short, +3
  within-package squadron reuse, +2 same airbase, +2 half range, +2
  quickest arrival with the reference's previous-best rebalancing.
  A counter-air wing IS taskable for strike at a reduced rating —
  scored, never gated.
- **Mission recovery**: the C2 "drawn = committed" simplification
  closes — when a flight's mission-over deadline passes, its
  SURVIVORS (drawn − the ledger's per-flight booked losses) return to
  the tasking pool (ledger: apply_mission_recovery, the draw's
  mirror). Only deaths keep a draw spent.
- **The data surface**: world_json emits `atm_schedules` (the 32-block
  takeoff bitmasks behind the id list) + the team priority tables;
  WorldState parses them; ITeamSource carries the accessors. The
  boundary discipline unchanged.
- **The Campaign mode switch** (`CampaignConfig::atm_pipeline`,
  default OFF — the legacy ladder's goldens stay byte-identical,
  pinned by test): campaign_qc and the campaign session arm it (the
  session's new default). One cycle-time fix rides along: every due
  cycle now fires at its OWN due time (a big tick == N small ticks
  exactly; the pre-C4 code fired all cycles at the advanced clock —
  equivalent only for single-cycle horizons, and slot scheduling
  exposed it).
- Verified on TestCamp (QC acceptance): 4 h tasking + 20 min
  INTSTRIKE — 8 cycles, 523 packages with 180 escorts, 143 slot
  snaps, 406 aircraft recovered, 240 routes / 1,157 wps, 88 bombs /
  66 features destroyed / 20 objectives written back, exit 0,
  campaign_result.json byte-identical across runs.

The C3 role-fallback bridge is retired at its armed sites (the QC
and the session now run the pipeline; the config flag stays for
hosts that want the legacy shape).

### C5 — the 24-hour war (the acceptance) (LANDED)
`campaign_qc --war <hours>` runs the long-horizon mode: both sides
generate, fly, fight, attrite, recover, and adapt over the ATM
pipeline — hours of sim time, headless, deterministic. The C1 ledger
+ the C2 one-pool tasking + the C3 routed generation + the C4 ATM
pipeline make the run MEANINGFUL: every hour's packages, draws,
losses, recoveries, and resupply reshape the next hour's force. The
run's artifacts ARE the "core game functionality replicated"
certificate.

Three pieces, in the layer each belongs to:

| Piece | Library | What it is |
|-------|---------|------------|
| `CampaignWarHarness` | f4-simulation | The acceptance runner: composes the SAME `CampaignSession` the viewer drives, advances it in fully-drained 4-sim-second batches (byte-equivalent to any other tick split — the C2 pin), samples the war every `--war-sample` seconds, and derives the four verdicts. Runs the whole war TWICE in-process and compares the ledger BYTES (the MD5 is the certificate humans re-derive with `md5sum`). |
| `Simulation::retire_aircraft` + `CampaignSessionOptions::wreck_hold_sec` | f4-simulation | The entity-churn bound: killed aircraft retire (roster + wingman pairs + combat policies + world entity) `wreck_hold` sim-seconds after their `EntityKilledMessage` — the ledger booked the loss at EVENT time, so the corpse's removal never races the books. 0 = the pre-C5 lifetime (wrecks freeze forever; every golden pins it). FreeFalcon's own shape: the sim object dies, the campaign object's bookkeeping lives on. |
| `--war` mode | campaign_qc | The CLI face: per-hour progress lines (cycles, draws, spawns, live roster, throughput, RSS), three artifacts, and the exit gates. |

The four C5 gates (per-sample, run 0):

- **DETERMINISM (exit 9)**: run 1's ledger bytes differ from run 0's.
  The comparison is the bytes themselves; the MD5s are the certificate.
- **LEDGER DRIFT (exit 10)**: a one-pool identity broke — a team pool
  outside `[0, initial]`, the tasking view outside `[0, remaining]`,
  the team books disagreeing with the squadron books (guarded on the
  ledger's own unmatched-flow counters), or a monotone counter going
  backwards.
- **ENTITY LEAK (exit 11)**: the roster identity broke —
  `live != initial + spawned − retired` at any sample. The churn
  bound's deterministic form (RSS is diary telemetry, never a gate —
  platform-dependent bytes are not acceptance evidence).
- **WAR ALIVE (exit 12)**: the clock stopped (a run of frozen advance
  batches aborts the war), no cycle fired in a sample (when the cycle
  period is shorter than the cadence) or the whole war (a period
  beyond the horizon), or a belligerent that has EVER drawn went
  silent for a full sample with aircraft taskable (a side that never
  drew from t0 is fixture data — role mix — visible in the diary's
  per-team `drawn_total` rows, not a gate: the false stall is worse
  than the missing gate).

The inherited tasking gates ride along (war edition): exit 6 (drew
nothing with belligerent air), 7 (drew but no routes / nothing
materialized), 8 (ATM armed, no packages).

Artifacts: `campaign_result.json` (run 0's ledger — byte-stable),
`campaign_qc_summary.json`'s `war` block (DETERMINISTIC content only —
verdicts, counters, MD5, per-team final pools; no wall-clock, no
RSS, no ticks/sec), and `campaign_war_diary.json` (one row per
sample WITH the performance telemetry — explicitly NOT byte-stable,
and in its own file for exactly that reason). The diary is the
watch surface: throughput (ticks/sec), RSS growth, live-roster
trend, per-team pool trajectories.

Verified on real data (TestCamp, v71, regenerated world JSON): a
6-minute war (`--war 0.1 --war-sample 60 --tasking-cycle 60`, 2
in-process runs) — 6 cycles, 556 intents, 413 ATM packages + 143
escorts, 115 routes (0 failures), 1,362 aircraft drawn (ROK 672 /
DPRK 690 — both belligerents generating), 48 synthetic aircraft
flown alongside the capped 48 saved flights (96 live, all airborne
by t=360 s), ledger MD5 identical across the two runs and equal to
`md5sum campaign_result.json`, all four verdicts green, exit 0.
Reinforcement verified with a 60 s cadence (28 delivered on the
deficit); the default 12 h cadence fires the stale-anchor catch-up
at t≈0 and refills the deficits at hour 12 — one believable refill
inside the 24-hour horizon. Debug-build throughput on the dev
container: ~330-540 ticks/sec at ~96 live aircraft (a 24-hour war
is a multi-hour Debug run — Release builds are the acceptance
medium; the diary's ticks-per-sec column makes the rate visible).

Known limitation, honest by design: A/A combat is dark for campaign
flights in this slice (the combat chain's arming — see
COMBAT_CHAIN_PLAN), so war runs book bomb damage and attrition-side
ledger events but no air kills, and the reaper's churn machinery
stays quiet until that arming lands (its mechanics are pinned by the
harness tests regardless: hold → retire-once → roster identity).
**Closed by C6 below.**

### C6 — arming the campaign flights (A/A goes live) (LANDED)

The C5 limitation was structural, not missing machinery. Every piece
of the fight already existed and was test-pinned — weapons, radar,
RWR, track files, BVR/WVR/defeat tactics, the arbiter, the kill→ledger
flow, the reaper — but none of it was ATTACHED to campaign flights:
`spawn_aircraft_for_flight` deliberately left `combat_enabled` off
(campaign_bridge.cpp's own note: the A/A rungs on an
omniscient-GCI picture would break route-following). C6 is the
integration tranche that flips that, on three legs:

1. **The components** (C6.1). `Simulation::arm_campaign_aircraft(id)`
   attaches, per campaign aircraft, the M3 combat set the scenario
   path already uses — `RadarSimComponent`, `RwrComponent`,
   `SignatureComponent`, `DamageStateComponent`, the gun, the NCTR
   identity — plus the one piece the scenario path gets from
   authoring and the campaign path never had: a
   `RadarBackedDetectionPolicy` per brain, owned by the Simulation
   (the same `combat_policies_` store the scenario path uses, so
   `retire_aircraft` reaps campaign policies exactly like scenario
   ones). This is the M2 flip COMBAT_CHAIN_PLAN deferred: campaign
   SensorFusion reads radar truth, GCI-omniscience goes dark.
   Seeding follows the scenario discipline: per-aircraft
   `radar_rng_seed + arm_index` (a monotone counter — spawn order is
   deterministic, so the seeds are too).

2. **The doctrine** (C6.3) — the answer to "would break
   route-following". Mission ROLE decides who fights:
   - **Fighting categories** (CAP, Sweep, Intercept, Escort) arm the
     full ladder — BVR + WVR + guns + release — and get the doctrine
     A/A loadout (C6.4: AIM-120 + AIM-9 stations when the wire
     loadout carries none, mirroring `standard_fighter`). Their job
     is the fight; maneuvering off the route IS the mission.
   - **Every other category** (Strike, SEAD, CAS, Recon, Support,
     Other, untasked) fly **defensive-only**: the brain's combat
     ladder is ON (RWR warning → MissileDefeat reacts, GunsJink
     maneuvers) but the engagement rungs stand down through
     FreeFalcon's own mechanism — the BRAINDAT.brn archetype. The
     shipped `SEAD`/`Strike`/`Waypointer` archetypes disarm
     BVREngage/WVREngage/MissileEngage/GunsEngage while keeping
     MissileDefeat armed (verified against the fixture data: every
     engagement row 0, every defensive row 1). The strike rung is
     untouched — bombs keep falling exactly as the A-G slice pinned.
     No new brain API: the archetype gate is the reference's own
     doctrine vocabulary, already load-bearing for scenario brains.
   - The role comes from `f4::campaign::mission_category` over the
     flight's mission byte, stamped on `CampaignOriginComponent` at
     spawn (both spawn paths) — the campaign identity already carried
     everything else; the mission byte completes it.

3. **The opt-in** (C6.1b). `CombatConfig::campaign_armed` (scenario
   JSON `"combat": {"campaign_armed": true}`, default false) gates
   the whole tranche; `CampaignSessionOptions::aa_combat` (default
   false) writes it; `campaign_qc --aa-combat` arms the war. Every
   existing golden, session test, and QC run is byte-identical with
   the flag off — the same contract `wreck_hold_sec = 0` keeps.

Wiring: the bulk path arms at `initialize()` (spawn loop); the
spawner path arms in `CampaignSession::adopt_new_spawns_()` (the
one-world cadence — every late spawn is registered AND armed on the
same per-campaign-second walk). Brain data loads eagerly at
`initialize()` when `campaign_armed` (the build-tree
`simdata/braindata.json` default, or `brain_data_path`), and the
arm call fails loudly when no disengaged archetype exists — silent
degradation would hand the doctrine a fighting strike flight
without anyone noticing.

The kill chain closes with no new code: missile/gun damage →
`DamageStateComponent::killed` → `EntityKilledMessage` → the C1
sink books the loss (squadron + team, via `CampaignOriginComponent`)
→ the reaper retires the wreck → the next cycle tasks a weaker
force. A/A losses were always LEDGER-side ready (`air_losses`
since C1); what was missing was an aircraft that could die in the
air.

Acceptance (C6.5): `campaign_qc --war --aa-combat` on TestCamp —
air kills booked (both belligerents scoring), the reaper retiring
wrecks (roster identity holding at every sample), the four C5
verdicts still green, and the two runs' ledger MD5s still identical
(combat determinism: the only new randomness is the radar's seeded
detection rolls — seed-derived per aircraft, spawn-order
deterministic). The war block gains `aa_combat: true` so the
artifact records which war ran.

Out of scope, deliberately: WVR guns kills at campaign scale ride
the existing M4 machinery unchanged (no new doctrine), support
flights get no self-defense employment beyond MissileDefeat
(WVR/BVR archetypes stay disarmed — FreeFalcon's own choice for
tankers/AWACS), and the per-archetype WVR entry bands stay at the
BVRModule default for fighting roles (fixture .brn ranges are
authored for the reference's skill model, not ours — tuning is a
later, data-driven tranche once the WST import lands).

Verification (TestCamp v71, regenerated world JSON, Release build):
LANDED — a 6-minute armed war (`--war 0.1 --war-sample 60
--tasking-cycle 60 --aa-combat`, 2 in-process runs): 6 cycles, 556
intents, 413 ATM packages + 143 escorts, 115 routes (0 failures),
1,362 drawn (ROK 672 / DPRK 690), 96 live aircraft (48 saved + 48
synthetic, 80 airborne at the end), 96 armed (33 fighters by role
doctrine + 63 defensive), **the war loop's first A/A kill** —
booked with full attribution (killer credit to the shooter's
squadron, victim team/squadron/flight, t=356.7 s), ledger MD5
identical across the two runs and equal to `md5sum
campaign_result.json`, all four C5 verdicts green, exit 0. A
12-minute observation run shows the fights compounding (12 → 39
brains engaged, merges at 0 NM); the merge phase was the throughput
cost (Release: ~480 tps pre-fight, ~37 tps at peak merge over 80
airborne — the WVR/defensive sweep across the roster; documented in
the diary, never gated). **Closed by PERF-1** — the shared air-picture
snapshot (PERFORMANCE_PLAN.md §3): the per-brain sensor-fusion
database walk collapsed 96× → 1× with the ledger bytes unchanged, and
the armed wars now sustain a 140–200 tps floor. The reaper: this run's
kill retires at
t=656.7 s (hold 300, beyond the 360 s horizon) — the mechanics stay
pinned by the harness unit tests.

C6 surfaced and closed TWO integration bugs the unarmed war could
never see (both are real fidelity fixes, not C6 features):

1. **The team mapping had no sides.** TestCamp's player slot is a
   neutral placeholder (team "XX") while the war is ROK(2) vs
   DPRK(6) — the B.3 `owner_team_string` mapped EVERY aircraft to
   "green": 96 airborne, zero hostile pairs, zero fights possible.
   Now: player-belligerent saves keep the classic mapping; a
   neutral-player save maps the WAR PAIR (the first at-war pair in
   slot order, deterministic) to blue/red, everyone else green.

2. **The radar painted the parking ramps.** The M2 placeholder
   scanned every transform-bearing entity: at campaign scale 48
   armed radars × ~4,400 entities per sweep detected HALF of all
   candidates every second (measured: 125k track-creating
   detections/s in a 36 s war) — ground clutter flooding the track
   stores, a perf AND fidelity bug (FreeFalcon's air picture never
   paints parked vehicles). The shared
   `TransformComponent::is_ground_clutter()` predicate (stationary
   AND below every Korea terrain post — nothing genuinely airborne
   is ever clutter; a stationary rig at 20,000 ft stays visible)
   now skips clutter in the radar scan AND the SensorFusion
   rebuild. Also: the search bar steers onto the ground track
   (FreeFalcon's radar is boresighted to the nose; a fixed north
   bar meant an east-flying fighter never painted the hostile off
   its nose). North-flying rigs keep the exact bar they were
   pinned with.

Known limitation, honest by design: the sim's air picture is
TWO-SIDED (blue/red/green), so a third armed team in a war-pair
save (the U.S. squadron above) engages whichever side its
own-relative hostility rule marks hostile. The allied-to-a-side
mapping (member flags / stance toward BOTH war-pair members) is a
follow-up refinement, documented here.

## 6. What does NOT change

- The IDataSource boundary: f4-campaign still never sees EntityWorld,
  f4-world-convert, or a flight model. The ledger speaks campaign
  identity (team slots, VU_IDs) and numbers; the sink does the ECS
  resolution in f4-simulation, where both sides are already linked.
- The aircraft component set: `CampaignOriginComponent` is ADDED by the
  campaign spawn path only; scenario aircraft, features, vehicles are
  untouched.
- The bus contract: plain structs, one per event; the sink is just
  another subscriber (the spawner's mirror image).
- Determinism: no RNG, no clocks of their own; ordering is bus order.
  The zero-event ledger is the identity element (pinned by the
  golden-identity test — a fresh ledger changes nothing).
- The no-ledger tasking path: B.3's own-pool behavior, byte-identical
  goldens — the ledger is an ATTACHMENT, not a replacement.

## 7. Known gaps (deliberate, documented)

- ~~**Ground losses** book only the CREDIT side (ag_kills); battalion
  roster attrition lands with the ground-war tranche.~~ — LANDED
  with **G1** ([GROUND_WAR_PLAN.md](GROUND_WAR_PLAN.md)): the
  `GroundWar` engine (battalion movement, the front line, contact
  attrition, objective capture, the last_resupply cadence), the
  ledger's ground books + write-back, the session's entity mirror,
  `campaign_qc --ground-war` (exit 13). The two-side war-pair
  limitation and the first-slice capture doctrine are G1's own
  documented gaps.
- **Gun damage to features** without a bomb impact event is picked up
  only when it destroys (the sync diffs destroyed-state; damaged-only
  gun hits on non-impacted objectives ride the fstatus diff — covered
  when they change the bitmap).
- **World JSON re-emission**: `apply_to` mutates the in-memory typed
  WorldState; writing that back out as a world JSON file needs a
  WorldState→JSON emitter (f4-world currently only reads; the emitter
  lives in f4-world-convert over decode structs). The .cam save-side
  re-encoder does not exist for ANY subsystem yet — both land together
  with the campaign save-write tranche.
- **Reinforcement depth** (C2): the team-level `replacements_avail`
  strategic stock is decoded and exposed (ITeamSource) but not
  CONSUMED — the squadron-level wire budgets are the operative source
  this slice; the stock-to-budget replenishment flow (and the
  `last_resupply`/`last_repair` timers for ground supply and objective
  feature repair) land with their consumers (the ground-war and
  repair tranches). (Drawn aircraft surviving their mission now
  RETURN via C4 mission recovery — apply_mission_recovery; the
  remainder of this entry is the resupply-depth story.)
- **RoE overflight walls** (C3): the stance vocabulary now carries
  Neutral/Hostile (the two denying classes), but score() still SCORES
  rather than walls — the 32000 lethal denial (and the A*'s >120
  impassable test, already ported) arms with the RoE refinement
  tranche.
- **ATM scope** (C4): the pipeline composes MAIN + ESCORT flights —
  the reference's multi-strike feature analysis (BestTargetFeature
  loops vs feature HP) and its support-flight SHARING (FindSupport
  Flights: AWACS/tanker/JSTAR/ECM racetracks) need the feature-damage
  loop and loiter-racetrack routes respectively (the weapons and
  loiter tranches). GetPriority's PO/package/distance/random terms
  need strategy-layer data (the request's context vocabulary decodes;
  the ACTION system that files contextual requests is the strategy
  tranche). Enemy-requested BARCAP/SWEEP (ADDBARCAP/ADDSWEEP →
  RequestEnemyMission) — the ladder already generates both sides'
  defensive CAP every cycle; the requester-driven path lands with the
  strategy layer.
- **Threat-map coverage** (C3): only AD battalions whose entity
  types resolve into the theater DB's UCD paint — the fixture UCD is
  an 8-entry sample (3 of 247 AD battalions on TestCamp); the full
  theater's UCD (game data) paints the rest with no code change.
- **Per-action altitude shaping, loiter racetracks, tanker waypoints**
  (C3): each lands with its consumer (the loiter and fuel tranches) —
  documented in route_builder.hpp. (Package-shared ingress and TOT
  slotting landed with C4's package composition — escorts share the
  main flight's route; takeoffs snap to the airbase schedules.)

## 8. Implementation order (C4 onward)

1. ~~C4 ATM phases + FindBestAir (replacing the C3 role-fallback
   bridge)~~ — LANDED (plus C4-FIX-2: the Start Session crash/freeze
   tranche — the ClassTable hoisted into a Simulation member the
   BubbleManager can actually outlive, the session owning the spawner's
   airfield/map/template lenders, and create() moved to a worker
   thread so the viewer's Start Session never freezes; the session
   panel shows a live "Starting session…" state and `--session` gives
   headless smoke coverage. Plus C4-FIX-3: the campaign's OWN thread —
   `CampaignSessionRunner` advances the session in short mutex-guarded
   batches while the render loop locks the same mutex for its frame
   read+draw scope (the UI never blocks on a 240-tick advance again);
   and full 3D coverage — every session entity (flying + parked
   aircraft, deaggregated vehicles/personnel) renders, with the deagg
   bubble following the CAMERA when zoomed in, so the interactive
   session is watchable, not just runnable).
2. ~~C5 the long run (the 24-hour war, now over the ATM pipeline)~~ —
   LANDED: `campaign_qc --war` + `CampaignWarHarness` + the wreck
   reaper (see §5's C5 section for the verification numbers and the
   exit-gate table).
3. ~~C6 arming the campaign flights (A/A goes live)~~ — LANDED:
   `Simulation::arm_campaign_aircraft` + the mission-role doctrine
   (BRAINDAT archetypes) + `CampaignSessionOptions::aa_combat` +
   `campaign_qc --aa-combat` (see §5's C6 section; the war runs book
   air kills now, and the reaper finally has something to reap).
4. ~~G1 the ground war (battalion maneuver + the front line)~~ —
   LANDED: `f4-campaign/ground_war.hpp` (the engine) + the ledger's
   ground books + `apply_ground_to` + the session's entity mirror +
   `campaign_qc --ground-war` and exit 13 (see
   [GROUND_WAR_PLAN.md](GROUND_WAR_PLAN.md) for the verification
   numbers — the 1-hour war's ledger MD5 is the certificate, the
   front line moves, and the books balance on both sides of the
   DMZ).
5. ~~G2 the interdiction link (CAS against real battalions)~~ —
   LANDED: the shared FLOT + battalion ranking, the unit-target
   tasking rung (both ladders, `unit_strike`), the unit-position
   route resolution, the battalion blast endpoint +
   `GroundUnitLossMessage`, the sink's booking arm, and
   `campaign_qc --unit-strike` + exit 14 (see
   [INTERDICTION_PLAN.md](INTERDICTION_PLAN.md) for the
   verification numbers — the 0.3 h/1 h wars' MD5s are the
   certificates, air power thins the line, and the engine's pull
   decays the roster).

---

*This document supersedes the "out of scope" framing of NEXT_PHASE_PLAN
§B.2's write-back gap. C1's design notes live in the headers
(`result_ledger.hpp`, `campaign_origin.hpp`, `campaign_result_sink.hpp`,
`world_writeback.hpp`) and C2's in `campaign.hpp` (the one-pool
contract) + `src/squadron_snapshot.hpp` (the shared force snapshot) —
the same place every tranche records its decisions.*
