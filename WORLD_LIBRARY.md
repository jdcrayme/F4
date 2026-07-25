# F4Flight World Library Architecture
## Entity Management, Sensor Networks, Communications, and Dynamic Campaign Integration

---

## Overview

The **World Library** is the central integration layer connecting:
1. **Persistent World State** — All entities (aircraft, ground units, objectives, bases)
2. **Sensor Networks** — Radar, IRST, RWR querying world objects
3. **Communications System** — Inter-aircraft radio, tactical datalinks, GCI
4. **Dynamic Campaign System** — Mission/package generation feeding into DigiAI autopilot
5. **Entity Lifecycle** — Aggregated ↔ Deaggregated (campaign ↔ sim representation)

This library operates at a higher level than individual aircraft, providing the environment in which they act.

---

## 1. Entity Database & World State

### 1.1 Core Architecture

**FreeFalcon's Approach**:
- `VuDatabase` — Distributed entity lookup by `VU_ID`
- `CampBaseClass` — Abstract base for all campaign entities (aggregated state)
- Aggregation ↔ Deaggregation — Units switch between campaign-level (abstract) and sim-level (concrete) representations
- Grid-based partitioning for spatial queries

```cpp
namespace F4 {
  namespace World {
    
    // Entity type enumeration
    enum class EntityType {
      AIRCRAFT,
      GROUND_UNIT,      // Tank, APC, etc.
      OBJECTIVE,        // Airbase, city, factory, SAM site
      WEAPON,           // Missile, bomb, chaff bundle
      SENSOR_CONTACT    // Radar/IRST track from a sensor
    };
    
    // Unique entity identifier (analogous to VU_ID in FreeFalcon)
    using EntityID = std::uint64_t;
    
    // Position in world (meters, ENU relative to bullseye or ECEF)
    struct WorldPosition {
      double x, y, z;
      
      float Distance(const WorldPosition& other) const;
      float BearingTo(const WorldPosition& other) const;
    };
    
    // Entity base class with common properties
    class Entity {
    public:
      virtual ~Entity() = default;
      
      EntityID GetID() const { return id_; }
      EntityType GetType() const { return type_; }
      int GetTeam() const { return team_; }
      
      virtual WorldPosition GetPosition() const = 0;
      virtual float GetHeading() const = 0;
      virtual float GetAltitude() const = 0;
      
      // Spotting/visibility
      virtual bool IsVisibleTo(int observer_team) const = 0;
      virtual float GetRCS() const { return 0.0f; }  // Radar cross-section (m²)
      
      // Health/status
      virtual float GetHealth() const = 0;
      virtual bool IsDead() const { return GetHealth() <= 0.0f; }
      
    protected:
      EntityID id_;
      EntityType type_;
      int team_;
      WorldPosition position_;
    };
  }
}
```

### 1.2 World Database

```cpp
namespace F4 {
  namespace World {
    
    class WorldDatabase {
    public:
      // Entity access
      Entity* FindEntity(EntityID id);
      std::vector<Entity*> FindByType(EntityType type);
      std::vector<Entity*> FindByTeam(int team);
      std::vector<Entity*> FindByTeamAndType(int team, EntityType type);
      
      // Spatial queries (grid-based)
      std::vector<Entity*> FindNear(const WorldPosition& pos, float radius_m);
      std::vector<Entity*> FindInBox(
          const WorldPosition& min, 
          const WorldPosition& max
      );
      
      // Registration/deregistration
      void Register(Entity* entity);
      void Deregister(EntityID id);
      void Update(Entity* entity);  // Spatial index update
      
      // Batch queries (efficient)
      struct QueryResult {
        std::vector<Entity*> entities;
        int query_count;
      };
      QueryResult QueryAll(EntityType filter = EntityType::AIRCRAFT);
      
    private:
      std::unordered_map<EntityID, std::unique_ptr<Entity>> entities_;
      
      // Spatial grid for efficient proximity queries
      class SpatialGrid {
      public:
        void Insert(Entity* e, const WorldPosition& pos);
        void Remove(EntityID id);
        std::vector<Entity*> QueryRadius(const WorldPosition& pos, float radius);
        
      private:
        static constexpr float GRID_CELL_SIZE = 10000.0f;  // 10 km cells
        std::unordered_map<std::uint64_t, std::vector<Entity*>> grid_;
        
        std::uint64_t PositionToGridKey(const WorldPosition& pos) const;
      };
      
      SpatialGrid spatial_grid_;
    };
  }
}
```

