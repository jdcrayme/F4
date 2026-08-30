# Combat Chain — From Flying to Fighting

> **Status**: Active plan. Milestone M1 (f4-weapons core) is the first deliverable.
> **Prerequisite**: Digi flight-control stack landed (digi_full_mission lands, taxis
> back, parks; 1,421/1,421 tests per worklog NAV-DIAG1).
> **Companion**: [AI Implementation Plan](AI_IMPLEMENTATION_PLAN.md) (§5 Steps 6–12
> are the tactic modules this plan feeds), [ECS Decoupling Plan](ECS_DECOUPLING_PLAN.md),
> [Next Phase Plan](NEXT_PHASE_PLAN.md).

---

## 1. Where we are

The simulation can *fly* end to end: campaign data converts to a typed
`WorldState`, aircraft spawn from real squadrons at real airbases, the Digi
brain taxis, takes off, navigates (LNAV), and lands — all trace-verified and
deterministic. The layered state machine (`f4-state-machine`), the sensor-fusion
picture (`f4-ai::SensorFusion`), and the two-pass ECS tick
(brains ≥ 75, physics < 75) are in place.

What the simulation **cannot** do is *fight*. Verified by reading current
source (not by trusting plan documents):

| Capability | Status | Evidence |
|---|---|---|
| Radar detection model | ❌ none | `SensorFusion` is geometry + GCI-omniscience (`detected_by_gci` is unconditional within theater) |
| Weapons inventory | ❌ none | no WeaponStore component anywhere (`rg weapon|missile` finds only message-name comments) |
| Missile guidance / flyout | ❌ none | no missile entity, no PN code |
| Damage / kill model | ❌ none | `DamageBitmapComponent` covers objective *features* only; aircraft/vehicles have no hit-point state |
| Combat tactics (BVR/WVR/defeat) | ❌ none | `AI_IMPLEMENTATION_PLAN.md` Steps 8–12 unstarted |
| Layered FSM consumers | 2 of N | ATC SM + stall SM — the 26-mode DigiMode ladder has no combat modes to arbitrate yet |

The combat chain is the core of "core game functionality", and every later
milestone (combat AI, campaign dynamics, debrief) consumes it from the bottom
up. Build order therefore runs **effects → sensors → tactics → scenario**,
mirroring the way the flight-control work went: pure testable layers first,
behavior last.

## 2. Milestones

### M1 — f4-weapons: weapons & effects core *(this patch)*

An engine-agnostic, renderer-free library answering three questions:

1. **What is in the jet?** — `WeaponStoreComponent` (ECS, passive): stations,
   rounds, selected station, category queries, expend/jettison.
