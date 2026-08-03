# F4 — Modern C++ Libraries for Flight Simulation

A ground-up reimplementation of FreeFalcon's core subsystems
(campaign, flight model, AI) as independent, engine-agnostic, testable
C++20 libraries. See `Docs/ARCHITECTURE PROPOSAL.md` for the full design.

## Libraries

### f4-geo — Strong-typed coordinate frames & geodesy

Header-only C++20 library providing distinct types for the three absolute
position representations (sim-local `WorldPosition`, geodetic `LatLonAlt`,
Earth-centered `ECEFPosition`) plus the `TheaterDatum` that binds the sim
frame to the Earth. Extends the f4-units philosophy (phantom dimensions) to
coordinate frames: no implicit conversion, every crossing is a named call.

```cpp
#include <f4/geo/f4_geo.hpp>
using namespace f4::geo;

// Source of truth: the sim-local frame, feet, z-up.
WorldPosition ownship{5000.0, -3000.0, 20000.0};

// Earth-frame views require a datum (the sim↔Earth bridge):
TheaterDatum datum(LatLonAlt{38.0 * DEG_TO_RAD, -77.0 * DEG_TO_RAD, 0.0});
LatLonAlt lla = to_lla(ownship, datum);          // flat-earth, <1m at theater scale
ECEFPosition ecef = to_ecef(ownship, datum);     // exact WGS84 (DIS wire format)

// Relative/reference views (Category B — carry a reference, not freely
// interconvertible with absolute types):
BRA bra = to_bra(ownship, target);               // bearing/range/alt from ownship
BullseyeOffset be = to_bullseye(bullseye, target);
```

**Modules**: `constants.hpp` (WGS84 + unit factors), `position.hpp` (the three
absolute strong types), `datum.hpp` (`TheaterDatum`), `conversions.hpp` (the
conversion lattice — exact WGS84 LLA↔ECEF, flat-earth World↔LLA, composed
World↔ECEF), `relative.hpp` (`BRA`, `BullseyeOffset` with inverse `from_bullseye`).

**Tests**: 30 (WGS84 known points, LLA↔ECEF round-trip, World↔LLA round-trip
identity, heading rotation, BRA/Bullseye geometry). Zero external dependencies
beyond the standard library — maximally portable and testable.

**Design rationale**: DIS-readiness is a design gate, not a deliverable. A
future `f4-dis` adapter is `to_ecef(pos, datum)` on transmit, `to_world(ecef,
datum)` on receive — no rewrite. Real-world data import (terrain, airbases,
waypoints given as lat/lon) flows through the same datum into the sim frame.

### f4-units — Compile-time physical quantity types

Header-only C++20 library providing type-safe dimensional analysis with
zero runtime overhead. Phantom dimensions (CAS, Mach) prevent accidental
mixing with physical quantities.

```cpp
#include <f4/f4_units.hpp>
using namespace f4::literals;

auto dist   = 1000.0_ft;
auto meters = dist.to<f4::Meters>();   // 304.8 m
auto speed  = 250.0_kn + 50.0_mps;    // heterogeneous addition
auto mach   = 0.85_mach;               // typed Mach number
auto cas    = 450.0_kcas;              // typed CAS (not a Speed!)
```

**Tests**: 121 unit tests (conversions, arithmetic, temperature, aviation,
derived quantities). Zero domain coupling.

### f4-math — Numerical mathematics

Header-only C++20 library providing tables, interpolation, integrators,
filters, vectors, quaternions, and solvers. Direct ports of FreeFalcon's
`simlib/math.cpp` filter routines (FLTust, FWTust, F7Tust, FITust,
FIAdamsBash) with exact coefficient preservation.

