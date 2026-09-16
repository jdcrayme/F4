# Fidelity Tiers — Optimization Tranche (FID-OPT)

> Follow-on tranche to [FIDELITY_TIERS_PLAN.md](FIDELITY_TIERS_PLAN.md) §7
> (the two optimization items that plan deliberately left open). The
> Fidelity Tiers phase is COMPLETE (FID-1..6 + FID-VIEW-1); this tranche
> attacks the two cost centers its own certificate named.

Status: **IN PROGRESS** — FID-OPT-1 and FID-OPT-2 landed; FID-OPT-3
(the component-update budget) measured, not designed.

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

## 4. What remains — FID-OPT-3 candidate (measured, not designed)

The deep-horizon fight minutes still cost ~400–660 µs/tick and the
armed certificates' per-sample gates still dilate there (the 60× 3-h
worst sample 15.1×; the 20× 4-h tail 11–23×). The fusion-rebuild
throttle is done — the post-OPT-2 tick split over the same 6 sim-hours
names what is left:

- tick total 281.8 s, of which `update_all` 268.8 s (95%);
- the picture walk 36.2 s + the fusion rebuilds 3.7 s are INSIDE that
  window now, leaving **~228 s of active-component work**: the radar
  sim scans, the steering modules, the flight models, the RWR sweep
  over the materialized set — everything the concurrent fight actually
  simulates.

That is the next tranche item's budget. The fusion and the walk are no
longer the story.

## 5. What does NOT change

- The save/load pipeline, the ATM ladder, the ground war engine, the
  C2 clock model — untouched.
- Full-fidelity sessions — the throttle only engages through the
  combat ladder's demand gate and the shared picture; the baseline
  rate is unchanged within noise (55.3× → 54.6× on the same war).
- The scenario player / viewer paths — `update_all` semantics are
  preserved exactly; the picture cadence is host-side and
  combat-gated.
- The bubble manager, the FID tier machinery — consumers of the
  world, not the picture; untouched. The FID-5 aggregate feed rides
  the walk (aggregate contacts refresh at the picture cadence too,
  the same ≤100 ms bound; the launch veto is id-based and unaffected).

---

*Evidence for §1: `F4_TICK_PROF=1` + a temporary cache-size diagnostic
on the 1-hour TestCamp armed war, this sandbox, 2026-09-15 (recorded
in Docs/history/worklog.md). The diagnostic was removed before the
implementation landed. Evidence for §3: the temporary
`F4_FUSION_PROF=1` harness (per-second brain/tier/walk/rebuild
counters over the roster) on 2×3-hour TestCamp armed wars at 60×,
same sandbox and day; removed before the implementation landed — its
counters and the before/after tables above are the record.*
