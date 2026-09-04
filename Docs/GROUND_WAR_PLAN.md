# Ground War — Battalion Maneuver & the Front Line (Tranche G1)

> **Status**: LANDED. The campaign loop's air side closed C1–C6 (and
> PERF-1 kept it fast); the GROUND side was the documented gap —
> CAMPAIGN_LOOP_PLAN §7's first line: "Ground losses book only the
> CREDIT side; battalion roster attrition lands with the ground-war
> tranche." G1 is that tranche: a headless, deterministic ground-war
> ENGINE over the same IDataSource boundary, writing through the same
> result ledger, moved by the same one clock. **Prerequisite**: C5
> (the war harness + the wreck reaper) and C6 (the armed campaign).
> **Companion**: [Campaign Loop Plan](CAMPAIGN_LOOP_PLAN.md) (the loop
> this closes the ground side of), [Performance Plan](PERFORMANCE_PLAN.md).

---

## 1. The problem: a frozen battlefield

B.3 through C6 built a war that flies: tasking draws aircraft, the
spawner materializes them, they fly routes, fight, die, and write it
all back. The ground sat underneath it FROZEN. Six hundred battalions
stood at their save-time grid cells forever; objective ownership never
changed no matter what either army did; the ledger's only ground
number was the SHOOTER's ag_kills credit — the victim's own attrition
did not exist as state. A campaign where the front never moves is a
mission generator, not a war: FreeFalcon's ground loop
(maneuver → contact → attrition → capture) is what makes air interdiction
MEAN anything (the bridges you blow feed an army that is actually
trying to advance).

The same structural gap C1 called out for air losses applied on the
ground: kills landed on entities, nothing could say which battalion
thinned. And the territory layer was strictly read-only — the threat
map's ownership grid was a snapshot, not a battlefield.

## 2. G1 — the engine (f4-campaign/ground_war)

`GroundWar` is the campaign-side twin of the `Campaign` ladder: a
headless state machine over `ICampaignSource` / `ITeamSource` /
`IObjectiveSource` / `IUnitCoreSource`, bound at construction to the
result ledger (the ONE writer), advanced by `tick(delta)` on the
caller's clock. No EntityWorld, no f4-world-convert, no flight model
— the boundary discipline every other campaign piece keeps.

What one update fires, in order:

| Phase | What it does | FreeFalcon correspondence |
|-------|--------------|---------------------------|
| **Orders** (per `orders_sec`) | Scores the ENEMY's objectives and assigns each mobile battalion a target (garrison slots per objective, wire-order ties) | campobj.cpp `DoCalculations` objective scoring: front proximity `(200 − dist) × 0.2` capped ±30, priority bonus +50/>95 and +20/>90 — the reference's own terms, integer, `random(5)` dropped (determinism) |
| **Front line** (rebuilt with orders) | Per grid column, the midpoint between the south side's furthest-north objective and the north side's furthest-south (±3 column band; sides by territory centroid) | the FLOT `DistanceToFront` serves; column shape the viewer/QC paint |
| **Movement** (per `update_sec`) | Mobile battalions walk toward targets at wire `movement_speed` (kph) or subtype-family defaults; sub-grid travel in 1/256 fixed point (the wire's own `position`-byte semantics); fatigue > 75 and supply < 25 halve speed; ENGAGED battalions are pinned | gndunit movement at unit speed; the reference's combat movement gate |
| **Engagement** (per update) | Opposing battalions within 2 grid exchange attrition: each side's take ∝ enemy combat power (strength × supply × morale), a few vehicles per HOUR per pair at parity; fractional kills accumulate fixed-point; roster decays highest-group-first; morale erodes, fatigue accrues, zero-strength battalions die | the battalion firefight exchange (tempo deliberately at hours — battalions fight for hours, not minutes) |
| **Capture** (per update) | A battalion at an enemy objective (≤ 2 grid) with no enemy defender in contact and ≥ 6 vehicles flips the owner; the capturer garrisons its prize | the capture ladder's own rule (the reference's GTM Capture class ordering) |
| **Resupply** (per `resupply_period_sec`, default OFF) | Supply +25 / fatigue −25 / morale +10 per fire, anchored on the .cmp `last_resupply`, catch-up-once | the ground-supply cadence the C2 notes deferred to "the ground-war tranche" — this tranche, consumed |
| **Air-loss pull** (per update) | AG kills the C1 sink booked against battalion ENTITIES (`apply_ground_loss`, air=true) are applied to the engine's rosters — index-tracked over the arrival-ordered log, applied once, never re-booked | air power thinning the line the ground war then fights with |