```cpp
#include <f4/math/f4_math.hpp>
using namespace f4::math;

// 2-D bilinear interpolation with cached breakpoint indices
Table2D<double, double, double> cl_table(mach, alpha, cl_data);
double cl = cl_table(0.8, 4.0);

// FF-faithful lag filter ( FLTust )
LagFilter lag;
double y = lag.step(input, tau, dt);

// 4th-order Runge-Kutta integrator
RK4Integrator<Vec3d> integ({0.0, 0.0, 1000.0});
integ.step([](const Vec3d& s) { return Vec3d{0.0, 0.0, -9.81}; }, 0.01);
```

**Modules**:
- `scalar.hpp` — limit, deadBand, wrapPi, wrap2Pi, lerp, rescale, sign
- `table.hpp` — Table1D, Table2D with Clamp/Error/Extrapolate boundary modes
- `integration.hpp` — Euler, Trapezoidal (Heun), AB2, AB4 (RK4 bootstrap), RK4
- `filters.hpp` — LagFilter, LeadFilter, WashoutFilter, LeadLagFilter,
  IntegratorTustin, AdamsBash2Filter (FF FLTust/FLeadTust/FWTust/F7Tust/FITust/FIAdamsBash)
- `vec3.hpp` — Vec3<T> with dot, cross, normalize, hadamard
- `quat.hpp` — Quat<T> with Hamilton product, axis-angle, Euler ZYX, slerp
- `solver.hpp` — Newton-Raphson with bisection fallback, pure bisection

**Tests**: 192 unit tests covering every module, including convergence-order
verification against analytical solutions (exponential decay, harmonic
oscillator). Zero domain coupling — test targets link only against f4-math.

### f4-convert — Legacy .dat to JSON aircraft data converter

Static library + CLI tools that convert FreeFalcon's legacy `.dat` aircraft
definition files to open JSON format. The libraries (`f4-data`, when built)
never see the binary/legacy formats — only the converted JSON.

```bash
# Convert a single aircraft
dat2json f16c.dat f16c.json

# Validate a .dat file's structure
dat_validate f16c.dat

# Diff two JSON aircraft files field-by-field
json_diff old.json new.json --threshold 1e-9
```

**Components**:
- `dat_parser` — recursive-descent parser for the `.dat` format, ported from
  F4Flight's `dat_loader.cpp` and verified against FreeFalcon's `readin.cpp`.
  Captures every AuxAeroData key/value pair verbatim into `rawAuxAeroData`
  for no-loss round-trip fidelity.
- `json_io` — JSON serialization using nlohmann/json. Bidirectional and
  lossless: `dat → json → re-parse → compare` produces zero differences.
- `rosetta/auxaero_field_map.json` — the Rosetta Stone: all 455 AuxAeroData
  keys extracted from FreeFalcon's `AuxAeroDataDesc` table in `readin.cpp`.
  This is the contract between `f4-convert` (which reads `.dat`) and
  `f4-data` (which exposes typed `AircraftConfig`).
- CLI tools: `dat2json`, `dat_validate`, `json_diff` — thin `main()`s that
  call into the library. Logic lives in the library where it's testable.

**Tests**: 40 tests (parser unit tests on a synthetic fixture, JSON round-trip
tests, dat→json→re-parse integration tests, real-aircraft tests against 24
genuine FreeFalcon `.dat` files). Build-time fixture generation via CMake
custom command produces 25 JSONs in `${BUILD_DIR}/generated_fixtures/` for
`f4-data` tests to consume.

### f4-data — Aircraft configuration loading, validation, and table access

Static library that owns the `AircraftConfig` struct, provides validation,
and bridges the raw config tables to `f4-math`'s `Table2D` for bilinear
interpolation.

```cpp
#include <f4/data/f4_data.hpp>
using namespace f4::data;

// Load a config from JSON (produced by f4-convert's dat2json)
auto result = loadConfig("f16.json");
if (!result.ok) { /* handle error */ }

// Validate
auto report = result.config.validate();
if (!report.ok()) { std::cerr << report.format(); }

// Build a Table2D view of the CL table and do a lookup
auto cl_table = makeClTable(result.config.aero);
double cl = cl_table(0.8, 4.0);  // Mach 0.8, alpha 4 deg
```