### 1.3 Aggregation vs. Deaggregation

In FreeFalcon, a `Flight` (aggregated campaign entity) can "deaggregate" into individual `AircraftClass` entities (sim representation). Reverse is true for campaign abstraction.

```cpp
namespace F4 {
  namespace World {
    
    // Aggregated representation (campaign level)
    class AggregatedFlight : public Entity {
    public:
      AggregatedFlight(EntityID id, int team, int aircraft_count);
      
      // Campaign-level state
      int GetAircraftCount() const { return aircraft_count_; }
      int GetHealthyAircraft() const { return healthy_aircraft_; }
      float GetAverageHealth() const;
      
      // Aggregate kinematics (formation centroid)
      WorldPosition GetPosition() const override;
      float GetHeading() const override;
      float GetAltitude() const override;
      
      // Deaggregate into individual aircraft
      std::vector<EntityID> Deaggregate(WorldDatabase* db);
      
    private:
      int aircraft_count_;
      int healthy_aircraft_;
      std::vector<EntityID> component_ids_;  // Individual aircraft
    };
    
    // Individual aircraft (sim level)
    class SimulatedAircraft : public Entity {
    public:
      SimulatedAircraft(EntityID id, int team);
      
      // Individual sim state
      void UpdateFromAirframeModel(const F4::Flight::AircraftState& state);
      
      // Aggregate back up
      EntityID GetFlightID() const { return flight_id_; }
      void SetFlightID(EntityID fid) { flight_id_ = fid; }
      
      // Sensor properties
      float GetRCS() const override;
      
    private:
      EntityID flight_id_;
      F4::Flight::AircraftState state_;
    };
    
    // Aggregation manager
    class AggregationManager {
    public:
      // Convert flight from campaign → sim representation
      void Deaggregate(EntityID flight_id, WorldDatabase* db);
      
      // Combine individual aircraft back to flight (for autopilot, landing, etc.)
      void Aggregate(const std::vector<EntityID>& aircraft_ids, WorldDatabase* db);
      
    private:
      std::unordered_map<EntityID, AggregatedFlight*> aggregates_;
      std::unordered_map<EntityID, SimulatedAircraft*> simulated_;
    };
  }
}
```

---

## 2. Sensor Network System

### 2.1 Radar as a World Query

Radars don't search a single aircraft's viewpoint—they query **world objects** to determine what's visible.

```cpp
namespace F4 {
  namespace Sensors {
    
    // Track/contact detected by a sensor
    struct SensorContact {
      EntityID target_id;
      WorldPosition position;
      float range_m;
      float bearing_deg;
      float altitude_m;
      float closure_rate;  // positive = approaching
      float confidence;    // 0–1, affected by RCS, chaff, ECM
      CampaignTime last_update;
      
      // Target quality (determines what player sees on HUD)
      enum Quality { SUSPECTED, INTERMITTENT, TRACK, LOCKED } quality;
    };
    
    // Base sensor class
    class Sensor {
    public:
      virtual ~Sensor() = default;
      
      // Perform sensor update
      virtual std::vector<SensorContact> Update(
          float dt,
          const WorldPosition& ownship_pos,
          const WorldDatabase* world
      ) = 0;
      
      void SetOwner(EntityID owner) { owner_id_ = owner; }
      EntityID GetOwner() const { return owner_id_; }
      
      bool IsActive() const { return is_active_; }
      void SetActive(bool active) { is_active_ = active; }
      
    protected:
      EntityID owner_id_;
      bool is_active_;
      std::vector<SensorContact> current_contacts_;
      
      // Common query helpers
      bool CanSeeTarget(const Entity* target, const WorldPosition& ownship);
    };
  }
}
```

### 2.2 Radar Implementation

