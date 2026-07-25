# FreeFalcon Architecture Analysis
## High-Level System Design for F4Flight Replication

**Objective**: Extract and abstract FreeFalcon's flight model and AI systems as independent, clean C++ libraries.

---

## 1. System Overview

FreeFalcon is a modular flight simulator built on a **layered architecture**:

```
┌─────────────────────────────────────────────────┐
│        UI Layer (UI95, Graphics, MFDs)          │
├─────────────────────────────────────────────────┤
│  Avionics & Systems (FCC, ICP, Lantirn, SMS)    │
├─────────────────────────────────────────────────┤
│  Simulation Core (Aircraft, Airframe, Controls) │
├─────────────────────────────────────────────────┤
│  AI Decision Logic (DigitalBrain, Tactics)      │
├─────────────────────────────────────────────────┤
│  Flight Dynamics (AirframeClass, 6-DOF Physics) │
├─────────────────────────────────────────────────┤
│  Math & Utils (MathLib, Geometry, DataFiles)    │
└─────────────────────────────────────────────────┘
```

The **DigiAI** (Digital Brain) is responsible for autonomous aircraft decision-making and sits **above** the flight model, consuming and directing its state.

---

## 2. Core Component Breakdown

### 2.1 **Flight Dynamics Layer** (`src/sim`)

#### Key Classes:
- **`AircraftClass`**: Master aircraft entity aggregating all systems
- **`AirframeClass`**: 6-DOF physics engine (forces, moments, integration)
- **`AeroData`**: Lookup tables for aerodynamic coefficients (Cl, Cd, Cy) indexed by Mach/AOA
- **`DecoyDispenser`**: Countermeasure management

#### Responsibilities:
- State representation: position, velocity, attitude, rates
- Force/moment calculation from control inputs + environmental effects
- Integration of equations of motion
- Damage modeling and degradation
- Engine thrust and fuel management

#### Design Pattern:
- **Data-driven**: Aerodynamic data loaded from external files (lookup tables)
- **Modular**: Airframe logic separate from vehicle-level systems
- **Timestep-based**: Fixed or variable integration with `SimLibElapsedTime`

#### Key Methods to Abstract:
```cpp
// Core 6-DOF simulation
class Airframe {
    void ExecuteAerodynamics();      // Compute Cl, Cd, Cy forces
    void ExecuteEngineModel();       // Thrust, fuel flow, engine state
    void IntegrateState();           // Update position, velocity, attitude
    void ApplyDamage();              // Degrade performance
};
```

---

### 2.2 **Aircraft Systems** (`src/sim/include`)

FreeFalcon models discrete aircraft systems as independent subsystems:

#### **Flight Control Surfaces**
- Elevator, aileron, rudder, flaperons
- Canards (for F-16), variable geometry wings
- **Modeled via**: `PilotInputs` + `DOFsNSwitches` (degrees of freedom + switch states)

#### **Avionics Suite** (Complex)
- **`FireControlComputer (FCC)`**: Master avionics controller
  - Master mode selection (AA, AG, Nav, ILS, etc.)
  - Sub-mode logic (CCIP, CCRP, Snapshot, etc.)
  - Gun/missile firing solutions
  
- **`MFDClass`**: Multifunction Display driver
  - Mode handling (Radar, TFR, FLIR, Nav, etc.)
  - Symbol generation and rendering abstraction
  
- **`SMSClass`**: Stores Management System
  - Weapon inventory and station management
  - Jettison logic, chaff/flare dispensing
  
- **`NavigationSystem`**: INS/TACAN/GPS fusion
  - Waypoint management
  - Mark points, data-link points
  - Instrument modes (NAV, ILS, TACAN)

- **`LantirnClass`**: Targeting Pod simulation
  - FLIR imagery
  - Laser range-finder, designator
  - Handoff-on-contact (HonC)

- **`ICPClass`**: Integrated Control Panel (pilot input device)
  - Steerpoint editing
  - Mode/frequency selection
  - Datalink controls

