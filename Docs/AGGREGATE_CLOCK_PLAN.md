# The Aggregate Clock — Moving the Campaign Layer to the Event-Driven Design (AGG-CLK)

> **Status**: Active plan. **AGG-0 LANDED** (the FID-DEF-GOV patch: Tiered is
> the session default fidelity policy; the viewer runner's dilation is an
> AIMD delivery governor). **AGG-1 LANDED** (the catch-up clock: the
> campaign pass left the tick stream — one pass per advance() call
> carrying the drained whole seconds as ONE engine delta).
> **AGG-2a LANDED** (the transition-triggered publishers: the damage
> sync diffs only the objectives a transition marked — O(changes) — and
> the verdict emit computes only after the capture tail moved; the
> deterministic due-queue + stagger primitive is in
> `f4-campaign/due_queue.hpp`). **AGG-2b LANDED** (the air-picture
> roster: f4-entities' `SpatialIndex` wired as the radar/detection
> membership term — the FID_OPT §5 20.8 s residual — plus the per-unit
> detection cadences: the radar scans and the picture walk share one
> roster, and the armed war's radars stagger their sweep phases behind
> the `stagger_sensor_phases` gate). AGG-3..5 are the open roadmap. This
> document is
> the canonical record of the 2026-10 time-compression investigation: why
> campaigns were CPU-limited below ~20× while the FreeFalcon reference has
> no problem at 64×, and the migration path from the fixed-dt whole-war
> driver to the reference's event-driven aggregate economics.

---

## 1. The diagnosis, in full

**The symptom.** Campaign sessions on local hardware could not sustain time
compression above ~20×; the FreeFalcon reference runs 64× without strain.

**The measured ceiling.** Under the fixed-dt contract, the maximum
sustainable compression is literally:

```
max_compression = max_sustainable_ticks_per_wall_second ÷ (1 / sim_dt)
```

With `sim_dt = 1/60 s` (`campaign_session.hpp`), every +1× of the speed
preset demands 60 more full ticks per wall second. The measured numbers:

| Configuration | Sustained | Source |
|---|---|---|
| FullFidelity, armed war (FID-6 baseline) | 25.3× | FIDELITY_TIERS_PLAN §certificate |
| Tiered, same war (FID-6 certificate) | 58.1× | FIDELITY_TIERS_PLAN |
| Empty theater, pre-FID-OPT walk | ~48× | FID_OPT_PLAN §1 (311–327 µs/tick flat) |
| ~96 live aircraft, war-diary throughput | ~5.5–9× | CAMPAIGN_LOOP_PLAN (330–540 ticks/s) |

The user's "<20×" on local hardware sits exactly on this curve.

**Three cost multipliers, ranked.**

1. **FullFidelity default (now removed — AGG-0).** Every spawned campaign
   aircraft ran the full FM (six minor-step EOM integrations, ~3.7 µs per
   aircraft-tick) + brain glue (~2.9 µs/tick) + sensors at 60 Hz from spawn
   to recovery — including the ~4,000 parked airframes' potential flights.
   The reference runs the flight model ONLY inside the player bubble.
2. **The constant per-tick theater-walk tax.** Pre-FID-OPT, even ZERO live
   aircraft paid ~16,252 virtual dispatches/tick (8,446 entities × 2
   passes × `priority()`) ≈ 311–327 µs/tick → a ~48× ceiling with an empty
   war. FID-OPT-1 (active cache), FID-OPT-2 (fusion/air-picture cadences),
   FID-OPT-3 (sensor sweeps) and CAMP-OPT-1 (p7 spinner) attacked this;
   the residual is named in FID_OPT_PLAN §5 (FM floor, brain glue, the
   SpatialIndex radar term — wired by AGG-2b).
3. **One-clock coupling (the structural remainder — AGG-1..4).** The
   campaign ladder, ground war, naval war, aggregate flights, damage sync,
   and the eight `emit_*` event walks all ride the 60 Hz accumulator,
   sliced into whole campaign seconds (`CampaignSession::advance()`,
   campaign_session.cpp:861-967). The engines *inside* are cadence-gated
   and delta-shaped — but the DRIVER pays O(theater) once per campaign
   second, so the campaign layer's cost is ∝ compression even in Tiered
   mode.

## 2. The reference: HandleCampaignThread's economics

What FreeFalcon does (`freefalcon-central/src/campaign/campupd/campaign.cpp`,
`falclib/timerthread.cpp`) — the design AGG-CLK restores:

- **One scaled clock, no extra work per compression unit.** The timer
  thread advances `vuxGameTime += delta × gameCompressionRatio`. Nothing
  else knows the preset exists.
- **The campaign catches up, capped.** `HandleCampaignThread` computes
  `deltatime = vuxGameTime − CurrentTime`, **capped at 1 campaign-minute
  per iteration** (campaign.cpp:2672-2685), on its own thread, self-paced
  (`Sleep(1)` + the sim handshake in the doUI build).
- **Per-unit cadence gates in CAMPAIGN time.** `UpdateUnit`
  (campupd/update.cpp:90-169) walks the unit list every iteration but each
  unit early-outs on `CurrentTime − lastCheck > u->UpdateTime()` — an
  integer compare — and only then does one `MoveUnit(GetMoveTime())`
  chunk. Intervals come from campaign.ini ([ATM]: FlightMoveCheckInterval,
  FlightCombatCheckInterval, AirUpdateCheckInterval — tens of
  campaign-seconds), jittered ±jitter to spread the herd (HOTSPOT_FIX,
  unit.cpp:6401-6416).
- **Negative feedback, never a pegged core.** If the campaign falls more
  than 1 minute behind, `SetTemporaryCompression(gameCompressionRatio / 2)`
  — "Slow things down" (campaign.cpp:2680-2684) — and on catch-up,
  "Back to full speed" (campaign.cpp:2686-2690). The CPU can never be
  outrun by the request; the request de-rates to the hardware.