**Components**:
- `aircraft_config.hpp` — the `AircraftConfig` struct (owns all aircraft
  data: geometry, aero tables, engine tables, roll command, limiters,
  aux aero, verbatim `.dat` capture). Includes `validate()` and
  `Limiter::limit()`.
- `config_loader.hpp` — `loadConfig(path)` / `writeConfig(cfg, path)` via
  nlohmann/json. Bidirectional and lossless.
- `table_accessors.hpp` — `makeClTable()`, `makeCdTable()`, `makeCyTable()`,
  `makeThrustTable()`, `makeRollRateTable()` — build `f4::math::Table2D`
  views from the raw config vectors with Clamp boundary mode and cached-index
  optimization.

**Tests**: 38 tests (validation logic, config loading from all 25 generated
JSONs, f16 field spot-checks, table interpolation correctness, round-trip
integrity).

### f4-state-machine — Type-safe state machines with transition tables

Header-only C++20 library providing transition-table-driven state machines
with a fluent builder, per-state entry/exit actions (UML 2 semantics), guard
predicates, a layered priority-ladder machine (for the AI DigiMode), and a
built-in text trace for observability. Zero dependencies.

```cpp
#include <f4/fsm/f4_fsm.hpp>
using namespace f4::fsm;

// Build a 6-state stall SM from a transition table
auto sm = StateMachine<StallState, StallEvent>::Builder()
    .initial(StallState::None)
    .on(StallState::None, StallState::EnteringDeepStall, StallEvent::AoAExceed)
    .on(StallState::EnteringDeepStall, StallState::DeepStall, StallEvent::TimerExpired)
    // ...
    .build();

// Attach a trace for debugging (zero overhead when not attached)
Trace<StallState, StallEvent> trace;
sm.set_trace(&trace);
// Each transition is recorded as one greppable text line:
//   tick=1 from=None to=EnteringDeepStall event=AoAExceed fired=1
```

**Modules**:
- `transition.hpp` — one (from, event) → (to, action) row, with optional
  matcher, guard, and reason string
- `state_machine.hpp` — `StateMachine` + fluent `Builder`, UML-2 entry/exit
  actions, `on()`/`on_if()` (value vs predicate matching), trace + observer
- `layered.hpp` — `LayeredStateMachine` priority ladder (AI DigiMode)
- `trace.hpp` — bounded ring buffer + `to_text()` / `summary()`
- `serialize.hpp` — `to_text(sm)` table dump (no JSON dependency)

**Tests**: 41 unit tests (core SM behavior, stall SM lifecycle, ATC/landing
SM with payload-carrying variant events, layered priority preemption, trace
recording and text emission, serialization round-trip). Zero dependencies.

**Design principle**: observability is not optional. Every transition is
recorded with enough context to diagnose it from text alone — no screenshot
parsing. The trace is zero-overhead in production (opt-in via raw pointer).

### f4-entities — Component-based entity system

Static library replacing FreeFalcon's deep inheritance hierarchy
(`VuEntity -> FalconEntity -> SimBaseClass -> ... -> AircraftClass`,
200+ member god-classes) with a lightweight ID handle + typed components.
Entities are generation-tagged stable handles; behavior/data lives in
components that can be added, removed, and queried independently.

```cpp
#include <f4/entities/f4_entities.hpp>
using namespace f4::entities;

EntityWorld world;
EntityHandle h = world.create();
h.set_tag(tags::ROLE, TagValue::from(std::string("fighter")));
h.set_tag(tags::TEAM, TagValue::from(std::string("blue")));

// TransformComponent carries the strong-typed f4::geo::WorldPosition:
auto& tf = h.add<TransformComponent>();
tf.position = f4::geo::WorldPosition{5000.0, -3000.0, 20000.0};
tf.qw = 1.0;   // identity orientation

// Query all blue fighters:
auto blue_fighters = world.with_tag(tags::TEAM, TagValue::from(std::string("blue")));

// Spatial radius query (linear scan; SpatialIndex available for hot paths):
auto nearby = world.within_radius(0, 0, 0, 50000.0);
```