Spatial bucketing (6-grid cells, the threat map's own MAP_RATIO)
keeps contact detection O(N) per update: 672 battalions, 1,440
updates over a 24-hour war, ~nothing on the tick budget (measured
over a whole 2-hour war: 1,000–1,300 tps ground-only, RSS flat).

**The war pair** is the same named-slot RelType::War rule
`Campaign::belligerent_teams()` uses, first at-war pair in slot order
(TestCamp: ROK 2 / DPRK 6; the kunsan rig: USA 1 / DPRK 6). Neutral
teams' battalions stand down. A war-less world is an inert engine —
pinned by test.

## 3. The books and the state (one writer, one certificate)

- **The LEDGER owns the ground write side** (result_ledger.hpp, G1
  section): `apply_ground_loss` (exchange + air events, per-battalion
  run_losses, team ground books), `apply_objective_capture` (the
  territorial log, capturing team's counter),
  `sync_ground_unit` (final-state sync: strength, position,
  supply/morale/fatigue, destroyed — last write wins, run_losses
  monotone event-derived). `to_json()` gains an OPTIONAL `"ground"`
  object — totals, per-team rows, VU-sorted battalion states,
  arrival-ordered loss and capture events — emitted only when ground
  activity exists, so ground-quiet runs emit byte-identical pre-G1
  documents (the C1 zero-event identity, ground edition; the artifact
  version stays 2 — unknown-key skipping is f4-json's own rule).
- **The ENGINE owns the live state** (movement targets, rosters,
  morale) and syncs dirty battalions to the ledger every update — the
  C5 determinism certificate therefore covers the GROUND bytes with
  zero new machinery: the war harness MD5s one document that now
  contains both wars.
- **The WRITE-BACK** (`ground_writeback.hpp`,
  `apply_ground_to(WorldState&)`): battalion x/y, roster, losses
  (uchar-saturating), supply/morale/fatigue, heading, last_move /
  last_combat, dest_x/dest_y; objective owner flips (first_owner
  untouched, the wire's save-start semantics). Activity-gated — a
  unit that never moved, lost, or was resupplied is not written; the
  unmatched-VU loudness rule applies.
- **The C1 sink's AG path closes the documented gap**: an
  origin-ful shooter killing a battalion ENTITY now books the victim
  too (`apply_ground_loss` air=true, killer provenance carried) —
  the "credit side only" era ends; the engine pulls it on the next
  update.

## 4. The session wiring (f4-simulation)

`CampaignSessionOptions::ground_war` (default false — the opt-in
contract `aa_combat` keeps: ground-off sessions are byte-identical to
the pre-G1 shape, pinned by test), with `ground_update_sec` (60),
`ground_orders_sec` (1800), `ground_resupply_sec` (0 = the
golden-identity default; `campaign_qc --ground-war` arms 43200, the
air reinforcement cadence's own rhythm).

The engine rides the SAME whole-campaign-second cadence the ladder
does (its own accumulator gates on the update granularity — one big
tick == N small ones, the C2 pin). After every engine update, the
ENTITY MIRROR walks the engine's battalions: transforms follow the
grid positions (× 1024 ft, the bridge's own constant),
`GroundTacticalComponent` follows supply/morale/fatigue/heading,
`UnitCoreComponent::roster` decays, destroyed battalions flip their
ALIVE tag — the 3D world's ground units march. On flight-less
scenario-list worlds the session populates the sim world first (the
same `populate_world` call the campaign_flights path makes — a world
with no flights never needed battalion entities before the ground
war did).

The war harness's diary, summary, and report carry the ground columns
(battalions, mobile, losses, destroyed, captures, engaged pairs,
front columns, march distance) — deterministic content in the
byte-stable artifacts, telemetry in the diary. The stats block and
the viewer panel read the same numbers. `campaign_qc --ground-war`
arms it; **exit 13** fires when an armed ground war produced nothing
(the exit-5 philosophy, ground edition: movement alone passes — a
0.1 h smoke war moves, fights happen at hours scale, captures at
days).

## 5. What does NOT change

- The IDataSource boundary: f4-campaign still never sees EntityWorld
  or f4-world-convert; the engine speaks VU_IDs, team slots, grid
  cells, and numbers. The entity mirror lives in f4-simulation where
  both sides already meet.
- The ledger's air books: ground events touch neither the aircraft
  pools nor the squadron counters — the C5 drift identities hold
  unchanged with the ground war live (pinned by the harness test).
- The bus contract, the spawner, the ATM pipeline, the reaper: the
  ground war is a new subscriber to the same clock, not a change to
  any existing subscriber.
- Determinism: no RNG, no wall clocks, no unordered iteration —
  wire-order walks, row-major buckets, integer fixed point (the two
  libm calls in movement — sqrt for step normalization, atan2 for
  the heading byte — are pure functions of integer inputs and never
  reach the document).

## 6. Known gaps (deliberate, documented)

- **Brigade doctrine**: parent_id/element_ids are decoded and
  carried, but tasking is per-battalion. The reference's
  division/brigade maneuver structure lands with the strategy
  tranche that owns it.
- **Artillery stands in the line**: SP/towed/rocket battalions trail
  at half speed and fight with direct fire — no indirect standoff
  (the reference's artillery model needs the battery/WST data the
  theater import carries; the real-data tranche's).
- **Two-side machine**: the war pair is two slots (the C6
  air-picture's own documented limitation, shared). A third armed
  team's battalions stand down; the multi-side generalization lands
  with the alliance mapping both sides wait on.
- **Deagg-vehicle kills** (bubble entities) do not decay the parent
  battalion's roster — the deagg→parent mapping is the viewer
  tranche's. Battalion-ENTITY kills (air=true) do.
- **Capture doctrine is first-slice**: an undefended objective in
  contact range flips on the next update (the save's intermingled
  mid-war dispositions resolve in the first minutes — 67 captures
  in the 6-minute TestCamp smoke run, honestly reported). Contested
  duration, garrison strength scaling, and retreat-before-annihilation
  are data-driven tuning once the WST import lands.
- ~~**AG-vs-battalion at campaign scale**: the sink's air-loss path is
  wired and unit-pinned, but nothing in the QC war shoots battalion
  entities yet (bombs damage features, missiles target aircraft) —
  it fires when the unit-targeting weapons tranche lands.~~ — LANDED
  with **G2** ([INTERDICTION_PLAN.md](INTERDICTION_PLAN.md)): bombs
  now attrite battalions (the aggregate blast endpoint), CAS tasks
  against front-line-ranked enemy battalions, the sink books
  air-sourced losses the engine pulls.

## 7. Verification (TestCamp v71, Release build)

- **The 6-minute smoke war** (`--war 0.1 --war-sample 60
  --tasking-cycle 60 --ground-war`, 2 in-process runs): exit 0, all
  four C5 verdicts green, ledger MD5 identical across the runs
  (951a8406… at the first cut, 3cbd2372… after the ground-block comma
  fix — both pinned by re-derivation), ground columns: 672/280
  battalions, 67 captures (the save's frozen mid-war dispositions),
  204 contested front columns, 364 grid marched.
- **The 1-hour war** (same shape, `--war 1`, 2 runs, 402 s wall):
  exit 0, **deterministic MD5 8171e8b6a8dfeb8057f747a06d5b173e**
  (re-derivable with `md5sum campaign_result.json`), all verdicts
  green, and the ground books carrying the war's arc: 61 updates, 13
  vehicle losses (ROK 8 / DPRK 5 — mutual attrition), 1 battalion
  destroyed (its run_losses exactly its 2-vehicle roster), 77 captures
  (76 northward, 1 south), the front WIDENING 204 → 290 contested
  columns as captured ground became the new line, 2,520 grid marched,
  1,085–1,300 tps, RSS flat at 244 MB.
- **The 2-hour observation** (ground-only, run 1): losses compounding
  2 → 99 vehicles, 84 captures, attrition tempo matching the design
  (hours, not minutes) — the diary's ground columns tell the story.
- **Suites**: 2,220/2,220 Debug + Release (2,205 + 15 new: the 14
  engine tests — war pair, inert war-less world, movement, static
  and neutral stand-down, contact attrition, power-ratio and
  destruction, capture both ways, resupply catch-up-once, air-loss
  single-booking, byte determinism, write-back + zero-activity
  identity, ground-block shape — plus the session mirror/identity
  pair and the harness ground case). The ground-block comma shape is
  pinned explicitly (the Reader's walk is delimiter-lenient; the
  artifact's consumers are not — the bug class that shipped once,
  found by parsing the artifact, never again).
- **One real bug caught by the rig** (the probe earned its keep):
  the orders pass's `best_score` sentinel was −1, and distant
  objectives score NEGATIVE (score − distance/2) — every candidate
  lost to the sentinel and no army ever marched. The sentinel is now
  a large negative with the doctrine documented in the code: distance
  RANKS candidates, it never vetoes them.

## 8. What comes next (the queue after G1)

- `.cam` re-encoding / save-write (the write-backs are in-memory
  state mutations; the wire re-encoder lands for ALL subsystems at
  once — CAMPAIGN_LOOP_PLAN §7's own framing).
- Real-data import (the full 247-entry UCD paints movement speeds,
  ranges, and class scores; `FALCON4.WST` carries the battery data
  the artillery standoff needs).
- ~~Ground-unit targeting for air weapons~~ — LANDED with G2 (the
  interdiction link: CAS battalion targeting + the bombs booking;
  the guns/missiles-vs-battalion rung remains the queue's).
- Flight-control stabilization (unchanged at the head of the queue).