```cpp
namespace F4 {
  namespace Sensors {
    
    class Radar : public Sensor {
    public:
      enum Mode { OFF, STANDBY, SEARCH, ACQ, TRACK, CW_ILLUMINATION };
      
      Radar(EntityID owner, float max_range_m = 100000.0f);
      
      void SetMode(Mode m) { mode_ = m; }
      Mode GetMode() const { return mode_; }
      
      // Returns all contacts within radar range
      std::vector<SensorContact> Update(
          float dt,
          const WorldPosition& ownship_pos,
          const WorldDatabase* world
      ) override;
      
      // Lock a target
      void LockTarget(EntityID target_id) { locked_target_ = target_id; }
      EntityID GetLockedTarget() const { return locked_target_; }
      
      // Radar-specific properties
      void SetScanPattern(float az_half_angle, float el_half_angle);
      void SetAzimuthCenter(float az) { az_center_ = az; }
      void SetElevationCenter(float el) { el_center_ = el; }
      
    private:
      Mode mode_;
      EntityID locked_target_;
      float max_range_;
      float az_half_angle_, el_half_angle_;  // Scan limits
      float az_center_, el_center_;           // Scan center
      float cycle_time_;                       // Sweep duration
      
      // Chaff/ECM effects
      struct CountermeasureEffect {
        float chaff_effectiveness;  // 0–1
        float ecm_effectiveness;    // 0–1
      };
      
      CountermeasureEffect EvaluateECM(
          const Entity* target,
          float range,
          const WorldDatabase* world
      );
      
      // Tracking algorithm
      void TrackLocked(float dt, Entity* target, SensorContact& contact);
    };
    
    class InfraredSearchAndTrack : public Sensor {
      // IRST for passive targeting
    public:
      std::vector<SensorContact> Update(
          float dt,
          const WorldPosition& ownship_pos,
          const WorldDatabase* world
      ) override;
      
    private:
      // Seeker slew angles
      float slew_az_, slew_el_;
    };
    
    class RadarWarningReceiver : public Sensor {
      // Passive receiver of emitting radars
    public:
      std::vector<SensorContact> Update(
          float dt,
          const WorldPosition& ownship_pos,
          const WorldDatabase* world
      ) override;
      
    private:
      void DetectRadarEmissions(const WorldDatabase* world);
    };
  }
}
```

### 2.3 Sensor Fusion

Combines multiple sensor inputs for more accurate targeting.

```cpp
namespace F4 {
  namespace Sensors {
    
    class SensorFusion {
    public:
      SensorFusion(EntityID owner);
      
      void AddSensor(std::unique_ptr<Sensor> sensor);
      
      // Master track list (fused from all sensors)
      struct FusedTrack {
        EntityID target_id;
        WorldPosition estimated_position;
        float confidence;
        CampaignTime last_update;
        int num_sensors_tracking;  // How many sensors see this
      };
      
      std::vector<FusedTrack> GetMasterTrackList();
      
      void Update(
          float dt,
          const WorldPosition& ownship_pos,
          const WorldDatabase* world
      );
      
      // Query
      FusedTrack* GetTrack(EntityID target_id);
      FusedTrack* GetBestTrack();  // Highest confidence
      
    private:
      std::vector<std::unique_ptr<Sensor>> sensors_;
      std::vector<FusedTrack> fused_tracks_;
      
      void FuseTracks();  // Combine sensor contacts into unified track
    };
  }
}
```

---

## 3. Communications System

### 3.1 Radio Network

Aircraft communicate via a centralized radio/datalink system. All transmissions are visible to listeners on the same frequency.