- **The FM never meets high compression.** Full-fidelity simulation exists
  only inside the player bubble, and `DoCompressionLoop` forces compression
  back toward 1× when action approaches (takeoff, events "two 1/2 minutes
  away" — campaign.cpp:2394-2520). Aggregate cost and FM cost are never
  multiplied together.

Net effect: FreeFalcon's campaign-layer CPU per wall second is bounded by
construction; compression changes a clock reading, not a work volume.

## 3. What already exists (the scaffolding inventory)

The migration is a re-wiring, not a rewrite. Already landed and reusable:

- **Tiered fidelity policy** (FID-1..6, FID-VIEW-1): `FlightAggregateEngine`
  (route-leg propagation, TIME mode interpolating on the save's own
  arrive/depart schedule, SPEED mode cruise walk; suspended while
  deaggregated), the bubble/airfield-ops/explicit deagg triggers, the
  handoff contract, the aggregate combat pass.
- **Cadence gates with due timers**: ground war and naval war
  (`update_sec = 60`, `next_update_ += update_sec`, catch-up-once
  resupply/repair timers), the air tasking cycle (`air_task_cycle_sec`,
  `next_cycle_`, `seconds_to_next_cycle()`).
- **The big-tick equivalence pins**: the C2 tests (one big ladder tick ==
  N small ones, byte-identical) and the war harness's fully-drained
  batches ("byte-equivalent to any other split of the same ticks",
  campaign_war_harness.cpp:172-178). These exist precisely to prove the
  AGG-1 clock move.
- **The FID-OPT walk/sweep repairs** (active behavioral cache, fusion
  cadences, sensor pre-gates, p7 roster cache) and the named residuals
  (the SpatialIndex radar term — wired since AGG-2b as the air-picture
  roster).
- **The honest delivery readouts**: `effective_speed()` EMA,
  `time_dilated()`, and (AGG-0) `delivery_scale()`.

## 4. The phases

### AGG-0 — defaults + governor — **LANDED**

Patch: `0001-FID-DEF-GOV-tiered-default-and-delivery-governor.patch`.

- `CampaignSessionOptions::fidelity_policy` defaults to `Tiered`
  (campaign_session.hpp); FullFidelity is the explicit pinned-baseline
  mode. The five rigs that relied on the implicit FullFidelity default
  (test_campaign_session, test_strategy_layer, test_campaign_supply,
  test_campaign_personnel_session, test_flight_state_diag) now set it
  explicitly.
- The viewer runner (`CampaignClientRunner`) replaces drop-the-debt with
  an AIMD delivery governor: `delivery_scale_` ∈ (0,1] under the preset;
  a capped batch halves the feed (the reference's "Slow things down"),
  16 clean batches double it back ("Back to full speed"), `set_speed`
  resets to full feed. Steady-state overload SLOWS the clock instead of
  losing seconds; `time_dilated()` reports only the residual case; the
  new `delivery_scale()` readout gives the UI the governor's answer.

### AGG-1 — decouple the campaign clock from the tick stream — **LANDED**

Patch: `0003-AGG-1-campaign-pass-leaves-the-tick-stream.patch`.

**The structural change.** Replace the per-campaign-second slicing in
`CampaignSession::advance()` (campaign_session.cpp:861-967) with the
reference's two-clock split:

- The sim accumulator keeps draining `sim_->tick(1/60)` — fixed dt, only
  deaggregated aircraft pay.
- The campaign layer advances by **catch-up**: `campaign_target += wall ×
  speed`, capped per frame (the reference's 1-minute analog; suggest 60
  campaign-seconds), then ONE `ladder_->tick(min(campaign_target −
  campaign_now, cap))` big tick per frame, and the ground/naval/flights
  passes ride their existing `next_due_` gates against that delta instead
  of the per-second accumulator (`advance_ground_`, `advance_naval_`,
  `advance_flights_`, `evaluate_combat_`, `sync_objective_damage`, and the
  `emit_*` walks all move to due-gated firing).
- The CAMP-HOST command journal keeps its granularity: commands apply at
  campaign-second boundaries INSIDE the big tick, so `step(ticks)`
  semantics and the replay axis hold.

**As built** (the honest-clock shape — see §5 for what re-pinned):

- The pass LEFT the drain loop: `advance()` drains the sim accumulator
  (unchanged shape — fixed dt, step-capped, honest drop), then fires ONE
  campaign pass with the whole seconds that drain produced, delta ≥ 1;
  a drain that completed no second runs no pass (the sub-second residue
  rides in `campaign_sec_accum_`).
- The delta is BOOKED as the product `steps × sim_dt`, not a per-tick
  sum: N × (1/60) rounds bit-exact (240 × 1/60 == 4.0) where N
  sequential additions drift ~1e-14 SHORT and would quantize the pass
  boundaries a whole second off (the summed 240-tick book is
  3.9999999999999907 — floor 3, not 4). The clocks cannot diverge:
  the campaign delta IS the drained sim time.
- The per-second accumulators (`ground_sec_accum_`, `naval_sec_accum_`,
  `flight_sec_accum_`) are GONE — the helpers take the pass's delta and
  feed each engine ONE `tick(delta)` (the C2 pins hold per engine).
- The catch-up is the DRAIN's whole seconds (the honest-clock rule),
  not an unbounded `campaign_target`: the AGG-0 delivery governor owns
  de-rating (the reference's `SetTemporaryCompression` shape) and the
  drain cap owns the residual drop. The reference's 1-minute cap is
  moot here — the drain caps at 240 ticks = 4 sim-seconds, so the big
  delta is ≤ 4 campaign-seconds by construction.
- The CAMP-HOST journal axis holds: `step(ticks)` segments around the
  pending `apply_tick` boundaries in the HOST (campaign_session_host.cpp)
  — the session's pass shape is invisible to it; replay commands still
  apply at exact engine-tick boundaries.

**Proof obligations, met:**

- The engine-level big-tick equivalences were already pinned (the C2
  `CampaignTick` pins, `OneBigTickEqualsNSmallOnes` for the war
  engines); a runtime probe re-executed the ladder pin for real
  (tick(1800) == 30×tick(60) == 1799+1+0, byte-identical summaries; a
  big delta STRADDLING the due boundary fires the cycle at the
  post-tick clock).
- Two new session pins (test_campaign_session.cpp):
  `BigCatchUpBatchesMatchAlignedSmallOnesByteForByte` (3 × advance(4.0)
  == 12 × advance(1.0), ledger JSON byte-identical when the big deltas
  land ON the tasking boundaries) and `StraddledBigTickKeepsTheBooksTotals`
  (a straddling big tick shifts the books' t_s to the batch edge; the
  one-pool totals do not move).
- The C5 24-hour acceptance re-certifies on the next harness run (the
  harness's 4-second batches now run ONE pass each — the campaign
  layer's wall share drops ∝ the batch length).

**The pass-count arithmetic (what this buys, honestly):** the pass-set
now fires `min(advance calls with δ≥1, campaign seconds consumed)` times
per wall second. Batch drivers (the war harness's 4-second batches, the
scenario player's drains, the replay's runs) cut the campaign-layer pass
count by the batch length (∝ frames, not ∝ campaign seconds). A frame
driver at presets below the frame rate (60 fps × 16×) still consumes
each campaign second in its own δ1 pass — the count there is the
clock's, not the driver's. The original plan deferred those δ1 passes
past a debt threshold in AGG-2; **AGG-2a retired that deferral
honestly** (see §4): with the two O(theater) walks dirty-gated, a δ1
pass's fixed cost is O(1) gated ticks + O(changes) + the O(flights)
tier heartbeat, and the heartbeat's latency (the death fold, the deagg
triggers) must not be traded away — AGG-3's bubble economics shrink the
heartbeat's surface instead.

### AGG-2 — the deterministic due-queue

Replace "walk everything, ask each if it's due" with an ordered scheduler:

#### AGG-2a — LANDED (the transition-triggered publishers + the scheduler primitive)

- **The damage sync is dirty-gated** — `CampaignResultSink` owns a
  `mark_objective_dirty(entity)` + `sync_dirty_objective_damage()` pair
  beside the full walk (kept as the end-of-run form for QC, the
  writeback, and the tests). The transitions that write the entity face
  mark the row: `handle_bomb_impact` marks its target (the face write
  happened before the event flew — f4-weapons owns the damage ledger);
  the session's `emit_repair_events_` mirror marks each repaired
  objective (its fstatus write is a transition on the same face). The
  dirty walk runs the SAME per-objective diff/book body as the full
  walk (extracted into `sync_objective_row_`), iterated ASCENDING by
  snapshot index — which IS wire order — so the changed subset's
  records land in exactly the order the O(objectives) walk would have
  booked them. The delta buffer clears first: what the caller drains
  after the call is what THIS sync collected, never a previous pass's
  residue. A quiet pass is an O(1) no-op.
- **The verdict emit is capture-gated** — the verdict's coarse state
  (band/leader/swing) moves ONLY when a capture moves the territory
  census (`compute_theater_verdict`'s leader/band/swing read ownership
  flips alone; the ledger's loss/strength rows feed the query face's
  team rows, never the event's coarse state — verified in
  `war_verdict.cpp`). `emit_capture_events_` re-arms the emit when the
  capture log's tail advances; `emit_verdict_events_` returns
  immediately unless armed. `verdict()` — the query face — stays
  on-demand for the viewers. The gate starts TRUE: the constructor's
  state is unseen, and the first pass emits exactly what the
  always-compute pass emitted (a mid-war save's loaded advantage is a
  real verdict event).
- **The scheduler primitive lands** —
  `f4-campaign/include/f4/campaign/due_queue.hpp`: `DueQueue<Payload>`
  keyed strictly `(due_time, priority, insertion_seq)` — deterministic
  by construction (no RNG, no wall clock, no float keys); node-handle
  pops; the cursor re-reads the head after every visit, so a
  visitor-scheduled re-arm with due <= now fires within the same pass,
  key-ordered (a later-due re-arm waits for its own pop). Plus
  `vu_hash` (FNV-1a over the VU — NOT std::hash, which is
  implementation-defined; the replay axis needs a value pinned to the
  spec) and `stagger_phase(vu, interval) = vu_hash(vu) % interval` —
  the reference's rand() jitter, made deterministic: per-unit first-due
  times spread across the cadence interval, same phase every build,
  every replay. Consumers: AGG-2b's per-unit detection cadences and
  AGG-4's discrete-event scheduler.
- **The δ1 threshold-deferral is retired, honestly.** The plan
  (and AGG-1's as-built note) expected AGG-2's scheduler to defer
  frame drivers' δ1 passes until the debt crossed a threshold. With
  both O(theater) walks dirty-gated, a δ1 pass's remaining fixed cost
  is the engines' O(1) due-gated ticks + the O(changes) emits + the
  O(flights) tier heartbeat — and the heartbeat (the death fold, the
  deagg triggers) MUST NOT be deferred: it watches the sim, and
  deferral trades observable latency (a dead lead ghosting, a merge
  trigger missed) for nothing. The heartbeat's surface shrinks with
  AGG-3's bubble economics instead; the due-queue stays landed for the
  consumers that genuinely gain (per-unit cadences, discrete events).
- **Verification** — the sink contract, runtime-probed: two identical
  3-objective worlds driven through the same five passes (quiet / one
  strike / two strikes in one pass / quiet / repair); world A per-pass
  full-syncs (the pre-AGG-2a shape), world B dirty-syncs — the
  damage_synced() collections match after every pass and the ledgers'
  byte-stable to_json() documents are byte-identical at the end; the
  negative surface (unknown-entity mark, target-less impact, markless
  sync) books nothing. 18/18 probe checks pass; 12/12 due-queue
  primitive checks pass (key order, due boundary, re-entrant re-arms,
  determinism across schedule orders, the stagger's spread — 600 VUs
  cover ≥50 of 60 slots). New tests: `ResultSink.
  DirtySyncIsTheFullWalksShadow`, `ResultSink.
  DirtySyncBooksTheRepairMark`, and the `test_due_queue` gtest
  (registered in f4-campaign/tests). The verdict gate's exactness is
  the census read (territory-only) + the existing event-stream pins;
  the war harness re-certifies upstream.

#### AGG-2b — LANDED (the air-picture roster + the per-unit detection cadences)

The two hot walks that paid the full transform bucket every pass to
reject the same 99.8% of candidates with the same clutter arithmetic
are wired to one shared membership index:

- **`f4-entities` owns the `AirPictureRoster`** (air_roster.hpp) — the
  SpatialIndex wiring FID_OPT §5 named. The roster holds the
  NON-CLUTTER membership (TransformComponent carriers failing
  `is_ground_clutter()`), in entity-index order, with a SpatialIndex
  over member positions captured at rebuild (`air_roster_within_radius`
  — the SAM-ring / formation / threat-query surface, O(ball) instead of
  O(theater)). Maintenance rule: rebuild when `structural_epoch()`
  moved (spawn/destroy/component changes — the same events the ref
  buckets maintain, latency = the next refresh) or on the
  CALLER-DRIVEN revalidation cadence (the behavioral flips no
  structural event marks: the taxi launch, the landing stop). The
  clock is the host-stamped sim time — never wall time — so the
  refresh schedule is replayable. `EntityWorld` owns one lazily
  (`refresh_air_roster(now, interval)` / `air_roster()`), the move ops
  leave the destination's instance empty (the lazy create IS the
  defensive rebuild, the ref-bucket shape).
- **The radar scan walks the roster** (f4-sensors): the Search branch
  refreshes the roster with its stamped sim clock and walks MEMBERS
  with fresh transform reads, re-applying the clutter + range gates
  idempotently per member. The per-candidate handle resolution the
  FID-OPT-3 ref-walk made cheap is now paid on the ~100-200-member
  air picture instead of ~7,400 transforms per radar per scan — and
  the bucket COPY per scan is gone with it. Track mode untouched.
- **The picture walk walks the roster** (f4-simulation,
  `push_air_picture_`): the contacts loop is the roster walk (same
  values, same order, the clutter gate re-run fresh per member so a
  landed member drops exactly when the full walk would drop it); the
  Step-13 datalink NODES leave the contact walk and collect from
  their own populations — the AwacsComponent and RadarComponent ref
  buckets, merged by slot index and deduped (dual-carriers), which
  reproduces the interleaved walk's node order and team-intern order
  byte-for-byte. Node liveness (the corpse rule) stays a FRESH
  per-walk read — the GCI-ghost kill keeps its ≤100 ms bound.
- **The per-unit detection cadences** — the AGG-2a primitive's named
  consumer. The radar's own `scan_interval_s` timer was already a
  self-re-arming per-unit cadence; what it lacked was the reference's
  HOTSPOT_FIX stagger ("spread the herd", unit.cpp's rand() % interval
  made deterministic). `RadarSimComponent::scan_phase_s` (default
  0.0) primes the timer inside its interval; the armed spawn paths
  fill it from the due-queue's FNV-1a `vu_hash` when the scenario's
  `stagger_sensor_phases` gate is on (the campaign path keys the
  flight's VU per arm index; the scenario path keys the radar seed +
  index; both fold into 1,000 phase slots). Default OFF = every
  pre-AGG-2b spawn schedule byte-identical (the golden-identity rule);
  the campaign war's 48 co-mounted radars stop landing their sweeps on
  the same tick once a session opts in.
- **The honest delta (the re-pin §5 blesses)**: a behavioral flip is
  observed within one revalidation window (1 s of sim — the radar's
  own scan-interval scale) instead of instantly; every VALUE the walks
  read is a fresh transform read, so the shared population's candidate
  sets, orders, RNG streams, contacts, and node liveness are exactly
  the uncached walks'. Pinned: the FID-OPT-3 clutter-invariance pin
  (0 vs 2,000 parked entities, identical per-seed detection timeline)
  now runs through the roster path and still passes byte-for-byte; the
  flip latency itself is pinned (`BehavioralFlipJoinsAtTheRosterCadence`,
  `LandedMemberLeavesTheCandidatePoolImmediately`); the roster's
  maintenance rule has its own suite (`test_entities_air_roster`, 11
  cases: priming, epoch, cadence, both flip directions, order, the
  radius surface, move-op self-healing); the sim-level pins
  (`AirPictureRosterHoldsTheNonClutterPopulation`,
  `StaggerSensorPhasesKeyPrimesTheRadarPhases`) cover the launch
  joining through the epoch and the phase key's golden identity.
- **The scenario key**: `combat.stagger_sensor_phases` (scenario.cpp's
  reader, the session JSON's writer, `CampaignSessionOptions::
  stagger_sensor_phases` — only read when aa_combat). Flipping the
  armed war's default is a follow-up certificate action, not part of
  this tranche.

### AGG-3 — aggregate-first spawn policy

Close FID-2's documented gap: synthetic ATM intents currently spawn
straight to Tier-B (per-aircraft). Route them through the aggregate tier —
spawn as aggregate rows, deagg only on the three existing triggers. Then
full-fidelity aircraft exist only near the eye and the FM floor stops
multiplying with compression (the reference's bubble economics).
Optional authenticity knob: auto-1× while any deaggregated aircraft is
live in the observer bubble (the `DoCompressionLoop` rule) — a UX rule
once AGG-1/2 land, not a perf need.

### AGG-4 — fully lazy aggregate state (the endgame)

TIME-mode flights already interpolate on the save's own schedule and the
tier view already extrapolates to now. Push to the conclusion: aggregate
position becomes a pure query `f(route, t)` computed on read; the due-queue
schedules only discrete events (waypoint arrivals, TOT windows, fuel gates,
recovery deadlines). Aggregate propagation cost → zero between events;
`MoveUnit`-style chunk stepping disappears. SPEED-mode flights keep the
walk until synthetic intents carry arrival schedules.

### AGG-5 — threading (optional, last)

After AGG-1/2 the per-frame campaign cost is small enough that the
runner + frame-lock model holds; a dedicated campaign thread (the
reference's `HandleCampaignThread` shape) becomes a latency nicety, not a
necessity. Do not build it first.

## 5. Determinism and re-pinning (what breaks, deliberately)

- The byte-identical ledger/event-stream pins WILL change: event emission
  cadence moves from per-campaign-second to per-big-tick (AGG-1) and then
  to transition-triggered (AGG-2). Re-pin with fresh MD5 certificates —
  the determinism contract itself (no RNG, no own clocks, wire order)
  is preserved by construction; only the emission timing moves.
- The CAMP-HOST journal `apply_tick` axis: keep command application at
  campaign-second boundaries inside the big tick; `step(ticks)` semantics
  unchanged.
- The QC gates and the four writebacks read final-state diffs — unchanged
  in shape; their SAMPLING cadence moves with the due gates (safe because
  aggregates are closed-form or cadence-gated — no transient states are
  skipped between due times that the engines model).

## 6. What does NOT change

- The FM's fixed 1/60 s dt ("Fix Your Timestep") — dt never scales; the
  tiers decide WHO pays it, never how big it is.
- Engine-agnostic boundaries: f4-campaign still never sees EntityWorld.
- The C1 ledger, the writebacks, the QC harnesses, the bubble manager,
  the MessageBus vocabulary.
- Config-driven cadences (never hard-code the reference's literal
  interval values — `update_sec`/`air_task_cycle_sec` stay data).

## 7. The ceiling arithmetic (for the next person who asks "why 20×")

- F4 (pre-AGG-1): cost/wall-second ≈ 60 × N × (walk + Σ live aircraft ×
  (FM + brain + sensors)) + compression × (per-second ladder passes).
  Ceiling = CPU budget ÷ per-campaign-second cost.
- Reference: cost/wall-second ≈ 10 walk-iterations × O(N) integer compares
  + (compression × per-campaign-minute aggregate cost), capped at 1
  campaign-minute/iteration with de-rate feedback. Ceiling ≈ min(UI cap,
  600×), and overload de-rates the preset instead of missing it.
- After AGG-1: campaign layer ∝ frames; ceiling = CPU ÷ (sim-layer cost
  of deaggregated aircraft only) — i.e., the bubble's size, exactly the
  reference's economics.
