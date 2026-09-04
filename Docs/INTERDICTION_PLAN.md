# Interdiction — Air Power Against the Line (Tranche G2)

> **Status**: LANDED. The campaign loop closed its air side (C1–C6)
> and its ground side (G1) — G2 closes the DIAGONAL: bombs kill
> vehicles, the books balance, the tasking ladder aims CAS at the
> front. CAS packages now draw, route to a front-line-ranked enemy
> battalion, drop a stick of iron, and the LINE THINS — the ledger's
> ground-loss rows carry `air=true`, the engine pulls them, the
> mirrored roster decays. The whole chain is opt-in (`unit_strike`,
> the aa_combat/ground_war contract): every pre-G2 golden re-verified
> byte-identical with the flag off (the C6 6-minute armed war
> `e8496c78…`, the G1 1-hour ground war `8171e8b6…`).
> **Prerequisite**: G1 ([Ground War Plan](GROUND_WAR_PLAN.md)) — the
> engine, the ledger's ground books, the entity mirror — and the
> C1/C3/C4 air chain (the sink, the route builder, the ATM pipeline,
> the strike fire control).
> **Companion**: [Campaign Loop Plan](CAMPAIGN_LOOP_PLAN.md) (the loop
> this closes the air→ground leg of), [Combat Chain Plan](COMBAT_CHAIN_PLAN.md)
> (the weapons themselves).

---

## 1. The problem: two wars that never touch

C6 made the air fight; G1 made the ground fight. They run on the same
clock, write the same ledger, and share one world — and never interact.
The G1 plan said it outright: *air interdiction is what makes the ground
war MEAN anything — the bridges you blow feed an army that is actually
trying to advance.* The loop's shape is
`simulate → attrite → retask`, and the middle word is still missing on
the diagonal: nothing an aircraft does changes what a battalion fights
with.

The machinery is HALF-built, deliberately:

- The booking side EXISTS, wired by G1 and waiting:
  `CampaignResultSink` resolves an origin-less victim killed by an
  origin-ful shooter and books `apply_ground_loss(air=true)`; the
  engine PULLS those events (`pull_air_losses_`, capped at remaining
  strength, applied once); the mirror syncs the thinned roster back to
  the entity.
- The delivery side EXISTS: strike flights carry MK-82/GBU-12 doctrine
  stores, the brain's A-G rung + StrikeModule release a stick when the
  delivery waypoint carries a target, and the ballistic bomb flies to
  its aim point.
- The MISSING MIDDLE: a bomb whose target is a battalion entity
  resolves no `FeatureSetComponent` and detonates harmlessly (a zeroed
  `BombImpactMessage`); and no tasking rung ever AIMS a synthetic
  mission at a unit — the ladder resolves delivery targets through the
  objective map only, and UNIT-class mission profiles (CAS) fly
  target-less, route-less, exactly as C3 documented when it deferred
  them ("UNIT/LOCATION targets wait for their resolution tranches").
  Even TestCamp's own saved CAS/BAI flights — which DO resolve their
  unit mission targets and DO release ordnance at them — have been
  dropping harmless iron on battalions since the A-G tranche.

G2 is the missing middle: bombs kill vehicles, the books balance, and
the tasking ladder aims CAS at the front.

## 2. The chain (one bomb, end to end)

