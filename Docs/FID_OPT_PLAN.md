# Fidelity Tiers — Optimization Tranche (FID-OPT)

> Follow-on tranche to [FIDELITY_TIERS_PLAN.md](FIDELITY_TIERS_PLAN.md) §7
> (the two optimization items that plan deliberately left open). The
> Fidelity Tiers phase is COMPLETE (FID-1..6 + FID-VIEW-1); this tranche
> attacks the two cost centers its own certificate named.

Status: **FID-OPT-1, FID-OPT-2, FID-OPT-3 LANDED.** The tranche's
original two named cost centers (the theater walk, the concurrent-fight
budget) are dead; the deep-horizon residual is measured and named (the
flight-model floor, the brain's glue, the post-OPT-3 radar term) —
see §5.

---

## 1. The problem, measured

The FID-6 certificate (real TestCamp, 20× tiered) sustained **58.1×**
vs the FullFidelity baseline **25.3×** — but its evidence named two
remaining levers:

1. **The theater walk** — "the session's fixed per-tick cost over the
   ~8,400-entity theater walk caps this host at ~48× with ZERO aircraft
   live" (FIDELITY_TIERS_PLAN §5, FID-6 first run).
2. **The concurrent-fight budget** — the deep-horizon armed war
   (hours 3–4, 32 live aircraft in sustained A/A) dilates to single
   digits (FID-5's second certificate run).

### The profile (this sandbox, TestCamp armed war, `F4_TICK_PROF=1`)

One-hour armed war at 20×, tick-phase totals over 252,000 ticks:

| phase      | total     | share  |
|------------|-----------|--------|
| update_all | 79.95 s   | 99.3%  |
| sweeps     | 0.40 s    | 0.5%   |
| intents    | 0.03 s    | 0.04%  |
| everything else | < 0.1 s | —   |

Per-tick update_all is **flat at 311–327 µs across the entire war** —
live aircraft barely move it. The cost is the WALK, not the simulation.

The walk's cargo (temporary diagnostic at first cache rebuild):

```
entities=8446 behavioral=8126
```

**8,126 behavioral components** — of which ~8,000 are the parked
squadron inventory (≈4,000 airframes × BrainComponent +
FlightModelComponent, attached by `spawn_aircraft_from_squadrons`,
each `set_dormant(true)`). Both classes' `update()` begins with
`if (dormant_) return;` — a documented no-op — but the components
still sit in `EntityWorld::behavioral_cache_`, so every tick pays:

- Pass 1: 8,126 × virtual `priority()` call + threshold compare
- Pass 2: 8,126 × virtual `priority()` call + range compare

≈ 16,252 virtual dispatches per tick × 60 ticks/sim-sec. At the
measured ~311 µs/tick that is ~38 ns per dispatch round trip — the
arithmetic closes exactly. The dormant *updates* were already fixed
(the `set_dormant` QC catch, ~25 ms → ~0.2 ms/tick); the dormant *walk*
is what remains.

## 2. FID-OPT-1 — the active-cache walk (LANDED)

**Design.** The behavioral cache splits into two lists built by ONE
rebuild walk:

- `behavioral_cache_` — every behavioral component (unchanged; the
  contract other code may rely on).
- `active_behavioral_cache_` — the non-dormant subset, in the same
  relative order.

`update_all()` iterates the **active** list in both passes. Everything
about the pass semantics is preserved exactly for active components:
priorities re-read per tick, entity-storage visit order, the two-pass
split, the no-mutation-during-update_all invariant.

**The dormant flag moves to the base.** `BehavioralComponentBase`
gains a non-virtual `dormant_` flag + `set_dormant()/is_dormant()`;
`EntityHandle::add<T>()` hands the component its owning world (the same
place `on_attached()` fires), so a dormant transition can invalidate
the cache through the base without a virtual call. BrainComponent and
FlightModelComponent drop their private flags and delegate (their
in-update early-returns stay as defense in depth — now they read the
base flag). The parked-inventory spawn path keeps calling
`brain.set_dormant(true); fm.set_dormant(true);` unchanged.

**Why byte-safety holds.** Skipping a dormant component's `update()`
is observationally identical to calling it: both classes' dormant
paths are pure early-returns with zero side effects (the original
QC catch made sure of that). The active set, the visit order, and
the pass split are all unchanged.

**Found-and-fixed en route: the subscription leak (a latent UAF the
speed-up exposed).** The first 4-hour armed run at 60× segfaulted at
~3 sim-hours. AddressSanitizer pinned it:
`heap-use-after-free` in a `TakeoffModule` `TaxiClearance` handler —
the per-entity AI modules (Takeoff ×2, Landing ×2, Refuel ×8
subscriptions) subscribed with `this`-capturing lambdas and NEVER
unsubscribed; an aircraft destroyed on kill/reagg/retire left its
handlers in the bus, and the next live brain's `TaxiRequest` publish
(the ATC answers synchronously inside the publish chain) invoked
handlers whose captured `this` was freed memory. Latent since the
modules landed — pre-OPT the deep-horizon armed war dilated so hard
that almost nothing materialized (no deaths → no dead handlers → no
crash); FID-OPT-1 makes the war keep up with the preset, aircraft
materialize and die, and the leak fired. Fix: `ScopedSubscriptions`
(f4-messaging) — an RAII bundle the modules bind at initialize and
whose destructor unsubscribes everything; `Simulation`'s member order
swapped (`bus_` before `world_`) so teardown destroys the world
(components unsubscribe) while the bus is still alive. The ATC's own
subscriptions are session-lifetime and safe (documented).

**Cost model.** Rebuild fills both lists in one walk (same O(world)
dynamic_cast sweep the current rebuild does — no new sweep). Dormant
transitions invalidate the cache exactly the way every spawn/destroy
already does (the parked-inventory spawn marks dirty at add time
anyway), so no new rebuild events are introduced. Per-tick work drops
from 2 × 8,126 virtual dispatches to 2 × (active count); the armed
war's active set is the live aircraft + their sensors + missiles in
flight (~50–200), i.e. the walk effectively dies.

**Measured (this sandbox, TestCamp):**

| config (armed, 20× preset) | before | after |
|---|---|---|
| update_all per tick, zero aircraft | ~317 µs | ~0.1 µs |
| tick total, 1 sim-hour (252k ticks) | 80.4 s | **0.12 s** |
| tiered sustained rate, 1 h | 59.5× | **~1300×** |
| FullFidelity baseline (same war) | 25.3× | 55.3× |
| 20× cert | green | green (1472× sustained) |
| 60× cert | exit 15 (honest) | **GREEN (1331×)** |

The ledger MD5 is IDENTICAL to the pre-OPT-1 run at the same preset
(`641174c7dccd5f0a9fb89d6ca9102b61`, 2-h armed) — the war's behavior
is byte-identical; only the host's speed changed. 60× — the preset the
plan has named since FID-6 — now passes on this sandbox, and the
identical MD5 across the 20×/60× presets re-proves the measurement
never perturbs the war.

**Acceptance met.** Six new `f4-entities` tests (dormant skip,
unpark-without-invalidate, active order preservation, idempotent
set_dormant, the spawn/unpark campaign pattern, the passive+dormant
edge) + the full suite green (the two pre-existing tree failures
unchanged).

## 3. FID-OPT-2 — the concurrent-fight budget (LANDED)

With the walk dead, the deep-horizon armed war re-measures clean until
the first aircraft materialize — then the concurrent-fight cost
appears. The FID-OPT-2 measurement (a temporary env-gated profiler on
the real TestCamp armed war — `F4_FUSION_PROF`, removed before
landing) re-attributed the budget, and the plan's own §3 arithmetic
was PARTLY WRONG in an instructive way:

**What the measurement found** (3-h armed war at 60×, fight minutes):

- The shared air-picture walk (`push_air_picture_`, the PERF-1
  build) costs **~1.4–1.7 ms per walk** — the ~4,400-entity transform
  scan + tag reads + contact fill — and ran on **21–37 of every 60
  fight ticks** (~30–53 ms per sim-second).
- The per-brain fusion rebuilds cost **~3–6 µs each** (the plan's
  arithmetic closed on these: ~200 contacts × ~40 ns), 200–283 per
  sim-second (~1–2.4 ms per sim-second).
- **The walk is 20–40× the fusion term.** The plan's sub-profile
  (F4_TICK_PROF) had split `update_all` — and `push_air_picture_`
  runs OUTSIDE `world_.update_all`, inside the same phase window — so
  the walk was invisible to the split and the whole budget got
  attributed to the rebuild loop.
- The mechanism: the legacy GCI rule sees every missile in the
  THEATER, so `missile_threat() != nullptr` — the beam-fight rule's
  trigger, written for "a missile is chasing ME" — was true for every
  combat brain for as long as ANY red missile was airborne anywhere.
  Every brain force-refreshed every tick, and any single one of them
  forced the shared walk via the demand gate.
- Threat tiers measured across the fight minutes: 68–85% of
  missile-threat brain-ticks had the nearest hostile missile inside
  the 50 NM RWR band; time-to-impact ≤ 15 s was ~0 (missiles are
  either in long flyout or already past); gun passes ~0.

**The design as landed — one bound, two cadences.** The design's own
"bounded latency (≤100 ms sim)" is the license: nothing a brain reads
through the combat refresh is ever more than 6 ticks (100 ms) stale.

1. **The fusion refresh tiering** (the plan's lever 1, verbatim):
   - **urgent** — the nearest hostile missile inside the fusion's OWN
     RWR warning band (`cfg_.max_rwr_range_nm`, 50 nm — no new magic
     number): every-tick `force_refresh()`, the classic beam fight,
     unchanged. The WVR gun-pass STT refresh is also unchanged.
   - **cadence** — a hostile missile visible beyond the band: the new
     `refresh_cadenced()` rebuilds at `kCombatCadenceTicks = 6` (10 Hz
     at the 60 Hz minor frame). Deterministic, integer-tick.
   - **quiet** — no hostile missile: the skill-interval timer,
     unchanged.
   Every rebuild of ANY kind restarts the cadence window;
   `will_rebuild_this_tick()` mirrors all three tiers exactly (pinned
   against actual rebuild events, not the intended ones).
2. **The shared picture's own cadence** (the measured addition — the
   design's lever 1 alone would have left the walk running every fight
   tick, because one urgent brain forces it): the walk is ALSO gated —
   at most one walk per `kPictureCadenceTicks = 6` while any brain
   demands the picture. Between walks a demanding tick hands the
   rebuilding fusions the **LAST snapshot** — bounded ≤ 100 ms
   staleness. Urgency buys rebuild rate, not picture freshness. The
   demand-less quiet periods grow the counter without bound, so the
   first demand after a quiet stretch walks at once.

**Found-and-fixed en route (both pinned by tests):**

- **The push-null invariant.** The first cut kept `push =
  &air_picture_` inside the walk branch, so demanding-but-not-walking
  ticks handed every rebuilding brain `nullptr` — and each urgent
  force-refresh fell back to its ~1 ms world-query path (measured:
  rebuilds went 5 µs → 977 µs each, the fusion term exploding 200×).
  The invariant the original per-tick build provided implicitly — a
  demanding tick ALWAYS hands a picture, fresh or cached — is now
  explicit and tested.
- **The cadence off-by-one.** The walk gate compared the PRE-increment
  counter, landing walks 7 ticks apart. Fixed to increment-then-compare
  (the same shape as the fusion's cadence): walks land exactly 6 apart
  under continuous demand.

**Measured (real TestCamp, post-OPT-2 vs the pre-OPT-2 tree):**

| metric (6 sim-hours = 2×3-h armed wars, 60×) | before | after |
|---|---|---|
| air-picture walks | 95,862 | **21,642 (4.4× fewer)** |
| walk time | 131.6 s | **36.2 s (3.6×)** |
| fusion rebuilds | 696k @ 4.79 s | 520k @ 3.67 s |
| missile-laden brain-seconds | 671,744 | 672,770 (war shape preserved) |

| certificate (real TestCamp) | before | after |
|---|---|---|
| 20× 1-h tiered | 1472× GREEN | **1324× GREEN, zero dilation** |
| — FullFidelity baseline | 55.3× | 54.6× (unchanged within noise) |
| 60× 1-h tiered | 1331× GREEN | **1707× GREEN** |
| 2-h armed, 20× | — | **410× GREEN, zero dilation** |
| 3-h armed, 60× (deep horizon) | sustained 52.36×, min sample 7.92× | **sustained 61.07× (clears 57), min sample 15.14×** |
| 4-h armed soak, 20× | crash-free | **crash-free** (187+ deaggs / 48 A/A kills / 46 aggregate flights / 41 live at the hour-4 tasking wave) |

The 2-h armed ledger MD5 changed (`73a06efd…`) — the first OPT patch
that does: the throttle shifts detection/reaction timing within the
licensed ≤100 ms bound, and the new ledger is the re-pinned golden.
The 3-h/60× armed certificate still exits 15 honestly: 60 dilated
samples remain (unchanged count) with the worst at 15.1× — see below.

**Tests.** 5 new fusion tests (imminent-in-band, distant-cadence,
the 6-tick bound, force-refresh window restart, skill-shorter-than-
cadence) + 1 integration test (the walk-age cycle 1..5,0 under
continuous demand; quiet-period growth past the bound; the
bounded-staleness consumption). Full suite 2,565 green — the two
pre-existing tree failures unchanged (one unrelated timing flake in
`CampaignSaver.MutatesTimers` observed once under `-j4`, passes
consistently in isolation and on rerun).

## 4. FID-OPT-3 — the sensor-sweep budget (LANDED)

The deep-horizon fight minutes still cost ~400–660 µs/tick and the
armed certificates' per-sample gates still dilate there (the 60× 3-h
worst sample 15.1×; the 20× 4-h tail 11–23×). The fusion-rebuild
throttle is done — the post-OPT-2 tick split over the same 6 sim-hours
named ~228 s of active-component work. The FID-OPT-3 measurement (a
temporary env-gated per-component profiler, `F4_COMP_PROF=1`, on the
3-hour TestCamp armed war at 60× — removed before landing) split that
budget, and it re-attributed AGAIN, harder than §3 did:

**What the measurement found** (3-h armed war, 60×, 1,296,000 ticks):

| term (whole war, instrumented) | total | share of `update_all` |
|---|---|---|
| **RadarSimComponent** | **161.9 s** | **62%** |
| FlightModelComponent | 33.9 s | 13% |
| BrainComponent | 26.6 s | 10% (the profiled module arms are 3.3 s of it — the rest is diffuse glue: interface resolution, fuel state, threat queries, intent bookkeeping) |
| MissileSimComponent | 0.05 s | ~0 |
| `update_rwr` (outside `update_all`) | 8.6 s | ~6.6 µs/tick, never cadenced |

The radar scan is the air-picture walk all over again — once per radar.
The per-scan mix explains where the 161.9 s lives: **7,406 candidates
per scan, 7,390 (99.8%) rejected by the ground-clutter gate**, 0 by
range, ~12 by the scan volume, ~2 detections. Cost **~1,259 µs/scan**:
the walk resolves every candidate through an EntityHandle + a
component-map lookup (~170 ns each) to reject it with two arithmetic
checks that need only the transform pointer. 99.8% of the dominant
term is pure waste.

**The design as landed — two levers, one licensed bound.**

1. **The radar scan walks pointers, not maps** (f4-entities +
   f4-sensors). `EntityWorld::with_component_ref<T>()` — the
   component-type index's pointer-carrying sibling: the same bucket,
   the same invariants (exactly the live entities carrying T, in
   entity-index order), lazy build + incremental maintenance +
   correct-or-absent + kept-when-empty, dropped defensively on world
   move. `perform_scan`'s Search walk reads the transform through the
   pair and applies the two cheap pre-gates (clutter, 8× range cutoff)
   INLINE; only survivors build handles. Byte-safety: the pre-gates
   precede the detection roll and draw no RNG, so the candidate SET
   the rolls see, its ORDER, and the RNG stream are exactly the
   pre-OPT-3 scan's; the loop's own gates re-run idempotently on the
   survivors and keep gating Track-mode candidates. Measured:
   **1,259 → 130 µs/scan (9.7×)**, the radar term **161.9 → 20.8 s
   (7.8×)**.
   Found-and-fixed en route: **the replacing-add stale pointer** — a
   component overwritten in place keeps its id but changes its object
   address; the id bucket's replace-is-a-no-op rule would have left
   the ref bucket pointing at the DESTROYED component. The ref
   on-add refreshes the pair's pointer in place (pinned by a test).
2. **The RWR sweep rides the licensed cadence** (f4-simulation,
   host-side). `kRwrCadenceTicks = 6` — the same ≤100 ms bound the
   combat refresh and the picture walk already carry. Aged BEFORE the
   gate (increment-then-compare — the off-by-one shape the §3 walk
   gate caught); initialized DUE so the war's first combat tick sweeps
   immediately. The sweep itself is UNCHANGED (a pure world function —
   direct callers, including every test, sweep exactly when they ask);
   between sweeps every RwrComponent keeps its LAST warning picture,
   transitions publish at the sweep. Measured: **8.6 → 1.9 s**.

**Tests.** 6 ref-bucket tests (id-set + order agreement with
`with_component`, pointer identity, incremental tail append,
remove/destroy correctness, the replacing-add pointer refresh, the
world-move drop) + 2 radar tests (clutter never tracks through the
ref walk; **the detection timeline is invariant to the clutter
population** — 0 vs 2,000 parked entities produce the identical
per-seed scan timeline, the byte-safety pin) + 1 integration test
(the RWR cadence arithmetic: DUE start, the age cycling 0..5, sweeps
exactly 6 apart; the end-to-end launch-warning flow runs under the
cadence inside the existing AiVersusAi test). Full suite 2,574 green
(2,565 + 9), the two pre-existing tree failures unchanged; one
unrelated parallel-run flake observed once, passed on rerun.

**Measured (real TestCamp, Release, post-OPT-3 vs the pre-OPT-3
tree — the component terms from the SAME instrumented yardstick):**

| metric (3-h armed war, 60×, instrumented runs) | before | after |
|---|---|---|
| radar scan, per scan | 1,259 µs | **130 µs (9.7×)** |
| RadarSimComponent total | 161.9 s | **20.8 s (7.8×)** |
| RWR sweep total | 8.6 s | **1.9 s (4.5×)** |
| detections per scan | ~2 | ~2 (war shape preserved) |
| dilated samples, same instrument | 60 | **30** |
| sustained rate, same instrument | 75.1× | **137.5×** |

| certificate (real TestCamp, clean runs) | before (§3) | after |
|---|---|---|
| 20× 1-h tiered + baseline | 1324× / baseline 54.6× | **1676.6× GREEN, zero dilation / baseline 54.1×** (unchanged within noise) |
| 60× 1-h tiered | 1707× GREEN | **1636.7× GREEN, zero dilation** |
| 2-h armed, 20× | 410×, ledger `73a06efd…` | **623.4× GREEN, zero dilation, ledger `641174c7…`** — the ORIGINAL pre-OPT-1 ledger: the RWR cadence's own ≤100 ms shift re-aligned the one marginal event the fusion cadence had displaced; the war's shape is the original's again |
| 3-h armed, 60× (deep horizon) | sustained 61.07×, min 15.14×, 60 dilated | **sustained 137.1× (2.2×; the 57 gate now clears 2.4× over), min sample 30.2× (doubled), 30 dilated** — still exit-15 honestly: the per-sample floor gate on the residual AND the Tier-B ceiling (33 live deaggregated > 32 — the faster host materializes deeper; a certificate parameter, not a sim defect) |
| 4-h armed soak, 20× | crash-free, 41 live, 46 dilated | **crash-free with `--accel-max-live 64` (deagg peak 46 — the war materializes deeper than the default ceiling), ZERO dilated samples, sustained 81.3×**; deterministic, leak-free |

The 1-h armed ledger MD5 (`76711c97…`) is byte-identical pre/post
OPT-3 across the 20× and 60× presets — the scan walk's output is
unchanged to the byte.

## 5. What remains (measured, not designed)

- **The flight-model floor** — 33.9 s of the instrumented 3-h war
  (13% of `update_all`, ~3.7 µs per aircraft-tick): the six
  minor-step EOM integrations. This is the physics; a throttle here
  is a fidelity change, not an optimization.
- **The brain's glue** — 26.6 s total, of which the profiled module
  arms are 3.3 s; the remaining ~23 s is diffuse (per-tick interface
  resolution, fuel state, threat queries, intent bookkeeping) at
  ~2.9 µs per brain-tick. No single lever named yet.
- **The post-OPT-3 radar term** — 20.8 s: the ref-bucket copy + the
  pre-gate walk. A 3D spatial hash (`SpatialIndex`) exists in
  f4-entities but is unwired (nothing maintains it against moving
  aircraft); wiring it would prune the walk to the cutoff ball.
- **The picture walk** — 36.2 s / 6 sim-hours (§3): could adopt the
  same ref primitive (`with_component_ref`) for its transform reads;
  named, not landed (2% of the tick, and the path is byte-pinned by
  the fusions).

## 6. What does NOT change

- The save/load pipeline, the ATM ladder, the ground war engine, the
  C2 clock model — untouched.
- Full-fidelity sessions — the radar scan's output is byte-identical
  (the 1-h armed ledger is unchanged across the 20×/60× presets) and
  the baseline rate is unchanged within noise (54.6× → 54.1× on the
  same war).
- The scenario player / viewer paths — `update_all` semantics are
  preserved exactly; the picture and RWR cadences are host-side and
  combat-gated.
- The bubble manager, the FID tier machinery — consumers of the
  world, not the picture; untouched. The FID-5 aggregate feed rides
  the walk (aggregate contacts refresh at the picture cadence too,
  the same ≤100 ms bound; the launch veto is id-based and unaffected).
- The radar's detection model, its scan cadence (`scan_interval_s`),
  Track mode, the RWR model, and its transition-only publishing —
  the scan's candidate SET and the roll stream are byte-identical;
  only the access path changed.

---

*Evidence for §1: `F4_TICK_PROF=1` + a temporary cache-size diagnostic
on the 1-hour TestCamp armed war, recorded in Docs/history/worklog.md;
removed before the implementation landed. Evidence for §3: the
temporary `F4_FUSION_PROF=1` harness (per-second brain/tier/walk/
rebuild counters over the roster) on 2×3-hour TestCamp armed wars at
60×; removed before landing. Evidence for §4: the temporary
`F4_COMP_PROF=1` per-component profiler (steady-clock totals per
component type inside update_all, per-arm timers inside the brain
update, per-scan candidate/gate counters inside the radar scan, the
sweeps-phase split) on 3-hour TestCamp armed wars at 60×, same
sandbox; removed before the implementation landed — its counters and
the before/after tables above are the record.*
