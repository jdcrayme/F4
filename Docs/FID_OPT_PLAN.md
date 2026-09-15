# Fidelity Tiers — Optimization Tranche (FID-OPT)

> Follow-on tranche to [FIDELITY_TIERS_PLAN.md](FIDELITY_TIERS_PLAN.md) §7
> (the two optimization items that plan deliberately left open). The
> Fidelity Tiers phase is COMPLETE (FID-1..6 + FID-VIEW-1); this tranche
> attacks the two cost centers its own certificate named.

Status: **IN PROGRESS** — FID-OPT-1 in flight.

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

## 3. FID-OPT-2 — the concurrent-fight budget (measured, designed; NOT in this patch)

With the walk dead, the deep-horizon armed war re-measures clean until
the first aircraft materialize — then a NEW cost appears, and the
sub-profiled tick (`F4_TICK_PROF` split of the update_all window)
names it:

- Zero aircraft (hours 0–1.4): update_all ≈ **0.1 µs/tick**, flat.
  The 1 sim-hour armed war costs 247 ms of tick time end to end.
- First deagg (hour ~1.4, active=6): the update_all window steps to
  ~65 µs/tick and then GROWS LINEARLY with the materialized set —
  ~120 µs/tick at 6 aircraft, ~700–800 µs/tick at 21 (the 4-hour
  run's deep-horizon arc).
- The sub-timers exonerate everything else: the rebuild runs ~1–4
  times per RUN (not per tick; ~5 ms each), safety O(n²) ≤ 11 ms
  total, ATC/weather ~1 ms, the deferred-queue flush is empty
  (nothing in production publishes deferred). What remains scales
  as **brains × picture-contacts**: each materialized aircraft's
  combat brain rebuilds its SensorFusion from the shared air
  picture EVERY tick (the PERF-1 design: one world walk builds the
  snapshot; each brain's fusion still consumes it per brain per
  tick). At ~200 contacts × ~40 ns × 6 brains the arithmetic closes
  on the measured ~52 µs/tick exactly.

So the concurrent-fight budget is **O(live-aircraft × picture
contacts) per tick in the fusion rebuild** — the price of every
materialized fighter searching continuously. The levers, in order of
preference (NONE ride this patch — each changes detection timing and
needs its own golden set):

1. **Fusion rebuild throttling**: rebuild a brain's fusion at a fixed
   cadence (e.g. every 6 ticks = 10 Hz) unless an urgent condition
   (missile in flight, an existing STT lock) forces the every-tick
   path. Deterministic (fixed N), bounded latency (≤100 ms sim),
   tiered by threat instead of uniform.
2. **Contact-set culling per brain**: feed each fusion only the
   contacts within its radar's honest range/geometry before the
   rebuild (the fusion's own filters run after the copy today).
3. **A hard concurrent-fight cap** (the plan's original phrasing):
   hold the rest abstract — last resort, it changes war outcomes.

The 4-hour armed war at 60× now completes cleanly (the UAF fix); its
per-tick arc is the FID-OPT-2 baseline to beat.

## 4. What does NOT change

- The save/load pipeline, the ATM ladder, the ground war engine, the
  C2 clock model — untouched.
- Full-fidelity sessions (no parked inventory in their worlds, the
  walk was already near-empty) — behavior and bytes identical.
- The scenario player / viewer paths — `update_all` semantics for
  active components are preserved exactly.
- The bubble manager, the FID tier machinery, the aggregate feed —
  consumers of the world, not the walk; untouched.

---

*Evidence for §1: `F4_TICK_PROF=1` + a temporary cache-size diagnostic
on the 1-hour TestCamp armed war, this sandbox, 2026-09-15 (recorded
in Docs/history/worklog.md). The diagnostic was removed before the
implementation landed.*