2. **What happens when something is fired?** — missiles as *entities*
   (FreeFalcon's `VuEntity` model): a `MissileComponent` (state) +
   `MissileSimComponent` (behavioral, physics pass) running a 3-DOF point-mass
   flyout: thrust/mass depletion, exponential-atmosphere drag, gravity,
   true proportional navigation (Zarchan vector form), seeker gimbal/range
   limits, proximity fuze, time-of-flight self-destruct.
   Guns as a world-level `GunStream` (tracers are NOT entities — 6,000 rpm
   would flood the ECS): ballistic points, dispersion, hit detection.
3. **What happens when something is hit?** — `DamageStateComponent`
   (hit points, killed flag, added to f4-entities so world/sim can populate
   from VCD `hit_points` later) + `apply_damage()` warhead-vs-strength model
   with range falloff + `EntityKilledMessage`.

Everything crosses the bus as plain structs (`MissileLaunchedMessage`,
`MissileDetonatedMessage`, `GunFiredMessage`, `DamageAppliedMessage`,
`EntityKilledMessage`) — the same message-pattern the flight model uses.
Weapon class data lives in a `WeaponClassTable` with a documented *built-in
placeholder set* (AIM-9M, AIM-7M, AIM-120C, M61, Mk-82); parsing the real
`FALCON4.WST` through f4-convert is deferred (see §5).

**Design decisions** (recorded so they don't get re-litigated):

- **Missiles are entities, bullets are not.** Missiles need per-missile ECS
  identity (targeting, kill attribution, future datalink/RWR, render) and
  there are O(10) in flight; bullets are O(1000) stateless tracers.
- **The missile sim does NOT depend on f4-flight-model.** A missile is a
  3-DOF point mass; pulling in the 6-DOF FCS/aero stack for it would couple
  the weapons layer to the manned-aircraft layer for no benefit. The small
  exponential-atmosphere helper is deliberately duplicated (10 lines) and
  documented as such.
- **Guidance reads the target's `TransformComponent` each tick.** No
  TargetInfo/sensor dependency at M1 — the fuze/seeker is geometry-only.
  When f4-sensors (M2) provides track quality, `MissileSimComponent` gains a
  `seeker_source` indirection; the state layout does not change.
- **Kill is a component transition + message, not an entity destroy.** What
  "dead" means (remove entity, freeze, spawn wreckage, campaign attrition)
  belongs to higher layers; f4-weapons sets `DamageStateComponent::killed`,
  publishes `EntityKilledMessage`, and lets the sim/host decide.

### M2 — f4-sensors: detection & tracks *(landed)*

`RadarComponent` today is objective-only data. M2 adds the airborne radar
model: scan volumes, detection probability (range/aspect/RCS/closure),
track files with quality decay, RWR (launch/lock warnings), IFF by NCTR
string. `SensorFusion` switches from GCI-omniscience to these detections;
`MissileSimComponent` gets its seeker-source indirection here.

*Landed as the `f4-sensors` library (leaf dep: f4-geo, f4-math,
f4-entities, f4-messaging). What this patch includes:* the pure detection
model (`detection.hpp`: fourth-root RCS scaling, aspect lobes, closure
modifier, 0.75-knee probability ramp), `ScanVolume`, track files
(`TrackStore`: quality gain/decay, Tentative→Established→Coasting→Dropped,
IFF by team, NCTR string), `RadarSimComponent` (ECS behavioral, priority
45: volume scan → seeded detection rolls → track updates → acquired/dropped
messages), the RWR (pure `RwrModel` + passive `RwrComponent` +
`update_rwr()` world sweep publishing Lock/Launch transitions), missile
seeker re-acquisition, and the two integration hooks: `MissileComponent::
seeker_source` (f4-weapons) and `SensorFusion::set_detection_policy`
(f4-ai). *Documented simplifications:* the detection envelope is a
physically-shaped placeholder until Falcon4.RCD data is imported (the
parameter card is shaped so the loader replaces defaults without touching
call sites); SensorFusion still defaults to legacy GCI rules — the policy
hook exists and is tested, the actual flip happens at M3 when the tactics
that need it land (flipping now would blind every AI with no replacement
picture).

### M3 — Combat AI modules (AI_IMPLEMENTATION_PLAN Steps 6–12)

RefuelModule + CollisionAvoidModule (Step 6–7), BVRModule (Step 8) with
`should_fire` / `compute_pk` reading **M1's** `WeaponClassTable` envelopes,
MissileModule defeat tactics (Step 10: crank/notch/beam/chaff-flare reacting
to **M2's** RWR), WVRModule (Step 9), WingmanModule (Step 11), DigitalBrain
26-mode arbiter over the layered FSM (Step 12). The layered FSM finally gets
its combat consumers.

### M4 — Combat E2E scenario + validation

A headless BVR intercept scenario (two flights, detect → engage → shoot-shoot
→ kill or defeat) plus a rendered variant in f4-scenario-player; combat events
added to the trace pipeline (`f4-recorder`) so every shot/detection/kill is
replayable; validation against FreeFalcon ranges (AI plan §6: MAR logic,
cooldown, shoot-shoot doctrine, beam-defeat geometry).

## 3. What does NOT change

- The aircraft entity component set (`Transform + FlightModel + VisualModel +
  Brain`) and the aircraft-binding design (`AIRCRAFT_BINDING_DESIGN.md`).
- The two-pass ECS tick contract. `MissileSimComponent` runs in the physics
  pass (priority 40 — after brains publish, before the host syncs transforms).
- The flight model, AI modules, and all existing libraries: f4-weapons is a
  new leaf dependency (f4-geo, f4-math, f4-entities, f4-messaging) — nothing
  existing links against it in this patch.
- Trace tooling: f4-weapons publishes on the same `MessageBus`; M4 will add
  recorder fields, not change the schema.

## 4. M1 work breakdown (this patch)

| # | Item | Files | Notes |
|---|------|-------|-------|
| 1 | Weapon class data | `weapon_types.hpp`, `weapon_class_table.{hpp,cpp}` | categories, guidance kinds, per-class flyout envelope + built-in placeholder set |
| 2 | Missile flyout | `missile.{hpp,cpp}` | pure 3-DOF point mass + PN + seeker + fuze; fully unit-testable without the ECS |
| 3 | Missile ECS binding | `missile_battery.{hpp,cpp}` | `MissileComponent` (state) + `MissileSimComponent` (behavioral); `launch_missile()` factory; `sweep_spent_missiles()` cleanup |
| 4 | Gun model | `gun.{hpp,cpp}` | `GunStream`: burst scheduling, ballistic tracers, dispersion, hit detection |
| 5 | Damage model | `damage.{hpp,cpp}` | `apply_damage()` (warhead power vs strength, range falloff, seeded RNG), `DamageStateComponent` in f4-entities |
| 6 | Stores | `weapon_store.{hpp,cpp}` | `WeaponStoreComponent` (passive), station/round bookkeeping, category queries |
| 7 | Messages | `messages.hpp` | launch/detonate/fire/damage/killed — plain structs, one per event |
| 8 | Tests | `tests/` | missile guidance sanity + limits, store bookkeeping, damage determinism, gun burst math, **E2E engagement** (shooter fires, missile guides, target killed, bus events observed) |

## 5. Out of scope (deferred, deliberately)

- **FALCON4.WST parsing** — real weapon class data via f4-convert (M2+; the
  built-in table's fields are shaped so the WST loader can replace defaults
  without touching call sites).
- **Air-to-ground / bombs loft & ballistics tables**, SAM batteries, naval
  units — the store and damage model are domain-neutral already; the flyout
  profiles for them land with the campaign-dynamics milestone.
- **Countermeasures state** (chaff/flare inventory, ECM) — belongs with
  MissileModule defeat tactics (M3) so the inventory shape is driven by the
  tactic's needs.
- **RWR/datalink in-flight target updates** — M2.
- **Rendering** (smoke trails, detonation effects) — f4-renderer consumers
  arrive with the M4 scenario.

## 6. Acceptance criteria for M1

1. `cmake --build build --target f4_weapons` succeeds; nothing existing breaks.
2. New unit tests pass (target: ~40 new cases):
   - Missile: burnout mass/thrust profile, drag deceleration, PN nulls LOS
     rate on a crossing target, max-G limit respected, seeker cone loss →
     ballistic, fuze within tolerance of aim point, self-destruct at TOF.
   - Damage: deterministic given a seed, monotone in warhead power, zero
     beyond lethal radius, kill transition publishes exactly one
     `EntityKilledMessage`.
   - Store: loadout → select → expend → empty-station refuses to fire.
   - Gun: rounds per burst vs rate, dispersion bound, hit applies damage.
3. E2E engagement test: shooter + target entities in a bare `EntityWorld`
   (no flight model); `launch_missile()` → `world.update_all()` ticks →
   detonation within fuze radius → target `DamageStateComponent.killed` →
   launch/detonate/damage/killed messages all observed → `sweep_spent_missiles()`
   removes the missile entity.
4. All existing tests still pass (new library is a leaf; no existing file
   changes except root CMakeLists + README).

## 7. Implementation order

1. `weapon_types.hpp` + `weapon_class_table` (data model).
2. `damage.hpp/cpp` + `DamageStateComponent` in f4-entities (the endpoint of
   every weapon; test the contract first).
3. `missile.hpp/cpp` (pure flyout) + tests.
4. `weapon_store.hpp/cpp` + tests.
5. `missile_battery` (ECS binding + factory + sweep) + `messages.hpp` +
   E2E engagement test.
6. `gun.hpp/cpp` + tests.
7. Root CMakeLists + README + worklog + patch.

---

*This document is the combat-chain plan. It feeds AI_IMPLEMENTATION_PLAN.md
Steps 8–12 (M3) and defers WST parsing and countermeasures explicitly.*