| Link | What it does | Where |
|------|--------------|-------|
| **Tasking** | UNIT-class delivery profiles (the profile table's own `target == "UNIT"` + `TPROF_ATTACK` + `WP_CAS` — exactly AMIS_CAS in the generated table) pick an enemy battalion: front-line ranked (distance to the contested-FLOT column, wire-order ties, rotation cursor), ledger-destroyed skipped | `Campaign::select_unit_target_`, ATM `generate_requests`, shared `rank_battalion_targets` |
| **Route** | The C3 builder resolves the target VU's grid position (objectives first, then UNITS — the world loader's own resolution order) and emits the attack profile: ingress → IP → target WP (the CAS action mapping, `WP_CAS → WP_GNDSTRIKE`) carrying the battalion's VU → turn → egress | `RouteBuilder::build` (unit resolution + action map) |
| **Plan** | The mission-plan builders resolve the delivery waypoint's `target_num` through the objective map, then the UNIT map (VU → battalion EntityId) — the same order; the flight-level fallback (`fp->target`) already resolved units | `build_mission_plan_from_flight` / `from_route` (+ the spawner threading) |
| **Release** | Unchanged: the brain's A-G rung fires the StrikeModule stick at the target's transform; `release_bomb` creates the ballistic entity aimed at the battalion's position | existing (M5/C-A-G) |
| **Blast** | NEW: at terminal, a target that resolves no objective but IS a battalion entity takes unit damage: one blast, `vehicles_killed = floor(power × falloff(miss) / hp-per-vehicle)` capped at the entity's mirrored roster strength; one `GroundUnitLossMessage` per bomb with kills ≥ 1 | `apply_battalion_damage` (f4-weapons) |
| **Books** | The sink (armed by `unit_strike`) resolves the victim's VU/team and the shooter's origin, books `apply_ground_loss(air=true, kills)` + per-vehicle `apply_ag_kill` credit — the exact branch G1 wired and waited for | `CampaignResultSink` |
| **Pull** | Unchanged: the engine's next update pulls the air-sourced loss, decays the roster (highest group first, capped), destruction at zero; the mirror syncs roster/ALIVE to the entity | existing (G1) |

The war's shape after G2: a CAS package draws, launches, ingress-egress
around the threat map, finds the front-line battalion the tasking
picked, drops a stick of iron — and the LINE THINS. The engine's next
orders pass sees a weaker battalion; the front's contact math changes;
the ledger's ground block carries the air-sourced loss rows. The two
wars are one war.

## 3. Design decisions (each documented in the code)

