# Next Phase — Phase 2: From Hand-Authored to Campaign-Derived Scenarios

> **Status**: Active plan, picks up where `SCENARIO_PLAYER_PLAN.md` left off.
> **Prerequisite**: Phase 1 patch (`f4-aircraft-binding-and-scenario-player.patch`) committed.
> **Companion**: [Aircraft Binding Design](AIRCRAFT_BINDING_DESIGN.md), [Scenario Player Plan](SCENARIO_PLAYER_PLAN.md), [ECS Decoupling Plan](ECS_DECOUPLING_PLAN.md)

---

## 1. Where we are

Phase 1 closed the binding-data-flow gap (visType[7] exposure) and delivered a working `f4-scenario-player` host that:

- Spawns **one** F-16 at a hand-authored parking spot.
- Renders the F-16 mesh + airport geometry (runway, taxi route, markers, compass).
- Ticks the `Simulation` (EntityWorld + MessageBus + ModelDatabase + FlightModelComponent + BrainComponent + VisualModelComponent).
- Supports `--screenshot` for headless smoke tests.
- Produces a valid trace JSON.

But the scenario is hand-authored: the parking-spot coordinates, taxi route, and runway threshold are hardcoded in `scenarios/kunsan_parking.json.in`. The campaign data — `f4-world::WorldState` with its real `GroundLayoutList` per airbase objective and its `Flight`/`Squadron` unit roster — is not yet wired into the scenario player.

## 2. Where we want to be

The next milestone is **scenario derivation from campaign data**. Concretely:

1. **Real Kunsan ground layout.** Load `save1.cam` via `f4-world-convert`, build a `WorldState`, find the Kunsan objective, extract its `GroundLayoutList` (runway/taxiway/parking), and convert that into a `ScenarioAirfield`.
2. **Aircraft spawning from `Flight` units.** Walk the `WorldState::units` vector, find `Flight`-class units (domain=air), resolve each one's `squadron_id` → `SquadronComponent::airbase` → the airbase objective → a parking spot, and spawn a child aircraft entity per flight.
3. **Multi-aircraft `Simulation`.** Today `Simulation` tracks a single `aircraft_entity_`. Phase 2 lifts that to a vector of aircraft entities, each ticked independently, each with its own brain + FM + visual model.

That gets us from "render one F-16 at fake coordinates" to "render the actual Kunsan campaign roster at their actual parking spots".

## 3. What does NOT change

