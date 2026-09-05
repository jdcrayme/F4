# Campaign Sim Performance Analysis — Why the Steps Are Slow

> **Status**: Analysis complete. Root cause identified. Fixes scoped but
> NOT implemented — this is the planning document for the fidelity-tiering
> tranche.
>
> **Triggered by**: "Why are the campaign sim steps so slow? Are we having
> things de-aggregated that shouldn't be? Are the aircraft using the wrong
> flight model? If a 1990s PC could run a campaign at 12x speed without a
> problem, we should be able to easily do the same thing."
>
> **Companions**: [Performance Plan](PERFORMANCE_PLAN.md) (PERF-1 landed,
> PERF-2 closed-by-evidence), [AI Implementation Plan](AI_IMPLEMENTATION_PLAN.md)
> §10 (Simple vs Complex Flight Model Selection — the deferred design),
> [Architecture Proposal](ARCHITECTURE%20PROPOSAL.md) §1437-1470 (the
> `IFlightModel` strategy sketch).

---

## 1. The headline

**Every active campaign aircraft runs the full 6-DOF flight model at
60 Hz × 6 sub-steps = 360 Hz effective integration.** There is no
simplified/kinematic tier for AI or distant aircraft. This is the dominant
bottleneck — 88% of per-tick time, measured by the project's own
PERF-2 profiling round (`PERFORMANCE_PLAN.md` §3).

The two other concerns raised are **not** the problem:
- **Deaggregation**: nothing is being de-aggregated that shouldn't be
  during headless ticks (§3 below).
- **Tick rate / campaign cadence**: the campaign ladder already ticks at
  a coarse cadence (once per campaign second), matching FreeFalcon. The
  cost is entirely in the *sim* layer that ticks aircraft (§4 below).

The 1990s PC comparison is real but the cause is **architectural**, not
raw-speed: FreeFalcon only ticks what's inside the player's SIM_BUBBLE,
and even those AI aircraft use a single-step "SuperSimple" algebraic
model. F4 ticks every active campaign flight at full 6-DOF (§5).

---

## 2. The flight model question — definitive answer

**Are AI aircraft using the full 6-DOF or a simple model?**

**Full 6-DOF, every one of them, every tick.**

### What was planned

`Docs/AI_IMPLEMENTATION_PLAN.md §10` (lines 1392-1428) documents the
design: `DigitalBrain::use_complex_model()` returns `false` for
ground/waypoint/loiter/landing/takeoff/refueling modes, `true` only for
combat modes (BVR/WVR/MissileDefeat/RTB) or when `threatPtr != NULL`. The
host was supposed to check this each frame and swap between
`FullFlightModel` and `SimplifiedFlightModel`.

`Docs/ARCHITECTURE PROPOSAL.md:1434-1470` sketches the `IFlightModel`
interface with three implementations: `FullFlightModel`,
`SimplifiedFlightModel`, `NullFlightModel`.

### What actually shipped

**Neither exists.** A codebase-wide search (excluding docs) for
`use_complex_model`, `SimplifiedFlightModel`, `SimpleFlightModel`,
`KinematicFlight`, `2dof`, `IFlightModel` finds **zero matches** in source.
The only flight model class is `f4::flight::FlightModel`
(`f4-flight-model/include/f4/flight/flight_model.hpp`) — the full 6-DOF
model. No strategy/policy pattern, no abstract base, no simplified variant.

### What runs per aircraft per tick

Every spawned aircraft (via `spawn_aircraft_for_flight` at
`campaign_bridge.cpp:446` or `spawn_aircraft_from_squadrons` at `:1681`)
gets a `FlightModelComponent` wrapping a `FlightModel` by value
(`flight_model_component.hpp:259` `FlightModel fm_;`).

`FlightModelComponent::update()` (`flight_model_component.hpp:74`):
```cpp
if (!initialized_) return;
if (dormant_) return;              // the ONLY skip path
fm_.update(dt, pending_input_, ground_z_ft_, ground_normal_);  // full 6-DOF × 6 minor
```

The `dormant_` flag is set ONLY by the squadron parked-inventory spawner
(`campaign_bridge.cpp:1806-1807`), for the ~1,000 un-tasked parked
airframes. **Active flights are NEVER marked dormant.**

