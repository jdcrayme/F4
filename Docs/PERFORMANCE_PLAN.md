# Performance — Sustaining the Armed War

> **Status**: PERF-1 LANDED (byte-identical, 3–4× at the
> merge, suites green); PERF-2 closed by evidence (nothing dominates —
> no room promoted); PERF-3 documented with the release-certificate
> command (the dev container cannot host multi-hour processes —
> measured).
> **Prerequisite**: C1–C6 LANDED (`CAMPAIGN_LOOP_PLAN.md` §5), M1–M4 LANDED
> (`COMBAT_CHAIN_PLAN.md`).
> **Companion**: [Campaign Loop Plan](CAMPAIGN_LOOP_PLAN.md) (the war
> harness whose diary is this plan's watch surface).

---

## 1. Where we are (measured, not guessed)

The C6 verification already named the symptom: a 12-minute armed
observation run showed Release throughput at ~480 tps pre-fight falling to
~37 tps at peak merge over 80 airborne — "the WVR/defensive sweep across
the roster; documented in the diary, never gated." C5 named the frame:
Debug 330–540 tps at ~96 live; Release is the acceptance medium; a
24-hour war is 5.18M ticks at 60 Hz.

The phase profiler (`F4_TICK_PROF=1`, `Simulation::tick`'s env-gated
bucket timing) on a 12-minute armed war (TestCamp v71, Release, this
container) settles where the time goes:

| Tick phase | Share (merge phase) | Notes |
|---|---|---|
| `update_all` (world ECS pass) | **95–97%** | brains + FMs + radars |
| `sweeps` (RWR rebuild + missile/bomb sweeps) | ~2.7% | bounded |
| `intents`, `sync`, `guns`, `ground`, `record` | <1.5% combined | noise |

Per-tick cost grows **4 ms → 88 ms** between t≈300 s (cruise) and t≈480 s
(merge) while the roster stays FLAT (96 live, 0 retired). The cost scales
with **engagement state**, not world size — and it compounds: slower ticks
stretch the merge in wall time, which keeps more missiles airborne, which
spreads the cost further.

### The root cause (one line of code with a 96× multiplier)

`BrainComponent::update` refreshes its SensorFusion picture every tick
while any hostile missile is visible (the beam-fight rule,
`brain_component.hpp` — `missile_threat() != nullptr` →
`force_refresh()`). That rule is right: a defensive brain needs a fresh
picture. The COST is what it refreshes FROM: each rebuild walks
`world_->with_component<TransformComponent>()` — the entity database, all
~4,400 transform-bearing entities of a populated save — and per candidate
pays an `EntityHandle::get` hash probe, a clutter predicate, and (for
survivors) two string-keyed tag lookups. Ninety-six brains × 60 Hz × the
same walk: the air picture — SHARED state, the same entities, the same
transforms — is re-derived per-brain, per-tick, from scratch.

The reference never does this. FreeFalcon's campaign loop iterates its
VU entity collections ONCE per update; digi targeting then reads shared
iteration state. The per-brain full-database walk has no reference shape
— it is an artifact of `SensorFusion` querying `EntityWorld` directly
(correct for a self-contained module, wrong at campaign scale).

Secondary measured/estimated costs ride the same multiplier (per rebuild,
×96 brains, ×60 Hz): `RadarBackedDetectionPolicy::classify` re-resolves
the ownship's radar and RWR components per CONTACT; the EWMA previous-
snapshot match is a linear scan; `update_rwr` pairs every receiver with
every emitter (~2.7% of merge time — real but not the fire).

## 2. Principles (inherited, restated)

- **Determinism is the acceptance, throughput is telemetry.** Every fix
  must keep the four C5 gates green and the double-run ledger MD5
  identical. Performance telemetry lives in the war diary (explicitly
  not byte-stable) — never in a gate.
- **Byte-identical first.** PERF-1's contract: the armed war's
  `campaign_result.json` MD5 is UNCHANGED by the fix (same entities, same
  order, same values, same arithmetic — the shared walk replaces 96
  identical walks). If a later room must change behavior to buy speed,
  it says so in its own words and re-certifies (new baseline MD5, gates
  green) — never silently.
- **The profiler is the gatekeeper.** No optimization lands on theory:
  each room cites its before/after `F4_TICK_PROF` measurement and its
  diary tps curve.
- **Layers stay put.** f4-ai gains no f4-sensors/f4-simulation dependency;
  the host remains the modules' eyes (the picture is PUSHED, never
  pulled). The ECS component-index and behavioral-cache contracts are
  untouched.