- The aircraft entity's component set (`TransformComponent + FlightModelComponent + VisualModelComponent + BrainComponent`) is unchanged. The entity ID remains the binding — there is still no `AircraftClass` wrapper. See `AIRCRAFT_BINDING_DESIGN.md`.
- The renderer in `f4-scenario-player/src/renderer.cpp` is unchanged. It already iterates entities with `VisualModelComponent`; we just need to call `draw_aircraft()` once per aircraft instead of once for the singleton.
- The `TakeoffModule` state machine is unchanged. It already handles taxi → hold-short → takeoff → flyout → done. Phase 2 just runs N copies in parallel.
- The `FlightRecorder` is unchanged in shape — one `FlightSnapshot` per tick per aircraft (we'll add an `entity_id` discriminator, already present).

## 4. Concrete work breakdown

### 4.1 `ScenarioAirfield` from `GroundLayoutList` — the bridge

`f4-world::ObjectiveState::ground_layout` is a `std::vector<GroundLayoutList>`. Each `GroundLayoutList` has a type (runway / taxiway / parking / holdshort) and a `std::vector<GroundLayoutPoint>` with `{x, y, z}` in grid coordinates. The grid-to-feet conversion is already implemented in `f4-world/src/world_loader.cpp` as `grid_to_feet(x, y, z)`.

New function in `f4-simulation`:

```cpp
namespace f4::simulation {

/// Derive a ScenarioAirfield from a real airbase objective's ground layout.
/// Picks the longest runway list as the active runway, finds the nearest
/// hold-short point to the threshold as the runway-end marker, and chains
/// taxiway + parking points into a single taxi route from parking to threshold.
///
/// Returns std::nullopt if the objective has no runway list (not an airbase).
std::optional<ScenarioAirfield>
derive_airfield_from_objective(const f4::world::ObjectiveState& obj,
                                int active_runway_id = 36);

} // namespace f4::simulation
```

This is the only new "bridge" function. It is pure: it takes a `ObjectiveState` and returns a `ScenarioAirfield`. No I/O, no side effects, fully unit-testable.

### 4.2 Aircraft spawning from `Flight` units — the roster

`f4-world::populate_units` already creates a `FlightPlanComponent` entity for each `Flight` unit in the campaign. Each `FlightPlanComponent` carries `squadron` (EntityId), `package` (EntityId), `mission`, `callsign_id`, `callsign_num`, `loadouts`. The `SquadronComponent` on the squadron entity carries `airbase` (EntityId of an objective).

The bridge from "Flight entity exists" to "spawn an aircraft entity per Flight" is what's missing. New function in `f4-simulation`:

```cpp
namespace f4::simulation {

/// Walk the EntityWorld populated by f4-world::populate_world, find every
/// entity with a FlightPlanComponent, and spawn a child aircraft entity for
/// each one. The aircraft entity inherits:
///   - position: the parking spot nearest to the squadron's airbase
///   - heading: the runway heading (or parking heading if no runway)
///   - callsign: derived from FlightPlanComponent::callsign_id/num
///   - vis_type_index: looked up via ClassTable::vis_type_for(squadron.entity_type, 0)
///
/// Returns the vector of spawned aircraft EntityIds.
std::vector<entities::EntityId>
spawn_aircraft_from_flights(entities::EntityWorld& world,
                             const f4::world_convert::ClassTable& ct,
                             const f4::models::ModelDatabase& db,
                             const f4::data::AircraftConfig& cfg,
                             const ScenarioAirfield& airfield);

} // namespace f4::simulation
```

This is the §4.3 gap closure that was deferred in Phase 1. Each spawned aircraft carries `TransformComponent + FlightModelComponent + VisualModelComponent + BrainComponent` — the same composition as Phase 1's singleton, just multiplied.

### 4.3 `Simulation` — multi-aircraft refactor

Today `Simulation::aircraft_entity_` is a single `EntityId`. Three small changes:

1. Replace `EntityId aircraft_entity_` with `std::vector<EntityId> aircraft_entities_`.
2. `tick()` syncs transforms + gear switches for every entity in the vector, not just one.
3. `record_snapshot()` writes one snapshot per aircraft per tick (already does — `FlightSnapshot::entity_id` discriminates).

The `spawn_aircraft()` private method becomes the legacy single-aircraft path; the new `spawn_aircraft_from_flights()` becomes the campaign-derived path. The scenario JSON carries a flag `"spawn_mode": "scenario_list" | "campaign_flights"` to pick which path runs.

### 4.4 `f4-scenario-player` — render N aircraft

The renderer already iterates the world for `VisualModelComponent`-bearing entities in principle; in practice it pulls the single `aircraft_entity_` from the sim. Change `Impl::draw_aircraft()` to iterate `sim.world().view<VisualModelComponent>()` (or the existing `EntityWorld` iteration API) and draw each one. The mesh cache stays keyed by `ModelRecord*` — multiple aircraft sharing the same model (e.g. an F-16 flight) reuse the same `Mesh` objects, just with different model matrices.

## 5. Out of scope for Phase 2

Explicitly deferred to Phase 3+:

- **Sensors / weapons / radar display.** `SensorComponent` and `WeaponStoreComponent` still don't exist. Adding them is a separate, larger task (`AI_IMPLEMENTATION_PLAN.md`).
- **`DigitalBrain` orchestrator.** `BrainComponent` still wraps only `TakeoffModule`. The 26-priority DigiMode layered state machine (Landing/Nav/Refuel/Collision/BVR/WVR/Missile/Wingman) is Phase 3+.
- **Recording playback.** Replaying a `trace.json` without running the sim. The recorder schema is already forward-compatible (`entity_id` discriminator is there); the playback host is a separate executable.
- **Multiplayer / network sync.** FreeFalcon's `VuEntity` was designed for this; we haven't touched the network layer.

## 6. Acceptance criteria for Phase 2

1. `cmake --build build --target f4-simulation` and `--target f4-scenario-player` succeed.
2. New unit tests pass:
   - `test_scenario_from_world.cpp`: derive a `ScenarioAirfield` from a synthetic `ObjectiveState` with a runway list + parking list; verify threshold, runway-end, and taxi route.
   - `test_spawn_from_flights.cpp`: populate a `WorldState` with 2 squadrons + 3 flights, run `spawn_aircraft_from_flights`, verify 3 aircraft entities exist with the right components.
3. `Simulation` no longer has `aircraft_entity_` (singular); it has `aircraft_entities_` (vector). Old single-aircraft tests still pass via the legacy `spawn_mode: "scenario_list"` path.
4. `f4-scenario-player scenarios/kunsan_from_campaign.json` opens a window with multiple F-16s at multiple parking spots, all rendered, all ticking.
5. `--screenshot` produces a PNG with > 1 aircraft visible.

## 7. Implementation order

1. **§4.1 bridge** — `derive_airfield_from_objective()` + tests. Pure, no dependencies on anything new.
2. **§4.3 multi-aircraft `Simulation`** — refactor `aircraft_entity_` → `aircraft_entities_`. Mechanical; existing tests should still pass.
3. **§4.2 spawn-from-flights** — `spawn_aircraft_from_flights()` + tests. Depends on §4.3.
4. **§4.4 renderer iteration** — change `draw_aircraft()` to iterate. Depends on §4.3.
5. **Integration** — wire it all together in `PlayerApp::load_scenario()`. Build a `kunsan_from_campaign.json` scenario that loads `save1.world.json` + `Falcon4.CT` and uses `spawn_mode: "campaign_flights"`.
6. **Patch + ship.**

---

*This document is the Phase 2 plan. It supersedes the "deferred" items in `SCENARIO_PLAYER_PLAN.md` §4.3 and §8.*