```cpp
namespace F4 {
  namespace Comms {
    
    // Message types
    enum class MessageType {
      RADIO_CALL,           // Voice radio (e.g., "Bogey dope", "Fox 3")
      TACTICAL_DATALINK,    // Secure digitial link (TDL, Link-16 style)
      FLIGHT_COMMAND,       // Lead → wingmen orders
      GCI_VECTOR,           // Ground control intercept
      MAYDAY,               // Emergency distress
      NAVIGATION_UPDATE,    // INS correction, waypoint sync
    };
    
    struct Message {
      EntityID sender_id;
      EntityID recipient_id;      // May be broadcast (0xFFFFFFFF)
      MessageType type;
      int frequency_hz;           // Radio frequency
      CampaignTime timestamp;
      std::string content;        // Text or serialized data
      
      bool IsEncrypted() const;
      bool IsDatalink() const;
    };
    
    // Frequency allocation
    class FrequencyAllocator {
    public:
      int AllocateFrequency(int team);
      void ReleaseFrequency(int freq);
      
    private:
      std::set<int> allocated_frequencies_;
      static constexpr int MIN_FREQ = 240_MHz;
      static constexpr int MAX_FREQ = 400_MHz;
    };
    
    // Central radio system
    class RadioNetwork {
    public:
      RadioNetwork();
      
      // Broadcast a message
      void Transmit(const Message& msg);
      
      // Receivers tune to a frequency
      void SubscribeTo(EntityID receiver, int frequency);
      void UnsubscribeFrom(EntityID receiver, int frequency);
      
      // Retrieve messages for a specific entity
      std::vector<Message> GetReceivedMessages(EntityID receiver);
      
      // Jamming effect
      void ApplyJamming(EntityID jammer, int frequency, float effectiveness);
      
    private:
      struct Channel {
        std::vector<Message> message_queue;
        std::set<EntityID> subscribed_receivers;
        float jam_effectiveness;  // 0–1
      };
      
      std::unordered_map<int, Channel> channels_;
      std::unordered_multimap<EntityID, int> subscriptions_;
    };
    
    // Tactical datalink (Link-16 style)
    class TacticalDatalink {
    public:
      // Share sensor data between allied units
      void ShareTrack(
          EntityID sharer,
          const Sensors::SensorContact& contact,
          int recipient_team
      );
      
      // Query shared tracks
      std::vector<Sensors::SensorContact> GetSharedTracks(int team);
      
    private:
      struct SharedTrack {
        EntityID source_id;
        Sensors::SensorContact contact;
        CampaignTime age;
      };
      
      std::vector<SharedTrack> shared_tracks_;
      
      void AgeOutOldTracks();
    };
  }
}
```

### 3.2 Voice Radio & Radio Calls

Broadcasts situational information that AI can respond to.

```cpp
namespace F4 {
  namespace Comms {
    
    // Radio call types (what pilots announce)
    enum class RadioCallType {
      BOGEY_DOPE,          // "Bogey dope from flight lead: bogey 30 miles"
      FOX_CALL,            // "Fox 3 away" (missile launch)
      MISSILE_LAUNCH,      // "SAM launch, break left!"
      SPLASH,              // "Splash one" (kill)
      WINCHESTER,          // "Winchester" (out of ammo)
      BINGO_FUEL,          // "Bingo fuel, returning to base"
      MAYDAY_CALL,         // Distress
      STATUS_CHECK,        // "Two, check in"
      FORMATION_CALL,      // "Spread formation"
    };
    
    class RadioCallHandler {
    public:
      void MakeRadioCall(
          EntityID speaker,
          RadioCallType call_type,
          const std::string& content
      );
      
      // Parse and respond to radio calls
      std::vector<RadioCallType> GetIncomingCalls(EntityID listener);
      
    private:
      struct RadioCall {
        EntityID speaker;
        RadioCallType type;
        std::string content;
        CampaignTime timestamp;
        int frequency;
      };
      
      std::vector<RadioCall> recent_calls_;
    };
  }
}
```

---

## 4. Dynamic Campaign → DigiAI Integration

### 4.1 Mission Planning & Waypoint Generation

The **Dynamic Campaign System** generates missions that are converted into waypoint lists for the DigiAI autopilot.

