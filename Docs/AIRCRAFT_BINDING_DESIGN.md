# Aircraft Binding Design — How an "Aircraft" is Represented in the ECS

> **Status**: Design note — captures the binding decision made when planning the scenario player.
> **Created**: 2026-08-09
> **Updated**: 2026-08-09 — host app renamed `f4-taxi-demo` → `f4-scenario-player` (better reflects that the same host will eventually run takeoff, landing, and combat scenarios, not just taxi). See [Scenario Player Plan](SCENARIO_PLAYER_PLAN.md), [Architecture Proposal §13](ARCHITECTURE%20PROPOSAL.md#13-f4-simulation--orchestration), [ECS Decoupling Plan](ECS_DECOUPLING_PLAN.md)

---

## 1. The question

In FreeFalcon, `AircraftClass` is a god-class — 200+ members, deep in a 6-deep inheritance chain (`VuEntity → FalconEntity → SimBaseClass → SimMoverClass → SimVehicleClass → AircraftClass`). It holds raw pointers to every subsystem an aircraft needs:

```cpp
class AircraftClass : public SimVehicleClass {
    AirframeClass*    af;          // flight model (6-DOF EOM, FCS, aero, engine, gear)
    DigitalBrain*     brain;       // AI (26-priority DigiMode layered state machine)
    DrawableBSP*      drawable;    // 3D LOD model (BSP tree + DX engine)
    SensorClass*      sensors[6];  // radar, RWR, etc.
    Weapon            Weapon[16];  // weapon stores
    // ... 180+ more members
};
```

That single class is the "binding" — every subsystem reaches the others via `af->platform->brain->...` chains. When we move to an ECS, the question is: **what is the ECS equivalent of `AircraftClass`?**

## 2. The answer: nothing — "aircraft" is a component-set, not a type

There is no `AircraftComponent`. There is no `AircraftClass` equivalent. The binding is the **entity ID itself**.

An "aircraft" is just an entity that happens to carry this set of components:

```
entity = 0x1234
  ├─ TransformComponent        (position ENU, quaternion, velocity, body rates)
  ├─ VisualModelComponent      (const ModelRecord*, active LOD, DOF/switch state)
  ├─ FlightModelComponent      (AirframeClass state, FCS, gear; implements IAircraftState + IPilotInputSink)
  ├─ BrainComponent            (DigitalBrain state, current DigiMode; runs in pass 1)
  ├─ SensorComponent[]         (radar, RWR — empty for the taxi demo, populated later)
  └─ WeaponStoreComponent      (16 hardpoint slots — empty for the taxi demo, populated later)
```

When the brain wants to command the flight model, it does **interface-based lookup on its own entity** — not a raw pointer dereference:

```cpp
// BrainComponent::update() — actual code in f4-ai/include/f4/ai/brain_component.hpp
auto* state = owner_->get_interface<flight::IAircraftState>();
auto* sink  = owner_->get_interface<flight::IPilotInputSink>();
if (!state || !sink) return;  // no flight model on this entity
const auto ai_out = module_.update(dt, state);
sink->set_pending_input(map_to_pilot_input(ai_out));
```

The brain doesn't even know it's talking to a `FlightModelComponent` — it just knows the entity provides the `IAircraftState` and `IPilotInputSink` interfaces. That's the decoupling the ECS was designed for.

## 3. What `VisualModelComponent` is — and isn't

`VisualModelComponent` is **only the renderable handle** — the ECS equivalent of `DrawableBSP* drawable` on `SimVehicleClass`. Nothing more.

```cpp
namespace f4::simulation {

struct VisualModelComponent : entities::Component<VisualModelComponent> {
    const f4::models::ModelRecord* model_record{nullptr};
    int active_lod{0};
    f4::models::ModelState model_state{};
    int texture_set{0};
};

} // namespace f4::simulation
```

It does **not**:
- Hold a pointer to the flight model (the brain and FM are siblings, resolved via the ECS)
- Hold a pointer to the brain (same)
- Drive any per-tick update (it's a passive `Component<T>`, not a `BehavioralComponent<T>`)
- Know about sensors, weapons, fuel, or anything else

It does:
- Tell the renderer which 3D model to draw
- Carry per-instance visual state (LOD, DOF/switch values, texture set) so the renderer can animate gear, flaps, etc.

The host syncs `model_state.switches` from the FM's gear flag each tick — that's the only write path. The renderer reads everything else.

## 4. Why `VisualModelComponent` lives in `f4-simulation`, not `f4-entities`

`VisualModelComponent` holds a `const f4::models::ModelRecord*`, so it depends on `f4-models`. `f4-entities` is currently dependency-free (only `f4-geo` + stdlib) and must stay that way — it's the substrate every other library builds on.

The component therefore lives in a new `f4-simulation` library (proposed in `Docs/ARCHITECTURE PROPOSAL.md` §13) that depends on both `f4-entities` and `f4-models`. This is the same pattern the ECS was designed for: any library can define new components via the `Component<T>` CRTP base + `type_index` key, without modifying `f4-entities`.

## 5. The composition moment

The equivalent of FreeFalcon's `new AircraftClass() + new AirframeClass(af, dataset) + new DigitalBrain(self, af)` is just four `add<T>()` calls on one entity:

```cpp
auto h = world_.create();

// 1. Transform — where the aircraft is
auto& tf = h.add<TransformComponent>();
tf.position = parking_spot;
// heading → quaternion about Z-up
tf.qw = std::cos(hdg * 0.5);  tf.qz = std::sin(hdg * 0.5);

// 2. Flight model — how it moves (implements IAircraftState + IPilotInputSink)
auto& fm = h.add<FlightModelComponent>();
fm.init(aircraft_cfg_, alt_ft, vt_ftps, hdg_rad, inAir=false);

// 3. Visual model — what the renderer draws (the ONLY new component type)
auto& vis = h.add<VisualModelComponent>();
vis.model_record = model_db_->model(vis_type);
vis.active_lod = 0;

// 4. Brain — who's flying (runs in pass 1, finds FM via interface lookup)
auto& brain = h.add<BrainComponent>();
brain.module().taxi_speed_kts = 15.0;
```

That entity **is** the aircraft. No wrapper class. The entity ID is the binding; the components are the parts.

## 6. Mapping table — FreeFalcon → ECS

| FreeFalcon concept | ECS equivalent | Status in repo |
|---|---|---|
| `AircraftClass` (god-class, 200+ members) | **an entity with the aircraft component-set** | not needed as a type — that's the point |
| `AirframeClass af` | `FlightModelComponent` (implements `IAircraftState` + `IPilotInputSink`) | exists, tested |
| `DigitalBrain brain` | `BrainComponent` (wraps `TakeoffModule`, runs in pass 1) | exists, tested (taxi + takeoff states) |
| `DrawableBSP* drawable` | `VisualModelComponent` (passive data: `ModelRecord*` + LOD + DOF/switch) | **NEW — added in `f4-simulation`** |
| `SensorClass*[] sensors` | `SensorComponent` (one per sensor) | not needed for taxi demo |
| `Weapon Weapon[16]` | `WeaponStoreComponent` (16 hardpoint slots) | not needed for taxi demo |
| `af->platform->brain->...` pointer chains | `EntityHandle::get<T>()` / `get_interface<I>()` sibling lookup | exists, used by `BrainComponent` |
| `new AircraftClass()` constructor | `world.create()` + 4× `h.add<T>(...)` | the composition moment (§5) |

## 7. What this buys us

- **No god-class to maintain.** Adding a new subsystem (e.g., a `DamageModelComponent`) doesn't touch `AircraftClass` because there is no `AircraftClass`. It's just another sibling component.
- **No back-pointer bookkeeping.** FreeFalcon's `new DigitalBrain(self, af)` requires passing both pointers and keeping them valid for the brain's lifetime. The ECS resolves siblings on demand via stable entity IDs.
- **Testability.** Each component is unit-testable in isolation. `BrainComponent` is tested with a mock `IAircraftState` — no flight model required.
- **Engine-agnosticism.** `VisualModelComponent` holds a `ModelRecord*`, not a Raylib `Mesh`. The renderer (in `f4-scenario-player`) is the only place that knows about Raylib. The simulation library stays pure C++.
- **Extensibility.** A "ground vehicle" is the same pattern with a different component-set (`TransformComponent` + `VisualModelComponent` + `GroundVehicleModelComponent` + `BrainComponent`). No new base class.

## 8. What this does NOT buy us (yet)

- **No automatic spawning from campaign data.** Today the host calls `spawn_aircraft()` with hardcoded scenario data from a hand-authored JSON. Closing the `visType[7]` gap in `f4-world-convert/src/class_table.cpp` (now done — see [Scenario Player Plan §4](SCENARIO_PLAYER_PLAN.md#4-gap-closure)) and extending `f4-world::populate_units` to spawn aircraft from `Flight`/`Squadron` campaign units is the next stretch goal.
- **No `DigitalBrain` orchestrator.** `BrainComponent` currently wraps only `TakeoffModule`. The 26-priority DigiMode layered state machine (Landing/Nav/Refuel/Collision/BVR/WVR/Missile/Wingman) is future work (see `Docs/AI_IMPLEMENTATION_PLAN.md`).
- **No sensors or weapons.** `SensorComponent` and `WeaponStoreComponent` don't exist yet. They're not needed for the taxi demo but will be added when the first air-to-air scenario is attempted.

---

*This document exists to prevent the same naming mistake from recurring. The component is `VisualModelComponent`. It is the renderable handle. The aircraft is the entity.*
