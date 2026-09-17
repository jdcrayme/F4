# Changelog

One line per landed milestone, newest first. The verbose task reports this
replaces live in `Docs/history/changes-archive.md`; the raw session log in
`Docs/history/worklog.md`. Current design docs live in `Docs/` (see
`Docs/README.md` for the index).

## CAMP-BASE-1 — the stock-save airbase bridge (synthesized squadron basing)

- **CAMP-BASE-1** — the stock campaigns shipped with the game
  (save0/1/2.cam + Instant.cam, .ver 63/65) now run the full ATO loop in
  the viewer session. They carry airbase VU 0 for every squadron — the
  link the game's own campaign engine establishes on first load, which
  never ran on them — so the ATM could base nothing: the ladder
  generated hundreds of intents per cycle and every one came out
  route-less, and the spawner (by design) materializes only routed
  synthetic intents; with zero save flights on top, nothing ever flew.
  The bridge: `f4-world`'s `synthesize_squadron_airbases()` assigns each
  unbased squadron the nearest acceptable objective (exact grid first,
  then nearest; owner = own team or ALLIED/FRIENDLY stance — the US
  wing's RK-owned fields; War/Hostile and the -5141 garbage slots never)
  and the session carries it on BOTH legs: the in-memory WorldState the
  adapters/ladder/ledger read, and — via the §6.1 emitter — a patched
  world JSON in the scenario temp dir that the sim re-loads (opt-in
  `CampaignSessionOptions::synthesize_airbases`, byte-identical off;
  the viewer arms it on every session start, campaign_qc via
  `--synthesize-airbases`). Validated on the real stock saves: save0
  94/94 squadrons based (routes 0→179, synthetic spawns 0→4 at the
  4-flight cap, takeoff slots 0→139), save1 72/72 (routes 0→178);
  TestCamp with the bridge off stays byte-identical (413 intents / 152
  routes), with it on the 26 unbased entries join (routes→173). Tests:
  `test_airbase_synthesis` (7 — acceptance/exact-grid/nearest/war-and-
  garbage rejection/unresolvable/flight-untouched) and
  `CampaignSession.SynthesizeAirbasesBasesTheUnbasedWing` (the
  end-to-end: session WS + sim world agree through the patched JSON on
  a fixture whose USA wing has no base within the positional fallback's
  radius).

## CAMP-CMD-2 — retask / abort / priority (the command surface completes)

- **CAMP-CMD-2** — the v1 command set completes behind the CMD-1 wire
  (Docs/CAMP_HOST_PLAN.md §3.3/§8). `flight_retask` replans a flight
  FROM WHERE IT IS: the session rebuilds the route through its own
  RouteBuilder (home airbase → target, threat-aware), splices at the
  new plan's ingress point with the flight's CURRENT position as the
  head, recomputes TOT and the mission-over deadline with the ATM's own
  arithmetic (cruise-speed estimate + loiter + doubled reserve), and
  lands the write on EVERY shape the flight is in — the aggregate row
  (SPEED mode from the retask point; a TIME-mode save flight retasks
  into speed), the stored synthetic intent, a save flight's world
  WaypointPlanComponent (so a later deagg spawn flies the NEW route),
  the live brains (BrainComponent::retask — the one sanctioned
  mid-flight plan swap: Enroute hands the route straight to the
  NavigationModule with reset steering, Approach/Complete refuse), and
  the ATM booking (the recovery clock follows the new plan; the takeoff
  slot survives). `flight_abort` closes the sortie: not-yet-launched →
  the aggregate SCRUBS (a distinct terminal state — tick, tier
  triggers, ops windows, and the air picture all skip it; a parked
  complement folds back and retires) and airborne → the RTB leg home
  ([current position → the route's own landing waypoint]); either way
  the package's books close NOW — the ATM booking releases its
  survivors through the Campaign's scrub (the ledger's
  apply_mission_recovery at the current clock; save-carried flights
  have no booking in this session's ledger — operational abort only),
  the abort record keeps the tier triggers from ever resurrecting the
  sortie, and the flights row reports the additive `aborted` tail.
  `objective_priority` makes the commander's weight (0..100, the save's
  own scale) the FIRST runtime write of the objective priority byte —
  the same field every tasking score reads (the request target term,
  the CAP station ranking, the enemy target rotation, the legacy
  select_target), echoed by the `objectives` query and persisted by the
  contract save(); re-set replaces. Refusals gain `unknown_objective`
  (the wire's fourth typed reason). Gate: the M4/M5-style pinned
  retask (a flight retasked mid-crank closes on the new target — or
  the TOT window deaggregates it and the materialized aircraft carries
  the new plan), the RTB abort with the books closing exactly once,
  the next-cycle scoring moving with the priority write, and the CMD-1
  identity statement extended: a journal carrying retask + priority +
  abort replays into the SAME ledger fingerprint under different step
  chunkings; the tampered-flight-id replay exits 23 with both
  fingerprints named. Engine primitives pinned at their own level
  (FlightAggregateEngine retask/scrub, ATM scrub_flight/
  reschedule_flight). No commands → byte-identical; full ctest green
  (the 3 pre-existing upstream data-drift pins untouched).

## CAMP-CMD-1 — the command journal + RoE doctrine

- **CAMP-CMD-1** — the identity statement's command half lands
  (Docs/CAMP_HOST_PLAN.md §2.3/§3.3/§5/§8): `roe_set` rides the P7
  fire-control path — team/mission/flight scopes join the session's
  doctrine store (one level per scope; an aircraft's effective level is
  the tightest of its carried byte and every matching scope), and the
  write is `Simulation::set_flight_roe`, the FULL recompute from the
  armed doctrine baseline so a command can LOWER as well as tighten
  (the old `apply_flight_roe` was a tighten-only ratchet; the spawn
  cadence and the FID-5 deagg path now serve the store too). Refusals
  are typed data (team 0/mission 0 are non-targets; a flight scope must
  name a roster flight). Every applied command journals —
  `f4-campaign-api/command_journal.hpp` (writer + reader + byte
  goldens; the intent-body encoder gives every intent a canonical wire
  spelling) — and replays tick-exactly: `EngineSessionHost::step`
  segments around the journal's pending apply ticks so the replay's
  step CHUNKING is irrelevant, `campaignd --replay-commands` verifies
  the final identity against the journal's footer (drift/pending → 23,
  wrong war at load → 23, malformed → 24) and composes with
  `--verify-journal` for the full assertion. `roe_changed` publishes
  (the HOST-2-pinned encoder, unchanged bytes) from the command path.
  Gate: the fire-control levels pinned at the gate level (2→1→0
  recompute vs the armed baseline, gun budget untouched) and at the
  OUTCOME level (the combat rig's t=13 kill pair: holds at t=2.5 s
  kill nobody); the record's journal regenerates the record's
  `ledger_fnv` under three different step chunkings; no commands →
  byte-identical (the C5/C6 identity suites stayed green); 30+ new
  ctest cases; full ctest green (2796 passed; the 3 pre-existing
  upstream data-drift pins untouched).

## CAMP-HOST-3 — the viewer becomes a client

- **CAMP-HOST-3** — the world viewer's campaign session refactored onto the
  f4-campaign-api contract (Docs/CAMP_HOST_PLAN.md §8): every Campaign-window
  read is a QUERY (time/stats/flights/tasking/books/threat via a per-advance
  snapshot gated on the runner's step serial — "once per advance, never per
  draw"), every act is a typed COMMAND (camera bubble → `focus`/`clear_focus`,
  the flights table's D/R → `select_deagg`/`select_reagg` with refusals
  surfaced as data, Write Back → the runtime-safe `save()`). The runner left
  the engine (pacing is host-side composition, plan §2.2): `CampaignClientRunner`
  drives `step(ticks)` with the FIFO FairMutex discipline relocated and the
  wall→tick accumulator the engine's advance() used to own — ceiling-clamped,
  fixing the unbounded budget-doubling overflow the real-session advance cost
  always masked. The `threat` query lands (v1.1-additive: viewer_team,
  cell_grid echo, both density bands — 171×171 on the kunsan war) plus the
  additive `route_waypoints`/`flight_role` tail on tasking rows. The TWO
  PLANES rule is now explicit in the code: contract = campaign state; the
  live entity graph stays on the EntityWorld through a named render-plane
  seam (a remote roster gets a `vehicles` query in its own tranche). Gate:
  viewer parity — **981 deleted / 2327 added** (the engine sheds 825 lines:
  campaign_session_runner + its test, relocated and rewritten over the
  contract); 23 new ctest cases — the runner pinned on a MOCK session (no
  engine in the link), the query walks pinned on golden DTO JSON
  (additive-tolerant), the threat dispatch, ThreatView goldens, and four
  engine-backed HOST↔engine parity cases on the kunsan rig; full ctest green
  (2781 passed; the 3 pre-existing upstream data-drift pins untouched); the
  golden identity intact.

## CAMP-HOST-2 — the event stream + journal

- **CAMP-HOST-2** — the engine's typed event stream lands (Docs/CAMP_HOST_PLAN.md
  §3.4/§8): ONE bus message (`f4::campaign::api::CampaignEvent`, the tagged
  envelope of the v1 families pinned in HOST-1) published at the sites that
  move the ledger books — session-side for mission_filed / tasking_cycle /
  reinforcement_delivered / objective_captured / objective_damage (the sink
  collects the damage diffs, the session fills owner + time), sink-side for
  kill (EntityKilledMessage gains a defaulted `cause` literal —
  "missile"/"gun"), the WeatherSystem observer for weather_changed (scenario
  sessions; roe_changed waits for CAMP-CMD-1). The journal: `campaignd
  --journal war.jsonl` records the COMPLETE engine-rate stream as JSONL
  (identity header + event lines + identity footer); `--verify-journal`
  replays a golden byte-for-byte and exits 23 at the first divergence (the
  plan's identity-drift guard; drift outranks the refusal rule at EOF). The
  wire: `subscribe {"kinds":[...],"teams":[...]}` arms the stream (per-family
  team matching; a kill matches either side), step responses carry
  `"events":N` + N event lines, and an un-subscribed client's wire is
  HOST-1-identical (arms by use). Event `t` is the engine's relative seconds
  (the books' axis). Gate: journal replay of the C6 fight reproduces the
  stream AND the ledger fingerprint; empty-journal saves byte-identical; one
  golden line per family. 62 new ctest cases (57 contract + 5 e2e), all
  green.

## CAMP-HOST-1 — the engine contract + campaignd

- **CAMP-HOST-1** — the campaign engine's host contract lands
  (Docs/CAMP_HOST_PLAN.md): `f4-campaign-api` (header-only, f4-json + std
  only — the session iface, v1 query DTOs with byte-stable encoders, the
  typed CommandIntent/CommandAck wire, the v1 event vocabulary pinned
  pre-emission, the line protocol with the exit namespace 20/21/22/24/25),
  the `EngineSessionHost` adapter in f4-simulation (queries serve the
  engine's own views verbatim; the FID family forwards; the CAMP-CMD queue
  refuses with NotImplemented + the tranche named), `campaignd` (the stdio
  JSON reference host), 58 ctest cases green, the golden identity intact.

## P7 — the ATM strategy layer (support flights, racetracks, enemy CAP, RoE)

- **P7** — the C4 pipeline's named queue item "the strategy layer files
  them" LANDED (Docs/ATM_STRATEGY_PLAN.md is the as-built reference).
  ONE FLAG (`CampaignConfig::strategy_layer`, default OFF — the golden
  identity; `campaign_qc --strategy` arms it). FOUR LEGS. (1) The loiter
  racetrack: `RouteBuilder`'s TPROF_LOITER routes emit a closed 4-WP
  circuit anchored at the target (the anchor carries the station
  contract — `station_time_s` = the profile's loitertime,
  `loop_waypoints` = 4; corners WPF_TURNPOINT-protected), and f4-ai's
  `NavigationModule` gains the STATION HOLD — the AI plan's deferred
  rung 17 (LoiterMode/OnStation): the anchor capture arms a one-shot
  timer and the module loops the circuit until it expires, then flies
  on (no new fsm state; routes without the contract behave
  byte-identically). (2) CAP-family station targeting: the ladder's
  BARCAP/TARCAP/ALERT requests station over ranked OWN objectives
  (objtype_priority/2 + priority scaling, wire-order ties, rotation
  cursor) instead of staying target-less. (3) FindSupportFlights:
  ADDAWACS/ADDTANKER/ADDECM packages share-or-file the support family —
  a station is the own objective nearest the package target; an
  existing same-byte flight whose station (and TOT window) covers the
  request SHARES (one tanker feeds a whole raid), else a
  `FlightRole::Support` flight files with its OWN racetrack station
  route and the support profile's ADDESCORT fighter escort (the new
  FlightRole::Support = 3; `supports_filed`/`supports_shared` count).
  (4) RequestEnemyMission: a delivery package's ADDBARCAP files a
  defender BARCAP over the threatened objective for the enemy's NEXT
  cycle (pending queue, dedup, cap 4; `enemy_caps_filed` counts).
  PLUS the RoE carry: `AtmRequestState` gains
  action_type/context/roe_check (the reader stops skipping, the
  converter emits roe_check), the byte rides seed → request → flight →
  intent, and `Simulation::apply_flight_roe` gates the armed brain's
  fire controls post-arm (1 = weapons TIGHT: BVR suppressed; 2 = weapons
  HOLD: everything; 0 = free, the pre-P7 default). QC acceptance on
  TestCamp (`--tasking 240 --max-flights 96 --strategy`): stations=96
  supports=85 shared=115 enemy_caps=48, exit 0 (the strategy gate, exit
  17, guards a strategy run that stations nothing). 22 new tests; suite
  2,639/2,639.
- **P7 queue**: the ACTION tables' contextual filings (the
  objective-damage-driven CAS/BARCAP/SEAD requests — the reactive BARCAP
  above is its RequestEnemyMission slice), GetPriority's PO/package
  terms, the campaign RoE doctrine (per-team/per-mission editing, the
  threat map's 32000 overfly walls), SWEEP station lines, tanker
  waypoints, and the full-data scaling pass (the strategy war over the
  uncapped TestCamp fleet).

## P6 — IR/visual sensors + countermeasures (the seduction tranche)

- **P6** — the AI plan's named queue item "IR/visual sensor models +
  countermeasures" LANDED (Docs/SENSORS_COUNTERMEASURES_PLAN.md is the
  as-built reference). THREE LEGS. (1) The passive sensors: f4-sensors
  gains `IrstComponent` (the SENSDATA/IRST airframe card — gimbal
  gates, ground factor, the sqrt-of-intensity range law, the radar's
  0.75-knee Pd ramp, seeded rolls) and `VisualComponent` (THE ORIGINAL's
  documented signal law: gain × VIS-grid factor / range² ≥ 1 —
  deterministic, no RNG), both over a shared `PassiveTrackStore`
  contact book; `SignatureComponent` carries the full five-grid
  signature record (`sig_data` + `IrPowerMode` band selection, 1.0
  data-free — the golden identity at the component level). (2) The
  countermeasures: f4-weapons gains `CountermeasureComponent` (chaff
  30/flare 15, salvo 2/1, 0.5 s pacing — the MissileModule defeat
  intents' long-documented "no consumption model exists" gap closed),
  decoy ENTITIES (the FreeFalcon VuEntity shape: chaff bloom 25 m² /
  flare, TEAM-tagged, priority 42 — after radar, before missiles),
  and `make_decoy_aware_seeker_source` — the documented
  `seeker_source` hook's first production user: one honest roll per
  decoy inside the seeker cone (IR missiles roll their OWN card's
  `flare_chance` — aim9p 0.4, sa7 0.5 — via
  `find_ir_seeker_flare_chance` over the shipped irstdata.json; radar
  missiles roll chaff at 0.5), sticky through burnout, one-honest-
  chance, IFF-gated. The seduction exposed a latent terminal-math
  proxy bug (min_range_ measured the SEEKER's picture, so a flare hit
  read as a direct kill): with the decoy-aware seeker the miss
  distance is measured against the ASSIGNED target — every legacy path
  byte-identical. (3) The host: `combat.ir_seeker_data_path` +
  `combat.countermeasures` scenario keys, deploy intents in the combat
  pass, seduction seekers on every AI-guided release, the decoy ttl
  sweep. THE GATE IS THE STORY: the first cut armed dispensers
  unconditionally and six pinned fights failed (the merge-fight AIM-9
  flew against flares — correct fidelity, WRONG landing discipline);
  with `countermeasures` defaulting FALSE every pre-tranche fight is
  byte-identical and the pinned harnesses pass unchanged. Suite
  2,617/2,617 (36 new tests: the sensor models, the deploy/sweep/
  seduction units, the seduced-missile-survives integration, and the
  AI-vs-AI E2E — the AI fires on its own, the victim's RWR lights, its
  brain beams, the dispenser releases under fire). Queue: SensorFusion
  fusion of the passive legs, ECM/jamming, throttle-driven IR bands,
  VCD countermeasure counts.

## P5 — tree hygiene, save-write verification, AAR closure
- **P5** — the two pre-existing tree failures are FIXED, not apologized
  for: (1) `JsonReader.RegisteredEscapesStillDecode` — f4-json's
  `Reader` held a `const std::string&` to its source, so
  `Reader r("literal")` was silent dangling-reference UB (the
  temporary died at the end of the declaration statement; the parse
  read freed SSO stack and the test's document came back with a
  garbage first byte). `Reader` now OWNS its source (`std::string` +
  `std::string_view` ctor) — one copy per constructed Reader; the
  literal/temporary construction form is safe for every caller.
  (2) `Sha256.ReproducesCommittedManifestFingerprints` — the committed
  manifest had drifted (kc10/terrain/world fingerprints stale; 5
  runtime-generated `Temp/` entries + the never-committed
  `Weapons/falcon4.wcd.json` declared but absent — the weather patch
  had reverted the prior session's repair). Manifest surgically
  repaired (36 entries, every file present and matching);
  `generate_manifest.py` now hard-excludes `Temp/` and the hash test
  skips `Temp/` entries loudly instead of failing a fresh clone.
  En route: `campaign_qc`'s out_dir default hardened (bare relative
  world filename → empty parent_path → `create_directories("")`
  threw). `--save-write` verified end to end on TestCamp: decode →
  run → fight → apply → save → decode (campaign_after.cam 327,883
  bytes, round-trips with objectives/units/teams intact). AAR
  REDESIGN CLOSED: the ClearedContact closure-bias targeted the
  PRECONTACT station (bias → 0 at 50 ft astern) while the latch gate
  sits at ±15 ft — the receiver crept the last 35 ft at
  integrator-noise speed (120 s stuck at −52 ft on the F4_AAR_TRACE
  CSV) and expired 12 s into Hold; the bias now targets the
  receptacle (USAF: cleared contact closes to the boom) and the full
  procedure completes in 319.6 s — Done, 5,000 lbs transferred — with
  `test_aar_e2e` pinning Departing/Done/fuel instead of tolerating
  their absence. Docs as-built: ARCHITECTURE PROPOSAL refreshed to
  as-built (the §3 inventory = 30 targets with real CMake edges),
  AAR_REDESIGN_PLAN + DIGI_AI_PHASE2_PLAN archived with
  supersession banners, AI_IMPLEMENTATION_PLAN status banner as-built,
  SAVE_WRITE_PLAN + the docs index updated. Suite green (see the
  patch's ctest record), the two tree failures gone for the first
  time since Task 56.

## Fidelity tiers (most recent)

- **FID-OPT-3** — the sensor-sweep budget: the radar scan walks
  pointers, not maps + the RWR sweep's licensed cadence
  (Docs/FID_OPT_PLAN.md §4): the optimization tranche's third item.
  MEASURED FIRST (the temporary `F4_COMP_PROF` per-component profiler,
  removed before landing), and the budget re-attributed AGAIN, harder
  than §3: the ~228 s of post-OPT-2 "component work" is 62% RADAR SCAN
  (161.9 s of the instrumented 3-h/60× armed war) — each radar's
  once-per-second sweep resolves ~7,400 candidates through an
  EntityHandle + a component-map lookup (~1,259 µs/scan) to reject
  99.8% of them with two arithmetic checks that need only the
  transform pointer, and finds ~2 detections. LANDED, two levers:
  (1) `EntityWorld::with_component_ref<T>()` — the component-type
  index's pointer-carrying sibling (same bucket, same invariants,
  same entity-index order; dropped on world move; the replacing-add
  pointer refresh found-and-fixed en route) + the scan's Search walk
  applies the clutter/range pre-gates INLINE so only survivors build
  handles — byte-identical output (the pre-gates draw no RNG; the
  candidate set, its order, and the roll stream are unchanged; pinned
  by the detection-timeline-invariant-to-clutter-population test);
  per-scan 1,259 → 130 µs (9.7×), the radar term 161.9 → 20.8 s
  (7.8×); (2) the RWR sweep rides the licensed ≤100 ms cadence
  host-side (`kRwrCadenceTicks = 6`, the same bound the combat
  refresh and the picture walk carry; the sweep itself unchanged) —
  8.6 → 1.9 s. The deep-horizon 3-h/60× armed certificate: sustained
  61.07× → **137.1×** (the 57 gate now clears 2.4× over), min sample
  15.14× → 30.2×, dilated samples 60 → 30; the 20× baseline
  unchanged (54.6× → 54.1×); the 1-h armed ledger byte-identical
  pre/post across presets; the 2-h armed ledger returned to the
  ORIGINAL pre-OPT-1 value (`641174c7…`) — the RWR cadence's own
  ≤100 ms shift re-aligned the marginal event the fusion cadence had
  displaced. Full suite 2,574 green (2,565 + 9: six ref-bucket, two
  radar, one RWR-cadence test), the two pre-existing tree failures
  unchanged. The residual is measured and named (plan §5): the
  flight-model floor (physics), the brain's diffuse glue, the
  post-OPT-3 radar term, the picture walk.

- **FID-OPT-2** — the concurrent-fight budget: the fusion-refresh
  tiering + the shared picture's own cadence (Docs/FID_OPT_PLAN.md
  §3): the optimization tranche's second item. MEASURED FIRST, and
  the measurement re-attributed the plan's own budget: the deep-
  horizon cost is NOT the per-brain fusion rebuild the §3 arithmetic
  had closed on (~3–6 µs each) but the SHARED AIR-PICTURE WALK it sat
  next to invisibly (~1.4–1.7 ms per walk, 20–40× the fusion term —
  `push_air_picture_` runs outside the `update_all` window the
  FID-OPT-1 sub-profile had split, so the walk never showed). The
  mechanism: the legacy GCI rule sees every missile in the theater,
  so the beam-fight rule ("a visible hostile missile refreshes every
  tick") pinned EVERY combat brain at 60 Hz for as long as any red
  missile was airborne anywhere, and any single brain forced the walk
  every fight tick. LANDED, one bound (≤100 ms staleness — the
  design's own latency license), two cadences: (1) the fusion refresh
  is TIERED BY THREAT — imminent (inside the fusion's own 50 NM RWR
  band) keeps the every-tick beam-fight refresh, a distant theater
  missile rides the new 6-tick (10 Hz) combat cadence, quiet brains
  keep the skill timer; `will_rebuild_this_tick` mirrors all three
  exactly; (2) the shared picture walk is ALSO cadence-gated — at
  most one walk per 6 ticks while any brain demands it, the LAST
  snapshot handed out between walks. FOUND AND FIXED EN ROUTE: the
  push-null invariant (the first cut handed `nullptr` on
  demanding-but-not-walking ticks, dropping every rebuilding brain
  onto its ~1 ms world-query path — rebuilds measured at 977 µs
  before the fix) and the walk-gate off-by-one (walks 7 ticks apart →
  exactly 6). Measured on real TestCamp: walks 95,862 → 21,642 (4.4×
  fewer) and walk time 131.6 s → 36.2 s across 6 sim-hours of armed
  war with the missile-laden brain-seconds IDENTICAL (671,744 →
  672,770 — the war's shape preserved); the 60× deep-horizon armed
  certificate's sustained rate 52.36× → **61.07× (the 60× sustained
  gate now clears)** and its worst sample 7.92× → 15.14×; the 20× 1-h
  cert 1324× GREEN (baseline 54.6×, unchanged within noise), the 60×
  1-h cert **1707×** GREEN, the 2-h armed cert 410× GREEN zero
  dilation, the 4-h armed soak crash-free through 187+ deaggs / 48
  A/A kills / the hour-4 tasking wave. The 2-h armed ledger MD5
  changed (`73a06efd…`) — the first OPT patch that does: the throttle
  shifts detection timing within the licensed bound; re-pinned as the
  golden. The residual deep-horizon dilation (60 dilated samples,
  worst 15.1×) is now NAMED: ~228 s of active-component work (radar
  sim scans, steering, FMs, RWR over the materialized set) — the
  FID-OPT-3 budget, measured, not designed. Tests: 5 fusion + 1
  integration; full suite 2,565 green (the two pre-existing tree
  failures unchanged).

- **FID-OPT-1** — the active-cache walk + the ScopedSubscriptions fix
  (Docs/FID_OPT_PLAN.md): the optimization tranche's first item, driven
  by the FID-6 certificate's own finding ("the session's fixed per-tick
  cost over the ~8,400-entity theater walk"). Landed: the DORMANT FLAG
  on `BehavioralComponentBase` (per-component, routed through the
  owning world so a transition between ticks is picked up without a
  manual invalidate) + the ACTIVE behavioral cache (one rebuild sweep
  fills both lists; `update_all` walks the non-dormant subset — the
  ~8,126-component walk was ~99% of tick time, of which ~8,000 were the
  parked squadron inventory's documented no-op updates paying two
  virtual priority() dispatches each per tick). BrainComponent and
  FlightModelComponent delegate their dormancy to the base; their
  in-update early-returns stay as defense in depth. Measured on real
  TestCamp: update_all ~317 µs → ~0.1 µs/tick with zero aircraft; the
  1 sim-hour armed war's tick work 80.4 s → 0.12 s; the 20× tiered
  certificate sustained 1472× (was 58.1×); **the 60× preset — the plan's
  named target since FID-6 — now passes GREEN at 1331×**; the
  FullFidelity baseline itself lifted 25.3× → 55.3× (the same parked
  mass was taxing it). The 2-h armed war's ledger MD5 is IDENTICAL to
  the pre-OPT run — the war's behavior is byte-identical, only the
  host got faster. FOUND AND FIXED EN ROUTE: the per-entity AI modules'
  bus subscriptions (Takeoff ×2 / Landing ×2 / Refuel ×8) were never
  unsubscribed — a destroyed aircraft's handlers stayed in the bus and
  the next live brain's TaxiRequest publish invoked handlers whose
  captured `this` was freed memory (ASAN: heap-use-after-free on the
  4-hour armed war at ~3 sim-hours; latent since the modules landed —
  pre-OPT the deep-horizon war dilated so hard almost nothing
  materialized). Fix: `ScopedSubscriptions` (f4-messaging RAII bundle;
  bind at initialize, unsubscribe on destruction) adopted by all three
  modules; `Simulation`'s member order swapped so the bus outlives the
  world at teardown. Six new f4-entities tests (dormant skip, the
  unpark transition, active order, idempotence, the campaign
  spawn/unpark pattern, the passive edge); the full suite green, the
  two pre-existing tree failures unchanged. FID-OPT-2 (the
  fusion-rebuild budget — the per-materialized-aircraft cost the
  walk's removal exposed: ~52 µs/tick at 3 aircraft, ~700 at 21) is
  measured and designed in the plan §3, deliberately not in this patch.

- **FID-5** — event-driven combat deagg (Docs/FIDELITY_TIERS_PLAN.md §4.5–4.6):
  the last open milestone of the phase, and the certificate's own lever.
  Landed: the AGGREGATE AIR PICTURE (f4-ai `AggregateContact` +
  `Simulation::set_air_picture_aggregates` — the session publishes the
  engine's airborne aggregates as coarse contacts with team strings and
  cruise velocity; the campaign-flight entities are excluded from the
  world walk so the feed is the single publisher), the COMMIT TRIGGER
  (a Tier-B fighter's engagement id matched against the published set
  deaggregates the contact's flight; the radar-backed policy's coarse
  aggregate rule makes them detectable; the launch veto eats releases
  aimed at the phantom id — no missile ever flies at a non-entity), the
  CONVERGENCE TRIGGER (two opposing aggregates whose predicted tracks
  land inside the 30-kft envelope at the 120-s lookahead deaggregate
  both — wire order, deterministic), the TRANSIENT COMBAT WINDOWS (a
  Combat deagg pins for `combat_window_sec`, then the standard reagg
  rules fold it), and SYNTHETIC INTENTS AS AGGREGATES (the session's
  MissionIntent subscription + the engine's `register_synthetic` +
  the spawner's deferral arm — the generated war rides the tier
  machinery instead of spawning straight to Tier-B; the intent spawn
  path gained the flight path's AirSpawnPose airborne override; the ops
  trigger gained the TOT arm so deliveries still happen). Plus the A/B
  divergence harness the FID-2 acceptance deferred
  (`test_aggregate_fm_divergence` — the FM led the measured leg by
  2.03×, the ratio and the fuel gap pinned). The certificate's second
  run on real TestCamp: 20× tiered GREEN at 58.1× sustained vs the
  25.3× FullFidelity baseline (~2.3× the pre-FID-5 tiered war); the
  2-h armed war ran 59.5× with the tier machinery cycling the generated
  missions (16 deaggs / 4 reaggs, identity green). 7 new session tests
  + the A/B harness; the full suite green and unchanged.

- **FID-VIEW-1** — the campaign view shows the war (Docs/FIDELITY_TIERS_PLAN.md):
  the viewer's Tiered default ran the war but drew none of it — the canvas
  live layer rendered only materialized aircraft, so every aggregate flight
  was invisible and a fresh session read as dead. Landed: the aggregate air
  picture (a pass over `flight_tiers()` — AGG translucent, HOME dimmed, LOST
  a gray cross, LIVE skipped for the materialized aircraft, team filter +
  cull + click-pick with the flights-table selection ring), the tasking
  countdown (`Campaign::seconds_to_next_cycle` → `Stats::next_tasking_sec` →
  a "next tasking cycle in MM:SS" war-status line — the ladder's first
  generated missions land a full 1800-s cycle in), and the viewer
  `--smoke-seconds <n>` long-window smoke (the 6/12 s default can never
  cross the cycle). Engines untouched; one new session stats test.
- **FID-6** — the acceleration certificate (Docs/FIDELITY_TIERS_PLAN.md):
  `campaign_qc --accel <x>` runs the war harness under the TIERED policy
  at an interactive preset and gates exit 15 (DILATION — a sample or the
  sustained pass below x×(1−tolerance)) and exit 16 (DEAGG CEILING — the
  deaggregated set over `--accel-max-live`) on top of the C5 set (6–14);
  `--accel-baseline` measures the same war at FullFidelity for the
  before/after. The C5 roster identity gained the tier term
  (`+ tier_deaggs` — the deagg spawn path bypasses the spawner's
  synthetic counter), the diary gained the FID columns (agg_live,
  tier counters, sim_rate, dilated), and the harness validates the new
  knobs. First TestCamp run: tiered war passes every C5 gate
  (deterministic — identical ledger MD5 at 20× and 60×; drift/leak/alive
  ok); 20× green exit 0; 60× honestly fires exit 15 (sustained 31.7×:
  the ~8.4k-entity theater walk caps the host at ~48× empty, the
  synthetic Tier-B mass drops it to ~25×; FullFidelity baseline 20.7×).
  5 new harness tests; the fidelity-tiers test rig's temp-dir race
  (ctest -jN) fixed.
- **FID-1..4** — air agg/deagg (Docs/FIDELITY_TIERS_PLAN.md): the tiered
  session runs the war the game's way — flights are campaign aggregates
  (`FlightAggregateEngine`: the save's own arrive/depart schedule or the
  cruise walk, per-aircraft fuel burn) until the camera bubble, an
  airfield-ops window, or a click deaggregates them (`AirSpawnPose`
  airborne handoff; lead roll-up fold-back; a lost aircraft folds the
  flight destroyed). Viewer: fidelity-tiers checkbox (Tiered by default),
  the flights table (click-to-select + D/R per row), the tier summary
  line. Full fidelity untouched and bit-identical; 18 new tests
  (`test_flight_aggregate` 11, `test_fidelity_tiers` 7).

## Environment & data (most recent)

- **73** — Weather v1 + day/night model: 3-state condition Markov chain (seeded, deterministic), solar twilight bands, visual detection scales with weather×daylight. Suite 2,513.
- **72** — Complete AuxAeroData record (all 443 fields) across the fleet.
- **71** — TowerATC (AI Tier 3): sequencing tower behind the stub interface.
- **70** — `.cam` re-encoder reaches byte-identity (write side).

## Flight control

- **63–66** — Pole-based diagnosis program: `diag_poles` (trim → Jacobian → eigenvalues), unstable aperiodic speed mode measured at 18 trims; mechanism = the G-hold law, refuting the back-side-of-drag-curve and alpha-bias hypotheses. P4.1 inner-loop correction (`kp05 = 1/K_nz`, real integrator) shrinks the mode 26×; STAB-P1 `alt_integral_gain` 1.2→0.6; FCS pitch speed damper implemented, **refuted by measurement**, kept default-off as negative evidence. `test_poles_envelope` CI gates (5) pin goldens.
- **69** — Three pre-existing E2E failures closed: shipped `korea.world.json` was invalid JSON (8,016 dangling keys), landing STAB-E47/E48/E49/E57–E62. Suite 2,435.
- **STAB-E series (~55 fixes)** — instrumented, trace-verified flight-control fixes; full `digi_full_mission` passes end to end.
- **DIGI-1/2, ALT-2…5** — airspeed-rotated gamma-hold law; NED→ENU quaternion fix; altitude-loop tranches.

## Campaign loop

- **C1–C6** — War loop closes: result ledger + write-back (C1), one pool for tasking/losses/resupply (C2), threat map + A* routes (C3), campaign thread + 7-phase ATM tasking + full 3D coverage (C4), 24-hour war acceptance harness + starved-worker fix (C5), A/A combat live (C6).
- **V-CAMP** — Live campaign session in the world viewer (time controls, flying flights, route inspection); runtime fixtures become build outputs.
- **G1/G2** — Ground war (battalion maneuver, front line, books) and the interdiction link (CAS against real battalions).
- **PERF-1** — Shared air picture: merge-phase collapse, closed output-identically at 3–4×.
- **62** — WorldState→JSON emitter; the closed save loop (§6.1).

## Combat chain

- **M1–M5** — f4-weapons core (M1), f4-sensors (M2), AI tactics: BVR→WVR merge, guns, 2-ship wingman (M3), BVR intercept acceptance harness + combat events in the recorder (M4), A/G employment + WVR/guns merge harness (M5, incl. Task 68).

## Data & no-binary runtime

- **57–60** — AAR redesign reconciliation; `@asset:` resolver with manifest hash verification (Tranche 0e.3); runtime glTF rewire, `f4-models` cut from `f4-simulation` (Tranche 0d renderer+simulation halves); viewer on-demand conversions into `Data/Temp`; CI repair (LZSS use-after-free).
- **Tranche 0a–0e** — JSON subset of `Data/` committed (runs anywhere without an F4 install); CMake boundary enforcement; TEX→PNG + glTF materials; `f4import` model/texture exporters.
- **SIMDATA waves 1–2** — maneuver table, brain archetypes, formations; class table, sensors, signatures fly as data.

## Platform & hygiene

- **SYMBOL-SVG-1** — `f4-xml` (vendored pugixml) + SVG symbol authoring (import/export, color roles, holes, earcut fills).
- **QC-PASS-1** — Viewer QC sweep: route clutter gating, honest speed feedback, 3D for selections, mechanical cleanup.
- **TERRAIN-TEX-1/2, VIEWER-V71-1, GLV3D series** — textured terrain, campaign-save v71 decode, 3D viewer pipeline.
- **ECS Phases 0–6** — stabilize; type safety; `Cursor::check_and_throw()`; dedup; angle strong-type migration (Phase 4); deferred-item resolution; pre-AI hardening.
- **STEP-0** — Green suite + CI + repo hygiene.

---
*Going forward: one line per landed task, appended at the top. Narration
belongs in the as-built doc for the subsystem, not here.*