## 3. Rooms

### PERF-1 — the shared air-picture snapshot (the 96× → 1× walk) (LANDED)

The host walks the world ONCE per tick and hands every brain the same
immutable picture:

| Piece | Library | What it is |
|---|---|---|
| `f4::ai::AirPicture` | f4-ai | A plain snapshot: contacts (entity id, position, velocity, interned team, `is_missile` role) in entity-index order, clutter-skipped by the shared `TransformComponent::is_ground_clutter()` rule — exactly the candidate set and order the per-brain walk produced. No new dependency: positions/velocities via `f4::geo`, team interning via a first-seen string table. |
| `SensorFusion::set_air_picture()` | f4-ai | Non-owning per-tick pointer. When set, `rebuild_target_list()` builds `targets_` FROM THE PICTURE (ownship still resolved from the world — one lookup, byte-identical values); when null, the legacy world walk runs unchanged (self-contained tests, standalone hosts). |
| `BrainComponent::set_air_picture()` | f4-ai | The host's injection point — same shape as `update_terrain_picture`/`update_traffic` (the host is the module's eyes). |
| `Simulation::push_air_picture_()` | f4-simulation | The once-per-tick walk (combat-gated): bucket copy → clutter filter → tag reads → team interning → one pointer to every roster brain. Runs before `update_all`, beside the other picture pushes. |
| `DetectionPolicy::prepare_batch()` | f4-ai | Optional per-rebuild hook (default no-op): the fusion calls it once before the classify loop; `RadarBackedDetectionPolicy` resolves the ownship's radar + RWR ONCE per batch instead of per contact. |

Acceptance (PERF-1): the 6-minute armed war's `campaign_result.json`
MD5 equals the pre-change baseline (captured on the C6 build:
`e8496c7819cbb7b64b8f9e0a2fdc7b64`); the four C5 verdicts stay green;
full suites green Debug + Release; the 12-minute profile shows
`update_all` merge-phase share and absolute per-tick cost materially
reduced (target: the every-tick-refresh path costs the picture walk once
+ per-brain contact work only — no per-brain database walk); the diary's
merge-phase tps reflects it. New units: picture-path vs world-path
TargetInfo equality (a synthetic world with teams, a missile, clutter,
and a stub policy), batch-prepare equivalence, and the host push contract.

**LANDED** (TestCamp v71, Release, this container):
- **Byte-identity: MD5 e8496c7819cbb7b64b8f9e0a2fdc7b64 UNCHANGED** —
  three consecutive post-change 6-minute armed wars produced the
  baseline bytes; all four verdicts green each time.
- **The demand gate became part of the room**: the first cut built the
  picture every combat tick and regressed the quiet phases (441 → 279
  tps at t=63 s — the walk costs ~1.2 ms over ~4,400 entities and
  pre-flight nobody consumes it). `SensorFusion::will_rebuild_this_tick
  (dt)` + `BrainComponent::wants_air_picture(dt)` gate the walk on the
  fusion's own rebuild decision, exactly (the Ground→Enroute in-tick
  entry is combat-uninitialized → world path → identical output).
  Cruise restored to 439 tps; the merge pays the walk once.
- **6-minute war**: wall 283 s → 183 s; merge sample 45 → ~156–177 tps.
- **12-minute war**: pre-PERF-1 it never COMPLETED inside 570 s
  (still degrading past 24 tps at t=423 s); post-PERF-1 it runs green
  end to end — wall 444.8 s, 12 cycles, 5 air losses, the reaper's
  first retirement, tps floor 140–179 through the fight and 192
  recovering after, RSS flat at ~250 MB.
- Suites 2,188/2,188 Debug + Release (2,183 + 5 new units: the
  picture-path equality pair, the demand predicate, the stale-pointer
  reset, and the policy batch equivalence).
- One real bug caught by the refactor itself: `-Wdangling-pointer` on
  the world path's hoisted team read (the tag optional dies with its
  scope) — fixed by copying the string out (SSO; no allocation).

