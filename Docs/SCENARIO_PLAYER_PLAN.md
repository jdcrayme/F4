# f4-scenario-player — Implementation Plan

> **Status**: Active plan. Replaces the earlier "f4-taxi-demo" concept.
> **Created**: 2026-08-09
> **Companion**: [Aircraft Binding Design](AIRCRAFT_BINDING_DESIGN.md), [Architecture Proposal §13](ARCHITECTURE%20PROPOSAL.md#13-f4-simulation--orchestration), [ECS Decoupling Plan](ECS_DECOUPLING_PLAN.md)

---

## 1. Why "f4-scenario-player", not "f4-taxi-demo"

The original working name was `f4-taxi-demo`. That name was misleading on two counts:

1. **Scope creep.** The same host that renders taxi also renders takeoff, landing, and (eventually) air-to-air. Naming the binary after the first scenario locks the architecture to one mode.
2. **It is not a demo.** A demo is throwaway. This host is the **engine-agnostic scenario runtime** that drives every future scenario — the equivalent of FreeFalcon's `SimDriver` + `RenderOTW` in one process, with the rendering backend swappable behind a trait.

`f4-scenario-player` says what it does: it loads a scenario file, plays it back, and renders it. Taxi is just the first scenario on the menu.

## 2. The first deliverable

The user's explicit ask: *"showing the aircraft and the airport geometry would be a good start."*

That means the v0 host must, at minimum:

- Spawn an F-16 at a parking spot on Kunsan airbase.
- Render the F-16 3D model (loaded from `KoreaObj.HDR/.LOD/.TEX`).
- Render the airport geometry around it: runway rectangle, runway threshold and far-end markers, the taxi route from parking to threshold, and the parking-spot marker.
- Run a Raylib window with an orbit/pan/zoom camera so the user can look around.
- Tick the simulation on each frame so the brain + flight model are alive, but pause-on-load so the aircraft sits at the parking spot until the user hits a key to start taxi.

That is the milestone. Once the F-16 and the airport geometry are both visible on screen, the rest of the work (taxi movement, takeoff rotation, recording playback) is incremental.

## 3. Architecture — what already exists

The repository already contains most of the pieces. This plan **does not** re-implement them; it wires them into a host executable.

| Layer | Crate | Status | Notes |
|---|---|---|---|
| Entity substrate | `f4-entities` | done, tested | `EntityWorld`, `Component<T>`, `TransformComponent`, `EntityHandle::get<T>()` |
| 6-DOF flight model | `f4-flight-model` | done, tested | `FlightModelComponent`, `AirframeClass`-equivalent; implements `IAircraftState` + `IPilotInputSink` |
| AI brain (taxi + takeoff) | `f4-ai` | done, tested | `BrainComponent` wraps `TakeoffModule`; uses sibling-lookup to find FM via interface |
| 3D model database | `f4-models` | done, tested | `ModelDatabase`, `ModelRecord`, BSP+DX parsers, `extract_model_geometry()` |
| Aircraft config | `f4-data` | done, tested | `AircraftConfig` + `loadConfig()` from JSON |
| ATC stub | `f4-ai::StubATC` | done, tested | Subscribes to `TaxiRequest` → emits `TaxiClearance` with route |
| Flight recorder | `f4-recorder` | done, tested | `FlightRecorder`, `FlightSnapshot` |
| JSON scenario loader | `f4-simulation::load_scenario` | done, tested | Parses scenario JSON, resolves asset paths |
| Orchestration | `f4-simulation::Simulation` | done, builds | Owns world + bus + model_db + recorder; `initialize()` / `tick()` / `write_recording()` |
| **Renderable handle** | `f4-simulation::VisualModelComponent` | **done**, tested | The new component — carries `const ModelRecord*` + LOD + DOF/switch state |
| Airport ground layout | `f4-world::WorldState` | done, tested | `GroundLayoutList` (runway/taxiway/parking) from `.cam` objectives |
| Renderer reference | `f4-models-viewer` | done, tested | Raylib `build_raylib_meshes()` + `DrawMesh()` + lit shader |

What does **not** exist yet:

- A **host executable** that combines `f4-simulation::Simulation` with `f4-models-viewer`'s renderer and adds airport geometry drawing.
- A real Kunsan scenario JSON with the airbase's actual ground layout (the test fixture uses placeholder coordinates).
- Exposure of `visType[7]` from `f4-world-convert/src/class_table.cpp` — currently the class-table parser reads the bytes but discards them. The scenario JSON carries the visual model index by hand, which works for v0.

## 4. Gap closure

Three small concrete changes were identified in the binding analysis. Status:

### 4.1 `visType[7]` exposure — DONE in this PR

`ClassTableEntry` now exposes `int16_t vis_type[7]`. The parser reads the 14-byte `visType[7]` array from each 81-byte CT record (offset 60, immediately after the 3-byte padding following `persistent_`). New accessor `ClassTable::vis_type_for(entity_type, slot=0)` returns the visual model index for a given entity type.

This is the **only** change required to close the data-flow gap from `Falcon4.CT` → `ModelDatabase`. The scenario JSON still uses a hand-authored `vis_type_index` for now — auto-derivation from `Flight`/`Squadron` units is a separate, larger task (§4.3).

### 4.2 `VisualModelComponent` — DONE (already in `f4-simulation`)

Already implemented, tested, and integrated with `Simulation::spawn_aircraft()`. See `f4-simulation/include/f4/simulation/visual_model_component.hpp`.

### 4.3 Aircraft spawning from campaign data — DEFERRED

Extending `f4-world::populate_units` to detect `Flight`/`Squadron` units (domain=air) and spawn child aircraft entities is a real task, but it is **not** required for the first deliverable. The scenario JSON approach is sufficient: the user hand-authors `aircraft[0]` with `callsign`, `vis_type_index`, `parking_spot`, `heading_rad`, and the simulation spawns exactly that aircraft. Auto-spawning becomes important when we want to populate the world with the full campaign roster (hundreds of aircraft across dozens of squadrons), which is a Phase 2 goal.

## 5. The host: `f4-scenario-player`

### 5.1 Directory layout

```
f4-scenario-player/
├── CMakeLists.txt
├── cli/
│   └── main.cpp                    # entry point: parse args, run player
├── include/f4/scenario_player/
│   ├── player_app.hpp              # PlayerApp public API (pimpl)
│   ├── airport_geometry.hpp        # AirportGeometry struct + loader
│   └── scenario_renderer.hpp       # owns Raylib window + draws scene
└── src/
    ├── player_app.cpp              # lifecycle: ctor/dtor/run
    ├── airport_geometry.cpp        # converts GroundLayoutList → line strips
    ├── scenario_renderer.cpp       # Raylib draw loop
    └── viewer_state.hpp            # private pimpl state
```

### 5.2 Public API

```cpp
namespace f4::scenario_player {

class PlayerApp {
public:
    PlayerApp();
    ~PlayerApp();

    /// Load a scenario from a JSON file. Throws on parse / asset load failure.
    void load_scenario(const std::filesystem::path& json_path);

    /// Run the render + sim loop until window close.
    void run();

    /// Schedule a single screenshot after `delay_sec` and exit.
    void schedule_screenshot(float delay_sec, const std::filesystem::path& path);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace f4::scenario_player
```

### 5.3 CLI

```
f4-scenario-player <scenario.json> [--screenshot out.png] [--width 1600] [--height 900]
```

If no scenario is given, the player prints a help message and exits. The `--screenshot` flag is for headless smoke tests in CI (same pattern as `f4-models-viewer`).

### 5.4 Render loop

Per frame:

1. `BeginDrawing()`; `ClearBackground` (sky-blue, not the viewer's neutral grey — we're outdoors now).
2. Update orbit camera from input.
3. If not paused, `sim.tick(dt)`.
4. `BeginMode3D(camera)`.
5. Draw the airport geometry:
   - **Runway**: a flat dark-grey rectangle from threshold to far end, with white threshold bars and centerline dashes.
   - **Taxi route**: yellow line strip from parking → hold short → threshold.
   - **Parking spot**: a small green cube at the F-16's spawn point.
6. Draw the F-16: convert `VisualModelComponent::model_record` → Raylib meshes (reuse `f4-models-viewer`'s `build_raylib_meshes`), apply the entity's transform (position + quaternion) as the model matrix, `DrawMesh` each.
7. Draw a 3D compass gizmo (N/E/S/W) for orientation reference.
8. `EndMode3D()`.
9. Draw a 2D HUD overlay: scenario name, sim time, FPS, tick count, paused state, callsign, KCAS.
10. `EndDrawing()`.

### 5.5 Coordinate conventions

The simulation uses **ENU feet** (east, north, up) for `TransformComponent::position`. Raylib uses **RH Y-up** (X right, Y up, Z toward viewer). Conversion: `raylib_x = enu_x`, `raylib_y = enu_z`, `raylib_z = -enu_y`. The F-16's model data is in **LH Y-up** (DirectX convention), so the per-vertex `to_raylib()` from `f4-models-viewer/src/viewer_state.hpp` applies at mesh-build time, and the entity's transform is applied as a parent matrix at draw time.

The aircraft's quaternion (`TransformComponent::qw/qx/qy/qz`) is body-to-world in ENU. We convert it to a Raylib `Quaternion` in RH Y-up by swapping axes (q stays a valid rotation; only the basis changes). The full transform matrix is then `MatrixTranslate(enu_to_raylib(pos)) * QuaternionToMatrix(q_rh)`.

### 5.6 Kunsan scenario fixture

A real Kunsan scenario needs the airbase's actual ground layout from the campaign. For the first deliverable we ship a hand-authored fixture (`scenarios/kunsan_parking.json`) that uses plausible coordinates:

- Parking spot: ENU (0, 0, 50) ft — south end of the field
- Runway 36 threshold: ENU (500, 8000, 50) ft
- Runway 36 far end: ENU (500, 13000, 50) ft
- Taxi route: 5 waypoints from parking to threshold
- vis_type_index: 1052 (F-16C Block 50, confirmed against `KoreaObj.HDR`)

A Phase 2 task will derive this fixture from `f4-world::populate_world()`'s `GroundLayoutList` data for the Kunsan objective. The data is already there — the bridge from `WorldState` to a `Scenario` is what's missing.

## 6. Build integration

The root `CMakeLists.txt` gets a new optional subdirectory, gated the same way as the existing viewers:

```cmake
option(F4_BUILD_SCENARIO_PLAYER "Build the f4-scenario-player (requires X11 + OpenGL dev headers)" ON)
if(F4_BUILD_SCENARIO_PLAYER)
    add_subdirectory(f4-scenario-player)
endif()
```

The player depends on `f4-simulation` (which transitively brings in `f4-entities`, `f4-flight-model`, `f4-ai`, `f4-models`, `f4-data`, `f4-recorder`, `f4-messaging`, `f4-state-machine`, `f4-json`, `f4-io`, `f4-geo`, `f4-math`, `f4-units`) plus `f4-world` (for `GroundLayoutList` types if we want to load real airbase layouts later) and Raylib + Dear ImGui (fetched the same way as in `f4-models-viewer`).

## 7. Test strategy

- **Unit tests** (`f4-scenario-player/tests/`):
  - `test_airport_geometry.cpp`: convert a synthetic `GroundLayoutList` to line strips, verify endpoints.
  - `test_coordinate_transform.cpp`: verify ENU → Raylib transform for known points.
- **Smoke test** (CI): run `f4-scenario-player --screenshot out.png scenarios/kunsan_parking.json`, assert PNG is non-empty and > 10 KB (basic "we rendered something" check). Same pattern as `f4-models-viewer`'s screenshot smoke test.
- **Headless run** (no GL context): `f4-simulation::Simulation` is already headless-capable (it's a pure library with no rendering hooks). The `--no-window` flag (Phase 2) will run the sim to completion and dump the trace JSON without ever opening a window. Useful for batch scenario validation.

## 8. Out of scope for v0

These are explicitly **not** in the first deliverable. Listed here so they don't get forgotten:

- Auto-derivation of scenarios from campaign data (`Flight`/`Squadron` units → spawned aircraft).
- Real Kunsan ground layout from `f4-world` (the fixture is hand-authored).
- Takeoff rotation and climb-out (the brain supports it; the renderer just needs to keep drawing).
- Multiple aircraft (the `Simulation` currently tracks one `aircraft_entity_`; supporting N is a small refactor of the sync loop).
- Sensors / weapons / radar display.
- Recording playback (replaying a `trace.json` without running the sim).
- Multiplayer / network sync (FreeFalcon's `VuEntity` was designed for this; we haven't touched the network layer yet).

## 9. Acceptance criteria for v0

1. `cmake --build build --target f4-scenario-player` succeeds.
2. `./build/f4-scenario-player/f4-scenario-player scenarios/kunsan_parking.json` opens a Raylib window.
3. The window shows:
   - An F-16 3D model sitting on the parking spot.
   - A runway rectangle in the distance.
   - A yellow taxi-route line from parking to threshold.
   - A small green cube marking the parking spot.
4. Orbit/pan/zoom camera works (left-drag, right-drag, scroll).
5. Spacebar toggles pause. When unpaused, the F-16 begins to taxi along the route.
6. The HUD shows scenario name, callsign, KCAS, sim time, and paused state.
7. `--screenshot` produces a valid PNG.

Once all seven pass, the first deliverable is complete and the next phase (real Kunsan layout, takeoff, recording playback) can begin.

---

*This document replaces the earlier `F4_TAXI_DEMO_PLAN.md`. The architecture described here is implemented across `f4-simulation` (orchestration), `f4-scenario-player` (host), and the gap-closure change to `f4-world-convert/src/class_table.cpp` (visType[7] exposure).*