`FlightModel::update()` (`flight_model.cpp:425`) → 6 × `minorStep()`
(`:331`) → each minor step runs:
1. `updateAtmosphere()` — 3-layer ISA
2. `fcs_.update()` — full FCS (pitch PI, roll rate-command, yaw PI)
3. `aero_.update()` — 2D Mach × α CL/CD/CY tables with bilinear interpolation
4. `engine_.update()` — 2D thrust tables + RPM spool lag
5. `EngineModel::bodyForces()` — thrust decomposition
6. `accelerometers()` — G computation
7. `updateStallSM()` — stall state machine
8. `eom_.update()` — quaternion kinematics + position integration
9. Fuel burn

The sub-step count is fixed at construction (`flight_model.hpp:242-243`):
```cpp
unsigned int minorPerMajor_{6};
double minorFrameTime_{1.0 / 360.0};  // 6 sub-steps of 1/360s = 1/60s major
```

### The cost multiplier

At a campaign merge with 96 active aircraft (the default
`max_flights = 48` saved + 48 synthetic):
- **F4**: 96 × 6 × 60 = **34,560 full 6-DOF minor steps per second**
- **FreeFalcon**: ~1 full 6-DOF (player) + ~5-20 SuperSimple (single
  algebraic step) per second

That's roughly a **100-1000× difference** in flight-model compute per
second.

### What FreeFalcon does instead

`Docs/FreeFalcon_Core_Systems_Reference.html:1706`:
> The model has two modes: a full 6-DOF model for the player aircraft and
> a "SuperSimple" model for AI/distant aircraft that bypasses the FCS and
> directly commands alpha/Nz.

`Docs/FreeFalcon_Core_Systems_Reference.html:1965-1972` (§8 Simplified Model):
> AI aircraft use a drastically simplified FCS:
> - **Roll**: first-order lag on stick command (`p = 0.75·stick·300° + 0.26·p_prev`)
> - **Pitch**: direct `Nz = cos(γ)·cos(φ) + stick·G_available`, then
>   `α = Nz·g / (q_som·CN_α)`
> - **Drag reduction**: uses 75% of true α for drag lookup
> - Includes `P_subS` (specific excess power) and `SustainedGs`

The `IsSet(Simplified)` flag drives this branch throughout
aero/roll/pitch/yaw/eom/ground-effect. It's a **single algebraic step per
major frame** — no FCS, no aero table interpolation, no minor sub-stepping.

---

## 3. The deaggregation question — definitive answer

**Are things being de-aggregated that shouldn't be during headless ticks?**

**No.** The deaggregation machinery is well-behaved:

1. **Squadron parked-aircraft deagg** happens ONCE at
   `Simulation::initialize()` (`simulation.cpp:1889`), not per-tick. The
   ~1,000 parked airframes are marked `dormant_ = true` and never tick. ✓

2. **Ground/naval battalion deagg** (`BubbleManager::update`) runs every
   tick, but:
   - Walks ~247 battalions × O(1) distance check = ~1,200 cheap ops/tick
     (microseconds, not milliseconds).
   - The bubble radius is 1024 ft (`GROUND_BUBBLE_SIZE`); most ticks
     produce 0 deagg / 0 reagg.
   - The ownship is `aircraft_entities_.front()` — whichever aircraft
     happens to be first in the roster. Not a player, but the bubble
     follows it correctly.

3. **The camera-driven "view bubble"** (`Simulation::set_view_bubble`) is
   a viewer-only feature — `view_bubble_active_` defaults false and is
   never set by `campaign_qc` or `CampaignWarHarness`. Not active in
   headless mode. ✓

4. **The ~4,400 campaign entities** (teams, objectives, battalions-as-
   aggregate-data, squadrons, flights-as-campaign-data, packages) are in
   the EntityWorld but are **passive data** (priority 0, not iterated by
   `update_all`). They cost nothing per tick beyond memory. ✓

The per-tick `update_bubble()` walk IS wasted work during headless ticks
(no player is moving), but it's a minor cost (§6, item #6), not a
deaggregation problem.

---

## 4. The 1990s PC comparison — architectural differences

### Why FreeFalcon could run a campaign at 12× on a 1990s PC

