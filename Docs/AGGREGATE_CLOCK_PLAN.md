# The Aggregate Clock — Moving the Campaign Layer to the Event-Driven Design (AGG-CLK)

> **Status**: Active plan. **AGG-0 LANDED** (the FID-DEF-GOV patch: Tiered is
> the session default fidelity policy; the viewer runner's dilation is an
> AIMD delivery governor). AGG-1..5 are the open roadmap. This document is
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
   the residual is named in FID_OPT_PLAN §5 (FM floor, brain glue,
   unwired `SpatialIndex` radar term).
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
  (the unwired `SpatialIndex` in f4-entities).
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

### AGG-1 — decouple the campaign clock from the tick stream

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

**Proof obligations.** Byte-equivalence via the existing C2 pins and the
war harness's drained-batch certificate; the C5 24-hour acceptance run
re-certified at high preset with the campaign layer's wall share measured
before/after (expect ∝ frames, not ∝ campaign seconds).

**Effort**: ~a focused week. **Risk**: event-stream consumers see events
in per-big-tick batches (order preserved — the stream's "order is the
engine's order" contract is per-batch); deliberate re-pin (§5).

### AGG-2 — the deterministic due-queue

Replace "walk everything, ask each if it's due" with an ordered scheduler:

- A timer wheel / ordered queue keyed `(due_time, priority, insertion_seq)`
  — deterministic by construction (no RNG, no wall clock), popping due
  work in the big tick.
- Deterministic staggering replaces the reference's `rand()` jitter:
  phase each unit's first due time by `hash(VU_ID) % interval`.
- Transition-triggered events: the per-second diff scans (`emit_*_events_`,
  `sync_objective_damage`) become dirty-flag publishers — capture publishes
  when the capture happens; damage syncs only touched objectives. The
  event vocabulary is unchanged; the stream goes sparse. This is the
  largest O(theater)-per-second → O(changes) win.
- Wire f4-entities' `SpatialIndex` for the radar/detection term (FID_OPT
  §5's 20.8 s residual) and give the air picture per-unit due cadences
  with cutoff-ball pruning — the reference's `DetectOneWay` cell-culling
  with a real index.

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