- **The kill model is point-blast, not per-vehicle.** A campaign
  battalion is an aggregate point entity (the reference deaggregates it
  into vehicles inside the bubble — the deagg→parent mapping is the
  viewer tranche's). One blast = one integer kill count from
  `warhead_power × falloff(miss) / kVehicleHitPointsLb`, floored,
  capped at remaining strength. `kVehicleHitPointsLb = 96` (a MK-82's
  192-lb warhead kills 2 vehicles at the fuze point — the reference's
  deagg-level dispersion folded into one documented constant; the real
  VCD hit points land with the real-data import, a one-line change).
  Tempo sanity: a doctrine 4-stick thins ~8 vehicles per pass; contact
  attrition still owns the hours-scale grind.
- **The battalion's life stays the ENGINE's.** The blast computes and
  publishes; it never mutates the entity's roster, ALIVE, or any
  DamageState — the ledger books, the engine pulls, the mirror syncs
  (G1's one-writer discipline, unbroken). A spent battalion's death
  still books through the engine's `mark_destroyed_` transition on
  sync, exactly like a contact-attrition death.
- **The message carries the count** (`GroundUnitLossMessage{target,
  shooter, vehicles_killed, sim_time}`): one rare event per effective
  bomb, the bus convention (not N entity-kill messages for a unit that
  is not dead). The sink's existing `EntityKilledMessage` battalion
  branch stays wired for true entity-level kills (deagg vehicles, a
  future missile/gun-vs-unit rung) — it is not this tranche's path.
- **The booking is opt-in at the SINK** (`unit_strike`, default off —
  the `aa_combat`/`ground_war` contract). Reason: the blast endpoint
  itself cannot know the session's flags (f4-weapons owns ballistics,
  not policy), and TestCamp's saved CAS/BAI flights ALREADY drop
  harmless ordnance on battalions today — with the booking ungated,
  every pre-G2 golden that flew a saved unit-targeted flight would
  change. Flag off: messages publish, nobody books, documents
  byte-identical. Flag on: the ledger fills.
- **Tasking-side gating mirrors it**: `CampaignConfig::unit_strike` +
  `AtmConfig::unit_strike` (default off) gate the UNIT-target rung in
  both ladders — CAS requests stay target-less and route-less with the
  flag off, exactly the C3-documented shape.
- **The front proxy is the FLOT itself**, computed the way the engine
  computes it (per contested column, between the belligerents' forward
  objective holdings): the front-line core is extracted from
  `GroundWar::rebuild_front_` into a shared free function so tasking
  and the engine cannot drift. CAS targets rank by distance to the
  nearest contested column — "close air support" means the battalions
  in contact's reach — wire-order ties, per-team rotation cursor (the
  objective rotation's own spread rule).
- **CAS's target action is the ground-strike action.** The mission
  table's `WP_CAS` is the reference's mission-table string; the wire
  action vocabulary has no distinct CAS byte — the CAS target point is
  a `WP_GNDSTRIKE` (14) delivery waypoint, the action the brain's
  strike rung already arms on. The mapping is data-driven (the
  profile's own `targetwp`), never a byte switch.
- **Strength views can be one ground-update stale** (the entity's
  mirrored roster is the blast's cap; the engine's pull is the truth
  floor). A same-minute over-kill self-heals on the engine's sync —
  `run_losses` stays the monotone event count, `strength` the synced
  live count (G1's own split). Deterministic in either order.

## 4. The QC surface

`campaign_qc --unit-strike` (with `--war`, typically alongside
`--aa-combat --ground-war`): the scenario JSON gains the flag, the
session arms the sink's booking + both ladders' unit-target rungs, and
the report/diary/summary gain the interdiction columns —
`agv` (air-sourced vehicle losses, the ledger's own counter), CAS
packages tasked against units, and the air-share of the ground books.
**Exit 14**: unit strike armed, the war ran, and air never attrited a
unit (the exit-5 philosophy, interdiction edition — CAS needs its TOT
window, so the 1-hour war is the honest acceptance horizon; the 0.1 h
smoke checks identity, not effect).

## 5. What does NOT change

- The IDataSource boundary: f4-weapons learns "an entity with a
  UnitCoreComponent and a roster" (f4-entities vocabulary), not
  campaign concepts; f4-campaign still never sees EntityWorld.
- The A/A books, the C5 drift identities, the harness MD5 machinery:
  one document, more rows. Flag off, byte-identical documents (the
  6-minute armed war, the G1 1-hour ground war, and every pre-G2
  golden re-verified).
- The bomb ODE, the release/impact contract, the objective feature
  damage endpoint: the battalion branch is a sibling of the feature
  branch at the same terminal site, sharing the falloff/damage core.
- The engine: zero ground_war.cpp behavioral change (the front-line
  extraction is a pure refactor — same inputs, same columns, the G1
  goldens re-run to prove it).

## 6. Known gaps (deliberate, documented)

- **No sensor story**: CAS acquires its target from the TASKING (the
  route's target), not from sensors — the air picture classifies
  ground units as clutter (correctly, for the air picture). A CAS
  acquisition/retargeting rung (target-of-opportunity, the reference's
  FAC/on-call loop) lands with the strategy layer that owns it.
- **Guns/missiles do not strafe battalions**: the blast endpoint is
  the bomb's (the delivery weapon CAS actually carries in the doctrine
  fill). AG missiles (Maverick-class) and gun runs against unit
  entities follow when their stores land in the wire map.
- **Salvo dispersion is not modeled**: a stick's bombs share the aim
  point's error envelope (each bomb's own miss is its release
  geometry); the reference's stick-pattern spacing is a WST/aiinput
  tunable this slice folds into the constant.
- **Front-line CAS only**: the ranking is FLOT-distance — no depth
  interdiction (BAI vs second-echelon units needs the strategy
  layer's target priorities), no CAS-vs-engaged-only weighting (the
  engine's engaged set is one ground-update stale to tasking).

## 7. Verification (TestCamp v71, Release build)

- **The goldens re-run (flag off)**: the C6 6-minute armed war
  (`--war 0.1 --aa-combat`) — exit 0, **MD5 `e8496c7819cbb7b64b8f9e0a2fdc7b64`
  byte-identical**; the G1 1-hour ground war (`--war 1 --ground-war`)
  — exit 0, **MD5 `8171e8b6a8dfeb8057f747a06d5b173e` byte-identical**
  (the front-line extraction refactor proven inert: same columns,
  same captures, same march; the new `air=0` column reads zero).
- **The interdiction war** (0.3 h, `--ground-war --unit-strike`, 2
  in-process runs): exit 0, all four C5 verdicts green, identical
  ledger MD5s (`4f3300de…`), **`agv=2`** — the first air-caused
  vehicle losses on the ledger's ground books (two rows,
  `air=true`, killer provenance carried).
- **The 1-hour interdiction war** (same shape): exit 0, MD5
  `f751c748…`, `losses=15 (air=2)` — and the ENGINE PULL proven in
  the books: battalion 4121's ledger record carries
  `strength_initial 11 → strength 9, run_losses 2` (the pull decays
  the roster; the sync lands it; the rest of the ground arc —
  77 captures, front 204→290, 2,520 grid marched — matches G1's,
  the interdiction's two vehicles sitting on top of it).
- **The combined war** (`--aa-combat --ground-war --unit-strike`,
  0.3 h): the three arms live together (4 A/A kills, the reaper
  reaping, 71 captures) and the exit-14 gate fires honestly — the
  CAS package drew and routed but was SHOT DOWN at t=877 s, 23 s
  short of its ~900 s TOT (the ledger's air-loss row:
  `victim_flight 28`). Interdiction is CONTESTED — the gate's own
  message guides the horizon (`--war >= 0.5`, the PERF-3-constrained
  full-length certificate).
- **Exit 14 plumbs** (armed + `agv=0` fires; verified on a 0.05 h
  run where CAS never TOTs).
- **Suites**: 2,226/2,226 Debug + Release (2,220 + 21 new: 8 weapons
  — the roster packing, the point-blast math (falloff, cap,
  destroyed, non-battalion/absent), the E2E bomb-at-battalion (one
  GroundUnitLossMessage, zeroed objective summary, untouched entity
  state) + the objective-path regression; 8 campaign — the pair, the
  front (contested midpoints, centroid sides, degenerate), the
  ranking (hostility, land/Battalion/roster filters, destroyed skip,
  front-distance, wire ties); 3 sink — armed books (loss + per-vehicle
  credit + the air counter), unarmed counts-only (the golden
  identity), stale-target loudness; 1 session — the ATM tasking CAS
  against real battalions + the arm-off identity; 1 spawner — the
  plan builder's unit-map resolution).
- **One real bug caught by the rig**: `objective_found` is true for
  ANY transform-carrying target (the diagnostic "the target exists")
  — the objective branch keyed on it and the unit branch never ran.
  The OBJECTIVE branch now keys on `features_total > 0` (a resolved
  feature set); unit targets and stale ids fall through to the
  unit/stale path, whose published bytes are identical to the
  pre-G2 zeroed summary (the goldens prove it).

## 8. What comes next (the queue after G2)

- `.cam` re-encoding / save-write (unchanged at the head of the
  known-gaps queue — the write-backs are in-memory mutations).
- Real-data import (the full UCD paints the vehicle hit points the
  kill constant approximates; `FALCON4.WST` carries the battery data
  the artillery standoff needs).
- Flight-control stabilization (unchanged).
- The AG-missile/gun rung against battalion entities (the sink's
  `EntityKilledMessage` battalion branch is wired and waiting for a
  producer — G2's aggregate blast rides its own message).
- The strategy layer (mid-run re-targeting, live-front CAS, the
  PO/package GetPriority terms — the ATM's documented deferred
  vocabulary).