| Aspect | FreeFalcon | F4 (current) |
|---|---|---|
| **Campaign thread** | Separate `CampaignThread`; tasking fires every `MIN_TASK_AIR` minutes; units move at campaign-clock rate (seconds) | Single-threaded; campaign ladder ticks once per campaign second ✓ (correct) |
| **Sim entities** | Only entities inside the `SIM_BUBBLE` (2.5 grid = 2560 ft for air, 1.0 grid = 1024 ft for ground) around the PLAYER exist as sim entities. Maybe 5-20 aircraft + 5-10 battalions' vehicles. | Every active campaign flight (96 by default, up to 449) is a full sim entity, regardless of distance from any "player." All ~4,400 campaign entities live in the EntityWorld (but most are passive data). |
| **AI flight model** | `SuperSimple` for all AI aircraft — single algebraic step: `α = Nz·g / (q̄·S·CN_α)`, 75% drag reduction, first-order roll lag. No FCS, no aero tables, no minor sub-stepping. Full 6-DOF reserved for the player. | Full 6-DOF for every active aircraft — 6 minor sub-steps per major tick, each running FCS + aero tables + engine + EOM + stall SM. No simplified tier exists. |
| **AI aircraft outside bubble** | Don't exist as sim entities — the campaign thread advances their position along their route at the campaign clock rate. Cost: ~0. | Tick at full 60 Hz × 6 minor steps with full brain ladder, radar, RWR, sensor fusion. |
| **Ground units outside bubble** | Aggregate campaign data — no vehicles spawned, no per-vehicle tick. | Same — aggregate data, no vehicles spawned (bubble only spawns vehicles within 1024 ft). ✓ (correct) |
| **Time acceleration** | Campaign thread runs independently; sim rate stays at the player's frame rate; 12× just means the campaign clock advances 12× relative to wall-clock. The sim still only ticks what's in the bubble. | Wall-clock scaling: `real_seconds = wall_sec × speed`; drain accumulator in whole 1/60 s ticks. Every tick runs the full 6-DOF on every active aircraft. 12× = 720 ticks/sec of full-fidelity work for 96 aircraft. |

### The core mismatch

FreeFalcon's 12× campaign acceleration is cheap because **the campaign
thread does the heavy lifting at coarse cadence** (moving aggregate unit
positions, firing tasking cycles), while **the sim thread only ticks what's
near the player** (a handful of aircraft, full 6-DOF for the player,
SuperSimple for the rest). 12× means the campaign clock advances 12× — the
sim's per-tick work doesn't scale with the speed multiplier.

F4's 12× means **the sim must execute 12× more ticks per wall-second** —
each one running full 6-DOF on every active aircraft. There's no "campaign
thread does the work" path; the campaign ladder ticks once per campaign
second (correct), but every aircraft in the save ticks at full 60 Hz
fidelity for every one of those seconds.

---

## 5. The tick loop — what happens per tick

`Simulation::tick(dt)` (`f4-simulation/src/simulation.cpp:1370`):

| Phase | File:line | What it does | Cost |
|---|---|---|---|
| Ground elevation pre-pass | `:1408` | For every aircraft: lookup FM, query terrain, set ground state | ~96 terrain queries/tick |
| Combat clock stamp | `:1451` | Sets sim time on radar/missile/bomb components | O(1) per combat entity |
| `push_wingman_lead_pictures()` | `:1464` | Lead picture to wingman brains | no-op when no wingman pairs |
| `push_safety_pictures()` | `:1471` (impl `:931`) | **O(N²) over airborne aircraft** — terrain + traffic picture per aircraft | 96×95/2 = 4,560 checks/tick |
| `push_air_picture_(dt)` | `:1479` (impl `:829`) | PERF-1 shared air picture — ONE walk over ~4,400 entities, demand-gated | ~1.2 ms measured |
| `world_.update_all(dt, bus_)` | `:1483` | **The big one — 88% of tick time.** Two-pass ECS tick (brains ≥75, physics <75) | dominated by FM |
| `execute_brain_combat_intents()` | `:1502` | Walks active roster, executes radar locks + weapon releases | proportional to armed brains |
| `update_rwr()` + sweeps | `:1519-1521` | O(receivers × emitters) — ~10% of post-PERF-1 tick time | ~96×96 |
| Per-aircraft FM→Transform sync | `:1527` | Pulls FM state into TransformComponent | ~96 aircraft |
| `update_guns()` | `:1582` | Gun burst physics + hit detection | proportional to gun bursts |
| `update_bubble()` | `:1593` (impl `:1948`) | Walks ~247 battalions, deagg/reagg | ~1,200 cheap ops |
| `record_snapshot()` + `record_fcs_trace_sample()` | `:1595-1596` | Per-aircraft snapshot (QC decimates) | ~96 aircraft |

### Tick dt and speed-up

