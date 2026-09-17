# ATM Strategy Layer (P7) — the strategy tranche, as-built

Status: **LANDED** (P7). Reference: FreeFalcon campaign/camptask/atm.cpp's
strategy layer — the request-filing half the C4 pipeline documented as
"the strategy layer files them" (atm.hpp's header note, C4's own
deliberate-simplification list). C4 built the 7-phase pipeline and ran it
on whatever requests the profile ladder generated; this tranche builds the
FILING side, the support-flight machinery, and the RoE carry.

The tranche is ONE flag: `CampaignConfig::strategy_layer` (default OFF —
the golden identity; every pinned test unchanged). `campaign_qc --strategy`
arms it (implies the ATM pipeline, which the strategy layer requires — the
legacy ladder has no filing side).

## What landed (the four legs)

### 1. The loiter racetrack (RouteBuilder + the NavigationModule's station hold)

- `RouteBuilderConfig::loiter_racetracks` (default false) arms the
  TPROF_LOITER route shape: instead of the pre-strategy single target WP,
  the builder emits a CLOSED RACETRACK CIRCUIT anchored at the target —
  the anchor IS the profile's target WP (same action byte — WP_CAP 12;
  `WP_ORBIT` rides it too: the orbit IS the racetrack, the sim's nav
  treats 12 as a plain station waypoint), plus three corners forming the
  box. Long leg ALONG the inbound course (the box spans the objective
  across the threat axis), corners turn right in the y-down grid frame —
  deterministic, documented. Corners carry WPF_TURNPOINT (critical — the
  eliminator never cuts them).
- The station contract rides the anchor: `RouteWaypoint::station_time_s`
  (the profile's loitertime in seconds) + `loop_waypoints = 4` (the wrap
  span, anchor included).
- f4-ai `NavigationModule` gains the STATION HOLD — the AI plan's
  deferred rung 17 (LoiterMode → "NavState::OnStation"): capturing the
  anchor arms a one-shot timer; the module LOOPS the anchor..corners span
  (it keeps flying the legs — a racetrack is just a closed leg circuit;
  no new fsm state, no synthetic geometry) until the timer expires, then
  sequences out of the span (egress, landing). `holding_station()` /
  `station_elapsed_s()` observe it. Routes without the contract (every
  saved route, every pre-strategy synthetic) behave byte-identically.
- DEVIATION (documented at RouteBuilderConfig): the box's L/W dimensions
  (20 × 8 grid ≈ 11 × 4.5 nm) are the port's shape constants — the
  reference reads its racetrack dimensions from aiinput.dat values our
  sources cannot see.

### 2. CAP-family station targeting (the defensive CAP gets a place to orbit)

- Pre-strategy: the ladder's CAP-family requests (BARCAP/TARCAP/ALERT —
  TPROF_LOITER + WP_CAP) stayed target-less (route-less; the C4
  documented shape).
- Strategy ON: `AirTaskingManager` ranks the team's OWN objectives
  (`own_objectives_` — objtype_priority/2 + the objective's own priority
  scaling, the same arithmetic the target term of request_priority_ uses;
  stable in wire order) and the CAP requests STATION over them, one
  station per request via a per-team rotation cursor (`station_cursor_`)
  — successive CAPs spread across the value list. `stations_targeted`
  counts them.
- With the racetrack routes armed (the campaign's route phase routes the
  loiter family under the strategy arm), a BARCAP is no longer a
  takeoff→land circuit: it flies to a ranked own objective and orbits a
  racetrack for the profile's loitertime.

### 3. FindSupportFlights — the support family, share-or-file