#### **Power Distribution**
- Enumerated power states per subsystem
- Cascading power loss effects (realistic avionics shutdown)

#### **Environmental Simulation**
- Weather effects on radar, visibility
- Terrain-following radar (TFR) logic
- Radar modes and tracking

---

### 2.3 **AI Decision Engine** (`src/sim/digi` + `src/campaign`)

#### **`DigitalBrain` Class** - The Core AI

**Inheritance**: `BaseBrain` → controls the `AircraftClass`

**Key Responsibilities**:
1. **Mode Selection**: Determines flight model complexity
   - `SIMPLE_MODE_AF`: Simplified flight model (combat non-threatening units, formation flying)
   - `SIMPLE_MODE_OFF`: Complex/realistic flight model (when threatened or player-controlled)
   - Controlled by `SelectFlightModel()` method

2. **Combat Decision Hierarchy**:
   ```
   ├─ Actions()            // Current behavioral mode
   ├─ DecisionLogic()       // Long-term strategy
   ├─ TargetSelection()     // Threat prioritization
   ├─ WeaponSelection()     // Load out strategy
   ├─ FireControl()         // Trigger decisions
   └─ RunDecisionRoutines() // Ongoing checks
   ```

3. **Tactical Maneuvers** (BVR/WVR):
   - **BVR Types**: Intercept, Formation, Side-Offset, Pince, Pursuit, Pump, Crank, Notch, Grind, etc.
   - **WVR Types**: Merge behaviors, Hit-and-Run, Limited/Unlimited circles
   - **Spike Reactions**: ECM, Beam-dump, Drag maneuvers
   - Enum-driven behavior selection based on threat assessment

4. **Formation Flight**:
   - `FindFormation()`: Locate and join formation
   - `ACFormationData`: External formation pattern database
   - Lead-wing relationships maintained in state

5. **Sensor Management**:
   - `SensorFusion()`: Combine radar, IRST, visual cues
   - Radar mode selection via `chooseRadarMode()`
   - Target track management

6. **GCI & Tactical Network**:
   - Communication with `TeamInfo` (doctrine, behavior settings)
   - Receives ground-controlled intercept vectors
   - Doctrine parameters: shoot-shoot percentages, engagement ranges

#### **Flight Model Selection Logic** (Critical):
```cpp
int DigitalBrain::SelectFlightModel() {
    if (self->OnGround()) 
        return SIMPLE_MODE_AF;  // Always simple while taxiing
    
    if (self->IsAcStatusBitsSet(PILOT_EJECTED) || pctStrength <= 0)
        return SIMPLE_MODE_OFF;  // Dead = realistic plummet
    
    if (threatPtr != NULL)
        return SIMPLE_MODE_OFF;  // Active threat = complex model
    
    switch (curMode) {
        case FollowOrdersMode:
        case WingyMode:
            return mpActionFlags[AI_USE_COMPLEX] 
                ? SIMPLE_MODE_OFF : SIMPLE_MODE_AF;
        
        case RefuelingMode:
            return g_bAIRefuelInComplexAF 
                ? SIMPLE_MODE_OFF : SIMPLE_MODE_AF;
        
        default:
            return SIMPLE_MODE_AF;  // Formation, nav, landing = simple
    }
}
```

#### **Control Output**:
The DigiAI generates control inputs consumed by AirframeClass:
```cpp
struct PilotInputs {
    float throttleCmd;     // 0.0–1.0
    float pitchStick;      // -1.0–+1.0
    float rollStick;       // -1.0–+1.0
    float rudderPedal;     // -1.0–+1.0
    int landingGear;       // up/down
    int flaps;             // various stages
    int airbrakes;         // on/off
    // ... weapon selections, radar modes, etc.
};
```

---

### 2.4 **Campaign & Mission Layer** (`src/campaign`)

- **`AirUnitClass`**: Squadrons, flights, individual aircraft
- **`DogfightClass`**: Multiplayer dogfight arena setup
- **`FlightClass`**: Group-level AI (lead gives orders to wing)
- **`ObjectiveClass`**: Mission targets and strategic points

---