- **`sim_dt = 1/60 s`** is fixed (`CampaignSessionOptions::sim_dt` default).
  The `Simulation::tick` comment is emphatic: *"dt is AUTHORITATIVE and
  FIXED... Scaling dt here would silently move every integrator and filter
  off its tuned discretization."*
- **Speed-up is wall-clock scaling only.**
  `CampaignSessionRunner::worker_loop_()` computes
  `real_seconds = wall_sec * speed_`, then drains the accumulator in whole
  `sim_dt` ticks. Tick dt never changes.
- **Per-advance cap**: `max_steps_per_advance = 240`. The war harness
  calls `session_->advance(4.0)` per batch — 4 sim-seconds = 240 ticks.

### Measured throughput

From `PERFORMANCE_PLAN.md`:
- **Debug**: 330-540 tps at ~96 live aircraft
- **Release, pre-PERF-1**: ~480 tps pre-fight, **~37 tps at peak merge**
- **Release, post-PERF-1**: 140-200 tps floor through merge
- **Projection for 24-hour war**: 5.184M ticks × 2 runs ≈ 15 h wall at 194 tps

---

## 6. Root causes — ranked by impact

### #1 — No simplified flight model for AI aircraft (THE big one)

- **Cost**: 96 aircraft × 6 minor steps × 60 Hz = 34,560 minorSteps/sec,
  each running full FCS + aero tables + engine + EOM + stall SM.
  `PERFORMANCE_PLAN.md §3 PERF-2`: "the flight-model integration pass
  (96 aircraft × 6 minor steps) + the brain ladder — pinned territory...
  irreducible without fidelity tiering."
- **Multiplier vs FreeFalcon**: roughly 100-1000× (FreeFalcon runs 1
  player at full 6-DOF + a handful at SuperSimple; F4 runs 96 at full 6-DOF).
- **Fix**: implement the `IFlightModel` strategy from
  `ARCHITECTURE PROPOSAL.md:1437-1470` + `use_complex_model()` from
  `AI_IMPLEMENTATION_PLAN.md §10`. SuperSimple is one algebraic step —
  likely 10-50× cheaper per aircraft per tick.
- **Files**: new `f4-flight-model/include/f4/flight/simplified_flight_model.hpp`
  + `FlightModelComponent` strategy member + `BrainComponent::use_complex_model()`
  gate.

### #2 — The sim ticks every active campaign flight at 1/60 s, not just the bubble

- **Cost**: even with SuperSimple, running 96 brains + 96 FMs + 96 radars
  + 96 RWRs at 60 Hz is a multiplier FreeFalcon never paid. FreeFalcon
  only ticks what's inside the SIM_BUBBLE.
- **Multiplier vs FreeFalcon**: typically 5-20× fewer entities ticking.
- **Fix**: fidelity tiering — only tick aircraft inside a configurable
  bubble around the "player" (or camera, or QC ownship) at full 60 Hz;
  tick distant aircraft at a coarser cadence (e.g., 1 Hz) with a
  kinematic model. `PERFORMANCE_PLAN.md §3 PERF-2`: "running
  parked/distant aircraft at a coarser rate... is a BEHAVIOR change
  requiring its own tranche and re-certification, deliberately out of scope."
- **Files**: `Simulation::tick()` would need a "bubble tier" pass that
  skips/decimates distant aircraft; the campaign ladder already moves
  them in whole seconds, so the bookkeeping exists.

### #3 — `push_safety_pictures()` is O(N²) over airborne aircraft every tick

- **Cost**: `simulation.cpp:931-1027` builds a vector of every airborne
  aircraft, then for each, scans every OTHER airborne aircraft for
  intruders within 1 NM. With 96 airborne: 96 × 95 / 2 = 4,560 distance
  checks per tick × 60 Hz = 273k/sec. Plus 96 terrain lookups × 3 probes.
- **Comment lies**: `simulation.cpp:950` says "n = scenario roster size
  (<= a handful) — negligible." At campaign scale n=96, not a handful.
- **Fix**: spatial-index the airborne aircraft (uniform grid or the
  existing `SpatialIndex`), query neighbors within 1 NM. O(N) instead
  of O(N²).
- **Files**: `simulation.cpp:931-1027`.

### #4 — `update_rwr` is O(receivers × emitters) every tick

- **Cost**: ~10% of post-PERF-1 tick time. 96 receivers × (96 radar
  emitters + N missile emitters) per tick.
