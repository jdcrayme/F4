# Fidelity Tiers — Air Aggregation/Deaggregation (Phase FID)

> **Status**: Active plan. **FID-1 (air-bubble plumbing), FID-2 (the
> aggregate flight propagator), FID-3 (the tier scheduler), FID-4 (the
> handoff contract), and FID-6 (the `--accel` certificate) are
> LANDED** — the session runs the war the original game's way: flights
> are campaign aggregates until the camera bubble, an airfield-ops
> window, or an explicit request deaggregates them, and the war
> harness certifies the acceleration (exits 15/16). FID-5 (event-driven
> combat deagg) remains the queue — and the certificate's first
> TestCamp run points straight at it (the war's live aircraft are all
> synthetic Tier-B; see FID-6's as-built notes). As-built notes live in
> the milestone sections; the deviations from the draft are recorded
> there (the aggregate tier rides the world's existing flight
> entities; v1 covers the save's flights — synthetic intents spawn
> straight to Tier-B).
> **Problem it closes**: the session runs every spawned aircraft through the
> full FM + AI + sensors at 60 Hz from spawn to recovery — 449 flights is
> "a headless-budget run, not a UI" (`campaign_session.hpp`), so the
> runner's speed presets (1x/10x/60x/240x) dilate and the live campaign
> cannot sustain even 1x (~0.8x effective). Time compression is a core
> campaign feature upstream; it is impossible here not because the clock
> won't scale but because the *cost* won't. This plan replicates
> FreeFalcon's aggregate/deaggregate mechanism: flights run as cheap
> campaign aggregates until an observer (or a mission phase) needs them
> in sim.
> **Prerequisite**: none — FID-1 started from landed surface (the
> BubbleManager, the AII parser, route legs, the ledger).
> **Companion**: [Campaign Loop Plan](CAMPAIGN_LOOP_PLAN.md) §7 (the
> strategy tranche this unblocks), [AI Implementation Plan](AI_IMPLEMENTATION_PLAN.md).

---

## 1. The problem: one fidelity for everything

Every Flight materializes all its aircraft at session start
(`spawn_aircraft_from_flights`, and `CampaignSimSpawner` on every
`MissionIntent`), and each aircraft ticks the full stack — flight model
(1/60 s major step, 1/360 s minor), the AI module ladder, sensor fusion,
radar, RWR, weapons — until mission recovery. Nothing culls by relevance.

The consequences are already measured, honestly, in the runner:
`time_dilated()` is documented as "the preset outran the CPU", and
`effective_speed()` exists because a 1x preset delivers ~0.8x. The
24-hour war harness (`campaign_qc --war`) works because headless runs
have no wall-clock budget; the interactive session does.

And the obvious lever is already forbidden by design. `Simulation::tick()`
takes a FIXED dt — the FM's minor step is tuned at 1/360 s, and the old
`set_time_scale()` path was removed precisely because it silently moved
that minor step (FLIGHT_CONTROL_STABILITY_PLAN §4.2 RC-2; the constraint
is restated in `simulation.hpp`'s `set_trace_time_scale` comment). Time
compression therefore means *running more ticks per wall second* at fixed
cost per tick — the only remaining lever is **how many entities run the
expensive path**. That is aggregation: the mechanism Falcon 4 was built
on, and the reason the upstream campaign could compress time at all.

## 2. The upstream mechanism

FreeFalcon's UnitClass family carries the whole design:

- `UnitClass::Deaggregate(FalconSessionEntity*)` (camplib/unit.cpp:1612)
  promotes a campaign unit to per-vehicle sim objects when the player
  enters its bubble. `SendDeaggregateData` (:1135) and
  `DeaggregateFromData` (:2126) are the serialized handoff — the
  aggregate's state materializes in the sim from an explicit transfer,
  not an implicit read. (The serialization exists for multiplayer
  ownership; we adopt it as the *contract*, even though no netcode
  exists here yet.)
- The bubble radii live in Falcon4.AII `[Sim]`: `SIM_BUBBLE_SIZE = 2.5`
  grid (air), `GROUND_BUBBLE_SIZE = 1.0` grid (ground); one grid unit =
  1024 ft → **2560 ft air / 1024 ft ground**.
- The economics: aggregated units advance along their campaign routes at
  campaign cadence with coarse state (position, `fuel_burnt`,
  `time_on_target`), and only in-bubble units carry per-vehicle sim
  objects. The player's time compression multiplies campaign time; the
  aggregate path's O(n) cheap updates keep up; only the bubble is
  expensive.
- Upstream's honesty gap, which we will NOT inherit: the aggregate path
  and the FM path diverge silently — no divergence bounds, no policy, no
  test. A ledger-discipline project pins both.

## 3. What already exists (the scaffolding inventory)

| Piece | Where | State |
|---|---|---|
| AII bubble radii parsed (air + ground, grid→ft) | `bubble_radii_from_aii()` (bubble_manager.hpp) | DONE — air radius documented "currently unused" |
| Ground deagg bubble, live per tick, camera-scaled | `BubbleManager::update(player_pos)`, `set_ground_radius_ft` (V-3DLIVE) | DONE — the pattern to mirror for air |
| Cadence-tiered simulation, in-repo proof | `advance_ground_()` — 60 s ground update, 1800 s GTM orders, resupply cadence | DONE — the ground war already runs coarse |
| Route legs with altitudes + TOT slots | `RouteWaypoint`/`RouteBuildResult`, SetWPTimes (C4) | DONE — the aggregate path's input |
| Campaign-level flight state carrier | WorldState flight fields: `flight_altitude`, `fuel_burnt`, `time_on_target`, `mission_over_time` | DONE — the decoder already defines them |
| Per-flight spawn | `spawn_aircraft_for_flight` (campaign_bridge.hpp) | DONE — the deagg materializer; needs a "from current campaign position" entry |
| Duplicate protection per flight_id | `CampaignSimSpawner` | DONE — needs reagg semantics (clear on destroy) |
| Booking of outcomes | result ledger + write-back (C1), `apply_mission_recovery` (C4) | DONE — the reagg roll-up's booking path |
| QC gate framework | `campaign_qc` exits 0–14 in use | FRAMEWORK — `--accel` takes exits 15/16 |
| Observability for divergence | f4-recorder `FlightSnapshot` traces | DONE — the harness input |
| Parked aircraft already cheap | `squadron_aircraft_entities_` tracked separately from `aircraft_entities_` | DONE — parked aircraft are not the cost; the 449 flying FMs are |

## 4. Design

### 4.1 The tier model

Two tiers in v1 (a mid tier is v2, documented in §7):

- **Tier A — AGGREGATE**: the flight is its campaign entity. It advances
  along its route legs at the air-aggregate cadence (60 campaign-seconds
  — the ground update's precedent; `air_agg_cadence_s` option). Legs,
  altitudes, and TOT slots come from the built route; fuel burns per leg
  from cruise data (f4-data aircraft config); position and `fuel_burnt`
  land in the WorldState flight fields the decoder already carries. No
  FM, AI, sensor, or weapon components exist for a Tier-A flight.
- **Tier B — DEAGGREGATED**: today's path, unchanged — per-aircraft
  entities at 60 Hz through the full stack.

A flight is in exactly one tier. The tier lives on the session's flight
table, not on entities — entities only exist for Tier-B flights.

### 4.2 Where each piece lives

- **f4-campaign**: the aggregate propagation engine
  (`advance_flights_aggregate(campaign_seconds)`) — pure campaign
  logic over routes + WorldState. f4-campaign does NOT link f4-entities;
  the boundary enforcement that exists today stands.
- **f4-simulation**: the BubbleManager extended to own air deagg —
  spawn/destroy flights through the bridge, mirroring its ground path —
  plus hysteresis state and the spawner-protection semantics of reagg.
- **f4-simulation session**: the tier scheduler. The 60 Hz pass iterates
  Tier-B entities only; a cadence hook beside `advance_ground_()` walks
  Tier-A flights. `CampaignSessionOptions` grows the fidelity policy
  (§4.7).
- **CampaignSimSpawner**: `MissionIntent` handling becomes "create /
  activate the Tier-A flight", NOT "spawn entities". The duplicate
  protection map becomes the deagg-generation check (cleared on reagg
  destroy, so a flight can deaggregate again).

### 4.3 Deagg triggers and thrash control

- **Observer bubble**: ownship (or camera, in the viewer path — the
  V-3DLIVE precedent) inside `air_radius_ft` — the AII
  `SIM_BUBBLE_SIZE` 2.5 grid, consumed at last.
- **Airfield-ops windows**: a flight within `ops_window_s` of its takeoff
  slot or its recovery deaggregates — takeoff, the pattern, and landing
  are per-aircraft behaviors (the landing/takeoff modules are Tier-B
  code by construction).
- **Explicit `force_deaggregate`** for tests, QC, and hosts (the
  BubbleManager already exposes the shape).
- **Thrash control**: deagg at `air_radius_ft`, reagg at 1.5× (a
  hysteresis band), plus a cooldown timer; mission-phase pinning — a
  flight that deaggregates for airfield ops stays deaggregated until the
  phase completes (push through gear-up-to-cruise; approach through
  shutdown).

### 4.4 The handoff contract (the DeaggregateFromData lesson)

Upstream serialized the handoff because the aggregate's state must
materialize exactly once, explicitly. Our contract:

- **Deagg** — for each vehicle in the flight: spawn via the bridge at
  the aggregate's current grid position/altitude/heading; fuel = the
  campaign fuel remaining, split per aircraft; loadout from the flight's
  decoded `loadout_stations`; the AI route = the remaining legs from the
  current leg's next waypoint. The aggregate then freezes — its fields
  stop advancing; the aircraft own the truth.
- **Reagg** — roll up from the LEAD aircraft: position = lead's snapped
  grid position, altitude = lead MSL, `fuel_burnt` += the burn since
  deagg (measured from the survivors), losses booked through the ledger
  (the C1 books, already live); TOT hits and target damage are already
  ledger state. Formation offsets are discarded — a documented
  divergence (§4.7).
- **Edge cases, each pinned by a test**: lead dies in-sim → roll up from
  the senior survivor; all die → the flight closes as destroyed in the
  ledger and no aggregate resumes; a flight reaggregating mid-package →
  the package's other flights are unaffected (tier is per-flight); deagg
  during an active engagement → §4.5's windows own it.

### 4.5 Combat under tiers — the one new design decision

- **Option A (v1, chosen): event-driven deagg.** The shared air picture
  carries aggregate contacts (§4.6). When a Tier-B fighter commits
  against a Tier-A contact, or two Tier-A flights' predicted tracks
  converge inside an engagement envelope within a lookahead window, BOTH
  flights deaggregate (seeded, deterministic) and the fight runs in-sim.
  One combat model, zero new outcome code; cost is bounded by engagement
  duration, not war duration.
- **Option B (v2, kept open): ledger-level abstract resolution** for
  out-of-bubble engagements — a coarse exchange model in f4-campaign
  over the same weapon/aircraft data. Needed eventually for scale (a
  24-hour war where every merge deaggregates is still O(engagements) of
  60 Hz work) and for the strategy tranche's theater-wide battles. Kept
  open by keeping engagement-start decisions in one place.
- **Rejected**: everything-in-sim (today's behavior) — it is the bug
  being fixed.

### 4.6 The shared air picture vs. aggregates

PERF-1's merged picture reads deaggregated entities. Tier-A flights must
appear as coarse contacts (grid position + velocity + altitude from the
aggregate state) or in-bubble BVR logic is blind to most of the war. The
picture gains an aggregate-contact form (f4-ai's AirPicture stays plain
structs — the same boundary discipline as `set_visual_range_scale()`).
Detection against aggregates applies the same detection policy at coarse
granularity (weather × day-night `visual_scale` already flows through
that seam). Lock/commit against an aggregate contact is the §4.5
trigger.

### 4.7 Determinism and the fidelity policy

- `CampaignSessionOptions::fidelity_policy`: **`full_fidelity`** (today
  — every flight Tier-B from spawn; bit-identical to landed behavior;
  every existing MD5 golden untouched) vs **`tiered`** (the new default
  for interactive sessions; QC gates opt in per gate).
- Tiered mode is itself deterministic: cadence advancement, deagg
  decisions, and combat windows are seeded/derived from fixed inputs —
  no wall-clock, no render coupling. The same contract the weather
  chain took.
- Divergence is documented, not hidden: the FID-2 harness flies the same
  cruise leg (a) FM-driven and (b) aggregate-propagated, and pins the
  position/fuel divergence bounds into the test suite. Upstream had this
  divergence unbounded and undocumented; our certificates don't inherit
  that.

## 5. Milestones

### FID-1 — air-bubble plumbing — LANDED

Landed: `Simulation::air_bubble_radius_ft()` exposes the AII-parsed
`SIM_BUBBLE_SIZE` (2560 ft default) — the dead config is consumed; the
scenario vocabulary gained `campaign_flights_deferred` (the tiered
session's world populates with NO per-flight aircraft — every side
system, the ATC registration included, builds exactly as today);
`CampaignSessionOptions::fidelity_policy` (`FullFidelity` default |
`Tiered`) arms the machinery. The "reagg clears the spawner's duplicate
protection" item dissolved as-built: the session owns a per-flight
deagg registry (`deaggregated_`, erased on reagg) and the spawner never
handled the save's flights (they carry no synthetic intent) — the
protection question was the wrong seam, the registry is the right one.

As-built note: the aggregate tier rides the world's EXISTING flight
entities (they were populated and frozen — the engine now moves them);
no new entity kind. The viewer's Start Session gained the
"fidelity tiers (aggregates until observed)" checkbox — Tiered is the
DEFAULT (the original game's own shape), full-fidelity is one click
away and bit-identical.

Acceptance met: full-fidelity suite green and unchanged (the two
pre-existing, unrelated failures in f4-json's escape test and f4-assets'
kc10 hash pin are the tree's own, present at clean HEAD); new tests:
`test_flight_aggregate` (11) + `test_fidelity_tiers` (7).

### FID-2 — aggregate flight propagation — LANDED

Landed: `f4-campaign`'s `FlightAggregateEngine` + `apply_flights_to`
(the GroundWar twin: snapshots the save's flights over the adapter
sources, advances at the 60 s cadence, writes dirty flights back).
As-built: two route modes — TIME (the save's own arrive/depart
schedule, reproduced exactly) and SPEED (the time-less routes walk at
the ATM's 12 grid/min cruise); fuel is a per-aircraft cruise burn
(70 lbs/min constant — the f4-data cruise table is a later tranche);
ops-window timing queries (`seconds_to_depart` /
`seconds_to_mission_over`) and leg bearing for the spawn pose.

As-built deviation on the acceptance: the divergence harness landed as
the engine test suite's pinned expectations (11 tests: schedule walk,
snap/carry leg math, fold monotonicity, determinism, write-back
identity) — the f4-recorder A/B trace harness (aggregate vs FM on the
same leg) moved to the FID-5 tranche, where the in-sim fights need it.

### FID-3 — the tier scheduler — LANDED

Landed: the session's `advance_flights_()` beside `advance_ground_()`
(same whole-second cadence, own accumulator), `sync_flight_entities_()`
(the mirror: transform + FlightPlanComponent fields, changed values
only, suspended flights skipped), `evaluate_tiers_()` per campaign
second — triggers ops > bubble, reagg rules force-pin > ops-pin >
bubble-hysteresis(×1.5)+cooldown. The sim's 60 Hz pass needs NO gate:
only deaggregated flights have aircraft entities, so the roster IS the
deaggregated set (the parked-aircraft zero-FM behavior was already the
separate-roster shape — now guarded by the full-fidelity session test).

As-built: the air bubble IS the view bubble (the camera's, V-3DLIVE
semantics — `set_view_bubble` now drives air + ground and runs an
immediate tier pass, so a PAUSED session deaggregates what you zoom
into), floored at the AII-parsed air radius. Ownship-driven air bubbles
are a host concern for a future ownship tranche.

### FID-4 — handoff semantics — LANDED

Landed: the bridge's `AirSpawnPose` (an airborne spawn override on
`spawn_aircraft_for_flight`: in-air FM init at the pose, the plan's
`Enroute` start phase — the LNAV scenarios' own contract, the handoff's
fuel) + the session's `deaggregate_flight_` / `reaggregate_flight_` +
the force API (`force_deaggregate_flight` / `force_reaggregate_flight`,
both immediate under the lock — the paused-session rule) + the
`flight_tiers()` UI snapshot. The ground-spawn rule as-built: PRE-TAKEOFF
(ops window) and ARRIVED-HOME flights ground-spawn (the ATC path —
an air spawn at deck altitude is never the right answer); everything
else air-spawns at the aggregate's position/heading/cruise/fuel.
Fold-back: lead aircraft's transform + fuel (capacity − remaining,
monotone into the engine), cursor reset to the next upcoming waypoint;
a dead/absent aircraft folds the flight as DESTROYED (the loss is
already booked at the C1 sink). Each edge case pinned in
`test_fidelity_tiers` (airborne spawn position/fuel/tier, fold
monotonicity, re-deagg after fold, camera-bubble cycle with cooldown,
ops ground spawn).

The viewer half (the user-facing act): the Campaign window's FLIGHTS
table — one row per flight (VU, team, mission, grid, alt, fuel burnt,
tier AGG/LIVE/HOME/LOST, the next ops window), click-to-select + pan,
per-row D (deaggregate) / R (fold) buttons — and the tier summary line
in the war-status block.

### FID-5 — event-driven combat deagg (queued)

The aggregate contacts in the shared air picture + the commit/convergence
triggers + transient deagg windows with phase-pinned reagg, and the
f4-recorder A/B divergence harness the FID-2 acceptance deferred.
Nothing landed.

### FID-6 — the acceleration certificate — LANDED

Landed: `campaign_qc --accel <x>` — the war-harness flow under the
TIERED policy with two new gates on top of the full C5 set (6–14):
**exit 15 DILATION** (a run-0 sample, or the pass's sustained rate,
below `x × (1 − tolerance)`; `--accel-tolerance`, default 0.05) and
**exit 16 DEAGG CEILING** (the deaggregated set breached
`--accel-max-live`, default 32; the end state checks too — the final
partial window is unsampled). Harness-side: `WarHarnessOptions::speed`
(≥ 2 arms the gate; 1.0 stays the ungated war), `dilation_tolerance`,
`max_deagg_aircraft`; the verdicts `zero_dilation` / `deagg_bounded`
with first-violation reports; the diary's FID columns (agg_live,
tier_deaggs/reaggs, sim_rate, dilated — tiered wars only, the
full-fidelity diary keeps its bytes); `--accel-baseline` measures the
same war at FullFidelity (ungated) so one summary carries the
before/after rates. The harness's create() validates the new knobs.

**The roster identity gained the tier term** (a real integration gap
the certificate's first run would have false-fired on): a deagg
materializes ONE aircraft through the bridge + `register_aircraft` —
NOT the spawner's `synthetic_spawned` — and every reagg retires it via
`retire_aircraft`. The C5 leak gate now reads `live == initial +
synthetic_spawned + tier_deaggs − retired` (zero in full-fidelity —
the pre-FID arithmetic unchanged). Pinned by the harness tests.

First TestCamp run (this sandbox host, 1 sim-hour, 2 passes):

- **The tiered war passes every C5 gate**: deterministic (identical
  ledger bytes across passes — §4.7 certified on the real fixture),
  drift ok, leak ok (the tier identity), alive. The ledger MD5 is
  IDENTICAL at 20× and at 60× (`e63edffd9a17f1d090272b78e0d33136`) —
  the measurement provably does not perturb the war.
- **60× fires exit 15 honestly**: sustained 31.7×, min sample 22.6×.
  **20× passes green** (dilated=0, ceiling ok, exit 0). Baseline
  (`--accel-baseline`): FullFidelity 20.7× vs tiered 31.7×.
- **Where the remaining cost lives** (the certificate's job): (a) the
  session's fixed per-tick cost over the ~8,400-entity theater walk
  caps this host at ~48× with ZERO aircraft live — an optimization
  tranche item, deliberately not this phase; (b) the war's live
  aircraft are ALL synthetic (34–48 by the second cycle) — the
  documented v1 gap, and the reason FID-5 is next: synthetic ATM
  intents spawn straight to Tier-B.
- **The ops windows stayed silent on this save** — data, not bug: the
  TestCamp snapshot's nearest future departure (any of 449 flights) is
  11.2 h out and no TOT lands in the hour, so nothing legitimately
  deaggregates (the FID-4 mechanics stay pinned by the crafted-world
  tests). The stale-schedule coverage question (mid-mission flights
  with past departs and unset mission_over) is a FID-5 tranche note.

Acceptance met: the harness's 5 new tests (gates green on a
sustainable preset, dilation fires when the preset outruns the CPU,
the ceiling fires with two pre-takeoff flights, the tiered identity
holds, speed-1.0 stays ungated) + the full-fidelity suite green and
unchanged; the real-war certificate runs (60× fired with numbers,
20× green exit 0). 60× remains the preset the plan NAMES — passing it
here needs a host ~2× faster than this sandbox plus FID-5's synthetic
tiering; the certificate will say so wherever it runs.

## 6. What does NOT change

- The FM's tuned discretization (1/60 major, 1/360 minor) is never
  dt-scaled (FLIGHT_CONTROL §4.2 RC-2 stands).
- The ledger remains the single book of record; in-sim combat remains
  THE combat model in v1.
- `full_fidelity` mode: bit-identical, goldens untouched, available to
  every gate forever.
- The BubbleManager's ground behavior and the FairMutex/threading design
  (C4-FIX-3).
- f4-campaign's no-f4-entities boundary — aggregate propagation is pure
  campaign logic.
- The runner's public surface (presets, budget, mutex) — only its
  *effective* throughput changes.

## 7. Known gaps (deliberate, documented)

- **v1 covers the SAVE's flights** — the mass cost. Synthetic ATM
  intents (no world flight) still spawn straight to Tier-B through the
  spawner; registering them as aggregates is a FID-5 tranche item.
- **No mid tier** (v2): a coarse point-mass tier (~5 Hz) for
  "near-bubble" flights. v1 is two-tier; the accel certificate will say
  whether the gap matters.
- **Out-of-bubble combat resolves via transient deagg** (§4.5A), not
  abstract ledger resolution — Option B is v2. (Until FID-5 lands,
  out-of-bubble flights simply do not fight in-sim; the ledger books
  what the observed war books.)
- **The certificate's measured ceiling has two non-FM parts** (FID-6's
  first run): the session's fixed per-tick cost over the ~8,400-entity
  theater walk (≈48× on the dev sandbox, zero aircraft — an
  optimization tranche, deliberately out of this phase), and the
  synthetic Tier-B mass (FID-5's tranche). Neither is the aggregate
  machinery; both are what the certificate exists to localize.
- **Divergence**: aggregate vs FM cruise paths differ within the
  engine tests' pinned bounds (upstream: unbounded and undocumented);
  the recorder A/B harness rides FID-5.
- **Formation state is discarded at reagg** (lead roll-up); wingman fuel
  states fold into the survivor average (documented).
- **Fuel is a constant per-aircraft cruise burn** (70 lbs/min) — the
  f4-data cruise table per airframe is a later tranche.
- **Multiplayer handoff** (upstream SendDeaggregateData's real purpose)
  is out of scope — no netcode exists; the contract is shaped so a wire
  format can slot in later.
- **Carrier ops** (cat/arrest, deck cycling) is its own future tranche;
  naval surface groups already ride the ground path (TaskForce).
- **Strategy-tranche support racetracks** (CAMPAIGN_LOOP §7) are still
  unbuilt — Tier-A makes them cheap, this plan does not build them.

## 8. Implementation order

1. FID-1 (independent — plumbing + the flag).
2. FID-2 (the propagation engine; needs nothing landed beyond FID-1's
   option plumbing).
3. FID-3 (needs FID-2).
4. FID-4 (needs FID-2; parallel with FID-3).
5. FID-6 (the certificate — landed before FID-5 by host direction;
   it certifies the FID-1..4 mechanism and pointed at what remains).
6. FID-5 (needs FID-3 + FID-4 — the last open milestone, and the
   certificate's own evidence says the biggest remaining lever: the
   war's live aircraft are all synthetic Tier-B).

Sequencing rule for other plans: **the strategy tranche
(CAMPAIGN_LOOP §7) must not start before FID-3 lands** — every added
support flight multiplies the 0.8x problem this plan exists to remove.

---

*References: FreeFalcon `src/campaign/camplib/unit.cpp` (:1135
SendDeaggregateData, :1612 Deaggregate, :2126 DeaggregateFromData);
Falcon4.AII `[Sim]` SIM_BUBBLE_SIZE / GROUND_BUBBLE_SIZE (parsed by
f4-world-convert's AiiConfig, resolved by `bubble_radii_from_aii()`).
In-repo: `bubble_manager.hpp`, `campaign_session.hpp`,
`campaign_session_runner.hpp`, `campaign_bridge.hpp`, `route_builder.hpp`,
`world_state.hpp`, `simulation.hpp`, `tools/campaign_qc.cpp`.*
