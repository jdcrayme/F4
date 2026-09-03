# Campaign Loop — Closing the War (Phase C)

> **Status**: Active plan. **C1 (the result ledger + sink + write-back),
> C2 (one pool — draws, netting, reinforcement), C3 (threat map +
> A* + route builder, generation-to-spawn), V-CAMP (the live
> campaign session — the viewer runs the war), and C4 (the ATM
> pipeline — 7-phase tasking, FindBestAir, escort pairing, TOT
> slots, mission recovery) are LANDED** — the rest of this document
> is the roadmap they opened.
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

### C5 — the 24-hour war (the acceptance)
`campaign_qc` grows a long-horizon mode: both sides generate, fly,
fight, attrite, adapt — hours of sim time, headless, deterministic
(byte-stable summary at the end). The C1 ledger + the C2 one-pool
tasking + the C3 routed generation + the C4 ATM pipeline make this
run MEANINGFUL: every hour's packages, draws, losses, recoveries, and
resupply reshape the next hour's force. That run's artifacts (summary
+ result + trace) are the "core game functionality replicated"
certificate.

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

- **Ground losses** book only the CREDIT side (ag_kills); battalion
  roster attrition lands with the ground-war tranche.
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
   bridge)~~ — LANDED.
2. C5 the long run (the 24-hour war, now over the ATM pipeline).

---

*This document supersedes the "out of scope" framing of NEXT_PHASE_PLAN
§B.2's write-back gap. C1's design notes live in the headers
(`result_ledger.hpp`, `campaign_origin.hpp`, `campaign_result_sink.hpp`,
`world_writeback.hpp`) and C2's in `campaign.hpp` (the one-pool
contract) + `src/squadron_snapshot.hpp` (the shared force snapshot) —
the same place every tranche records its decisions.*