- **Fix**: spatial-index emitters; only pair receivers with emitters
  within `max_rwr_range_nm` (50 NM). Or demand-gate the RWR rebuild like
  SensorFusion (the RWR picture doesn't need a 60 Hz refresh).
- **Files**: `f4-sensors/src/rwr.cpp:94`.

### #5 — SensorFusion EWMA linear scan

- **Cost**: `sensor_fusion.cpp:384` — for each contact in the new
  picture, linear-scan `prev_targets_` to find the previous snapshot.
  O(N²) per rebuild where N is the contact count. With 96 contacts:
  9,216 comparisons per rebuild per brain.
- **Fix**: hash-index `prev_targets_` by entity_id. O(N) per rebuild.
- **Files**: `f4-ai/src/sensor_fusion.cpp:382-388`.

### #6 — `update_bubble()` walks every battalion every tick (even headless)

- **Cost**: ~247 battalions × O(1) = ~1,200 ops/tick. Small but
  pointless when no player is moving.
- **Fix**: skip when the ownship hasn't moved more than (say) 100 ft
  since the last bubble update, or when running headless.
- **Files**: `simulation.cpp:1948`.

### #7 — Per-aircraft ground-elevation pre-pass walks every aircraft every tick

- **Cost**: `simulation.cpp:1408-1442` — for every aircraft, looks up FM,
  queries terrain elevation, sets ground state. ~96 aircraft × 1 terrain
  query per tick.
- **Fix**: skip parked/ground aircraft (ground state doesn't change);
  skip aircraft whose (x,y) hasn't moved more than one terrain post.
- **Files**: `simulation.cpp:1408`.

---

## 7. The fix path

The project's `PERFORMANCE_PLAN.md` deliberately deferred this as "PERF-2
— pinned territory, a behavior change requiring its own tranche and
re-certification." That tranche needs three things:

### 7.1 Implement `SimplifiedFlightModel` (highest leverage)

Port FreeFalcon's `supersimple.cpp` algebraic model behind an
`IFlightModel` interface. Wire `BrainComponent::use_complex_model()` to
swap between full and simplified based on DigiMode + threat state:
- **Complex (full 6-DOF)**: player aircraft, or AI in active combat
  (BVR/WVR/MissileDefeat), or when a missile threat is visible.
- **Simplified (SuperSimple)**: AI in navigation/waypoint/loiter/RTB/
  ground/takeoff/landing — the vast majority of aircraft at any moment.

Expected: 10-50× cheaper per aircraft per tick. This alone would bring
the 88% `update_all` cost down to a fraction.

### 7.2 Implement a sim bubble (the FreeFalcon architecture)

Only tick aircraft within N NM of the "player" (or ownship, or camera)
at full 60 Hz. Distant aircraft tick at a coarser cadence (1-5 Hz) with
the simplified model, or pure kinematic route-following.

This is the structural change that matches FreeFalcon's architecture.
The campaign ladder already moves distant aircraft in whole campaign
seconds — the sim just needs to stop also ticking them at 60 Hz.

### 7.3 Re-certify

The C5 determinism gates (MD5 byte-identity across two runs) would need
re-baselining, since the simplified model produces different (but
acceptable) outputs than the full 6-DOF. This is why the tranche was
deferred — it's a behavior change, not a pure optimization.

### 7.4 Quick wins (no behavior change, no re-cert)

These can land independently, before the fidelity-tiering tranche:
- **#3**: spatial-index `push_safety_pictures()` — O(N²)→O(N)
- **#5**: hash-index `SensorFusion::prev_targets_` — O(N²)→O(N)
- **#6**: skip `update_bubble()` when ownship hasn't moved
- **#7**: skip ground-elevation pre-pass for parked/ground aircraft

These won't move the needle on the 88% flight-model cost (the big win
needs §7.1), but they're low-risk algorithmic improvements that compound
once the flight-model cost drops.

---

## 8. Bottom line

The campaign sim is slow because **every AI aircraft is running the
player's flight model at 360 Hz**. A 1990s PC ran 1 full 6-DOF + a handful
of algebraic steps; F4 runs 96 full 6-DOF pipelines. The fix is exactly
what the project's own docs planned but never built: the
`SimplifiedFlightModel` from `AI_IMPLEMENTATION_PLAN.md §10` and the
`IFlightModel` strategy from `ARCHITECTURE_PROPOSAL.md §1437`. That's the
tranche to schedule next for campaign acceleration that matches FreeFalcon.