```cpp
namespace F4 {
  namespace Campaign {
    
    // Mission types (from FreeFalcon)
    enum class MissionType {
      CAP,                // Combat Air Patrol
      CAS,                // Close Air Support
      STRIKE,             // Air-to-ground strike
      SEAD,               // Suppression of Enemy Air Defense
      ESCORT,             // Fighter escort
      BOMBING,            // Strategic bombing
      INTERCEPT,          // Air-to-air intercept
      RECONNAISSANCE,     // Recon mission
      TRANSPORT,          // Troop/cargo transport
    };
    
    // Waypoint as generated by campaign
    struct CampaignWaypoint {
      int sequence;
      WorldPosition position;      // Target location
      float altitude_m;
      float speed_kts;
      CampaignTime time_on_target;  // Expected arrival
      
      enum Action {
        FLY_BY,
        FLY_OVER,
        LOITER,
        ATTACK_TARGET,
        RTB,           // Return to base
        LAND,
      } action;
      
      int loiter_time_minutes;
      EntityID target_id;          // If attacking, which entity
      MissionType context;
    };
    
    // Flight mission package (generated by ATM)
    class MissionPackage {
    public:
      EntityID package_id;
      int team;
      MissionType mission_type;
      std::vector<EntityID> flight_ids;  // Which flights are assigned
      
      // Waypoint list (fed into autopilot)
      std::vector<CampaignWaypoint> waypoints;
      
      WorldPosition ingress_point;
      WorldPosition target_area;
      WorldPosition egress_point;
      WorldPosition recovery_base;
      
      CampaignTime planned_takeoff;
      CampaignTime planned_landing;
      
      // Constraints
      int required_altitude_min, required_altitude_max;
      int required_speed_min, required_speed_max;
      float threat_level;  // Expected enemy opposition
      
      bool IsValid() const;
      std::string GetBriefing() const;
    };
    
    // Campaign mission request
    class MissionRequest {
    public:
      int priority;
      MissionType mission_type;
      EntityID target_id;
      WorldPosition target_area;
      int required_strength;       // # of aircraft needed
      CampaignTime time_on_target;
      int max_distance_nm;
      
      bool CanBeFilledBy(const Squadron* sq) const;
    };
    
    // Air Tasking Manager (ATM) - generates missions
    class AirTaskingManager {
    public:
      // Process mission requests and generate packages
      void Update(float dt, WorldDatabase* world);
      
      // Submit a mission request
      void RequestMission(const MissionRequest& req);
      
      // Get packages for a flight
      std::vector<MissionPackage*> GetAssignedPackages(EntityID flight_id);
      
      // Check out a package (marks it as active)
      MissionPackage* CheckOutPackage(EntityID flight_id);
      
    private:
      std::vector<MissionRequest> pending_requests_;
      std::vector<std::unique_ptr<MissionPackage>> packages_;
      
      void GeneratePackages();
      MissionPackage* BuildPackageForRequest(const MissionRequest& req);
      void AssignFlightsToPackage(MissionPackage* pkg);
    };
  }
}
```

### 4.2 Feeding Waypoints into DigiAI Autopilot

```cpp
namespace F4 {
  namespace AI {
    
    // Autopilot mode (fed by campaign)
    enum class AutopilotMode {
      STANDBY,         // Manual control
      NAVIGATION,      // Follow waypoints
      TARGET_TRACK,    // Track designated target
      RTB,             // Return to base
      LANDING,         // Autoland sequence
      FORMATION,       // Maintain formation
    };
    
    // Bridge between campaign and aircraft AI
    class FlightAutopilot {
    public:
      FlightAutopilot(EntityID flight_id);
      
      // Campaign loads mission into autopilot
      void LoadMission(const Campaign::MissionPackage* mission);
      
      // Get next waypoint
      const Campaign::CampaignWaypoint* GetCurrentWaypoint() const;
      const Campaign::CampaignWaypoint* GetNextWaypoint() const;
      
      // Update autopilot state
      void Update(
          float dt,
          const F4::Flight::AircraftState& aircraft_state,
          const Sensors::SensorFusion* sensors
      );
      
      // Get control command for this frame
      F4::AI::ControlCommand GetControlCommand() const;
      
      // Waypoint reached/aborted callbacks
      void OnWaypointReached();
      void OnWaypointSkipped();
      
      // Check if mission is complete
      bool IsMissionComplete() const;
      
    private:
      EntityID flight_id_;
      const Campaign::MissionPackage* current_mission_;
      int current_waypoint_index_;
      AutopilotMode mode_;
      
      // Route planner
      void PlanRouteToWaypoint(const Campaign::CampaignWaypoint& wp);
      
      // Altitude/speed controller
      ControlCommand ComputeNavigationCommand();
    };
  }
}
```

---

## 5. Complete Integration Flow

### 5.1 Frame Loop (Pseudo-code)

