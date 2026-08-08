# F4 AI Phase 2 — Digi AI Implementation Plan

> **Status**: Implementation-ready
> **Scope**: First digi AI modules + flight recording + visualization pipeline
> **Predecessor**: SensorFusion (Step 2 of AI_IMPLEMENTATION_PLAN.md §5)
> **Goal**: Demonstrate AI-controlled aircraft performing takeoff, landing, and aerial refueling with observable, replayable, LLM-debuggable recordings

---

## 1. Overview

This plan implements the **Record → Replay → Inspect** loop that makes every AI simulation run observable after the fact. The core principle: **build the observability infrastructure first, then the AI modules**. Without recording, you can only test AI control outputs in isolation — you can't verify that the aircraft *actually flies the right path* through the flight model.

### Success Criteria

| Scenario | Success Criterion |
|----------|------------------|
| **Takeoff** | Aircraft visible in 3D moving along taxi paths, aligning on runway centerline, lifting off at Vr |
| **Landing** | Aircraft visible capturing glide slope, flying traffic pattern, touching down on centerline |
| **AR** | Two aircraft visible, receiver maintaining position within boom envelope |
| **Formation** | Wingman visible holding offset position relative to lead |
| **BFM** | Fighter and bandit visible with maneuver geometry displayed |

---

## 2. New Library: f4-recorder

### 2.1 FlightSnapshot

Per-tick recording of one aircraft's complete observable state:

- **Timing**: tick, sim_time_s
- **Identity**: entity_id, callsign
- **Kinematics**: position (WorldPosition), heading/pitch/roll, altitude, airspeed, Mach
- **Controls**: pitch_cmd, roll_cmd, yaw_cmd, throttle_cmd, gear, brakes
- **AI brain**: ai_mode, ai_state, ai_event, ai_guard_result (human-readable strings)
- **Intended path**: target_position, target_description, cross_track_error, along_track_error, vertical_error
- **Ground state**: on_ground, ground_speed, gear_on_object
- **Engine**: rpm, afterburner, fuel

### 2.2 FlightRecorder

Accumulator + JSON export with two modes:

1. **Full trace** (`to_json`): every snapshot, for replay viewer
2. **LLM-friendly summary** (`to_summary_json`): phase-level aggregation with anomaly flags

### 2.3 PathGeometry

Reference path computation:

- `cross_track_error()` — perpendicular distance from intended path line
- `along_track_fraction()` — progress along intended path
- `glide_slope_altitude_ft()` — altitude at distance from threshold
- `PathSegment` — typed, named path with tolerance bounds
- `FlightPath` — intended + actual path with deviation stats
- `MultiAircraftScenario` — multiple aircraft + inter-aircraft relationships

### 2.4 Directory Layout

```
f4-recorder/
├── CMakeLists.txt
├── include/f4/recorder/
│   ├── f4_recorder.hpp          # umbrella
│   ├── snapshot.hpp             # FlightSnapshot
│   ├── flight_recorder.hpp      # FlightRecorder
│   └── path_geometry.hpp        # PathSegment, FlightPath, deviation math
├── src/
│   ├── flight_recorder.cpp      # JSON export + queries
│   └── path_geometry.cpp        # deviation computation
└── tests/
    ├── CMakeLists.txt
    ├── test_flight_recorder.cpp
    └── test_path_geometry.cpp
```

**Dependencies**: f4-geo, f4-json (lightweight — no flight-model, no entities, no AI)

---

## 3. ATC Protocol: Messages + StubATC

### 3.1 Message Types

All ATC communication flows over `MessageBus` as plain structs:

**Ground/Taxi:**
- `TaxiRequest` → `TaxiClearance` (with taxi route + runway ID)
- `HoldShortRequest` → `HoldShortClearance`

**Takeoff:**
- `TakeoffRequest` → `TakeoffClearance` (with runway heading, threshold, departure altitude)

**Landing:**
- `LandingRequest` → `LandingClearance` (with runway, glide slope, pattern altitude, DH)
- `ApproachClearance` → `ClearedToLand`
- `GoAroundMessage`
- `TaxiOffClearance`

**Air Refueling:**
- `RefuelRequest` → `TankerAssigned` (with tanker position, heading, altitude)
- `ContactRequest` → `ContactMade` / `ContactLost`
- `RefuelComplete` / `DisconnectMessage`

**Formation:**
- `FormationCommand` (with type, offsets)
- `JoinUp`

### 3.2 StubATC

Grants every request immediately. Uses `AirfieldConfig` for ground layout data:

