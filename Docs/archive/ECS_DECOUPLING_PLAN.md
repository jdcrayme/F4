# ECS Decoupling Plan — Decoupling World Implementation from Legacy File Formats

> **Status**: Complete — Phases 1–4 all done ✅  
> **Created**: 2025-08-05  
> **Completed**: 2026-08-05  
> **Companion**: [Architecture Proposal §8](ARCHITECTURE%20PROPOSAL.md#8-f4-entities--entity-system), [§18.5](ARCHITECTURE%20PROPOSAL.md#185-the-world-data-parser-new-milestone)  
> **Goal**: Make `EntityWorld` the runtime representation of the game world, with `WorldState` as a private implementation detail of the format bridge. **ACHIEVED**.

---

## 1. Problem Statement

The current data pipeline is:

```
binary .cam archive ──f4-world-convert──→ JSON ──f4-world──→ WorldState ──world_loader──→ EntityWorld
```

`WorldState` is a 1:1 mirror of the binary file layout, enriched with theater static data. This creates three specific coupling problems:

### 1.1 Field Names Are Format Names

`ObjectiveState::id_creator` / `id_num` are `VU_ID` — a FreeFalcon wire-format concept that has leaked into the domain model. `nameid` is an index into the binary name table. `obj_flags` is an opaque bitmap from the `.obj` sub-file. Every consumer of `WorldState` must understand these format-derived identifiers.

### 1.2 The Unit God-Struct

`UnitState` is a single ~400-line struct containing all subclass-specific fields for all six unit types (Battalion, Brigade, Squadron, TaskForce, Flight, Package), with comments like "only populated for the matching unit_class". This is the tagged-union anti-pattern implemented as a flat struct — the exact problem `f4-entities` was designed to solve (§8.1: "Replaces deep inheritance... with a lightweight ID handle + typed components").

### 1.3 Struct Growth Tied to Reverse-Engineering

`ObjectiveState` and `UnitState` carry comments like `// Phase 1 fix`, `// Phase 3`, `// NOT yet parsed`. Every newly decoded binary field forces a struct change, which recompiles everything that includes `world_state.hpp`. The struct's shape is driven by what has been reverse-engineered so far, not by what the domain model needs.

### 1.4 The Bridge Is Incomplete

Only `populate_teams()` exists. Objectives (2,659 per Korea save) and units (683 per Korea save) remain in `WorldState`'s vectors — they never become ECS entities. Anything that needs to reason about objectives or units (AI pathfinding, radar coverage, supply chains) must operate directly against format-derived structs.

---

## 2. Design Principles

### 2.1 Components Are Named After Domain Concepts, Not File Fields

An objective has a `SupplyStateComponent`, not `supply` + `fuel` + `losses` fields named after the `.obj` binary layout. An objective's identity is an `EntityId`, not a `VU_ID` `(creator, num)` pair.

### 2.2 Components Are Added Conditionally

Not every objective has radar. Not every objective has a ground layout. Not every unit has a waypoint plan. The ECS's `has<T>()` check replaces `if (field != 0)` guards on a god-struct. Systems query for the components they need; absence is not an error.

### 2.3 The Bridge Owns Format Knowledge

`world_loader.cpp` is the only place where `WorldState` fields are translated into domain components. Format concepts (`VU_ID`, `nameid`, `obj_flags`) are resolved or discarded here — they do not propagate into `EntityWorld`.

### 2.4 PropertyBag for Unstable Data

Fields still being reverse-engineered, fields that vary across Falcon versions (v63 vs v71), or fields without a clear domain meaning go into `PropertyBag` — a typed key-value component. Once a field is proven and has a domain meaning, it is promoted to a proper component.

### 2.5 Format Adapter Interface (Future)

The bridge functions operate against `IObjectiveSource` / `IUnitSource` interfaces, not `WorldState` directly. This allows future data sources (BMS saves, DIS streams, procedural generation) without touching bridge code. `WorldState` becomes an implementation detail of one concrete adapter.

---

## 3. Phase 1 — Decompose `CampaignIdentityComponent` and Add `TeamComponent`

**Scope**: Fix the "component serving two roles" smell. Establish the pattern that every entity kind gets its own focused component.

### 3.1 Changes

| File | Action |
|------|--------|
| `f4-entities/include/f4/entities/entity.hpp` | Add `TeamComponent`. Narrow `CampaignIdentityComponent` to `team_id` + `callsign` (remove `unit_type_name`). |
| `f4-world/src/world_loader.cpp` | Replace `CampaignIdentityComponent` usage with `TeamComponent`. |
| `f4-world/tests/test_world_loader.cpp` | Update assertions. |
| `f4-entities/tests/test_entity.cpp` | Add tests for `TeamComponent`. |

### 3.2 New Component

```cpp
struct TeamComponent : Component<TeamComponent> {
    int slot = 0;                    // 0..7
    uint8_t flags = 0;
    uint8_t colour = 0;
    std::string motto;
    std::vector<int16_t> stance;     // stance toward each other team
    std::vector<uint8_t> member;     // country memberships
    uint8_t air_experience = 0;
    uint8_t ground_experience = 0;
    uint8_t naval_experience = 0;
    uint8_t air_defense_experience = 0;
    int16_t first_colonel = 0;
    int16_t first_commander = 0;
    int16_t first_wingman = 0;
    int16_t last_wingman = 0;
};
```

### 3.3 Narrowed Component

```cpp
struct CampaignIdentityComponent : Component<CampaignIdentityComponent> {
    int team_id = 0;
    std::string callsign;
};
```

### 3.4 Gate

All existing tests pass. New `TeamComponent` tests pass. `populate_teams()` sets `TeamComponent` instead of `CampaignIdentityComponent`.

---

## 4. Phase 2 — Define Domain Components for Objectives and Units

**Scope**: Additive only — no existing code changes. New type definitions that the bridge (Phase 3) will use.

### 4.1 Step 2a — Move Shared Types

`ObjectiveLink`, `GroundLayoutList`, `GroundLayoutPoint`, `FeatureEntryState`, `FeatureClassState`, `WaypointState`, `PilotState`, `VehicleGroup`, and the `UnitClass`/`ObjectiveType` enums currently live in `world_state.hpp`. Move them to `f4-entities/include/f4/entities/types.hpp` since components reference them.

| File | Action |
|------|--------|
| `f4-entities/include/f4/entities/types.hpp` | **New file** — shared types moved here |
| `f4-entities/include/f4/entities/f4_entities.hpp` | Include `types.hpp` |
| `f4-world/include/f4/world/world_state.hpp` | Include `types.hpp` instead of defining inline |
| `f4-world-convert/include/f4/world_convert/objective_decoder.hpp` | Include `types.hpp` for `ObjectiveLink`, `ObjectiveType` |

### 4.2 Step 2b — Objective Components

| Component | Fields | Condition |
|-----------|--------|-----------|
| `ObjectiveTypeComponent` | `type`, `class_table_index`, `class_name` | Always |
| `OwnershipComponent` | `team`, `first_owner` | Always |
| `SupplyStateComponent` | `supply`, `fuel`, `losses`, `last_repair` | When supply data exists |
| `DamageBitmapComponent` | `fstatus` (raw 2-bit-per-feature bitmap) | When features have damage |
| `RadarComponent` | `detect_ratio[8]`, `range_km`, `name`, `radar_type_idx` | When objective has radar |
| `NetworkLinksComponent` | `links` (road/rail connections) | When objective has links |
| `GroundLayoutComponent` | `layouts` (runway/taxiway/parking) | When objective is an airbase |
| `FeatureSetComponent` | `features` (buildings/structures) | When objective has features |
| `ObjectivePriorityComponent` | `priority`, `nameid` | Always |

### 4.3 Step 2b — Unit Components

| Component | Applies to | Fields |
|-----------|-----------|--------|
| `UnitCoreComponent` | All units | `unit_class`, `domain`, `unit_subtype`, `class_table_index`, `roster`, `class_name` |
| `WaypointPlanComponent` | Units with waypoints | `waypoints` |
| `GroundTacticalComponent` | Battalion, Brigade, TaskForce | `supply`, `morale`, `fatigue`, `heading`, `final_heading`, `position`, `last_move`, `last_combat` |
| `HierarchyComponent` | Battalion, Brigade | `parent`, `children` |
| `SquadronComponent` | Squadron | `airbase`, `specialty`, kills, `fuel`, `pilots`, losses |
| `FlightPlanComponent` | Flight | `altitude`, `fuel_burnt`, `time_on_target`, `mission`, `package`, `squadron`, callsign |
| `PackageSupportComponent` | Package | `wait_cycles`, `interceptor`, `awacs`, `jstar`, `ecm`, `tanker` |
| `VehicleCompositionComponent` | Units with vehicle groups | `groups` |
| `UnitClassScoreComponent` | All units | `scores[16]` |

### 4.4 Step 2b — Utility Components

```cpp
struct PropertyBag : Component<PropertyBag> {
    std::unordered_map<std::string, int64_t>    ints;
    std::unordered_map<std::string, double>     floats;
    std::unordered_map<std::string, std::string> strings;
};

struct CampaignStateComponent : Component<CampaignStateComponent> {
    int32_t current_time = 0;
    int32_t te_start_time = 0;
    int32_t te_time_limit = 0;
    int32_t te_victory_points = 0;
    int32_t te_type = 0;
    int32_t te_number_teams = 0;
    int32_t te_team = 0;
    int32_t te_flags = 0;
    std::vector<int32_t> te_number_aircraft;
    std::vector<int32_t> te_team_pts;
};
```

### 4.5 Step 2c — Component Tests

Add compile-and-instantiate tests in `test_entity.cpp` for each new component type.

### 4.6 Gate

All existing tests pass. New component types compile and can be added/queried on entities.

---

## 5. Phase 3 — Write the Bridge Functions

**Scope**: Connect `WorldState` → `EntityWorld` for all entity types.

### 5.1 New API

```cpp
// f4-world/include/f4/world/world_loader.hpp

struct PopulatedWorld {
    EntityId campaign;
    std::vector<EntityId> teams;
    std::vector<EntityId> objectives;
    std::vector<EntityId> units;
};

std::vector<EntityId> populate_campaign(EntityWorld& world, const WorldState& ws);
std::vector<EntityId> populate_teams(EntityWorld& world, const WorldState& ws);      // existing, updated
std::vector<EntityId> populate_objectives(EntityWorld& world, const WorldState& ws);
std::vector<EntityId> populate_units(EntityWorld& world, const WorldState& ws,
                                     const std::unordered_map<uint32_t, EntityId>& obj_id_map);
PopulatedWorld populate_world(EntityWorld& world, const WorldState& ws);
```

### 5.2 Bridge Logic — Objectives

For each `ObjectiveState` in `ws.objectives`:

1. `world.create()` → set tags: `role="objective"`, `team=<name>`, `alive=true`
2. Add `TransformComponent` with `grid_to_feet(x, y, z)`
3. Add `ObjectiveTypeComponent`, `OwnershipComponent`, `ObjectivePriorityComponent`
4. Conditionally add: `SupplyStateComponent`, `DamageBitmapComponent`, `RadarComponent`, `NetworkLinksComponent`, `GroundLayoutComponent`, `FeatureSetComponent`
5. Add `PropertyBag` with format residue: `vu_id_creator`, `vu_id_num`, `entity_type`, `obj_flags`

Return a map: `VU_ID.num → EntityId` (used by `populate_units` to resolve cross-references).

### 5.3 Bridge Logic — Units

For each `UnitState` in `ws.units`:

1. `world.create()` → set tags: `role=<unit_class_name>`, `team=<name>`, `alive=true`
2. Add `TransformComponent` with `grid_to_feet(x, y, z)`
3. Add `UnitCoreComponent`
4. Add subclass-specific component based on `unit_class`:
   - Battalion/Brigade/TaskForce → `GroundTacticalComponent` + optional `HierarchyComponent`
   - Squadron → `SquadronComponent`
   - Flight → `FlightPlanComponent`
   - Package → `PackageSupportComponent`
5. Conditionally add: `WaypointPlanComponent`, `VehicleCompositionComponent`, `UnitClassScoreComponent`
6. Add `PropertyBag` with format residue

**Second pass**: Resolve `EntityId` cross-references (battalion→brigade, flight→package, flight→squadron, squadron→airbase) using the VU_ID→EntityId maps.

### 5.4 Bridge Logic — Campaign

Create a single entity with `CampaignStateComponent` and tag `role="campaign"`.

### 5.5 Tests

| Test | What it verifies |
|------|-----------------|
| `PopulateObjectives_Count` | Entity count == `ws.objectives.size()` |
| `PopulateObjectives_AllHaveTransform` | Every objective has `TransformComponent` with correct grid→feet |
| `PopulateObjectives_ConditionalRadar` | `h.has<RadarComponent>()` iff `o.has_radar` |
| `PopulateObjectives_NetworkLinksRoundTrip` | Link count, neighbor IDs, is_road/is_rail preserved |
| `PopulateUnits_Count` | Entity count == `ws.units.size()` |
| `PopulateUnits_SubclassComponents` | Battalion has `GroundTacticalComponent`, Flight has `FlightPlanComponent`, etc. |
| `PopulateUnits_HierarchyResolved` | Battalion's `HierarchyComponent::parent` → correct Brigade |
| `PopulateUnits_SquadronAirbaseResolved` | `SquadronComponent::airbase` → correct objective |
| `PopulateWorld_RealFixture` | End-to-end: JSON → WorldState → populate → 2,659 objectives, 683 units, 8 teams |

### 5.6 Gate

All tests pass. Every entity type from `WorldState` has a corresponding ECS entity. No consumer needs to touch `WorldState` directly — they work through `EntityWorld`.

---

## 6. Phase 4 — Slim Down `WorldState` and Add Format Adapter Interface

**Scope**: Make `WorldState` a private implementation detail. Open the door to alternative data sources.

### 6.1 Step 4a — Reduce `WorldState` to Private Header

Move `world_state.hpp` from `include/f4/world/` to `src/`. The public API of `f4-world` becomes:

```cpp
// f4-world/include/f4/world/f4_world.hpp
#pragma once
#include <f4/entities/entity.hpp>
#include <filesystem>
#include <string>

namespace f4::world {

struct PopulatedWorld { /* ... */ };

/// Load a world JSON file and populate an EntityWorld.
PopulatedWorld load(const std::filesystem::path& json_path, EntityWorld& world);

/// Load from an in-memory JSON string (for testing).
PopulatedWorld load_from_string(const std::string& json, EntityWorld& world);

} // namespace f4::world
```

### 6.2 Step 4b — Format Adapter Interface

```cpp
// f4-world/include/f4/world/data_source.hpp
struct IObjectiveSource {
    virtual int count() const = 0;
    virtual int16_t type(int i) const = 0;
    virtual f4::geo::WorldPosition position(int i) const = 0;
    virtual uint8_t owner(int i) const = 0;
    virtual bool has_supply(int i) const = 0;
    // ... expand as needed
    virtual ~IObjectiveSource() = default;
};

struct IUnitSource { /* analogous */ };
struct ITeamSource { /* analogous */ };
struct ICampaignSource { /* analogous */ };
```

Bridge functions change signature:

```cpp
// Before (Phase 3):
PopulatedWorld populate_world(EntityWorld&, const WorldState&);

// After (Phase 4):
PopulatedWorld populate_world(EntityWorld&,
                              ICampaignSource&, ITeamSource&,
                              IObjectiveSource&, IUnitSource&);
```

`WorldStateDataSource` wraps `WorldState` and implements all four interfaces. Future adapters (BMS, DIS, procedural) plug in without touching bridge code.

### 6.3 Step 4c — Update Viewer

Replace all direct `WorldState` access with `EntityWorld` queries:

| Before | After |
|--------|-------|
| `ws.objectives[i].x` | `h.get<TransformComponent>()->position.x` |
| `ws.objectives[i].owner` | `h.get<OwnershipComponent>()->team` |
| `ws.objectives[i].has_radar` | `h.has<RadarComponent>()` |
| `ws.units[i].unit_class == UnitClass::Flight` | `h.get<UnitCoreComponent>()->unit_class == UnitClass::Flight` |

### 6.4 Gate

All tests pass. Viewer compiles and runs correctly. `WorldState` is not in any public header. The adapter interface compiles and the `WorldStateDataSource` passes the same tests as the direct-bridge approach.

---

## 7. Commit Cadence

| Commit | Phase | Description |
|--------|-------|-------------|
| 1 | 1 | Add `TeamComponent`, narrow `CampaignIdentityComponent`, update `populate_teams` and tests |
| 2 | 2a | Move shared types to `f4-entities/include/f4/entities/types.hpp` |
| 3 | 2b | Add all domain component struct definitions |
| 4 | 2c | Add component instantiation tests |
| 5 | 3a | Implement `populate_objectives()` + tests |
| 6 | 3b | Implement `populate_units()` + two-pass ID resolution + tests |
| 7 | 3c | Implement `populate_campaign()` + `populate_world()` + end-to-end test on real fixture |
| 8 | 4a | Move `WorldState` to private header, refactor public API to `load()`/`load_from_string()` |
| 9 | 4b | Add `IDataSource` adapter interfaces + `WorldStateDataSource` |
| 10 | 4c | Update viewer to consume `EntityWorld` instead of `WorldState` |

Each commit is a clean compile + all tests green. Commits 1, 7, and 10 are natural "ship" boundaries.

---

## 8. What This Unlocks

After Phase 3 (minimum viable cleanup):

- **f4-ai**: pathfinding via `with_component<NetworkLinksComponent>()`, threat assessment via `with_tag(tags::TEAM, "red") + with_component<RadarComponent>()`, OOB traversal via `HierarchyComponent`
- **f4-simulation**: tick loop iterates `with_component<TransformComponent>()` for movement, `with_component<GroundTacticalComponent>()` for ground combat, etc.
- **New entity types without WorldState changes**: a weather front is `world.create() + TransformComponent + WeatherComponent`
- **Alternative data sources (Phase 4)**: BMS campaign saves, DIS/HLA network streams, procedurally generated scenarios

---

## 9. Entity Kind Taxonomy

After completion, the entity kinds and their component footprints are:

| Kind | Tags | Components (always) | Components (conditional) | Has position? |
|------|------|---------------------|--------------------------|---------------|
| Campaign | `role="campaign"` | `CampaignStateComponent` | — | No |
| Team | `role="team"`, `team=<name>` | `TeamComponent`, `CampaignIdentityComponent` | — | No |
| Objective | `role="objective"`, `team=<name>` | `TransformComponent`, `ObjectiveTypeComponent`, `OwnershipComponent`, `ObjectivePriorityComponent` | `SupplyStateComponent`, `DamageBitmapComponent`, `RadarComponent`, `NetworkLinksComponent`, `GroundLayoutComponent`, `FeatureSetComponent` | Yes |
| Battalion | `role="battalion"`, `team=<name>`, `domain="ground"` | `TransformComponent`, `UnitCoreComponent`, `GroundTacticalComponent` | `HierarchyComponent`, `WaypointPlanComponent`, `VehicleCompositionComponent` | Yes |
| Brigade | `role="brigade"`, `team=<name>`, `domain="ground"` | `TransformComponent`, `UnitCoreComponent`, `GroundTacticalComponent`, `HierarchyComponent` | `WaypointPlanComponent` | Yes |
| Squadron | `role="squadron"`, `team=<name>`, `domain="air"` | `TransformComponent`, `UnitCoreComponent`, `SquadronComponent` | — | Yes (at airbase) |
| Flight | `role="flight"`, `team=<name>`, `domain="air"` | `TransformComponent`, `UnitCoreComponent`, `FlightPlanComponent` | `WaypointPlanComponent` | Yes |
| Package | `role="package"`, `team=<name>`, `domain="air"` | `TransformComponent`, `UnitCoreComponent`, `PackageSupportComponent` | — | Yes (at lead flight) |
| TaskForce | `role="taskforce"`, `team=<name>`, `domain="naval"` | `TransformComponent`, `UnitCoreComponent`, `GroundTacticalComponent` | — | Yes |

No class hierarchy. No "global entity" special case. An entity is just an ID + tags + whatever components it needs. The presence or absence of components *is* the type system.