```
// Main simulation loop
void SimulationTick(float dt) {
    // 1. Campaign layer updates (slow—minutes timescale)
    campaign.UpdateATM(dt);           // Generate missions
    campaign.UpdateGroundUnits(dt);   // Move ground forces
    
    // 2. World database updates
    world_db.Update();
    
    // 3. Aircraft simulation (fast—milliseconds timescale)
    for (flight : active_flights) {
        // Get autopilot waypoint
        waypoint = flight.autopilot.GetCurrentWaypoint();
        
        // Sensor update
        flight.sensor_fusion.Update(
            dt,
            flight.position,
            &world_db
        );
        
        // DigiAI decision
        digi_brain.Update(
            dt,
            flight.state,
            flight.sensor_fusion,
            world_db
        );
        
        // Get control command (either from DigiAI or autopilot)
        if (autopilot_active) {
            control = flight.autopilot.GetControlCommand();
        } else {
            control = digi_brain.GetControlCommand();
        }
        
        // Physics integration
        for (aircraft : flight.components) {
            aircraft.airframe.SetControlInputs(control);
            aircraft.airframe.IntegrateOneStep(dt);
            
            // Update world database with new position
            world_db.Update(aircraft);
        }
        
        // Radio/comms processing
        radio_network.ProcessReceivedMessages(flight.lead_id);
        
        // Check for damage, fuel, etc.
        flight.CheckStatus();
    }
    
    // 4. Rendering & network sync (decoupled)
    renderer.RenderFrame(world_db);
    network_manager.SyncEntities(world_db);
}
```

### 5.2 Scenario: Campaign Generates Mission → Autopilot Executes

```
**T=0:00**
  → ATM detects SAM site at grid [AB12], requests CAP
  → Generates MissionPackage:
      · Takeoff: Nellis AFB (T+5 min)
      · Ingress: 100 nm north
      · CAP loiter box: [AB12±20nm] at 25,000 ft
      · Duration: 30 min
      · RTB via waypoint [AC10]
      · Landing: Nellis
      · Package contains 2 F-16 flights (4 aircraft each)

**T=0:05**
  → Lead flight checks out package, loads into autopilot
  → Waypoint 1: Nellis runway 27R (takeoff)
  → Waypoint 2: Initial climb heading 000°, 25,000 ft
  → Waypoint 3: CAP point alpha [AB12+15nm north]
  → Waypoint 4: CAP loiter (if no threat, loiter)
  → Waypoint 5: RTB via [AC10]
  → Waypoint 6: Nellis landing

**T=0:10**
  → Aircraft on runway, autopilot in NAVIGATION mode
  → Airframe executes takeoff control sequence
  → DigiAI enables FORMATION mode (wingmen follow lead)

**T=0:20**
  → Lead aircraft at waypoint 3 (CAP point)
  → Autopilot transitions to LOITER mode
  → Sensor fusion detects "bogey dope" from E-3 AWACS (via datalink):
      "Bandit 40 miles, heading 270"
  → DigiAI overrides autopilot, takes control
  → Digi engages intercept maneuver
  → Flight follows lead (formation)

**T=0:25**
  → Visual merge with enemy MiG-29s
  → BVR engagement begins
  → DigiAI manages target prioritization, weapon selection
  → Autopilot inactive (combat mode)

**T=0:30**
  → One enemy destroyed, one fled
  → DigiAI returns to waypoint navigation
  → Autopilot resumes, vector to waypoint 4
  → Fuel check: proceeding to RTB

**T=0:45**
  → Lead approaches waypoint 5 (RTB) then Nellis (waypoint 6)
  → Autopilot transitions to LANDING mode
  → Airframe executes final approach, autoland sequence
  → Aircraft landed, mission complete
```

---

## 6. Key Design Decisions

### 6.1 Decoupling

| Component | Depends On | Does NOT Depend On |
|-----------|-----------|-------------------|
| **World DB** | Nothing | Any simulator, renderer |
| **Sensors** | World DB, Entity positions | Rendering, UI |
| **Comms** | World DB, Entities | Rendering, campaign logic |
| **Campaign** | World DB | Rendering, individual aircraft AI |
| **Autopilot** | Campaign waypoints, Sensors | DigiAI internals |
| **DigiAI** | Autopilot, Sensors, World DB | Campaign |

### 6.2 Timestep Scales