### 2.5 **Data-Driven Configuration**

Key external data sources:
- **Airframe data**: Aerodynamic lookup tables (Cl, Cd, Cy vs. Mach, AOA)
- **Missile data**: TVM, acceleration, seeker logic
- **Bomb data**: Fuse logic, drop patterns
- **Formation data**: Relative position offsets, lead-wing tactics
- **Maneuver data**: BVR/WVR tactic definitions
- **Doctrine files**: Engagement rules per side (NATO, Red Team, etc.)

---

## 3. System Integration Points

### 3.1 **Frame Flow (Simplified)**

```
1. Simulation Step:
   a. Collect pilot/AI input        → PilotInputs struct
   b. Update aircraft state          → AircraftClass::Update()
   c. Execute airframe physics       → AirframeClass::ExecuteAerodynamics/Engines
   d. Integrate 6-DOF               → new position, velocity, attitude
   e. Update subsystems             → FCC, avionics, sensors
   f. AI decision cycle             → DigitalBrain::Update()
   g. Render/Network sync           → Graphics, multiplayer
```

### 3.2 **DigitalBrain ↔ AircraftClass Interface**

```
AircraftClass (Master)
  ├─ Owns: AirframeClass, SMSClass, FCC, Lantirn, ICP, etc.
  ├─ Owns: DigitalBrain (if AI-controlled)
  └─ Update() calls:
      ├─ DigitalBrain::Update()     [AI generates PilotInputs]
      ├─ Airframe::Step()            [Physics integration]
      └─ Systems::Update()           [Avionics, weapons, etc.]
```

### 3.3 **Threat Assessment & Targeting**

The DigiAI maintains:
- `targetPtr`: Current lock
- `targetData`: Range, closure, aspect angle
- `targetList`: Radar/IRST track list
- `threatPtr`: Highest-priority threat

Selects maneuvers based on:
- **Aspect angle** (nose-on, beam, tail)
- **Range** (within missile range? gun range?)
- **Energy state** (altitude, speed relative to opponent)
- **Runway status** (are landing zones available?)

---

## 4. Proposed Library Architecture for F4Flight

### 4.1 **Layer 1: Flight Dynamics Engine**
**Dependencies**: Math library only

```cpp
namespace F4 {
  namespace Flight {
    
    class Atmosphere {
      float GetDensity(float altitude_m);
      float GetTemp(float altitude_m);
      float GetSpeed(float altitude_m);  // Sound speed
    };
    
    class Aerodynamics {
      void LoadLookupTable(const std::string& file);
      float GetLift(float mach, float aoa_deg);
      float GetDrag(float mach, float aoa_deg);
      float GetSideFroce(float mach, float aoa_deg);
    };
    
    struct AircraftState {
      // Position
      double x, y, z;  // meters (ENU or ECEF)
      // Velocity
      double vx, vy, vz;
      // Attitude (quaternion preferred, or Euler angles)
      float pitch, roll, yaw;  // degrees
      // Angular rates
      float p, q, r;  // deg/sec
      // Engine state
      float throttle;    // 0–1
      float engineRPM;
      float fuelQuantity;
    };
    
    class Airframe {
      void SetState(const AircraftState& state);
      AircraftState GetState() const;
      
      void SetControlInputs(float pitch, float roll, float yaw, float throttle);
      
      // Core physics
      void ComputeAerodynamicForces();
      void ComputeEngineThrust();
      void IntegrateOneStep(float dt_sec);
      
      void ApplyExternalForce(const Vec3& force_newtons);
      void ApplyDamage(float damage_fraction);  // 0–1
    };
  }
}
```

**Design Decisions**:
- Decoupled from rendering, networking, UI
- SI units (meters, radians, kg, Newtons)
- Pure math: no I/O, no state machines
- Timestep agnostic (caller provides dt)

---

### 4.2 **Layer 2: Aircraft Systems**
**Dependencies**: Layer 1, Math, Data-driven configs