Out of scope, deliberately: changing the beam-fight refresh RULE (every
tick while a missile is visible stays — it is behavior, and PERF-1 makes
it affordable); throttling any module's cadence (same reason); the radar
sweep's own candidate walk and `update_rwr`'s emitter pairing (bounded —
~10% of post-PERF-1 tick time; see PERF-2's table; they get their own
room only if a future profile promotes them).

### PERF-2 — the post-fix profile round (CLOSED BY EVIDENCE — no room promoted)

Re-run the 12-minute armed war with `F4_TICK_PROF=1` after PERF-1 and
read the diary. Result: the war now COMPLETES (it could not before),
and the profile has no dominant fire left —

| Tick phase | Share (whole 12-min war, both runs) | Verdict |
|---|---|---|
| `update_all` | 88% | the flight-model integration pass (96 aircraft × 6 minor steps) + the brain ladder — pinned territory (FCS trace goldens; FLIGHT_CONTROL_STABILITY_PLAN owns that operating point) |
| `sweeps` (RWR rebuild + missile/bomb sweeps) | ~10% | real, bounded, O(receivers × emitters) at ~96×96 | 
| `intents`/`sync`/`guns`/`ground`/`record` | <2% combined | noise |

Nothing dominates the way the per-brain walk did (95–97%). The
remaining big block is per-aircraft PHYSICS — irreducible without
fidelity tiering (running parked/distant aircraft at a coarser rate),
which is a BEHAVIOR change requiring its own tranche and
re-certification, deliberately out of scope here. Per this plan's own
rule — "a candidate that the profile does not promote does not land" —
no PERF-2 optimization lands. The numbers are recorded; the floor is
sustained (140–192 tps through an entire 12-minute fight cycle); the
budget moves to PERF-3.

### PERF-3 — the 24-hour Release war (the acceptance run) (DOCUMENTED — run on a host session)

`campaign_qc --war 24 --aa-combat` (the C5 harness, C6 combat) runs to
its horizon in Release with the four gates green, the double-run MD5
identical, and the diary documenting the throughput floor. The wall-clock
budget: the run must fit a working session (hours, not days) — the exact
number the host sustains is recorded in the run's diary. `--war-max-wall`
guards the run. This is the certificate the performance phase exists
for: the C5 horizon, now armed, now affordable.

**The dev container cannot host it (measured, not assumed): single
tool calls cap at 10 minutes, and background processes are reaped
between calls — a detached `setsid nohup` launch died with an empty
log; a plain canary died identically two calls later.** What the dev
container CAN hold — and did — is the 12-minute armed war
end-to-end green (the run pre-PERF-1 could not complete at all):
both in-process runs, all four verdicts, the MD5 certificate, flat
RSS, and a sustained 140–192 tps floor through an entire
fight-resolution cycle. Projection from that floor: 24 sim-hours =
5.184M ticks per run, ~10.4M for the harness's two in-process runs,
≈ 15 h wall at 194 tps (≈ 7.5 h with `--war-runs 1`; determinism is
already certified at the 0.1 h and 0.2 h horizons by the same
harness, and the four gates run per-sample either way). The release
certificate command, for a host that can hold the process:

```
campaign_qc TestCamp.world.json --war 24 --aa-combat \
           --war-sample 1800 --war-max-wall 64800
```

## 4. What does NOT change

- The C5 gate semantics (determinism / drift / leak / alive) and their
  exit codes. RSS stays diary telemetry, never a gate.
- The ledger, the tasking pipeline, the doctrine, the reaper — untouched;
  PERF-1 is invisible to all of them by construction (byte-identity).
- `SensorFusion`'s world-query path — it stays, first-class, for hosts
  and tests that have no picture to push.
- The two-pass ECS tick, the component-type index, the behavioral cache.

## 5. Implementation order

1. ~~PERF-1 the shared air-picture snapshot + policy batch-prepare~~ —
   LANDED (see §3 PERF-1 for the verification numbers and the
   byte-identity proof).
2. ~~PERF-2 the post-fix profile round~~ — CLOSED BY EVIDENCE: no
   dominant cost remains; the profile promotes no room (§3 PERF-2).
3. PERF-3 the 24-hour Release war — DOCUMENTED (§3 PERF-3): the
   dev container reaps background processes and caps calls at 10
   minutes; the certificate command + the ~15 h (2-run) / ~7.5 h
   (1-run) projection are recorded for a host session, and the
   dev-container evidence (12-minute war green end to end, flat RSS,
   sustained floor) is in §3 PERF-1.