- **Campaign**: Update every 60 real seconds (~10 min game time)
- **Mission/Autopilot**: Update every 1 real second
- **Sensor/Comms**: Update every 0.1 real seconds
- **Aircraft Physics**: Update every 0.01 real seconds

This allows efficient computation—campaign runs rarely, aircraft frequently.

### 6.3 Authority Model

**Autopilot Authority**:
- Generates control inputs to follow waypoints
- Active during navigation phases

**DigiAI Authority**:
- Overrides autopilot during combat or threats
- Manages tactical decisions
- Falls back to autopilot for routing

**Campaign Authority**:
- Generates missions, coordinates packages
- Manages global resource allocation (fuel, ammo, pilots)
- Never interferes with real-time flight dynamics

---

## 7. Implementation Roadmap

### Phase 1: World Foundation
- [ ] Entity types (Aircraft, Ground Unit, Objective, Weapon, Sensor Contact)
- [ ] WorldDatabase with spatial indexing
- [ ] Entity lifecycle management (create, update, delete)
- [ ] Unit tests for spatial queries

### Phase 2: Sensors
- [ ] Base Sensor class + interface
- [ ] Radar sensor with search/track modes
- [ ] IRST sensor
- [ ] RWR (radar warning receiver)
- [ ] Sensor fusion (master track list)
- [ ] Test with mock entities

### Phase 3: Communications
- [ ] RadioNetwork frequency allocation
- [ ] Message routing (broadcasts, directed)
- [ ] TacticalDatalink (shared tracks)
- [ ] Radio call handler
- [ ] Jamming effects

### Phase 4: Campaign Integration
- [ ] MissionPackage structure
- [ ] AirTaskingManager (package generation)
- [ ] Waypoint-to-autopilot interface
- [ ] Load/unload missions dynamically

### Phase 5: AI Integration
- [ ] FlightAutopilot class
- [ ] DigiAI autopilot override logic
- [ ] Waypoint following
- [ ] Formation control via autopilot
- [ ] Combat override

### Phase 6: Multiplayer & Persistence
- [ ] Entity serialization (for network sync)
- [ ] Aggregation/deaggregation lifecycle
- [ ] Campaign save/load
- [ ] Network replication of world state

---

## 8. Comparison: FreeFalcon → F4Flight

| Aspect | FreeFalcon | F4Flight |
|--------|-----------|----------|
| **Entity DB** | `VuDatabase` (distributed) | Centralized `WorldDatabase` |
| **Spatial Queries** | Campaign grid + cell system | SpatialGrid (10 km cells) |
| **Radar** | Coupled to AircraftClass | Decoupled, world-aware Sensor |
| **Radio** | Broadcast via `gCommsMgr` | `RadioNetwork` with frequency management |
| **Campaign** | Hard-coded rules + state machines | `AirTaskingManager` + `MissionPackage` |
| **Waypoints** | Stored in UnitClass | `CampaignWaypoint` structures + `FlightAutopilot` |
| **Control Flow** | DigiAI directly drives aircraft | Autopilot ↔ DigiAI negotiation |
| **Aggregation** | Campaign ↔ Sim (complex) | Explicit `AggregationManager` |

---

## 9. References to FreeFalcon Source

**Campaign/Mission System**:
- `src/campaign/include/mission.h` — MissionRequestClass, MissionDataType
- `src/campaign/camptask/package.cpp` — PackageClass (mission packages)
- `src/campaign/include/atm.h` — AirTaskingManagerClass (scheduling)
- `src/campaign/include/flight.h` — FlightClass (waypoint lists)

**Entity Management**:
- `src/campaign/include/campbase.h` — CampBaseClass (aggregated entities)
- `src/campaign/include/unit.h` — Deaggregation logic
- `src/campaign/camplib/objectiv.cpp` — Objective deaggregation

**Sensor/Targeting**:
- `src/sim/radar/radardigi.cpp` — DigiAI radar tracking
- `src/sim/include/mfd.h` — MFDClass (avionics)

**Communications**:
- `src/falclib/msgsrc/campweaponfiremsg.cpp` — Message system

**World Queries**:
- `src/campaign/camplib/find.cpp` — FindObjective, FindUnit (world lookups)