```cpp
namespace F4 {
  namespace Avionics {
    
    class Radar {
      enum Mode { OFF, SEARCH, ACQ, TRACK, CW };
      void SetMode(Mode m);
      std::vector<Track> GetTracks() const;
      void Update(float dt, const AircraftState& ownship);
    };
    
    class NavigationSystem {
      void AddWaypoint(const WayPoint& wp);
      WayPoint GetCurrentWaypoint() const;
      float DistToWaypoint() const;
      Vec3 GetPositionError();  // INS vs. GPS
    };
    
    class WeaponStore {
      int GetLoadout() const;  // Inventory bitmask
      void SelectWeapon(int hardpoint);
      bool CanFire() const;
      void Fire(const TargetInfo& tgt);
    };
    
    class DamageModel {
      void ApplyDamage(int component, float damage);
      bool IsSystemOperational(int system) const;
      float GetPerformancePenalty() const;
    };
  }
}
```

---

### 4.3 **Layer 3: AI Decision Engine**
**Dependencies**: Layers 1-2, Tactics database

```cpp
namespace F4 {
  namespace AI {
    
    class ThreatAssessment {
      struct Threat {
        int id;
        float range;
        float closureRate;
        float aspectAngle;
        float priority;
      };
      
      std::vector<Threat> GetThreats() const;
      Threat GetPrimaryThreat() const;
    };
    
    class CombatTactics {
      enum BVRTactic { 
        Intercept, SideOffset, Pince, Notch, Crank, Grind 
      };
      
      BVRTactic SelectBVRTactic(const ThreatAssessment&);
      ControlCommand ExecuteTactic(float dt);
    };
    
    struct ControlCommand {
      float pitchCmd, rollCmd, yawCmd, throttleCmd;
      int flapsCmd, gearCmd;
      int radarMode, weaponSelect;
    };
    
    class DigitalBrain {
      enum Mode { 
        SIMPLE, COMPLEX
      };
      
      void Update(float dt, 
                  const AircraftState& state,
                  const Avionics& avionics);
      
      ControlCommand GetControlCommand() const;
      
    private:
      Mode SelectFlightModel();
      void DoDecisionLogic();
      void TargetSelection();
      void ManeuverSelection();
    };
  }
}
```

---

### 4.4 **Configuration & Data Management**
**Dependencies**: File I/O, data parsing

```cpp
namespace F4 {
  namespace Data {
    
    class AirframeDatabase {
      struct AirframeSpec {
        std::string name;
        float dryWeight;
        float maxThrust;
        Aerodynamics aeroModel;
        // ... engine, control authority, etc.
      };
      
      AirframeSpec Load(const std::string& type);
    };
    
    class TacticsDatabase {
      struct BVRProfile {
        std::vector<Maneuver> sequence;
        float priority;
      };
      
      std::vector<BVRProfile> GetProfilesFor(
          int aircraftType, int threatType
      );
    };
  }
}
```

---

## 5. Clean Modern C++ Best Practices

### 5.1 **No Global State (FreeFalcon Issue)**
- ❌ `extern AircraftClass *self` (DigiAI)
- ✅ Dependency injection: `DigitalBrain(AircraftClass& aircraft, Airframe& af)`

### 5.2 **Memory Management**
- ❌ Manual `new`/`delete` + SmartHeap pools
- ✅ `std::unique_ptr<Airframe>`, RAII for lifetime management

### 5.3 **Type Safety**
- ❌ Bit flags (`SIM_FLAGS & 0x0001`)
- ✅ `enum class` + `std::bitset<N>`

### 5.4 **Data vs. Logic Separation**
- ❌ Hardcoded magic numbers in code
- ✅ External YAML/JSON configs loaded once

### 5.5 **Interface Abstraction**
```cpp
// Instead of FreeFalcon's tightly coupled systems:
class Airframe {
  virtual ~Airframe() = default;
  virtual void SetControlInputs(const ControlCommand&) = 0;
  virtual AircraftState GetState() const = 0;
};

class SimpleAirframe : public Airframe { /* ... */ };
class ComplexAirframe : public Airframe { /* ... */ };
```

---

## 6. Integration Challenges & Mitigations

