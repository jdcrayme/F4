# Campaign Loop — Closing the War (Phase C)

> **Status**: Active plan. **C1 (the result ledger + sink + write-back)
> and C2 (one pool — draws, netting, reinforcement) are LANDED** — the
> rest of this document is the roadmap they opened.
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

## 4. The rest of the loop (C3–C5)

### C3 — threat map + pathfinder + route builder (M4.3–M4.5)
`ScoreThreatFast` port (altitude-banded threat map, updated from
entities), the generic A* (threat-map concept constraint, 2000-node
budget, partial-path fallback), and `BuildPathToTarget` with waypoint
elimination. Generated missions fly THEIR OWN routes, not just saved
ones. Validation: path cost matches the reference on the same map;
routes avoid high-threat bands.

### C4 — the ATM pipeline (M4.2) + squadron selection (M4.6)
The 7 composable tasking phases with budget awareness, package
composition from profile hints (escort pairing, TOT slotting against
the decoded `atm_airbases` schedules), `FindBestAir` scoring. The
synthetic ladder becomes the reference's actual decision shape.

### C5 — the 24-hour war (the acceptance)
`campaign_qc` grows a long-horizon mode: both sides generate, fly,
fight, attrite, adapt — hours of sim time, headless, deterministic
(byte-stable summary at the end). The C1 ledger + the C2 one-pool
tasking make this run MEANINGFUL: every hour's draws, losses, and
resupply reshape the next hour's force. That run's artifacts (summary
+ result + trace) are the "core game functionality replicated"
certificate.

## 5. What does NOT change

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

## 6. Known gaps (deliberate, documented)

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
  repair tranches). Drawn aircraft that survive their mission never
  RETURN to the pool in this slice (mission recovery is the C3/C4
  tranche's job — the accounting models "drawn = committed", the
  same simplification B.3 made, now visible in one place).

## 7. Implementation order (C3 onward)

1. C3 threat map (pure f4-math/f4-campaign slice, viewer overlay QC).
2. C3 A* + route builder (consumes the map; campaign_qc gains route QC).
3. C4 ATM phases + FindBestAir.
4. C5 the long run.

---

*This document supersedes the "out of scope" framing of NEXT_PHASE_PLAN
§B.2's write-back gap. C1's design notes live in the headers
(`result_ledger.hpp`, `campaign_origin.hpp`, `campaign_result_sink.hpp`,
`world_writeback.hpp`) and C2's in `campaign.hpp` (the one-pool
contract) + `src/squadron_snapshot.hpp` (the shared force snapshot) —
the same place every tranche records its decisions.*