- The reference's FindSupportFlights: packages whose profiles carry the
  ADD flags (ADDAWACS/ADDTANKER/ADDECM — the generated table puts them
  on the CAP family and the deep-strike family) either SHARE an existing
  support flight or FILE one. `file_support_flight_` (compose-time, per
  package):
  - The station is the OWN objective nearest the package target
    (`nearest_own_objective_` — the orbit parks just behind the
    threatened area). No own territory → nothing files.
  - SHARE: a booked or on-cycle flight of the same support byte, same
    team, STATION within `support_share_distance_grid` (30) of this
    request's station, TOT inside the support window (on station from
    its TOT through its loitertime, with a 30-minute early allowance)
    COVERS the package — no new flight (`supports_shared`). The radius
    measures the two ORBITS against each other, not the orbit-to-target
    leg.
  - FILE: `build_support_flight_` gains a station-target override; the
    flight's role is the new `FlightRole::Support` (3) — it carries its
    OWN station route (the campaign's route phase builds it per flight,
    never the package's route), not an escort link. The support
    profile's own ADDESCORT pairs a fighter escort onto the filed
    station (AWACS/JSTAR/TANKER carry it; ECM does not). `supports_filed`
    counts.
- The self-flag guard: AMIS_ECM carries ADDECM on itself — an ECM package
  never files an ECM support for itself.

### 4. RequestEnemyMission — the enemy tasking goes reactive

- A delivery package over an enemy objective (the delivery family with a
  resolved target) files a DEFENDER BARCAP request for the other
  belligerent's NEXT cycle: `file_enemy_barcap_` → the pending queue
  (`pending_enemy_`, per team slot, cap `max_pending_enemy_requests` = 4,
  dedup on mission+target). `enemy_caps_filed` counts.
- The defender's next `generate_requests` consumes the queue ahead of its
  own ladder walk (the defender responds to the threat before its routine
  tasking); a filing whose window slipped takes one 30-minute push. The
  requests carry `enemy_filed = true` (telemetry + QC).
- The ADDBARCAP flag data: the generated table carries it on OCASTRIKE /
  DEEPSTRIKE / STRATBOMB. The deterministic pick files AMIS_BARCAP (the
  reference's ADDBARCAP/ADDSWEEP pair — SWEEP stays the data-driven
  variant; no generated profile carries ADDSWEEP).

## RoE — the wire byte rides to the fire controls

- f4-world `AtmRequestState` gains `action_type` / `context` /
  `roe_check` (the reader stops skipping them; the converter emits
  `roe_check`). All default-zero: sources whose JSON predates the fields
  decode identically, and no committed Data/ world carries teams (the
  manifest fingerprints are untouched).
- The byte flows: backlog seed → `MissionRequest::roe` →
  `FlightTasking::roe` → `MissionIntent::roe` → the spawner's per-flight
  record → `Simulation::apply_flight_roe` (applied post-arm by the
  session's adopt cadence — the arm's configure_brain_combat owns the
  scenario-level holds; the flight's RoE rides on top).
- The vocabulary (the wire roe_check byte, passed verbatim; 0 = the
  unrestricted default on every pre-P7 record): **0 = weapons free** (no
  change), **1 = weapons TIGHT** (BVR missile employment suppressed —
  `bvr_hold` + the BVR fire control held; WVR heaters + guns keep the
  doctrine's own state), **2 = weapons HOLD** (every fire control tight —
  `set_hold_fire` + all three fire controls).
- The exact wire constants of the save's roe_check byte are the save's
  own; 0-must-mean-free keeps every decoded request at the default. The
  full campaign-RoE doctrine (per-team/per-mission RoE editing, the
  threat map's 32000 overfly walls) stays the RoE refinement tranche.

## The campaign's route phase (P7 shape)

Under the strategy arm the ATM path's phase 6 builds:
- delivery-family mains (the C3 shape, unchanged),
- unit-delivery mains (G2, unchanged),
- loiter-station mains (stationed CAPs — NEW),
- Support-role flights, per flight (their own station route — NEW; keyed
  by flight id, never the package's shared route).
Escorts keep sharing the package's route (the C4 shape, unchanged).
`MissionIntent::roe` carries the flight's RoE byte to the spawner.

## QC acceptance (TestCamp, the real save)

`campaign_qc testcamp.world.json --tasking 240 --max-flights 96 --strategy`:
- `strategy: stations=96 supports=85 shared=115 enemy_caps=48` — 96 CAP
  requests stationed, 85 support flights filed, 115 shares (one tanker/
  AWACS covering multiple packages), 48 enemy BARCAPs filed over 8
  cycles; exit 0 (the pre-existing gates 5-8 stay green; the new gate:
  exit 17 fires when a strategy-armed run drew aircraft but stationed
  nothing).
- The strategy run spawns the stationed CAPs and support stations the
  pre-strategy shape skipped (route-less CAPs never spawned) — the
  spawned fleet grows accordingly; hosts bound it with --max-flights (the
  full-data scaling pass is the Tier-3 queue item).
- `campaign_qc_summary.json` carries `tasking.strategy_stations /
  strategy_supports_filed / strategy_supports_shared / strategy_enemy_caps`;
  the campaign summary's atm block carries `stations_targeted /
  supports_filed / supports_shared / enemy_caps_filed` (strategy-armed
  runs only — the pre-strategy block stays byte-identical).

## Tests

- f4-ai `test_navigation_module.cpp` NavigationStationHold: the loop
  wraps until the timer expires then releases (≥2 laps pinned), the
  no-contract route is unchanged, a contract without a loop is inert,
  set_route resets the hold.
- f4-campaign `test_route_builder.cpp`: the armed racetrack shape
  (anchor contract + 3 protected corners + the 20-grid long leg +
  egress/land after), the WP_ORBIT orbit with its own timer, the
  disarmed golden (plain target WP, no contract).
- f4-campaign `test_atm.cpp` AtmStrategy: CAP stations over ranked own
  objectives (and the disarmed target-less golden), the support
  share-or-file pair (DEEPSTRIKE files AWACS + ECM with stations; a
  second package over the same target SHARES), RequestEnemyMission
  (filing + dedup + the defender's next-cycle pickup + the enemy_filed
  flag), the RoE carry (seed → request → flight), the disarmed compose
  golden, the strategy campaign determinism + summary block.
- f4-simulation `test_strategy_layer.cpp`: the station contract rides
  route → plan (and zeros ride zero-contract routes), the post-arm RoE
  gates (HOLD tightens everything; TIGHT holds BVR only — pinned as
  deltas against the doctrine's baseline; FREE changes nothing), the
  strategy-armed session (stationed CAPs spawn with racetrack plans over
  the `kunsan_strategy.world.json` rig — the routed fixture with its
  based squadron owned by a belligerent; identical runs byte-identical)
  and the strategy-off session golden (no stations, no filings, no
  anchors).

## What did NOT land (the queue)

- **GetPriority's PO/package/distance/random terms**: the deterministic
  subset (mission + target terms) remains the score; the PO and package
  terms need strategy-layer data the sources cannot see (the C4
  documented skip stands).
- **The ACTION system's contextual filings**: action_type/context now
  DECODE and ride `AtmRequestState`, but nothing generates contextual
  requests from them yet (the reference's ObjectiveClass/UnitClass
  ACTION tables — the objective-damage-driven CAS/BARCAP/SEAD filing —
  is the next strategy tranche; the reactive BARCAP filing above is its
  RequestEnemyMission slice).
- **Campaign RoE doctrine**: per-team/per-mission RoE editing, RoE-
  driven threat walls (the 32000 overfly denial arming), RoE display in
  the viewer — the RoE refinement tranche.
- **SWEEP station routes** (the TPROF_ATTACK sweep line) and **tanker
  waypoints** (the fuel-planning tranche) stay with their consumers.
- **Full-data scaling**: the strategy-armed war over the FULL TestCamp
  fleet (no --max-flights cap) is the Tier-3 full-data pass's concern
  (the full UCD/PLT_PARK conversion tranche).