| Challenge | FreeFalcon Approach | Proposed Fix |
|-----------|---------------------|--------------|
| **Coupling** | Everything refs `AircraftClass` globally | Inject dependencies, use interfaces |
| **Timesteps** | Hardcoded `SimLibElapsedTime` | Accept `dt` in method signatures |
| **Data Loading** | Monolithic resource system | Modular config loaders |
| **Flight Model Toggle** | Complex conditional logic in DigiAI | Encapsulate in pluggable strategy |
| **Power System** | Cascading enum flags scattered | Centralized `PowerBus` class |
| **Coordinate Systems** | Mix of cartesian, geodetic, ENU | Standardize on single system (ENU) |
| **Units** | Mix of feet, meters, knots, fps | SI units everywhere; convert at I/O |

---

## 7. Replication Checklist for F4Flight

### Phase 1: Flight Model
- [ ] 6-DOF rigid body dynamics (pitch, roll, yaw + linear motion)
- [ ] Aerodynamic lookup table system
- [ ] Engine thrust model (idle → max, afterburner)
- [ ] Fuel consumption
- [ ] Damage/degradation effects
- [ ] Landing gear, flaps, airbrakes, canards

### Phase 2: Aircraft Systems
- [ ] Basic avionics (HUD, instruments)
- [ ] Navigation system (waypoints, INS)
- [ ] Weapon stores, gun/missile firing logic
- [ ] Radar (basic search/track)
- [ ] Radar warning receiver (RWR)
- [ ] Chaff/flare dispensing

### Phase 3: AI
- [ ] Threat assessment (range, closure, aspect)
- [ ] BVR tactics (intercept, notch, crank, grind)
- [ ] WVR tactics (merge, scissors, yo-yo)
- [ ] Formation flight
- [ ] Weapon selection logic
- [ ] Target handoff between flights

### Phase 4: Integration
- [ ] Mission/campaign system
- [ ] Multiplayer synchronization
- [ ] Performance optimization
- [ ] UI/rendering integration

---

## 8. Key FreeFalcon Files to Reference

**Flight Dynamics**:
- `src/sim/include/airframe.h` — 6-DOF physics declarations
- `src/sim/include/aircrft.h` — AircraftClass master
- `src/falclib/include/arfrmdat.h` — Airframe data structures

**AI/DigitalBrain**:
- `src/sim/include/digi.h` — DigitalBrain declarations
- `src/sim/digi/dlogic.cpp` — Decision logic implementation
- `src/sim/digi/digimain.cpp` — Initialization & main loop

**Avionics**:
- `src/sim/include/fcc.h` — Fire Control Computer
- `src/sim/include/sms.h` — Stores Management System
- `src/sim/include/navsystem.h` — Navigation system
- `src/sim/include/lantirn.h` — Targeting pod

**Tactics & Maneuvers**:
- `src/falclib/tactics.cpp` — BVR/WVR maneuver selection
- `src/sim/digi/dformation.cpp` — Formation logic
- `ACFormationData` — Formation pattern database

**Configuration**:
- `src/falclib/theaterdef.cpp` — Theater/data loading
- `src/falclib/include/rules.h` — Simulation rules (flight model, avionics, weapon effects)

---

## 9. Conclusion

The FreeFalcon codebase demonstrates a **hierarchical, multi-scale flight simulation**:

1. **Physics-first**: 6-DOF airframe model drives everything
2. **Systems-rich**: Discrete avionics subsystems with independent state
3. **AI-aware**: Digital brain makes tactical decisions that modulate flight model complexity
4. **Data-driven**: Lookup tables + configuration files separate logic from parameters

For **F4Flight**, the key architectural win is:
- **Decouple layers** via clean C++ interfaces
- **Externalize data** (aero tables, tactics, formations)
- **Simplify state management** (no global `self` pointer)
- **Preserve realism** (keep the 6-DOF physics, DigiAI heuristics)

This allows **incremental, testable development** and **easy substitution** of components (e.g., simple vs. complex flight model, AI tactics) without tangling the entire codebase.