- Active runway ID, heading, threshold position
- Taxi route waypoints
- Pattern altitude, glide slope angle, decision height
- Departure altitude

When real ATC is built later, it implements the **same message types** — the AI modules never change.

### 3.3 StubTankerCoord

Embedded in StubATC. Grants `TankerAssigned` and `ContactMade` immediately. Uses `TankerConfig` for tanker position, heading, altitude, and boom envelope dimensions.

---

## 4. TakeoffModule

### 4.1 State Machine

```
RequestTaxi → Taxi → HoldShort → PrepToTakeRunway → TakeRunway → Takeoff → FlyOut → Done
                            ↘ Wait (holding for clearance)
                            ↘ EmergencyStop
```

### 4.2 Per-State Control Logic

| State | Throttle | Pitch | Gear | Brakes | Notes |
|-------|----------|-------|------|--------|-------|
| RequestTaxi | 0 | 0 | Down | On | Waiting for clearance |
| Taxi | 0.1 | 0 | Down | Off | Nose-steer to taxi waypoint |
| HoldShort | 0 | 0 | Down | On | Stopped at hold-short line |
| TakeRunway | 0 | 0 | Down | On | Holding brakes, waiting for clearance |
| Takeoff | MIL | 0→0.5 | Down | Off→Release | At Vr: rotate; detect liftoff → FlyOut |
| FlyOut | MIL | 0.3 | Down→Up | Off | Climb to departure altitude → Done |
| Done | — | — | — | — | DigitalBrain transitions to WaypointMode |

### 4.3 FreeFalcon Validation

| Behavior | Validation Method |
|----------|-----------------|
| Throttle to MIL on takeoff roll | Test: throttle reaches 1.0 during TakeRunway |
| Rotate at Vr | Test: pitch_cmd > 0 when vcas >= rotate_speed |
| Gear up above 200ft AGL | Test: gear_handle_down=false when alt > gear_up_alt |
| Climb to departure altitude | Test: transition to Done when alt >= departure_alt |

---

## 5. ScenarioRunner

Headless simulation loop that wires together AI + FlightModel + Recorder:

```cpp
for (tick = 0; tick < total_ticks; ++tick) {
    auto ai_output = brain->update(dt);
    auto pilot_input = map_to_pilot_input(ai_output);
    flight_model->update(dt, pilot_input);
    update_entity_transform();
    bus->flush_pending();
    recorder->record(build_snapshot(tick, sim_time));
}
```

Produces a JSON recording that can be loaded in the world viewer for replay.

---

## 6. ScriptedTanker

Trivial straight-and-level "AI" for AR demos:

- Constant heading, altitude, speed
- Updates position each tick via kinematic advance
- No state machine, no AI brain
- EntityWorld TransformComponent updated by ScenarioRunner

---

## 7. World Viewer Replay Mode

### 7.1 Replay Data Flow

```
f4-sim --scenario=takeoff_kunsan --record=trace.json
         ↓
FlightRecorder::to_json() → trace.json
         ↓
f4-world-viewer --replay=trace.json
         ↓
Step through snapshots, render aircraft + trail + AI state
```

### 7.2 Replay Viewer Features

**Phase 1 (cone + trail):**
- Load JSON recording via `FlightRecorder::load_json()`
- Step through time: Space=pause, Left/Right=step, +/-=speed
- Draw aircraft as cone oriented by heading/pitch/roll
- Draw trail behind aircraft (colored by cross-track error: green→red)
- Show AI state as text label above aircraft: "TakeoffMode | TakeRunway | V=120kts A=0ft"
- Overlay ground layout (existing viewer capability)

**Phase 2 (glide slope + path lines):**
- Draw intended glide slope as orange 3D line from threshold
- Draw traffic pattern legs as blue lines
- Color trail by vertical error (green=on slope, red=above/below)
- Show cross-track and vertical error values in HUD

**Phase 3 (aircraft meshes):**
- Load F-16 model from `f4-models` BSP/LOD parser
- Apply position + orientation from snapshot
- Animate gear (up/down from gear_handle_down)
- For AR: load both receiver and tanker models

**Phase 4 (multi-aircraft):**
- Draw relationship lines between aircraft (dashed for formation, solid for AR)
- Show boom envelope as wireframe box
- Color by in/out of tolerance

### 7.3 Implementation Approach

Extend `f4-world-viewer` with a new mode:

