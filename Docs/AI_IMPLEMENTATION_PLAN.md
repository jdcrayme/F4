# F4 AI Implementation Plan — f4-ai

> **Status**: Draft — For implementation reference
> **Source of Truth**: [FreeFalcon/freefalcon-central](https://github.com/FreeFalcon/freefalcon-central) (develop branch)
> **Companions**: [Architecture Proposal](ARCHITECTURE%20PROPOSAL.md) §12, [FreeFalcon Core Systems Reference](FreeFalcon_Core_Systems_Reference.html), [Falcon4 File Layout](FALCON4_FILE_LAYOUT.md)
> **Predecessor Lessons**: F4Flight `digi/` module — see §1.5

---

## Table of Contents

- [1. Goals & Non-Goals](#1-goals--non-goals)
- [2. Sequencing Rationale](#2-sequencing-rationale)
- [3. FreeFalcon Reference — DigitalBrain Anatomy](#3-freefalcon-reference--digitalbrain-anatomy)
- [4. f4-ai Architecture](#4-f4-ai-architecture)
- [5. Implementation Steps](#5-implementation-steps)
- [6. FreeFalcon Validation Mapping](#6-freefalcon-validation-mapping)
- [7. Testing Strategy](#7-testing-strategy)
- [8. Observability & Tracing](#8-observability--tracing)
- [9. Skill Level System](#9-skill-level-system)
- [10. Simple vs Complex Flight Model Selection](#10-simple-vs-complex-flight-model-selection)
- [11. Range Bands & Mode Transitions](#11-range-bands--mode-transitions)
- [12. Message Types](#12-message-types)
- [13. Directory Layout & Build](#13-directory-layout--build)
- [14. Risks & Mitigations](#14-risks--mitigations)

---

## 1. Goals & Non-Goals

### Goals

1. **Engine-agnostic AI brain**: The AI produces `AIControlOutput` (stick/throttle/fire flags) and consumes only the abstract interfaces of f4-entities, f4-geo, f4-messaging, and f4-flight-model. No rendering, no DirectX, no Falcon-specific globals.

2. **Validated against FreeFalcon**: Every behavior module has a one-to-one mapping to FreeFalcon source (file, function, enum value). The plan specifies exactly what to validate and how.

3. **Procedural before tactical**: Takeoff, landing, navigation, and air refueling are implemented before BVR/WVR. These are deterministic, measurable, and exercise the full integration stack — they prove the infrastructure before the judgment-based combat AI depends on it.

4. **Ground truth, not injection harnesses**: The AI always operates against `EntityWorld` populated from real campaign data (via f4-world). No synthetic target injection for anything beyond unit-level component tests.

5. **Observable by default**: Every state machine transition, mode change, and weapon decision produces greppable text traces via f4-state-machine's `Trace`.

### Non-Goals

1. **Not a line-by-line port**: FreeFalcon's `digi.h` is 1209 lines of god-class. We decompose it into focused modules with narrow interfaces.

2. **Not F4Flight v2**: F4Flight's `digi/` reached 50 headers / 26 sources / 58 test scenarios before the rewrite failed due to accumulation of complexity without observability. We design abstractions first, validate against small consumers, then build domain code on top.

3. **Not a UI/rendering system**: f4-ai is a library. Visualization is the consumer's problem (f4-world-viewer exists for interactive debugging).

4. **Not networking (f4-dis)**: DIS/HLA networking is a future library that composes with f4-ai but is not part of it.

### 1.1 The F4Flight Lesson (Architecture Proposal §18.6)

Three specific failures inform this plan:

| F4Flight Failure | F4 Remedy | Plan Enforcement |
|------------------|-----------|-----------------|
| Complexity accumulated faster than it could be observed | Design abstractions first, validate against small real consumers (stall SM pattern) | §5 requires each module to have a working trace + validation test before the next module starts |
| HTML/screenshot visualization was a dead end | Text traces: greppable, diffable, committable | §8 mandates trace attachment for every SM and mode-change logging |
| Test harnesses *became the AI's world* | Real world data via f4-world | §6.1 requires integration tests against `save1.world.json` |

---

## 2. Sequencing Rationale

The original roadmap (Architecture Proposal §17) sequenced AI after campaign. The revised sequence (§18.4) puts AI before campaign. This plan further refines the AI-internal order based on the **procedural-before-tactical** principle:

```
Procedural (deterministic, measurable)          Tactical (judgment-based, harder to validate)
─────────────────────────────────────────>      ────────────────────────────────────────>
Takeoff → Landing → Navigation → AAR →          BVR → WVR → Missile → Wingman → DigitalBrain
```

**Why this order specifically:**

1. **SensorFusion first** — every module needs a target list. This is non-negotiable infrastructure.

2. **Takeoff before Landing** — takeoff is a 2–3 state SM that validates the AI→FlightModel control path (`PilotInput`). Landing is a 17-state SM that validates approach geometry, gear/flap scheduling, and go-around logic. Takeoff proves the path exists; landing proves it works at precision.

3. **Navigation before AAR** — waypoint following is the simplest consumer of the geo→state-machine→messaging stack. AAR is precision formation flying, which is navigation-plus-formation. You need working navigation before you can hold position relative to a moving tanker.

4. **AAR before BVR** — AAR proves formation discipline at high precision. BVR formation (wingman spacing during a crank) is the same skill at lower precision. If AAR works, BVR formation will too. If AAR doesn't work, BVR formation will silently degrade and you'll blame the wrong thing.

5. **BVR before WVR** — BVR has clearer success criteria (maintain weapon envelope, fire at correct range, separate after shot). WVR is reactive with high behavioral variance. Validate the combat infrastructure with the more constrained problem first.

6. **Wingman after BVR/WVR** — wingman depends on proven formation (from AAR), proven comms (from ATC), and proven combat tactics (from BVR/WVR). It composes all of them.

7. **DigitalBrain last** — it's the orchestrator that composes all modules. Every module must be individually validated before composition.

---

## 3. FreeFalcon Reference — DigitalBrain Anatomy

This section catalogs the exact FreeFalcon structures that f4-ai must replicate or replace. It is the validation ground truth.

### 3.1 Class Hierarchy

| FreeFalcon Class | File | Lines | f4-ai Replacement |
|-------------------|------|-------|-------------------|
| `BaseBrain` | simbrain.h | 53 | `IAIBrain` interface |
| `DigitalBrain` | digi.h | 1209 | `DigitalBrain` (composed modules) |
| `FACBrain` | facbrain.cpp | — | Future `FACBrain` (out of scope) |

### 3.2 FrameExec() — Per-Frame Driver (digimain.cpp:566)

This is the heartbeat. The f4-ai equivalent is `DigitalBrain::update(dt)`:

```
FreeFalcon FrameExec:                    f4-ai DigitalBrain::update(dt):
  1. Clamp controls                       1. Clamp controls
  2. Fuel check → eject                   2. Fuel/emergency check
  3. Gear up after takeoff WP             3. (NavigationModule responsibility)
  4. Clear fire flags                     4. Clear fire flags
  5. CheckLead()                          5. (WingmanModule responsibility)
  6. Set target update rate               6. SensorFusion::update_interval(skill)
  7. DoTargeting()                        7. SensorFusion::update(dt)
  8. SetCurrentTactic() → DecisionLogic() 8. mode_ladder_.process(e) → dispatch
  9. Actions()                            9. active_module_->update(dt) → AIControlOutput
  10. Monitor ataDot/rangeDot             10. SensorFusion::monitor_geometry()
  11. Clamp controls                      11. Clamp controls
```

### 3.3 DigiMode Enum — 26 Values with Priority

The priority ladder is implemented as a `LayeredStateMachine` (f4-state-machine). Each concern is its own layer with an idle state:

| Priority | FreeFalcon Mode | f4-ai Layer | Idle State |
|----------|----------------|-------------|------------|
| 0 | TakeoffMode | TakeoffModule | `TakeoffState::Done` |
| 1 | GroundAvoidMode | GroundAvoidModule | `GAState::Clear` |
| 2 | CollisionAvoidMode | CollisionAvoidModule | `CAState::Clear` |
| 3 | GunsJinkMode | (part of WVRModule) | `WVRState::None` |
| 4 | MissileDefeatMode | MissileDefeatModule | `MDState::None` |
| 5 | LandingMode | LandingModule | `ATCState::NoATC` |
| 6 | DefensiveModes | (part of WVRModule) | `WVRState::None` |
| 7 | RefuelingMode | RefuelModule | `RefuelState::NoTanker` |
| 8 | SeparateMode | (part of WVRModule) | `WVRState::None` |
| 9–10 | Accel/Merge | (part of WVRModule) | `WVRState::None` |
| 11–12 | Missile/Guns Engage | (part of BVRModule/WVRModule) | idle |
| 13–14 | Roop/OverB | (part of WVRModule) | `WVRState::None` |
| 15 | WVREngageMode | WVRModule | `WVRState::None` |
| 16 | BVREngageMode | BVRModule | `BVRState::None` |
| 17 | LoiterMode | NavigationModule | `NavState::OnStation` |
| 18 | FollowOrdersMode | NavigationModule | `NavState::FollowingOrders` |
| 19 | RTBMode | NavigationModule | `NavState::RTB` |
| 20 | WingyMode | WingmanModule | `WingState::Following` |
| 21 | BugoutMode | NavigationModule | `NavState::None` |
| 22 | WaypointMode | NavigationModule | `NavState::Waypoint` |

**AddMode() override rules** (dlogic.cpp:729):
- LandingMode cannot override defensive/engagement modes (except MissileDefeat)
- BugoutMode is sticky — only MissileDefeat can override it
- Can't enter WVREngage if already Landing
- Engagement modes override LandingMode
- Default: lower priority number wins (higher priority)

### 3.4 WaypointState Enum (17 values)

```
NotThereYet, Arrived, Stabalizing, OnStation, PreRoll,
Departing, HoldingShort, HoldInPlace, TakeRunway, Takeoff,
Taxi, Upwind, Crosswind, Downwind, Base, Final, Final1
```

### 3.5 BVR Profiles & Intercept Types

**18 BVR Profiles**: Plevel1a, Plevel1b, Plevel2a, Plevel2b, Plevel3a, Plevel3c, Pbeamdeploy, Pbeambeam, Pwall, Pgrinder, Pwideazimuth, Pshortazimuth, PwideLT, PShortLT, PSweep, PDefensive

**20 Intercept Types**: BvrFollowWaypoints, BvrFlyFormation, BvrSingleSideOffset, BvrPince, BvrPursuit, BvrNoIntercept, BvrPump, BvrCrank, BvrCrankRight/Left, BvrNotch, BvrNotchRight/Left, BvrNotchRightHigh/LeftHigh, BvrGrind, BvrCrankHi/Lo, BvrCrankRightHi/Lo, BvrCrankLeftHi/Lo

### 3.6 WVR Tactics (11 values)

```
WVR_NONE, WVR_RANDP, WVR_OVERB, WVR_ROOP, WVR_GUNJINK, WVR_STRAIGHT,
WVR_BUGOUT, WVR_AVOID, WVR_BEAM, WVR_BEAM_RETURN, WVR_RUNAWAY
```

### 3.7 Landing/ATC States (digi_landme.cpp, ~4778 lines)

**Landing states (19)**: noATC, lReqClearance, lReqEmerClearance, lIngressing, lTakingPosition, lAborted, lEmerHold, lHolding, lFirstLeg, lToBase, lToFinal, lOnFinal, lClearToLand, lLanded, TaxiOff, lEmergencyToBase, lEmergencyToFinal, lEmergencyOnFinal, lCrashed

**Takeoff states (11)**: tReqTaxi, tReqTakeoff, tEmerStop, tTaxi, tWait, tHoldShort, tPrepToTakeRunway, tTakeRunway, tTakeoff, tFlyOut, tTaxiBack

### 3.8 Air Refueling States (5 values)

```
refNoTanker → refVectorTo → refWaiting → refRefueling → refDone
```

Source: digi_refuel.cpp (~1030 lines)

### 3.9 ATCFlags (32-bit bitmask)

```
Landed=0x01, PermitRunway=0x02, PermitTakeoff=0x04, HoldShort=0x08,
EmerStop=0x10, TakeoffAborted=0x20, MissionCanceled=0x40,
RequestTakeoff=0x80, Refueling=0x100, NeedToRefuel=0x200,
ClearToLand=0x400, PermitTakeRunway=0x800, WingmanReady=0x1000,
AceGunsEngage=0x2000, SaidJoker=0x4000, SaidBingo=0x8000,
SaidFumes=0x10000, SaidFlameout=0x20000, HasTrainable=0x40000,
FireTrainable=0x80000, AskedToEngage=0x100000, ReachedIP=0x200000,
HasAGWeapon=0x400000, OnSweep=0x800000, InShootShoot=0x1000000,
CheckTaxiBack=0x2000000, WaitingPermission=0x4000000,
StopPlane=0x8000000, SaidRTB=0x10000000, HasCanUseAGWeapon=0x20000000,
WaitForTarget=0x40000000, DonePreflight=0x80000000
```

### 3.10 Collision Avoidance Constants

```
hRange = 200 ft
hRangeSq = 40000
reactFact = 0.55
GS_LIMIT = 9.0 G
reactTime = (GS_LIMIT / maxGs) * reactFact
```

Source: digi_cavoid.cpp

### 3.11 Maneuver Data

9×9 table from `sim\acdata\brain\mnvrdata.dat`, indexed by `ACMnverClass`:

```
MnvrClassF4, MnvrClassF5, MnvrClassF14, MnvrClassF15, MnvrClassF16,
MnvrClassMig25, MnvrClassMig27, MnvrClassA10, MnvrClassBomber
```

**ManeuverClassData flags**: CanLevelTurn, CanSlice, CanUseVertical, CanOneCircle, CanTwoCircle, CanJinkSnake, CanJinkLoaded, CanJinkUnloaded

### 3.12 Wingman Action Flags (6 values)

```
AI_RTB(0), AI_LANDING(1), AI_FOLLOW_FORMATION(2),
AI_ENGAGE_TARGET(3), AI_EXECUTE_MANEUVER(4), AI_USE_COMPLEX(5)
```

### 3.13 Formation Types (16+ values)

```
WMWedge, WMTrail, WMStack, WMResCell, WMBox, WMArrowHead,
WMFluidFour, WMVic, WMFinger4, WMEchelon, WMForm1-4,
WMKickout, WMCloseup, WMToggleSide, WMIncreaseRelAlt, WMDecreaseRelAlt
```

---

## 4. f4-ai Architecture

### 4.1 Output Interface

The AI brain produces control outputs that map directly to `f4::flight::PilotInput`:

```cpp
// f4-ai/include/f4/ai/ai_output.hpp
namespace f4::ai {

struct AIControlOutput {
    double pitch_stick = 0.0;   // -1.0 to 1.0  → PilotInput::pstick
    double roll_stick  = 0.0;   // -1.0 to 1.0  → PilotInput::rstick
    double yaw_pedal   = 0.0;   // unclamped    → PilotInput::ypedal
    double throttle    = 0.0;   // 0.0 to 1.5   → PilotInput::throttle
    bool   missile_fire = false;
    bool   gun_fire     = false;
    // Extended controls (set by specific modules, zero/default otherwise):
    double speed_brake  = 0.0;  // 0.0 to 1.0  → PilotInput::speedBrake
    bool   gear_handle  = true; // true=down    → PilotInput::gearHandle
    bool   wheel_brakes = false;               → PilotInput::wheelBrakes
    bool   parking_brake = false;              → PilotInput::parkingBrake
    bool   nose_steer   = false;               → PilotInput::noseSteerOn
};

} // namespace f4::ai
```

### 4.2 Brain Interface

```cpp
// f4-ai/include/f4/ai/ai_brain.hpp
namespace f4::ai {

enum class SkillLevel { Recruit = 0, Rookie = 1, Veteran = 2, Ace = 3 };

class IAIBrain {
public:
    virtual ~IAIBrain() = default;
    virtual void initialize(
        uint64_t ownship_id,
        entities::EntityWorld& world,
        messaging::MessageBus& bus,
        SkillLevel skill
    ) = 0;
    virtual AIControlOutput update(double dt) = 0;
    virtual std::string current_mode() const = 0;
};

} // namespace f4::ai
```

### 4.3 TargetInfo — Sensor Fusion Output

```cpp
// f4-ai/include/f4/ai/target_info.hpp
namespace f4::ai {

struct TargetInfo {
    uint64_t entity_id = 0;
    double range_ft = 0.0;          // slant range
    double range_nm = 0.0;          // nautical miles
    double range_rate_fps = 0.0;    // closing rate (positive = closing)
    double azimuth_rad = 0.0;       // bearing from ownship nose
    double elevation_rad = 0.0;     // elevation angle
    double ata_rad = 0.0;           // aspect tail angle (0 = nose-on)
    double ata_from_rad = 0.0;      // aspect from target's perspective
    double atadot = 0.0;            // rate of change of ATA (EWMA smoothed)
    double rangedot = 0.0;          // rate of change of range (EWMA smoothed)
    double threat_score = 0.0;      // computed threat priority
    int combat_class = 0;           // 0=unthreatening, 1=possible, 2-4=fighter
    bool is_missile = false;
    bool is_hostile = false;
    geo::WorldPosition position{};  // target's world position
    geo::WorldPosition velocity{};  // target's velocity vector
};

} // namespace f4::ai
```

### 4.4 DigitalBrain as Composed Modules

```cpp
// f4-ai/include/f4/ai/digital_brain.hpp
namespace f4::ai {

class DigitalBrain : public IAIBrain {
public:
    void initialize(...) override;
    AIControlOutput update(double dt) override;
    std::string current_mode() const override;

    // Module access (for testing/debugging):
    SensorFusion&       sensors()       { return sensors_; }
    NavigationModule&   navigation()    { return nav_; }
    TakeoffModule&      takeoff()       { return takeoff_; }
    LandingModule&      landing()       { return landing_; }
    RefuelModule&       refuel()        { return refuel_; }
    BVRModule&          bvr()           { return bvr_; }
    WVRModule&          wvr()           { return wvr_; }
    MissileModule&      missile()       { return missile_; }
    CollisionAvoidModule& collision()   { return collision_; }
    WingmanModule&      wingman()       { return wingman_; }

private:
    // Per-frame orchestration (matches FreeFalcon FrameExec):
    void check_emergencies();        // fuel, damage, ejection
    void clamp_controls();           // enforce stick/throttle limits

    // Owned modules:
    SensorFusion         sensors_;
    NavigationModule     nav_;
    TakeoffModule        takeoff_;
    LandingModule        landing_;
    RefuelModule         refuel_;
    BVRModule            bvr_;
    WVRModule            wvr_;
    MissileModule        missile_;
    CollisionAvoidModule collision_;
    WingmanModule        wingman_;

    // Mode priority ladder:
    fsm::LayeredStateMachine<DigiMode, DigiEvent> mode_ladder_;

    // Ownship state (read from EntityWorld each frame):
    uint64_t ownship_id_ = 0;
    entities::EntityWorld* world_ = nullptr;
    messaging::MessageBus* bus_ = nullptr;
    SkillLevel skill_ = SkillLevel::Rookie;
};

} // namespace f4::ai
```

### 4.5 Dependency Graph

```
f4-ai
 ├── f4-flight-model    (PilotInput, AircraftState, FlightModel)
 ├── f4-entities        (EntityWorld, EntityHandle, TransformComponent, SpatialIndex)
 ├── f4-messaging       (MessageBus, publish/subscribe)
 ├── f4-state-machine   (StateMachine, LayeredStateMachine, Trace)
 ├── f4-geo             (WorldPosition, BRA, TheaterDatum, to_bra)
 ├── f4-data            (AircraftConfig, engine/aero tables)
 └── f4-math            (Table2D lookup, interpolation)
```

---

## 5. Implementation Steps

Each step is a milestone with: deliverables, FreeFalcon validation targets, test requirements, and a "done when" gate.

---

### Step 1: Library Scaffold

**Deliverables**:
- `f4-ai/CMakeLists.txt`
- `f4-ai/include/f4/ai/` — umbrella header + all module forward declarations
- `f4-ai/src/` — empty implementation files
- `f4-ai/tests/CMakeLists.txt`
- Root `CMakeLists.txt` updated with `add_subdirectory(f4-ai)`
- Namespace: `f4::ai`

**Conventions** (matching existing libraries):
- C++20, `-Wall -Wextra -Werror`, header-only where possible
- Umbrella header `f4_ai.hpp` includes all public headers
- Test naming: `test_<module>.cpp`
- CMake `F4_AI_AS_INTERFACE` option for header-only (following f4-geo pattern)

**Done when**: `cmake --build build --target F4::ai` succeeds with zero warnings.

---

### Step 2: SensorFusion Module

**FreeFalcon source**: `sfusion.cpp` (~504 lines), `targeting.cpp` (~200 lines)

**Deliverables**:
- `include/f4/ai/sensor_fusion.hpp`
- `include/f4/ai/target_info.hpp`
- `src/sensor_fusion.cpp`
- `tests/test_sensor_fusion.cpp`

**Design**:

```cpp
class SensorFusion {
public:
    struct Config {
        double max_radar_range_nm = 80.0;     // radar detection range
        double max_visual_range_nm = 10.0;    // visual detection range
        double max_engage_range_nm = 40.0;    // default engagement range
        double update_interval_sec = 5.0;     // target list refresh (skill-dependent)
    };

    void initialize(uint64_t ownship_id, entities::EntityWorld& world,
                    messaging::MessageBus& bus, SkillLevel skill, Config cfg = {});
    void update(double dt);

    // Accessors:
    const std::vector<TargetInfo>& targets() const;
    const TargetInfo* primary_target() const;    // highest threat score
    const TargetInfo* threat_target() const;      // highest-priority threat
    const TargetInfo* missile_threat() const;     // nearest incoming missile

    // Detection sources (match FreeFalcon sfusion.cpp):
    bool detected_by_radar(const TargetInfo& t) const;
    bool detected_by_rwr(const TargetInfo& t) const;
    bool detected_by_visual(const TargetInfo& t) const;
    bool detected_by_gci(const TargetInfo& t) const;

private:
    void update_target_list();              // rebuild from EntityWorld query
    void compute_geometry(TargetInfo& t);   // BRA, ATA, range rate via f4-geo
    void compute_threat_score(TargetInfo& t); // FreeFalcon scoring logic
    int guess_combat_class(const TargetInfo& t); // speed/alt heuristic
    void apply_skill_delay(double dt);      // reaction time per skill level
};
```

**FreeFalcon Validation**:

| Behavior | FreeFalcon Source | Validation Method |
|----------|-------------------|-----------------|
| 4 detection sources (GCI/RWR/Radar/Visual) | sfusion.cpp: any one enables canSee | Unit test: set each source, verify detection |
| Threat scoring: hostile +50 if combatClass 2-4 | sfusion.cpp threat scoring | Unit test: verify score computation for known inputs |
| ATA > 90° → score /2 | sfusion.cpp: `if (ataFrom > 90) score /= 2` | Unit test: verify score halving |
| Combat class guess: speed>300kts or alt<10000ft → fighter(4) | sfusion.cpp:504 | Unit test: verify classification at boundary values |
| Skill-dependent update interval | digimain.cpp:566 | Unit test: Recruit=10s, Ace=1s |
| EWMA smoothing on ataDot/rangeDot | digimain.cpp (0.85/0.15 blend) | Unit test: verify EWMA convergence |

**Done when**: All 6 validation tests pass; SensorFusion produces a valid target list from a populated `EntityWorld`.

---

### Step 3: TakeoffModule

**FreeFalcon source**: `digi_landme.cpp` (takeoff states), `dlogic.cpp` (TakeoffMode in Actions)

**Deliverables**:
- `include/f4/ai/takeoff_module.hpp`
- `src/takeoff_module.cpp`
- `tests/test_takeoff_module.cpp`

**State Machine** (from FreeFalcon takeoff ATC states):

```
tReqTaxi → tTaxi → tHoldShort → tPrepToTakeRunway → tTakeRunway → tTakeoff → tFlyOut
                              ↘ tWait (holding for clearance)
                              ↘ tEmerStop (emergency stop)
```

With events: `RequestTaxi`, `ClearanceGranted`, `RunwayAssigned`, `TakeoffCommand`, `Liftoff`, `EmergencyStop`, `FlyOutComplete`

**Design**:

```cpp
enum class TakeoffState {
    RequestTaxi, Taxi, HoldShort, Wait, PrepToTakeRunway,
    TakeRunway, Takeoff, FlyOut, EmergencyStop, Done
};

enum class TakeoffEvent {
    RequestTaxi, ClearanceGranted, RunwayAssigned,
    TakeoffCommand, Liftoff, EmergencyStop, FlyOutComplete
};

class TakeoffModule {
public:
    void initialize(entities::EntityWorld& world, messaging::MessageBus& bus);
    AIControlOutput update(double dt, const flight::AircraftState& state);

    TakeoffState state() const;
    bool is_complete() const { return state() == TakeoffState::Done; }

    // Configuration:
    double rotate_speed_kts = 140.0;     // Vr (aircraft-dependent, from config)
    double climb_rate_fpm = 500.0;       // initial climb rate
    double gear_up_alt_ft = 200.0;       // retract gear above this AGL

private:
    fsm::StateMachine<TakeoffState, TakeoffEvent> sm_;
    fsm::Trace<TakeoffState, TakeoffEvent> trace_;
    // Entry actions: configure throttle, release brakes, rotate
};
```

**FreeFalcon Validation**:

| Behavior | FreeFalcon Source | Validation Method |
|----------|-------------------|-----------------|
| Throttle up to MIL on takeoff roll | digi_landme.cpp takeoff sequence | Test: verify throttle reaches 1.0 during TakeRunway state |
| Rotate at Vr (pitch stick input) | digi_landme.cpp | Test: verify pstick > 0 when vcas >= rotate_speed |
| Gear up above 200ft AGL | digimain.cpp:566 (gear up check) | Test: verify gear_handle=false when alt > gear_up_alt |
| FlyOut: accelerate + climb to departure altitude | digi_landme.cpp | Test: verify climb to initial altitude within time budget |

**Done when**: AI aircraft lifts off from a runway entity, climbs to pattern altitude, and transitions to WaypointMode. Trace shows correct state sequence.

---

### Step 4: LandingModule

**FreeFalcon source**: `digi_landme.cpp` (~4778 lines)

This is the most complex procedural module. The 17+ landing states and 11 takeoff states form the largest single state machine in the AI.

**Deliverables**:
- `include/f4/ai/landing_module.hpp`
- `src/landing_module.cpp`
- `tests/test_landing_module.cpp`

**State Machine** (from FreeFalcon landing ATC states):

```
noATC → lReqClearance → lIngressing → lHolding → lFirstLeg → lToBase →
lToFinal → lOnFinal → lClearToLand → lLanded → TaxiOff

Emergency paths:
lReqEmerClearance → lEmerHold → lEmergencyToBase → lEmergencyToFinal → lEmergencyOnFinal → lLanded

Abort path:
lAborted → (back to lHolding or RTB)

Crash:
lCrashed (terminal)
```

**Design**:

```cpp
enum class LandingState {
    NoATC, ReqClearance, ReqEmerClearance, Ingressing, TakingPosition,
    Aborted, EmerHold, Holding, FirstLeg, ToBase, ToFinal, OnFinal,
    ClearToLand, Landed, TaxiOff, EmergencyToBase, EmergencyToFinal,
    EmergencyOnFinal, Crashed
};

// Events carry payloads (matching f4-state-machine on_if pattern):
struct ClearanceEvent { double runway_heading_rad; int runway_id; };
struct PositionEvent { double distance_to_fix_ft; };
struct AltitudeEvent { double altitude_agl_ft; };
struct SpeedEvent { double vcas_kts; };
struct GoAroundEvent { std::string reason; };

using LandingEvent = std::variant<
    RequestClearance, EmergencyClearance, ClearanceGranted, ClearanceDenied,
    ReachHoldingFix, ReachBase, ReachFinal, ClearedToLand,
    Touchdown, Crash, GoAround, TaxiComplete
>;

class LandingModule {
public:
    void initialize(entities::EntityWorld& world, messaging::MessageBus& bus);
    AIControlOutput update(double dt, const flight::AircraftState& state);

    LandingState state() const;
    bool is_landing() const;
    bool is_on_ground() const;

    // Approach configuration (entry actions set these):
    double approach_speed_kts() const;     // Vref + wind correction
    double glide_slope_angle() const;      // typically 3°
    double decision_height_ft() const;     // DH for go-around

private:
    fsm::StateMachine<LandingState, LandingEvent> sm_;
    fsm::Trace<LandingState, LandingEvent> trace_;
    // Approach geometry: compute bearing/distance to runway threshold
    // Autopilot: heading hold, altitude capture, speed hold via NavChannel
};
```

**Key Implementation Details**:

1. **Approach geometry**: Use `f4::geo::to_bra(ownship_pos, runway_threshold)` to compute bearing and distance to the runway. The glideslope is a 3° descent path from the threshold.

2. **Entry actions** (UML 2 semantics via f4-state-machine):
   - `OnFinal::on_enter`: configure gear down, flaps landing, speed to Vref
   - `ClearToLand::on_enter`: commit to landing, disable go-around below DH
   - `Landed::on_enter`: throttle to idle, brakes on, nose steer on

3. **Go-around decision**: If not `ClearToLand` and reaching decision height, emit `GoAround` event. If visual reference lost below DH, go around. This replaces FreeFalcon's scattered `if` checks.

4. **Speed management**: Decelerate through approach: downwind → base → final, managing speed via throttle and drag (speed brake if needed).

**FreeFalcon Validation**:

| Behavior | FreeFalcon Source | Validation Method |
|----------|-------------------|-----------------|
| Request clearance from ATC | digi_landme.cpp | Integration test: publish ATCClearanceMessage, verify state transition |
| Fly rectangular traffic pattern | digi_landme.cpp pattern legs | Test: trace shows FirstLeg → ToBase → ToFinal → OnFinal sequence |
| Configure gear/flaps on entering final | digi_landme.cpp entry actions | Test: verify gear_handle=true after OnFinal entry |
| Go-around at DH if not cleared | digi_landme.cpp | Test: emit DH altitude without clearance, verify GoAround event |
| Touchdown: throttle idle, brakes on | digi_landme.cpp | Test: verify control outputs after Landed entry |
| 19 landing + 11 takeoff states covered | digi_landme.cpp state list | Test: enumerate all states, verify each reachable via trace |

**Done when**: AI aircraft flies a complete traffic pattern from ingress to touchdown on a runway entity, with correct state transitions in the trace. Go-around works correctly. Emergency landing path works.

---

### Step 5: NavigationModule

**FreeFalcon source**: `digi_waypoint.cpp`, `autopilot.cpp` (~323 lines)

**Deliverables**:
- `include/f4/ai/navigation_module.hpp`
- `src/navigation_module.cpp`
- `tests/test_navigation_module.cpp`

**Design**:

```cpp
enum class NavMode { Waypoint, HeadingHold, AltitudeHold, SpeedHold, RTB, Loiter };

struct NavCommand {
    double desired_heading_rad = 0.0;
    double desired_altitude_ft = 0.0;
    double desired_speed_kts = 0.0;
    bool on_station = false;
};

class NavigationModule {
public:
    void initialize(entities::EntityWorld& world, messaging::MessageBus& bus);
    AIControlOutput update(double dt, const flight::AircraftState& state);
    NavCommand command() const;

    // Waypoint management:
    void set_waypoints(std::vector<geo::WorldPosition> wps);
    void set_current_waypoint(size_t index);
    bool advance_waypoint();              // move to next WP, return false if done
    const geo::WorldPosition& current_waypoint() const;
    size_t waypoint_index() const;

    // Hold modes:
    void hold_heading(double heading_rad);
    void hold_altitude(double altitude_ft);
    void hold_speed(double speed_kts);

    // RTB:
    void return_to_base(geo::WorldPosition homebase);

private:
    // Autopilot channels (from FreeFalcon autopilot.cpp):
    struct PIDLoop {
        double kp, ki, kd;
        double integral = 0, prev_error = 0;
        double update(double error, double dt);
    };
    PIDLoop heading_pid_;   // roll command from heading error
    PIDLoop altitude_pid_;  // pitch command from altitude error
    PIDLoop speed_pid_;     // throttle command from speed error

    std::vector<geo::WorldPosition> waypoints_;
    size_t wp_index_ = 0;
    NavMode mode_ = NavMode::Waypoint;
};
```

**Key Implementation Details**:

1. **PID autopilot** (from FreeFalcon `autopilot.cpp`): Three independent channels — heading (→ roll stick), altitude (→ pitch stick), speed (→ throttle). Each is a PID loop with configurable gains.

2. **Waypoint following**: Compute `BRA` to current waypoint via `f4::geo::to_bra(ownship, wp)`. When range < acceptance radius (default 500 ft), advance to next waypoint.

3. **RTB**: Special waypoint sequence — current position → homebase. Fuel state triggers RTB automatically (Joker/Bingo fuel).

**FreeFalcon Validation**:

| Behavior | FreeFalcon Source | Validation Method |
|----------|-------------------|-----------------|
| Follow waypoint sequence | digi_waypoint.cpp | Test: 3-WP course, verify arrival at each within 500ft |
| Heading hold: converge to commanded heading | autopilot.cpp PIDLoop | Test: command 90° from 0°, verify heading converges within 5° |
| Altitude hold: converge to commanded altitude | autopilot.cpp | Test: command FL200 from FL100, verify altitude captures |
| Speed hold: converge to commanded speed | autopilot.cpp | Test: command 300kts from 250kts, verify speed captures |
| Advance waypoint when within acceptance radius | digi_waypoint.cpp | Test: verify wp_index increments at correct range |

**Done when**: AI aircraft follows a 3-waypoint course, arriving at each within tolerance, with smooth heading/altitude/speed transitions. PID loops converge without oscillation.

---

### Step 6: RefuelModule

**FreeFalcon source**: `digi_refuel.cpp` (~1030 lines)

**Deliverables**:
- `include/f4/ai/refuel_module.hpp`
- `src/refuel_module.cpp`
- `tests/test_refuel_module.cpp`

**State Machine** (5 states, matching FreeFalcon exactly):

```
NoTanker → VectorTo → Waiting → Refueling → Done
                ↑                      │
                └── (if contact lost) ──┘
```

Events: `TankerAssigned`, `ReachTanker`, `ContactMade`, `ContactLost`, `RefuelComplete`, `Disconnect`

**Design**:

```cpp
enum class RefuelState { NoTanker, VectorTo, Waiting, Refueling, Done };
enum class RefuelEvent {
    TankerAssigned, ReachTanker, ContactMade, ContactLost,
    RefuelComplete, Disconnect
};

class RefuelModule {
public:
    void initialize(entities::EntityWorld& world, messaging::MessageBus& bus);
    AIControlOutput update(double dt, const flight::AircraftState& state);

    RefuelState state() const;
    bool is_refueling() const { return state() == RefuelState::Refueling; }

    // Tanker:
    void assign_tanker(uint64_t tanker_entity_id);
    uint64_t tanker_id() const;

private:
    fsm::StateMachine<RefuelState, RefuelEvent> sm_;
    fsm::Trace<RefuelState, RefuelEvent> trace_;

    // Formation position behind tanker:
    double desired_offset_ft = -50.0;     // 50ft behind
    double desired_alt_offset_ft = 0.0;   // same altitude
    double desired_lateral_ft = 0.0;      // centerline

    // Precision autopilot for boom contact:
    // Uses NavigationModule's PID loops with tighter gains
};
```

**Key Implementation Details**:

1. **Approach**: Navigate to tanker position using `NavigationModule::hold_heading/altitude/speed`. The tanker entity's `TransformComponent` provides the reference position.

2. **Formation joining**: Use precision PID loops (tighter gains than navigation) to hold position relative to the tanker. Compute relative position via `to_bra(ownship, tanker_pos)`.

3. **Contact maintenance**: In `Refueling` state, maintain position within boom envelope (±5ft lateral, ±10ft vertical, ±30ft longitudinal). If position drifts outside envelope, the `ContactLost` event fires and we transition back to `Waiting`.

**FreeFalcon Validation**:

| Behavior | FreeFalcon Source | Validation Method |
|----------|-------------------|-----------------|
| Navigate to tanker vector | digi_refuel.cpp | Test: tanker at known position, verify heading convergence |
| Join formation behind tanker | digi_refuel.cpp | Test: verify position within ±30ft of desired offset |
| Maintain boom contact | digi_refuel.cpp | Test: verify position stays within boom envelope for 60s |
| Contact lost → return to Waiting | digi_refuel.cpp | Test: perturb position outside envelope, verify ContactLost |
| 5-state lifecycle | digi_refuel.cpp | Test: trace shows NoTanker→VectorTo→Waiting→Refueling→Done |

**Done when**: AI aircraft joins a tanker, maintains boom contact for 60 simulated seconds, and disconnects cleanly. Trace shows correct 5-state lifecycle.

---

### Step 7: CollisionAvoidModule

**FreeFalcon source**: `digi_cavoid.cpp`

**Deliverables**:
- `include/f4/ai/collision_avoid_module.hpp`
- `src/collision_avoid_module.cpp`
- `tests/test_collision_avoid_module.cpp`

**Design**:

```cpp
enum class CAState { Clear, Avoiding, GroundAvoid };
enum class CAEvent { ThreatDetected, ThreatCleared, GroundProximity, PullUp };

class CollisionAvoidModule {
public:
    void initialize(entities::EntityWorld& world);
    AIControlOutput update(double dt, const flight::AircraftState& state);

    CAState state() const;

    // FreeFalcon constants (from digi_cavoid.cpp):
    static constexpr double H_RANGE_FT = 200.0;       // horizontal separation
    static constexpr double REACT_FACT = 0.55;        // reaction time factor
    static constexpr double GS_LIMIT = 9.0;           // max G for reaction calc
    // reactTime = (GS_LIMIT / maxGs) * REACT_FACT

private:
    fsm::StateMachine<CAState, CAEvent> sm_;
    void check_aircraft_proximity();    // EntityWorld::within_radius()
    void check_ground_proximity();      // compare alt vs terrain elevation
};
```

**FreeFalcon Validation**:

| Behavior | FreeFalcon Source | Validation Method |
|----------|-------------------|-----------------|
| Horizontal range check (200ft) | digi_cavoid.cpp hRange | Test: aircraft at 150ft → avoid, at 250ft → clear |
| G-limited reaction time | digi_cavoid.cpp reactFact | Test: verify reactTime calculation |
| Ground proximity → pull up | digi_cavoid.cpp GroundAvoidMode | Test: alt AGL < 200ft, verify pitch-up command |
| 9G limit | digi_cavoid.cpp GS_LIMIT | Test: verify command doesn't exceed 9G |

**Done when**: AI avoids a co-altitude aircraft at 150ft separation with correct pull-up command. Ground avoidance triggers below 200ft AGL.

---

### Step 8: BVRModule

**FreeFalcon source**: `bvrengage.cpp` (~679 lines), `dlogic.cpp` BVR decision

**Deliverables**:
- `include/f4/ai/bvr_module.hpp`
- `src/bvr_module.cpp`
- `tests/test_bvr_module.cpp`

**Design**:

```cpp
enum class BVRState { None, Entering, Employing, Separating };
enum class BVREvent { TargetDetected, InRange, WeaponFired, WeaponMiss,
                      ThreatDetected, BugOut, SeparationComplete, LostTarget };

// BVR tactics (from FreeFalcon BVRInterceptType):
enum class BVRTactic {
    FollowWaypoints, FlyFormation, SingleSideOffset, Pince, Pursuit,
    NoIntercept, Pump, Crank, CrankRight, CrankLeft,
    Notch, NotchRight, NotchLeft, NotchRightHigh, NotchLeftHigh,
    Grind, CrankHi, CrankLo, CrankRightHi, CrankRightLo,
    CrankLeftHi, CrankLeftLo
};

// BVR profiles (from FreeFalcon BVRProfileType):
enum class BVRProfile {
    Level1a, Level1b, Level2a, Level2b, Level3a, Level3c,
    BeamDeploy, BeamBeam, Wall, Grinder, WideAzimuth, ShortAzimuth,
    WideLT, ShortLT, Sweep, Defensive
};

struct BVRCommand {
    BVRTactic tactic = BVRTactic::FollowWaypoints;
    double desired_range_nm = 0.0;
    double desired_altitude_ft = 0.0;
    double offset_heading_rad = 0.0;     // crank/notch offset angle
};

class BVRModule {
public:
    void initialize(entities::EntityWorld& world, messaging::MessageBus& bus,
                    SensorFusion& sensors);
    AIControlOutput update(double dt, const flight::AircraftState& state,
                           const TargetInfo& target);

    BVRState state() const;
    BVRCommand command() const;

    // Tactic selection (from FreeFalcon BvrChooseTactic):
    void choose_tactic(const TargetInfo& target, double mach, double alt);
    void set_profile(BVRProfile profile);

    // Range bands (from FreeFalcon dlogic.cpp):
    static constexpr double BVR_ENTRY_RANGE_MULT = 1.3;  // maxAAWpnRange * 1.3
    static constexpr double WVR_ENTRY_RANGE_NM = 3.0;    // → WVR transition
    static constexpr double SEPARATE_RANGE_NM = 2.0;     // → bug out
    static constexpr double MERGE_RANGE_FT = 1000.0;     // → merge

private:
    fsm::StateMachine<BVRState, BVREvent> sm_;
    fsm::Trace<BVRState, BVREvent> trace_;
    BVRProfile profile_ = BVRProfile::Level1a;
    BVRTactic tactic_ = BVRTactic::FollowWaypoints;
    int action_step_ = 0;                  // multi-step tactic progression
    double re_eval_interval_sec_ = 5.0;
    double bvr_tactic_timer_ = 0.0;
};
```

**Key Implementation Details**:

1. **MAR (Minimum Abort Range)**: The range at which the AI must turn cold to avoid entering the threat's weapon envelope. Computed from threat's max weapon range and geometry.

2. **Crank**: Turn offset (typically 30-60°) from the target's bearing to maintain radar contact while reducing closure rate. The offset direction (left/right) is tactical choice based on formation position and threat geometry.

3. **Notch**: Beam maneuver (turn 90° to threat radar) to break radar lock. Used when spiked (RWR detects radar in SAM mode).

4. **Tactic progression**: BVR tactics are multi-step (`action_step_` counter). FreeFalcon's `bvractionstep` progresses through: detect → employ → assess → re-engage/separate.

**FreeFalcon Validation**:

| Behavior | FreeFalcon Source | Validation Method |
|----------|-------------------|-----------------|
| 18 profiles load | bvrengage.cpp ChoiceProfile | Test: enumerate all profiles, verify tactic selection |
| Crank offset (30-60° from target bearing) | bvrengage.cpp BvrCrank | Test: verify heading offset from target bearing |
| Notch: 90° beam to threat radar | bvrengage.cpp BvrNotch | Test: verify 90° offset from threat bearing |
| Range band transitions (BVR→WVR at 3NM) | dlogic.cpp range checks | Test: target at 4NM→BVR, 2.5NM→WVR |
| Bugout at 2NM separation | dlogic.cpp | Test: verify SeparateMode entry |
| 5-second re-evaluation interval | digimain.cpp | Test: verify tactic re-evaluation timing |

**Done when**: AI executes a crank maneuver maintaining 40NM range, fires missile at correct MAR, and separates after shot. Trace shows BVR state transitions.

---

### Step 9: WVRModule

**FreeFalcon source**: `wvrengage.cpp` (~78 lines), `merge.cpp` (~275 lines), `gunsjink.cpp`, `mdefeat.cpp`

**Deliverables**:
- `include/f4/ai/wvr_module.hpp`
- `src/wvr_module.cpp`
- `tests/test_wvr_module.cpp`

**Design**:

```cpp
// WVR tactics (from FreeFalcon WvrTacticType):
enum class WVRTactic {
    None, RandP, OverB, Roop, GunJink, Straight,
    BugOut, Avoid, Beam, BeamReturn, RunAway
};

enum class WVRState { None, Offensive, Defensive, Neutral, BugOut };
enum class WVREvent {
    TargetDetected, InMerge, OffensiveAdvantage, DefensiveThreat,
    ThreatCleared, BugOutTimer, MergeComplete, LostTarget
};

struct WVRCommand {
    WVRTactic tactic = WVRTactic::None;
    double g_command = 0.0;           // desired G-load
    double roll_command = 0.0;        // desired roll rate
};

class WVRModule {
public:
    void initialize(entities::EntityWorld& world, messaging::MessageBus& bus,
                    SensorFusion& sensors);
    AIControlOutput update(double dt, const flight::AircraftState& state,
                           const TargetInfo& target);

    WVRState state() const;
    WVRTactic current_tactic() const;

    // Maneuver class data (from mnvrdata.dat):
    void set_maneuver_class(int ac_class);  // maps to ACMnverClass

private:
    fsm::StateMachine<WVRState, WVREvent> sm_;
    fsm::Trace<WVRState, WVREvent> trace_;
    WVRTactic tactic_ = WVRTactic::None;
    double wvr_tactic_timer_ = 0.0;
    bool bugged_out_ = false;

    // Maneuver capability flags (from ManeuverClassData):
    bool can_level_turn_ = true;
    bool can_slice_ = true;
    bool can_use_vertical_ = true;
    bool can_one_circle_ = true;
    bool can_two_circle_ = true;
    bool can_jink_snake_ = true;
    bool can_jink_loaded_ = true;
    bool can_jink_unloaded_ = true;
};
```

**FreeFalcon Validation**:

| Behavior | FreeFalcon Source | Validation Method |
|----------|-------------------|-----------------|
| 11 WVR tactics selectable | wvrengage.cpp WvrTacticType | Test: enumerate all tactics |
| Guns jink: random pitch/roll | gunsjink.cpp | Test: verify stick input variation during jink |
| Merge maneuver at 1000ft | merge.cpp MergeCheck | Test: verify merge entry at range |
| Over-bank (30° excess) | dlogic.cpp OverBMode | Test: verify bank angle exceeds commanded by 30° |
| Roll out of plane (Roop) | dlogic.cpp RoopMode | Test: verify roll command perpendicular to lift vector |
| Bug-out timer (90 sec) | dlogic.cpp bugoutTimer | Test: verify bug-out after 90s in defensive |

**Done when**: AI executes offensive BFM (turning toward target's blind cone), transitions to defensive when threatened, and jinks when under guns. Trace shows WVR tactic transitions.

---

### Step 10: MissileModule

**FreeFalcon source**: `mengage.cpp`, `mdefeat.cpp`

**Deliverables**:
- `include/f4/ai/missile_module.hpp`
- `src/missile_module.cpp`
- `tests/test_missile_module.cpp`

**Design**:

```cpp
class MissileModule {
public:
    struct Config {
        double fire_cooldown_sec = 4.0;       // between shots (shoot-shoot doctrine)
        double max_pk_range_nm = 20.0;        // maximum launch range for Pk > 0
        double min_pk_range_nm = 5.0;         // minimum range (too close to employ)
        double shoot_shoot_threshold = 0.5;   // Pk threshold for shoot-shoot
    };

    void initialize(entities::EntityWorld& world, messaging::MessageBus& bus,
                    SensorFusion& sensors, Config cfg = {});
    AIControlOutput update(double dt, const flight::AircraftState& state,
                           const TargetInfo& target);

    // Fire control (from FreeFalcon mengage.cpp FireControl):
    bool should_fire(const TargetInfo& target, double mach, double alt) const;
    double compute_pk(const TargetInfo& target) const;  // probability of kill

    // Missile defeat (from FreeFalcon mdefeat.cpp):
    bool is_defeating() const;
    bool should_chaff() const;
    bool should_flare() const;

private:
    double missile_shot_timer_ = 0.0;
    bool in_shoot_shoot_ = false;
    Config cfg_;
};
```

**FreeFalcon Validation**:

| Behavior | FreeFalcon Source | Validation Method |
|----------|-------------------|-----------------|
| Fire at MAR with sufficient Pk | mengage.cpp FireControl | Test: target at MAR, Pk > threshold → fire |
| 4-second cooldown between shots | mengage.cpp missileShotTimer | Test: verify no fire within cooldown |
| Shoot-shoot doctrine | dlogic.cpp InShootShoot | Test: verify two missiles fired within doctrine window |
| Missile defeat: beam maneuver | mdefeat.cpp | Test: incoming missile → beam command |
| Chaff/flare at correct conditions | mdefeat.cpp | Test: radar missile → chaff, IR missile → flare |

**Done when**: AI fires missile at correct MAR with Pk threshold, observes cooldown, and defends against incoming missiles with beam + chaff/flare.

---

### Step 11: WingmanModule

**FreeFalcon source**: `winglogic.cpp`, `wingactions.cpp`, `wingradio.cpp` (~867 lines), `flitlead.cpp`

**Deliverables**:
- `include/f4/ai/wingman_module.hpp`
- `src/wingman_module.cpp`
- `tests/test_wingman_module.cpp`

**Design**:

```cpp
enum class WingState { Following, Executing, Breaking, Rejoining };
enum class WingEvent {
    FormCommand, EngageCommand, BreakCommand, RejoinCommand,
    RTBCommand, LeadLost, FormationBroken
};

// Formation types (from FreeFalcon):
enum class FormationType {
    Wedge, Trail, Stack, ResCell, Box, ArrowHead, FluidFour,
    Vic, Finger4, Echelon
};

class WingmanModule {
public:
    void initialize(entities::EntityWorld& world, messaging::MessageBus& bus,
                    SensorFusion& sensors);
    AIControlOutput update(double dt, const flight::AircraftState& state);

    WingState state() const;
    FormationType formation() const;

    // Commands (from FreeFalcon wingman action flags):
    void command_rtb();
    void command_formation(FormationType form);
    void command_engage(uint64_t target_id);
    void command_maneuver(int maneuver_id);

    // Formation keeping:
    geo::WorldPosition desired_position() const;  // relative to lead
    double formation_tolerance_ft() const;         // skill-dependent

private:
    fsm::StateMachine<WingState, WingEvent> sm_;
    FormationType formation_ = FormationType::Finger4;

    // Formation geometry (from FreeFalcon formdata.cpp):
    double lateral_spacing_ft = 3000.0;    // default nmi * 2
    double altitude_offset_ft = 0.0;
    double form_side_ = 1.0;               // +1 right, -1 left

    // Reference to flight lead:
    uint64_t lead_id_ = 0;
};
```

**FreeFalcon Validation**:

| Behavior | FreeFalcon Source | Validation Method |
|----------|-------------------|-----------------|
| Follow flight lead in formation | wingactions.cpp AiFollowLead | Test: lead maneuvers, wingman follows within tolerance |
| Formation type changes | formdata.cpp | Test: command Finger4, verify position offset |
| Respond to engagement order | winglogic.cpp | Test: command_engage → verify wingman targets designated entity |
| 16+ formation types | FreeFalcon enum | Test: enumerate all, verify position computation |
| Radio call on order receipt | wingradio.cpp | Test: verify message published on bus |

**Done when**: Wingman maintains Finger4 formation within 500ft tolerance during lead maneuvers, responds to engagement commands, and publishes radio messages.

---

### Step 12: DigitalBrain Integration

**FreeFalcon source**: `digimain.cpp` (FrameExec), `dlogic.cpp` (DecisionLogic)

**Deliverables**:
- `include/f4/ai/digital_brain.hpp`
- `src/digital_brain.cpp`
- `tests/test_digital_brain.cpp`

**The update() method** maps FrameExec 1:1:

```cpp
AIControlOutput DigitalBrain::update(double dt) {
    // 1. Read ownship state from EntityWorld
    auto& transform = handle_.require<entities::TransformComponent>();
    flight::AircraftState state = read_aircraft_state(transform);

    // 2. Emergency checks
    check_emergencies();

    // 3. Update sensor fusion
    sensors_.update(dt);
    auto* target = sensors_.primary_target();
    auto* threat = sensors_.threat_target();

    // 4. Determine effective mode from priority ladder
    auto mode = mode_ladder_.effective_mode();

    // 5. Dispatch to active module
    AIControlOutput output{};
    switch (mode) {
        case DigiMode::TakeoffMode:    output = takeoff_.update(dt, state); break;
        case DigiMode::LandingMode:    output = landing_.update(dt, state); break;
        case DigiMode::WaypointMode:   output = nav_.update(dt, state); break;
        case DigiMode::RefuelingMode:  output = refuel_.update(dt, state); break;
        case DigiMode::BVREngageMode:  output = bvr_.update(dt, state, *target); break;
        case DigiMode::WVREngageMode:  output = wvr_.update(dt, state, *target); break;
        // ... etc
    }

    // 6. Apply collision avoidance overlay (always active)
    auto ca = collision_.update(dt, state);
    if (ca.has_override) output = ca.override_output;

    // 7. Clamp controls
    clamp_controls(output);

    return output;
}
```

**Done when**: A complete 60-second AI flight from takeoff → waypoint navigation → BVR engagement → landing, with no crashes and correct mode transitions throughout. Trace output is greppable and shows the full mode lifecycle.

---

## 6. FreeFalcon Validation Mapping

This section provides the complete cross-reference between f4-ai modules and FreeFalcon source files, enabling side-by-side validation.

### 6.1 Integration Test Scenarios

These scenarios use real data from f4-world (`save1.world.json` + Korea terrain):

| Scenario | Modules Exercised | FreeFalcon Reference | Success Criteria |
|----------|-------------------|---------------------|-----------------|
| **S1**: F-16 takeoff from Kunsan | Takeoff, SensorFusion, Navigation | digi_landme.cpp takeoff states | Aircraft lifts off, climbs to departure altitude, transitions to WaypointMode |
| **S2**: F-16 landing at Kunsan | Landing, SensorFusion, Navigation | digi_landme.cpp landing states | Aircraft flies traffic pattern, touches down on runway |
| **S3**: 4-ship formation flight | Navigation, Wingman, SensorFusion | wingactions.cpp | All 4 aircraft maintain Finger4 within 500ft |
| **S4**: AAR behind tanker | Refuel, Navigation, SensorFusion | digi_refuel.cpp | Aircraft joins tanker, maintains boom contact 60s |
| **S5**: BVR 2v2 engagement | BVR, Missile, SensorFusion, Collision | bvrengage.cpp | AI fires at MAR, cranks, separates. No fratricide |
| **S6**: WVR 1v1 dogfight | WVR, Missile, SensorFusion | wvrengage.cpp | Fight lasts >30s. AI transitions offense/defense |
| **S7**: Full sortie | All modules | digimain.cpp FrameExec | Takeoff → nav → BVR → RTB → land. 0 crashes. Mode trace is complete |

### 6.2 Validation Protocol

For each scenario:

1. **Set up**: Load `WorldState` from `save1.world.json`, populate `EntityWorld`, create AI aircraft with `DigitalBrain`
2. **Run**: Execute 60-300 simulated seconds at 240Hz (minor frame = 1/240s)
3. **Collect trace**: Dump all SM traces and mode transitions
4. **Validate**: Compare trace against FreeFalcon's expected mode sequence
5. **Diff**: Run same scenario before/after code change; diff traces to catch regressions

---

## 7. Testing Strategy

### 7.1 Test Levels

| Level | Scope | Fixtures | Example |
|-------|-------|----------|---------|
| **Unit** | Single module, no EntityWorld | Mock inputs | `test_sensor_fusion.cpp`: verify threat scoring |
| **Integration** | Module + EntityWorld + MessageBus | `save1.world.json` entities | `test_landing_module.cpp`: land at Kunsan |
| **Scenario** | Full DigitalBrain | Full WorldState + terrain | `test_digital_brain.cpp`: S1-S7 |

### 7.2 Test Naming Convention

```
test_<module>.cpp          — unit tests for <module>
test_<module>_integration.cpp — integration tests (uses EntityWorld)
test_scenario_s<n>.cpp     — scenario tests S1-S7
```

### 7.3 Skill-Level Sweep

Every module test must be run at all 4 skill levels to verify skill-dependent behavior:

```cpp
for (auto skill : {SkillLevel::Recruit, SkillLevel::Rookie,
                   SkillLevel::Veteran, SkillLevel::Ace}) {
    brain.initialize(id, world, bus, skill);
    auto output = brain.update(dt);
    // verify skill-appropriate behavior
}
```

### 7.4 Regression Testing

Traces are committable. A passing scenario's trace is checked in as `tests/traces/s<n>_baseline.txt`. Future runs diff against baseline:

```bash
diff tests/trace/s1_baseline.txt build/s1_trace.txt
```

Any diff = regression. This is the direct application of §18.6's "traces are diffable" principle.

---

## 8. Observability & Tracing

### 8.1 Trace Attachment

Every state machine in f4-ai has a `Trace` attached. The pattern (established by the stall SM migration):

```cpp
// In module initialization:
sm_ = make_landing_sm();
trace_.set_capacity(4096);            // bounded ring buffer
sm_.set_trace(&trace_);              // zero overhead when null
trace_.set_trace_rejections(true);    // log rejected transitions too
```

### 8.2 Trace Output Format

Every trace line is greppable:

```
tick=4521 from=Holding to=ToBase event=ReachHoldingFix fired=1 guard=PASS reason="range<1nm"
tick=4522 from=ToBase to=OnFinal event=ReachBase fired=1 guard=PASS reason="abeam_fix"
tick=4523 from=OnFinal to=OnFinal event=GoAround fired=0 guard=FAIL reason="cleared_to_land=1"
```

### 8.3 Mode-Change Logging

The `LayeredStateMachine` produces a mode trace:

```
tick=0 mode=TakeoffMode layer=0 prev=NoMode
tick=1200 mode=WaypointMode layer=22 prev=TakeoffMode
tick=54000 mode=BVREngageMode layer=16 prev=WaypointMode
tick=56400 mode=SeparateMode layer=8 prev=BVREngageMode
tick=60000 mode=LandingMode layer=5 prev=WaypointMode
```

### 8.4 Entity-Prefixed Traces

For multi-aircraft scenarios, each AI brain prefixes its trace with the entity ID:

```
entity=F16-1 tick=4521 from=Holding to=ToBase ...
entity=F16-2 tick=4521 from=OnFinal to=ClearToLand ...
```

This enables `grep "entity=F16-1" trace.log` to extract a single aircraft's history.

---

## 9. Skill Level System

`SkillLevel { Recruit=0, Rookie=1, Veteran=2, Ace=3 }` affects the following parameters:

| Parameter | Recruit | Rookie | Veteran | Ace | FreeFalcon Reference |
|-----------|---------|--------|---------|-----|---------------------|
| Target update interval | 10s | 7s | 5s | 1s | digimain.cpp:566 |
| Reaction delay | 3.0s | 2.0s | 1.0s | 0.3s | dlogic.cpp |
| Gun jink timing (min) | 4.0s | 3.0s | 2.0s | 1.0s | gunsjink.cpp |
| Max G utilization | 5.0 | 6.0 | 7.5 | 9.0 | maxGs member |
| Formation tolerance | 1000ft | 750ft | 500ft | 250ft | wingactions.cpp |
| Shoot-shoot doctrine | Never | Sometimes | Usually | Always | dlogic.cpp InShootShoot |
| Fuel awareness | None | Joker only | Joker+Bingo | Continuous | ATCFlags SaidJoker/SaidBingo |
| Missile PK threshold | 0.7 | 0.5 | 0.3 | 0.2 | mengage.cpp |

---

## 10. Simple vs Complex Flight Model Selection

The AI must signal which flight model to use. This is determined by the current DigiMode (from FreeFalcon digimain.cpp):

**Always SIMPLE**: On ground, pilot ejected/dying

**SIMPLE mode** (SimplifiedFlightModel):
- WaypointMode, LoiterMode, LandingMode, TakeoffMode, RefuelingMode
- FollowOrdersMode/WingyMode (unless `AI_USE_COMPLEX` flag set)

**COMPLEX mode** (FullFlightModel):
- Any mode when threatPtr != NULL
- RTBMode, BVREngageMode, GunsEngageMode, MissileEngageMode
- GunsJinkMode, CollisionAvoidMode, OverBMode, RoopMode, WVREngageMode

**f4-ai implementation**: The `DigitalBrain` exposes a `use_complex_model()` method that the host checks each frame:

```cpp
bool DigitalBrain::use_complex_model() const {
    auto mode = mode_ladder_.effective_mode();
    if (is_on_ground_) return false;
    if (sensors_.threat_target() != nullptr) return true;
    switch (mode) {
        case DigiMode::BVREngageMode:
        case DigiMode::WVREngageMode:
        case DigiMode::MissileEngageMode:
        case DigiMode::GunsEngageMode:
        case DigiMode::GunsJinkMode:
        case DigiMode::CollisionAvoidMode:
        case DigiMode::RTBMode:
        case DigiMode::OverBMode:
        case DigiMode::RoopMode:
            return true;
        default:
            return false;
    }
}
```

---

## 11. Range Bands & Mode Transitions

From FreeFalcon dlogic.cpp, these range thresholds determine mode transitions:

| Range | Condition | Mode Transition | FreeFalcon Source |
|-------|-----------|----------------|-------------------|
| > maxEngageRange (40-60 NM) | No engagement possible | Stay in WaypointMode | dlogic.cpp |
| maxAAWpnRange × 1.3 | BVR entry threshold | → BVREngageMode | dlogic.cpp |
| 8 NM | AG self-defense range | → defensive if ground threat | dlogic.cpp |
| 6 NM | AG ignore if high aspect | → ignore ground threat | dlogic.cpp |
| 3 NM | WVR entry | → WVREngageMode | dlogic.cpp |
| 2 NM | Separation/bogout | → SeparateMode | dlogic.cpp |
| 4.5 NM | WVR hysteresis | Prevent BVR↔WVR oscillation | dlogic.cpp |
| 1000 ft | Merge | → MergeMode | merge.cpp |
| 200 ft | Collision avoidance | → CollisionAvoidMode | digi_cavoid.cpp |

**Hysteresis**: The 3NM→WVR / 4.5NM→BVR hysteresis band prevents mode oscillation at the boundary. Once in WVR, the AI stays until range > 4.5NM before returning to BVR.

---

## 12. Message Types

Messages published and consumed by f4-ai on the `MessageBus`:

### Published by AI

| Message | Publisher | When | Fields |
|---------|-----------|------|--------|
| `TargetAcquiredMessage` | SensorFusion | New target detected | entity_id, range, combat_class |
| `ModeChangeMessage` | DigitalBrain | DigiMode changes | from_mode, to_mode, entity_id |
| `MissileFireMessage` | MissileModule | AI fires missile | target_id, weapon_type, range |
| `GunFireMessage` | WVRModule | AI fires guns | target_id |
| `LandingRequestMessage` | LandingModule | AI requests landing | runway_id, aircraft_id |
| `GoAroundMessage` | LandingModule | AI goes around | reason, runway_id |
| `RefuelRequestMessage` | RefuelModule | AI requests refuel | tanker_id, aircraft_id |
| `RadioCallMessage` | WingmanModule | Radio call | text, call_type, from_id |

### Consumed by AI

| Message | Consumer | Action |
|---------|----------|--------|
| `StallStateChangeMessage` | DigitalBrain | Re-evaluate tactical posture |
| `DamageMessage` | DigitalBrain | Emergency jettison if < 50% |
| `ATCClearanceMessage` | LandingModule | Transition to cleared state |
| `WingmanCommandMessage` | WingmanModule | Execute ordered maneuver |
| `RWRContactMessage` | SensorFusion | Add RWR detection source |

---

## 13. Directory Layout & Build

### 13.1 Directory Structure

```
f4-ai/
├── CMakeLists.txt
├── include/
│   └── f4/
│       └── ai/
│           ├── f4_ai.hpp              # umbrella header
│           ├── ai_output.hpp          # AIControlOutput
│           ├── ai_brain.hpp           # IAIBrain interface
│           ├── target_info.hpp        # TargetInfo struct
│           ├── sensor_fusion.hpp      # SensorFusion class
│           ├── digital_brain.hpp      # DigitalBrain class
│           ├── takeoff_module.hpp     # TakeoffModule
│           ├── landing_module.hpp     # LandingModule
│           ├── navigation_module.hpp  # NavigationModule
│           ├── refuel_module.hpp      # RefuelModule
│           ├── collision_avoid_module.hpp
│           ├── bvr_module.hpp         # BVRModule
│           ├── wvr_module.hpp         # WVRModule
│           ├── missile_module.hpp     # MissileModule
│           └── wingman_module.hpp     # WingmanModule
├── src/
│   ├── sensor_fusion.cpp
│   ├── digital_brain.cpp
│   ├── takeoff_module.cpp
│   ├── landing_module.cpp
│   ├── navigation_module.cpp
│   ├── refuel_module.cpp
│   ├── collision_avoid_module.cpp
│   ├── bvr_module.cpp
│   ├── wvr_module.cpp
│   ├── missile_module.cpp
│   └── wingman_module.cpp
└── tests/
    ├── CMakeLists.txt
    ├── test_sensor_fusion.cpp
    ├── test_takeoff_module.cpp
    ├── test_landing_module.cpp
    ├── test_navigation_module.cpp
    ├── test_refuel_module.cpp
    ├── test_collision_avoid_module.cpp
    ├── test_bvr_module.cpp
    ├── test_wvr_module.cpp
    ├── test_missile_module.cpp
    ├── test_wingman_module.cpp
    ├── test_digital_brain.cpp
    └── traces/
        ├── s1_baseline.txt
        ├── s2_baseline.txt
        └── ...  (scenario trace baselines)
```

### 13.2 CMakeLists.txt

```cmake
cmake_minimum_required(VERSION 3.20)

add_library(F4::ai ALIAS f4-ai)

add_library(f4-ai
    src/sensor_fusion.cpp
    src/digital_brain.cpp
    src/takeoff_module.cpp
    src/landing_module.cpp
    src/navigation_module.cpp
    src/refuel_module.cpp
    src/collision_avoid_module.cpp
    src/bvr_module.cpp
    src/wvr_module.cpp
    src/missile_module.cpp
    src/wingman_module.cpp
)

target_include_directories(f4-ai PUBLIC include)
target_link_libraries(f4-ai PUBLIC
    F4::flight-model
    F4::entities
    F4::messaging
    F4::fsm        # state-machine
    F4::geo
    F4::data
    F4::math
)

target_compile_features(f4-ai PUBLIC cxx_std_20)

# Tests
if(BUILD_TESTING)
    add_subdirectory(tests)
endif()
```

---

## 14. Risks & Mitigations

| Risk | Severity | Mitigation |
|------|----------|-----------|
| **Landing SM complexity** (4778 lines → 19 states) | High | Build incrementally: start with a simplified 8-state approach (Ingressing → Holding → ToBase → ToFinal → OnFinal → ClearToLand → Landed → TaxiOff). Add emergency/abort paths only after the happy path works. |
| **BVR profile data** (18 profiles × 20 tactics) | Medium | Start with Level1a profile + Crank/Pump/Notch tactics only. These cover 80% of BVR behavior. Add remaining profiles as data-driven configuration. |
| **Maneuver data** (9×9 table from mnvrdata.dat) | Medium | Parse `mnvrdata.dat` in f4-convert. Store as JSON. Load via f4-data. Default to F-16 class if data unavailable. |
| **Autopilot tuning** (PID gains) | Medium | Start with FreeFalcon's published gains. Validate against step-response tests. Tune only after integration tests show oscillation or divergence. |
| **AI↔FlightModel coupling** (which model, when) | Low | The `use_complex_model()` method is a pure function of DigiMode + threat state. No heuristics, no tuning. |
| **F4Flight trap** (accumulation without observability) | High | Every module has a trace BEFORE it has behavior. No module is "done" until its trace is greppable and a baseline is checked in. |
| **Injection harness drift** (test harness ≠ reality) | High | Integration tests (S1-S7) use `save1.world.json` entities. Unit tests use minimal mock EntityWorlds. No synthetic scenario between these two extremes. |

---

## Appendix A: FreeFalcon Source File Index

| File | Purpose | Approx Lines |
|------|---------|-------------|
| digi.h | DigitalBrain class declaration | 1209 |
| digimain.cpp | FrameExec() — per-frame driver | ~566 |
| dlogic.cpp | DecisionLogic(), mode selection | ~936 |
| targeting.cpp | TargetSelection(), DoTargeting() | ~200 |
| sfusion.cpp | SensorFusion(), threat scoring | ~504 |
| merge.cpp | MergeCheck(), MergeManeuver() | ~275 |
| bvrengage.cpp | BvrEngageCheck(), BvrChooseTactic() | ~679 |
| wvrengage.cpp | WvrEngageCheck(), WvrEngage() | ~78 |
| gunsjink.cpp | GunsJinkCheck(), GunsJink() | — |
| mengage.cpp | MissileEngageCheck(), FireControl() | — |
| mdefeat.cpp | MissileDefeat() | — |
| gengage.cpp | Ground engagement | — |
| gndattck.cpp | Ground attack logic | — |
| digi_waypoint.cpp | FollowWaypoints() | — |
| autopilot.cpp | PIDLoop(), AltHold() | ~323 |
| digi_refuel.cpp | Air refueling SM | ~1030 |
| tankbrn.cpp | Tanker brain | — |
| digi_landme.cpp | Landing/ATC system | ~4778 |
| digi_cavoid.cpp | CollisionCheck(), CollisionAvoid() | — |
| facbrain.cpp | FACBrain | — |
| flitlead.cpp | CommandFlight() | — |
| winglogic.cpp | AiRunDecisionRoutines() | — |
| wingactions.cpp | AiFollowLead() | — |
| wingradio.cpp | AiMakeRadioResponse() | ~867 |
| formdata.cpp | ACFormationData | — |
| simbrain.h | BaseBrain | 53 |

## Appendix B: Enum Cross-Reference

| f4-ai Enum | FreeFalcon Enum | Count | Source |
|-------------|-----------------|-------|--------|
| `DigiMode` | `DigiMode` | 26 | digi.h |
| `SkillLevel` | (skillLevelBits) | 4 | digi.h |
| `TakeoffState` | (takeoff ATC states) | 10 | digi_landme.cpp |
| `LandingState` | (landing ATC states) | 19 | digi_landme.cpp |
| `RefuelState` | `RefuelStatus` | 5 | digi.h |
| `WVRTactic` | `WvrTacticType` | 11 | digi.h |
| `BVRProfile` | `BVRProfileType` | 18 | digi.h |
| `BVRTactic` | `BVRInterceptType` | 22 | digi.h |
| `FormationType` | (WM* enums) | 16+ | formdata.cpp |
| `CAState` | (GroundAvoidMode/CollisionAvoidMode) | 3 | dlogic.cpp |
| `WingState` | (wingman action flags) | 4 | winglogic.cpp |
| `AGDoctrine` | `AG_DOCTRINE` | 4 | digi.h |
| `AGApproach` | `AG_APPROACH` | 5 | digi.h |
| `WaypointState` | `WaypointState` | 17 | digi.h |