**Modules**: `entity.hpp` (`EntityId`, `EntityWorld`, `EntityHandle`, tags,
`Component`/CRTP, `TransformComponent` using `f4::geo::WorldPosition`,
`CampaignIdentityComponent`), `spatial_index.hpp` (3D hash grid for radius
queries, cell-sized to the typical query radius).

**Tests**: 20 (entity lifecycle, generation bump on destroy, component
add/get/has/remove, tag filtering, within-radius, spatial index
insert/query/update/remove across cell boundaries). Links f4-geo (PUBLIC) —
the strong-typed position is the design decision made concrete.

### f4-world-convert — FreeFalcon .cam campaign archive → JSON

Static library + CLI that convert FreeFalcon's binary `.cam` campaign
archives to open JSON. A `.cam` file is a "campressed" container holding
10+ typed sub-files (`.cmp` campaign metadata, `.obj` objectives, `.uni`
units, `.tea` teams, `.wth` weather, `.ver` version, ...). The converter
parses the container, LZSS-decompresses the `.cmp` sub-file (faithful port
of FreeFalcon's decompressor), decodes the campaign header + 8 team slots,
and preserves undecoded sub-files as base64 for incremental decoding.

```bash
# Convert a real campaign save to JSON
cam2json save1.cam            # -> save1.json (259 KB)
```

```json
{
  "archive":     { "file_size": 197340, "subfiles": [ {name,offset,size}, ... ] },
  "version":     63,
  "campaign":    { "current_time": 32400000, "teams": [
                     {"slot":1,"name":"U.S."}, {"slot":2,"name":"ROK"},
                     {"slot":3,"name":"Japan"}, {"slot":4,"name":"CIS"},
                     {"slot":5,"name":"PRC"}, {"slot":6,"name":"DPRK"}, ...
                  ] },
  "raw_subfiles": { "obj": {...}, "uni": {...}, "tea": {...}, ... }
}
```

**Components**: `lzss.hpp` (LZSS decompressor — byte-exact port of
FreeFalcon's `LZSS_Expand`), `cam_archive.hpp` (`.cam` container parser),
`campaign_decoder.hpp` (`.cmp` decode: LZSS-expand + struct parse of
`CurrentTime`, TE block, `team_name[8][20]`/`team_motto[8][200]`),
`world_json.hpp` (JSON emitter with base64 preservation of undecoded sub-files).

**Tests**: 16 (LZSS byte-exactness against real `.cmp` payload, container
manifest parsing, campaign header decode, team-name extraction, JSON emit).
Validated against a real Korea-theater `save1.cam` fixture.

### f4-world — Typed WorldState from campaign data

Static library that loads the JSON produced by `f4-world-convert` into typed
structs (`CampaignState`, `TeamState[8]`, `WorldState`) and populates an
`f4-entities` `EntityWorld` from them. Each team becomes an entity tagged
with `role=team`, `team=<name>`, `alive=true` and carrying a
`CampaignIdentityComponent`. This is the keystone from §18.5: the entity
system is now populated from **real** campaign data, structurally retiring
the injection-harness trap.

```cpp
#include <f4/world/f4_world.hpp>
using namespace f4::world;

WorldState ws;
ws.load("save1.json");              // real campaign data
f4::entities::EntityWorld ew;
auto team_ids = populate_teams(ew, ws);   // 7 entities (skips empty slot 0)

// The EntityWorld now answers tag queries against REAL team identities:
auto rok = ew.with_tag(tags::TEAM, TagValue::from(std::string("ROK")));
```

**Tests**: 7 (JSON field loading, team-slot parsing, entity creation with
correct tags/identity, tag-based queries). End-to-end test loads the real
`save1.cam`-derived JSON and verifies all 8 team names (ROK, Japan, PRC,
DPRK, U.S., CIS, Gorn) round-trip from binary → JSON → typed structs.

### f4-flight-model — 6-DOF flight dynamics

Static library implementing the full 6-DOF flight dynamics: atmosphere,
aerodynamics, flight control system, engine, equations of motion, gear
model, and a 6-state stall state machine (built on f4-state-machine). The
FlightModel orchestrator ties them together with sub-stepping and a trim
solver.

```cpp
#include <f4/flight/f4_flight.hpp>
using namespace f4::flight;

// Load config (from f4-data)
auto result = f4::data::loadConfig("f16.json");
auto cfg = result.config;

// Initialize at 10000 ft, 500 ft/s, heading North, airborne
FlightModel fm;
fm.init(cfg, 10000.0, 500.0, 0.0, true);
fm.trim();  // find 1-G level flight trim

// Run the simulation
PilotInput input;
input.throttle = 0.5;
for (int frame = 0; frame < 3600; ++frame) {  // 60 seconds at 60 Hz
    fm.update(1.0/60.0, input, 0.0, {0,0,-1});
}

// Read the results
double altitude = -fm.state().kin.z;      // ft
double airspeed = fm.state().kin.vt;       // ft/s
double mach     = fm.state().mach;
double gLoad    = fm.state().loads.nzcgs;
```

**Modules**:
- `constants.hpp` — physical constants (GRAVITY=32.177, atmosphere params)
- `aircraft_state.hpp` — runtime state structs (kinematic, aero, engine, FCS, gear)
- `atmosphere.hpp` — 3-layer ISA model, Mach↔KCAS conversion
- `aerodynamics.hpp` — CL/CD/CY lookup, ground effect, stall model, force transformation
- `fcs.hpp` — G-command PI controller, roll rate command, yaw stub
- `engine.hpp` — turbofan with afterburner, spool dynamics, fuel flow
- `eom.hpp` — quaternion kinematics, body rates, ground clamp
- `gear.hpp` — strut compression, friction, gear actuation
- `stall_state.hpp` — 6-state stall SM (None → EnteringDeepStall → DeepStall
  → Spinning/FlatSpin → Recovering → None), built on f4-state-machine
- `flight_model.hpp` — orchestrator with sub-stepping, trim solver, and stall SM

**Tests**: 26 tests (atmosphere model validation, trim convergence, 60-second
stability run, FCS response, throttle response, ground operations, multi-
aircraft init, stall SM integration with trace verification).

## Building

```bash
cmake -B build -S .
cmake --build build
ctest --test-dir build/f4-units/tests --output-on-failure
ctest --test-dir build/f4-math/tests --output-on-failure
ctest --test-dir build/f4-geo/tests --output-on-failure
ctest --test-dir build/f4-state-machine/tests --output-on-failure
ctest --test-dir build/f4-entities/tests --output-on-failure
ctest --test-dir build/f4-convert/tests --output-on-failure
ctest --test-dir build/f4-data/tests --output-on-failure
ctest --test-dir build/f4-world-convert/tests --output-on-failure
ctest --test-dir build/f4-world/tests --output-on-failure
ctest --test-dir build/f4-terrain/tests --output-on-failure
ctest --test-dir build/f4-flight-model/tests --output-on-failure
```

Requires CMake 3.20+ and a C++20 compiler (MSVC 19.28+, GCC 10+, Clang 12+).
Tests use [Google Test](https://github.com/google/googletest) v1.14.0,
fetched automatically via CMake's FetchContent.

### Visual Studio on Windows

The libraries build cleanly with the Visual Studio 17 2022 generator:

```cmd
cmake -B build -G "Visual Studio 17 2022"
cmake --build build --config Release
:: Open build\F4.sln in Visual Studio, set a startup project, F5 to debug.
```

The `f4-world-viewer` target is set up with `VS_DEBUGGER_WORKING_DIRECTORY`
and `VS_DEBUGGER_COMMAND_ARGUMENTS` so pressing F5 in Visual Studio will
launch the viewer pointed at the bundled `save1.world.json` +
`korea.terrain.json` fixtures (after running the `world` target once).

## World Data Pipeline

The F4 sim consumes two kinds of JSON files, both produced by converter CLIs:

```
THEATER.MAP/.MEA/.O2  ─→  f4-terrain-convert (terrain2json)  ─→  korea.terrain.json
                                                                         │
save1.cam  ─────────→  f4-world-convert (cam2json)  ─→  save1.world.json ─┘
                                                                         │
                                                                         ↓
                                                               f4-world (loader)
                                                                         │
                                                                         ↓
                                                              WorldState { terrain,
                                                                            campaign,
                                                                            objectives,
                                                                            units, teams }
```

**Generate both at once:**

```bash
cmake --build build --target world
```

This produces `build/korea.terrain.json` (~85 KB) and `build/save1.world.json`
(~430 KB) from the bundled fixtures. The terrain JSON is intentionally small
and human-readable for easy diffing — 16,384 tile-type entries as a flat
JSON array.

### Format

`korea.terrain.json`:
```json
{
  "theater": "korea",
  "width": 128,
  "height": 128,
  "tile_types": [0, 0, 0, 1, 2, 3, ...]   // 16,384 entries, TileType enum
}
```

`save1.world.json`:
```json
{
  "theater": "korea",
  "terrain_file": "korea.terrain.json",   // relative path
  "version": 63,
  "campaign": { ... },
  "objectives": { "count": 2659, "items": [...] },
  "units": { "count": 683, "items": [...] },
  ...
}
```

## Interactive World Viewer

`f4-world-viewer` is a Raylib + Dear ImGui desktop app for inspecting world
data. It's the primary way to validate what's in the world before developing
more advanced systems (digi AI, ATO).

**Run it:**

```bash
cmake --build build --target f4-world-viewer
./build/f4-world-viewer/f4-world-viewer build/save1.world.json build/korea.terrain.json
```

Or from Visual Studio: set `f4-world-viewer` as the startup project and press F5.

**Features:**
- Color-coded terrain tiles (water / lowland / hills / mountains / peaks)
- Campaign objectives as circles, colored by owner team
- Campaign units as squares with destination lines
- Click-to-inspect any objective or unit → ImGui panel shows all decoded fields
- Pan (drag), zoom (wheel), fit-to-world (View menu)
- Layer toggles (terrain / objectives / units / grid / legend)
- File menu wraps the CLI converters in-process:
  - **Open World JSON** — load an existing `*.world.json`
  - **Open Terrain JSON** — load an existing `*.terrain.json`
  - **Import .cam Archive** — runs `cam2json` in-process, writes a `.world.json`
    next to the source, and loads it
  - **Import THEATER.\* Binary** — runs `terrain2json` in-process and loads
    the resulting terrain JSON
- F2 takes a screenshot (saved as `f4_viewer_screenshot.png` in the CWD)

This is the starting point for a future world editor: the same load/render
pipeline will gain edit/save capabilities as new systems come online.

## Design Principles

- **Engine-agnostic**: No dependency on DirectX, UI, or rendering. These
  libraries compute simulation state; rendering is a separate concern.
- **Testable in isolation**: Each library compiles, links, and unit-tests
  without any of the others. f4-math has zero coupling to f4-units (proven
  by the test target linkage).
- **Preserve functionality, not code**: FreeFalcon is the source of truth
  for behavior. The implementation is free to use modern architectures.
- **Zero-cost abstractions**: Template-based unit types, `constexpr`
  configuration, inline table lookups.