```cpp
// In viewer_app.cpp or new replay_mode.hpp:
struct ReplayState {
    f4::recorder::FlightRecorder recording;
    std::size_t current_tick{0};
    bool paused{false};
    int speed_multiplier{1};  // 1x, 2x, 5x, 10x
    bool show_trail{true};
    bool show_intended_path{true};
    bool show_ai_labels{true};
    bool show_mesh{false};    // Phase 3
};
```

The existing `viewer_state.hpp` pattern (Raylib + ImGui) already supports mode switching. Replay mode would be a new `update_replay()` / `draw_replay()` path in the main loop.

---

## 8. LandingModule (Design)

### 8.1 State Machine (19 states)

```
NoATC → ReqClearance → Ingressing → Holding → FirstLeg → ToBase →
ToFinal → OnFinal → ClearToLand → Landed → TaxiOff

Emergency: ReqEmerClearance → EmerHold → EmergencyToBase → EmergencyToFinal → EmergencyOnFinal → Landed
Abort: Aborted → (back to Holding or RTB)
Terminal: Crashed
```

### 8.2 Key Implementation Details

- **Approach geometry**: `to_bra(ownship, runway_threshold)` for bearing/distance
- **3° glide slope**: altitude = threshold_alt + distance × tan(3°)
- **Entry actions**: OnFinal → gear down, flaps landing, speed Vref
- **Go-around**: Not cleared + reaching DH → GoAround event
- **Speed management**: Decelerate through pattern legs

### 8.3 Landing Recording

The recording captures:
- Intended path: ingress → hold → downwind → base → final → runway
- Actual vs intended for each leg
- Glide slope capture/track accuracy
- Go-around events and reasons

---

## 9. RefuelModule (Design)

### 9.1 State Machine (5 states)

```
NoTanker → VectorTo → Waiting → Refueling → Done
              ↑                      │
              └── (ContactLost) ────┘
```

### 9.2 Boom Envelope

From FreeFalcon `digi_refuel.cpp`:
- Lateral: ±5 ft
- Vertical: ±10 ft
- Longitudinal: ±30 ft (boom length)

### 9.3 Contact Maintenance

- Receiver steers to tanker's offset position (50ft behind, same altitude)
- PID loops with tight gains for precision formation
- Drift outside envelope → ContactLost → back to Waiting
- LLM debugging: show relative geometry (range, bearing, elevation, closure rate)

---

## 10. LLM Debugging Workflow

### 10.1 Three Export Levels

**Level 1: Human-readable trace** (existing, from §8.2):
```
tick=4521 from=Holding to=ToBase event=ReachHoldingFix guard=PASS reason="range<1nm"
```

**Level 2: Structured flight summary** (new, from `to_summary_json()`):
```json
{
  "phases": [
    { "mode": "TakeoffMode", "state": "Taxi", "duration_s": 45.0,
      "max_cross_track_error_ft": 8.2 }
  ],
  "anomalies": []
}
```

**Level 3: Anomaly-flagged detail** (new, per-anomaly context windows):
```json
{
  "anomaly": {
    "type": "path_deviation", "tick": 487,
    "cross_track_error_ft": 42.3,
    "context_window": { "tick_range": [477, 497] }
  }
}
```

### 10.2 Workflow

```
1. Run:    f4-sim --scenario=takeoff_kunsan --record=trace.json
2. Summarize: f4-trace-summary trace.json > summary.json
3. Debug:  f4-trace-debug trace.json --anomaly=42 > debug_context.json
4. LLM:    Feed summary.json + debug_context.json → diagnosis → fix
5. View:   f4-world-viewer --replay=trace.json
```

---

## 11. Implementation Sequence

| Step | Deliverable | Effort | Depends On | What You See |
|------|------------|--------|-----------|-------------|
| **0a** | f4-recorder library | 1-2 days | f4-geo, f4-json | JSON recording files |
| **0b** | ATC message types + StubATC | 1 day | f4-messaging | Bus-wired ATC protocol |
| **1** | TakeoffModule | 2-3 days | f4-ai, StubATC | AI control output sequence |
| **2** | ScenarioRunner | 1 day | TakeoffModule + FlightModel + Recorder | Full JSON recording |
| **3** | Viewer replay mode (cone + trail) | 2-3 days | f4-world-viewer, f4-recorder | **3D takeoff demo** |
| **4** | LandingModule | 3-5 days | NavigationModule PID | Full approach recording |
| **5** | Viewer: glide slope + path lines | 1 day | LandingModule | **3D landing demo** |
| **6** | Aircraft mesh in replay viewer | 2-3 days | f4-models, BSP parser | **Real F-16 model** |
| **7** | ScriptedTanker + RefuelModule | 2-3 days | NavigationModule | AR recording |
| **8** | Viewer: two meshes + boom envelope | 2 days | RefuelModule | **3D AR demo** |
| **9** | LLM trace summary + debug tools | 1-2 days | f4-recorder | CLI debugging workflow |

**Steps 0a-3** give you the first working demo: a takeoff you can watch in 3D on Korea terrain with the Kunsan ground layout.

---

## 12. Files Changed / Added

### New Library: f4-recorder
```
f4-recorder/CMakeLists.txt
f4-recorder/include/f4/recorder/f4_recorder.hpp
f4-recorder/include/f4/recorder/snapshot.hpp
f4-recorder/include/f4/recorder/flight_recorder.hpp
f4-recorder/include/f4/recorder/path_geometry.hpp
f4-recorder/src/flight_recorder.cpp
f4-recorder/src/path_geometry.cpp
f4-recorder/tests/CMakeLists.txt
f4-recorder/tests/test_flight_recorder.cpp
f4-recorder/tests/test_path_geometry.cpp
```

### Modified: f4-ai (new modules)
```
f4-ai/CMakeLists.txt                           (added sources + f4-recorder dep)
f4-ai/include/f4/ai/f4_ai.hpp                  (added module includes)
f4-ai/include/f4/ai/atc/messages.hpp           (NEW - ATC message types)
f4-ai/include/f4/ai/atc/stub_atc.hpp           (NEW - StubATC)
f4-ai/include/f4/ai/modules/takeoff_module.hpp (NEW - TakeoffModule)
f4-ai/include/f4/ai/modules/scripted_tanker.hpp(NEW - ScriptedTanker)
f4-ai/include/f4/ai/scenario_runner.hpp        (NEW - ScenarioRunner)
f4-ai/src/takeoff_module.cpp                   (NEW)
f4-ai/src/scenario_runner.cpp                  (NEW)
f4-ai/tests/CMakeLists.txt                     (added test_takeoff_module)
f4-ai/tests/test_takeoff_module.cpp            (NEW)
```

### Modified: Root
```
CMakeLists.txt                                 (added f4-recorder subdirectory)
```

### Future (not in this patch)
```
f4-ai/include/f4/ai/modules/landing_module.hpp
f4-ai/include/f4/ai/modules/navigation_module.hpp
f4-ai/include/f4/ai/modules/refuel_module.hpp
f4-ai/include/f4/ai/modules/wvr_module.hpp
f4-ai/include/f4/ai/modules/bvr_module.hpp
f4-ai/include/f4/ai/modules/wingman_module.hpp
f4-world-viewer/src/replay_mode.hpp            (replay mode)
f4-world-viewer/src/replay_mode.cpp
```

---

## 13. Testing Strategy

### Unit Tests

| Test | Validates |
|------|-----------|
| test_flight_recorder | Recording, queries, JSON export, summary export, anomaly detection |
| test_path_geometry | Cross-track error, along-track fraction, glide slope computation |
| test_takeoff_module | SM construction, ATC interaction, per-state control outputs, trace |

### Integration Tests (future)

| Test | Validates |
|------|-----------|
| test_takeoff_integration | TakeoffModule + FlightModel + StubATC → recording → path fidelity check |
| test_landing_integration | LandingModule + FlightModel + StubATC → recording → glide slope accuracy |
| test_ar_integration | RefuelModule + ScriptedTanker + StubATC → recording → envelope tracking |

### Scenario Tests (future)

| Scenario | Validates |
|----------|-----------|
| S1: Takeoff Kunsan Rwy 36L | Complete taxi + takeoff + departure |
| S2: Landing Osan Rwy 09R | Full traffic pattern + approach + landing |
| S3: AR over Korea | Vector to tanker + contact + refueling + disconnect |

---

## 14. Risks and Mitigations

| Risk | Mitigation |
|------|-----------|
| TakeoffModule gets stuck in a state | SM trace records every transition; greppable for debugging |
| Flight recording too large for LLM | `to_summary_json()` provides phase-level aggregation; anomaly-flagged detail windows |
| World viewer replay performance with large traces | Snapshot rate configurable (record every Nth tick); lazy loading |
| StubATC too simple for realistic testing | It defines the protocol; real ATC implements same messages; swap at bus wiring level |
| ScenarioRunner doesn't handle multi-aircraft | Design supports it (entity_id per snapshot); extend in Phase 4 |
| 3D viewer replay coupling | Replay loads JSON, not simulation state; fully decoupled from sim loop |
