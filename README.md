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

### f4-messaging — Type-safe message bus with explicit thread boundaries

Static library replacing FreeFalcon's 76+ `FalconEvent` subclasses (each
with `#pragma pack(1)` `DATA_BLOCK`s, manual `Encode`/`Decode`, routed via
`switch(FalconMsgID)`) with three small primitives: plain-data message
structs, a type-indexed `MessageBus` for fan-out, and a thread-safe
`MessageQueue<Msg>` for SPSC patterns.

```cpp
#include <f4/messaging/f4_messaging.hpp>
using namespace f4::messaging;

MessageBus sim_bus;
sim_bus.subscribe<DamageMsg>([](const DamageMsg& m) {
    // ... apply damage to m.target_id ...
});

// Same-thread delivery (hot path):
sim_bus.publish(DamageMsg{target_id=7, source_id=1, strength=0.5f});

// Cross-thread delivery (campaign → sim):
MessageBus campaign_bus;
// In the campaign thread:
send_to(sim_bus, MissionAssignMsg{...});
// In the sim thread, at the top of the tick:
sim_bus.flush_pending();   // delivers all cross-thread messages
```

**Modules**: `bus.hpp` (`MessageBus` — `subscribe` / `publish` /
`publish_deferred` / `flush_pending` / `send_to` friend; `MessageQueue<Msg>`
— `push` / `drain`).

**Threading model**: `publish()` and `publish_deferred()` are safe to
call concurrently from multiple producer threads. Each subsystem owns a
bus; each tick starts with `flush_pending()`. Handlers may themselves
`publish` / `publish_deferred` — the swap-then-drain `flush_pending`
prevents unbounded recursion.

**Tests**: 28 (subscribe/publish/unsubscribe semantics, multi-handler
dispatch order, payload-carrying messages, deferred + flush, send_to
cross-bus, recursive-flush safety, 4-thread × 250-msg stress,
bidirectional sim+campaign topology). Zero external dependencies.

### f4-json — Minimal dependency-free JSON reader/writer

Static library providing the JSON reader and writer used by f4-world,
f4-terrain, and f4-world-viewer/settings. Replaces three duplicated
hand-rolled implementations (~410 LoC of copy-pasted parser code) with
one shared library. Header-only; the API is shape-compatible with the
local `JsonReader` classes that lived in f4-world and f4-terrain, so the
refactor was mechanical.

```cpp
#include <f4/json/f4_json.hpp>
using f4::json::Reader;
using f4::json::Writer;

// Walk a known schema (the pattern used by f4-world, f4-terrain):
Reader r(json_string);
r.skip_ws(); r.expect('{');
while (!r.consume('}')) {
    std::string key = r.read_string();
    r.expect(':');
    if      (key == "name")  name = r.read_string();
    else if (key == "width") width = r.read_int();
    else                     r.skip_value();
    (void)r.consume(',');
}

// Emit (the pattern used by f4-terrain's to_terrain_json):
Writer w;
w.raw("{\n");
w.raw("  \"theater\": "); w.string("Korea"); w.raw(",\n");
w.raw("  \"width\": ");   w.number(128);     w.raw("\n");
w.raw("}\n");
std::string out = w.str();
```

**Modules**: `reader.hpp` (`Reader` — recursive-descent parser for
objects/arrays/strings/numbers/bool/null with escape decoding including
basic `\uXXXX`), `writer.hpp` (`Writer` — `raw` / `string` /
`number<T>` / `number_key` / `string_key` with templated integral
overloads to avoid ambiguity).

**Tests**: 45 (peek/expect/consume, escape decoding, integer/float
parsing, skip_value across object/array/bare tokens, writer escaping,
round-trip preservation of all field types, nested-object round-trip).
Zero external dependencies.

**Why not nlohmann/json here?** f4-geo, f4-entities, f4-messaging, and
f4-state-machine all keep a zero-deps stance so they can be lifted into
any host project. f4-world and f4-terrain inherit that stance. We
control both ends of the wire format — a 200-line reader is sufficient
and faster to compile than a 25k-line header. nlohmann/json stays in
f4-convert / f4-data where its richer random-access API is needed.

### f4-install — Falcon 4.0 install layout locator

Static library that owns the layout knowledge of a Falcon 4.0 / FreeFalcon
installation: where `FALCON4.ct` lives, how to enumerate theaters under
`terrdata/`, how to find `.cam` saves under `campaign/`, and how to
resolve any well-known file by name. Zero external dependencies beyond
the standard library — portable across Windows, macOS, and Linux.

```cpp
#include <f4/install/f4_install.hpp>
using namespace f4::install;

// Point at the user's Falcon 4.0 install:
auto inst = Installation::detect("/path/to/falcon4");
if (!inst.valid()) { /* show error */ }

// Enumerate theaters (from terrdata/theater.lst + directory scan):
for (const auto& t : inst.theaters()) {
    std::cout << t.display_name << " (" << t.key << ")\n";
}

// Enumerate campaigns (recursive walk of campaign/):
for (const auto& c : inst.campaigns_for("korea")) {
    std::cout << "  " << c.stem << " — " << c.cam << "\n";
}

// Auto-resolve FALCON4.ct for a given .cam (searches next to .cam, up
// directories, then install root, then CWD fallback):
auto ct = inst.find_class_table(c.cam);
```

The viewer and the `cam2json` CLI both call into `f4-install` so the
install-layout knowledge lives in one place. `f4-world-convert`'s
existing `find_class_table()` helper now delegates here.

**Tests**: 38 (install validation, FALCON4.ct discovery at root/sim/terrdata,
theater discovery with case-insensitive matching, `theater.lst` parsing
with comments/quotes/lowercase/inline-comments, `theater.ini` title
reading, campaign scanning with flat + nested layouts mixed, install-aware
class-table resolution at each search step, `resolve()` on valid/invalid
installs). Zero external dependencies.

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

### f4-ai — AI brain (planned)

Composed-module AI brain replacing FreeFalcon's 1209-line `DigitalBrain` god-class.
Each tactical behavior is an independent module with its own state machine and
trace. A `LayeredStateMachine` resolves the 26-priority DigiMode ladder.

```cpp
#include <f4/ai/f4_ai.hpp>
using namespace f4::ai;

DigitalBrain brain;
brain.initialize(ownship_id, world, bus, SkillLevel::Veteran);

// Per-frame update — produces control output for FlightModel
for (int frame = 0; frame < 3600 * 60; ++frame) {  // 60 seconds at 60 Hz
    AIControlOutput output = brain.update(1.0 / 60.0);
    PilotInput input = to_pilot_input(output);
    fm.update(1.0 / 60.0, input, groundZ, groundNormal);
}
```

**Modules** (planned — see `Docs/AI_IMPLEMENTATION_PLAN.md`):
SensorFusion, TakeoffModule, LandingModule, NavigationModule,
RefuelModule, CollisionAvoidModule, BVRModule, WVRModule,
MissileModule, WingmanModule, DigitalBrain orchestrator.

**Dependencies**: f4-flight-model, f4-entities, f4-messaging, f4-state-machine,
f4-geo, f4-data, f4-math.

### f4-weapons — Weapons & effects core

Static library implementing the bottom of the combat chain
(`Docs/COMBAT_CHAIN_PLAN.md`, Milestone M1): weapon class data, loadout
stores, a 3-DOF guided-missile flyout (proportional navigation, seeker
cone/range limits, proximity fuze, time-of-flight self-destruct), a
ballistic gun model, and the warhead-vs-strength damage model. Missiles are
ECS entities (FreeFalcon's VuEntity model); gun tracers are not. Combat
events cross the MessageBus as plain structs.

```cpp
#include <f4/weapons/f4_weapons.hpp>
using namespace f4::weapons;

WeaponClassTable table = WeaponClassTable::with_builtins();
const auto amraam = table.find_by_name("AIM-120C");

// What the jet carries (passive ECS component)
auto& store = shooter.add<WeaponStoreComponent>(
    WeaponStoreComponent::standard_fighter(table));

// Fire: validates + debits the store, spawns the missile entity,
// publishes MissileLaunchedMessage
auto missile = launch_missile(world, bus, shooter, target, table, amraam, sim_time);

// Per tick (inside the sim loop): world.update_all() ticks every
// MissileSimComponent (physics pass). When a fuze fires, damage lands on
// the target's DamageStateComponent and the bus carries the events.
sweep_spent_missiles(world);   // between ticks
```

**Modules**:
- `weapon_types.hpp` / `weapon_class_table.hpp` — weapon data cards
  (categories, guidance kinds, flyout envelopes; built-in placeholder set,
  FALCON4.WST import deferred)
- `weapon_store.hpp` — `WeaponStoreComponent` (stations, rounds, selection)
- `missile.hpp` — pure 3-DOF point-mass flyout + PN guidance + fuze
- `missile_battery.hpp` — missile-as-entity ECS binding (`MissileComponent`
  + `MissileSimComponent`), `launch_missile()`, `sweep_spent_missiles()`
- `gun.hpp` — `GunStream` ballistic tracers, dispersion, proximity hits
- `damage.hpp` — `apply_damage()` (power vs hit points, range falloff)
- `messages.hpp` — launch/detonate/fire/damage/killed bus events

**Dependencies**: f4-geo, f4-math, f4-entities, f4-messaging. Deliberately
NOT dependent on f4-flight-model or f4-ai.

### f4-sensors — Sensor model (radar, tracks, RWR)

Static library implementing the "eyes" of the combat chain
(`Docs/COMBAT_CHAIN_PLAN.md`, Milestone M2): an airborne radar with scan
volumes and a probability-of-detection model (fourth-root RCS scaling,
aspect lobes, closure effect), track files with quality build-up and
exponential decay (Tentative → Established → Coasting → Dropped), IFF by
team tag, NCTR identity strings, and a radar warning receiver (search
strobe / lock / launch warnings). Detections are sampled against a seeded
mt19937 per radar — same seed, same scenario, same detection timeline.

```cpp
#include <f4/sensors/f4_sensors.hpp>
using namespace f4::sensors;

// Add to a radar-equipped aircraft (configure before the sim loop starts;
// config is baked lazily on the first update).
auto& radar = jet.add<RadarSimComponent>();
radar.own_team = "blue";
radar.params.reference_range_nm = 40.0;

// Per tick: world.update_all() runs the scan (priority 45 — physics pass).
// Tracks accumulate in radar.tracks(); command_track() STT-locks.
const TrackFile* t = radar.tracks().find(target_id);
if (t != nullptr && t->state == TrackState::Established) {
    radar.command_track(target_id);
}

// Between ticks: refresh every RWR in the world from the radar picture.
update_rwr(world, bus, sim_time);   // publishes RwrWarningMessage on
                                    // new lock/launch only
```

Missiles consume the sensor picture through
`MissileComponent::seeker_source` (f4-weapons); the AI consumes it through
`SensorFusion::set_detection_policy` (f4-ai) — both hooks are wired by the
host, so f4-sensors stays a leaf library with no AI/weapons dependency.

**Modules**:
- `radar_types.hpp` — `RadarParameters`, `ScanVolume`, `RadarMode`,
  `TargetSignature`
- `detection.hpp` — pure detection model (range + probability, no RNG)
- `track_store.hpp` — `TrackFile` / `TrackStore` (quality, decay, IFF, NCTR)
- `signature.hpp` — `SignatureComponent` (target RCS; default 5 m²)
- `radar_component.hpp` — `RadarSimComponent` (ECS behavioral, priority 45)
- `rwr.hpp` — `RwrModel`, `RwrComponent`, `update_rwr()`, `RwrWarningMessage`
- `messages.hpp` — radar track acquired/dropped bus events

**Dependencies**: f4-geo, f4-math, f4-entities, f4-messaging. Deliberately
NOT dependent on f4-ai or f4-weapons (the integration hooks are std::function
/ pure-virtual interfaces the host wires).

### f4-simulation — Orchestration + the combat chain host

The tick-loop owner: EntityWorld + MessageBus + ModelDatabase +
AircraftConfig, aircraft spawning (scenario list or campaign-derived),
two-pass ECS update (brains ≥ 75, physics < 75), FM→Transform sync, flight
recording. With M3 integration it is also the layer that *drives* the
combat chain: scenario `"combat": {"enabled": true}` attaches the combat
component set to every scenario aircraft (weapon store, signature, radar,
RWR, damage state) and the tick runs the sensor/weapon sweeps the ECS
can't run itself (sim-time stamping, `update_rwr`, `sweep_spent_missiles`).

```cpp
#include "f4/simulation/simulation.hpp"
#include "f4/simulation/combat_bridge.hpp"

f4::simulation::Simulation sim(scenario, scenario_dir);
sim.initialize();                 // combat components attach here

// ... ticks drive radar scans, track files, RWR warnings, missile flyouts ...
for (int i = 0; i < n; ++i) sim.tick(scenario.sim_dt);

// A host (or, at M3, BVRModule) fires through the sim's own table:
const auto amraam = sim.weapon_table().find_by_name("AIM-120C");
f4::weapons::launch_missile(sim.world(), sim.bus(), shooter_handle,
                            bandit_id, sim.weapon_table(), amraam,
                            sim.sim_time_s());

// The AI's "eyes" flip from GCI-omniscience to radar truth through the
// host-side policy adapter (f4-ai stays a pure interface):
f4::simulation::RadarBackedDetectionPolicy policy(sim.world(),
                                                  shooter_id.value);
sensor_fusion.set_detection_policy(&policy);
```

**Combat-relevant pieces**: `scenario.hpp` (`CombatConfig`, per-aircraft
`team`), `combat_bridge.hpp` (`attach_combat_loadout`,
`RadarBackedDetectionPolicy`), `simulation.hpp` (`weapon_table()`),
`simulation.cpp` (tick integration — all gated on `combat.enabled`, so a
non-combat world is unchanged). Combat-disabled scenarios carry none of
the combat components.

**V-CAMP**: `campaign_session.hpp` — the live campaign loop as ONE
frame-driven object (the `campaign_qc` wiring, composed for hosts):
C1 ledger + C2 one-pool tasking + C3 routed generation over the
simulation's own world and bus, with the spawner's generated missions
REGISTERED into the tick roster through `Simulation::register_aircraft()`
(the one-world closure — "materialized" means FLYING, not counted).
One clock: fixed `sim_dt` ticks; the campaign ladder and the damage
sync advance in whole campaign seconds off the same ticks. The world
viewer drives it (play/pause, 1x–240x wall-clock presets, the campaign
clock, war status, live aircraft + routes + the threat overlay).

```cpp
f4::simulation::CampaignSessionOptions opts;
opts.world_json = "TestCamp.world.json";      // absolute paths
opts.aircraft_config = "<build>/generated_fixtures/f16.json";
opts.mission_profiles = "<build>/generated_campaign/MissionProfiles.json";
opts.max_flights = 48;                        // interactive budget

auto session = f4::simulation::CampaignSession::create(opts, &err);
session->set_paused(false);
while (running) {
    session->advance(frame_dt * speed);       // speed presets scale dt;
}                                             // the tick dt never changes
const std::string result_json = session->ledger_json();  // byte-stable
```

**Dependencies**: f4-entities, f4-messaging, f4-flight-model, f4-flight-api,
f4-ai, f4-data, f4-geo, f4-math, f4-units, f4-state-machine, f4-models,
f4-recorder, f4-json, f4-io, f4-world, f4-world-convert, f4-terrain,
f4-weapons, f4-sensors, f4-campaign. See `Docs/COMBAT_CHAIN_PLAN.md` (M3),
`Docs/AIRCRAFT_BINDING_DESIGN.md`, and `Docs/CAMPAIGN_LOOP_PLAN.md` (V-CAMP).

### f4-campaign — headless dynamic campaign + the result ledger (the war loop)

Static library implementing the campaign layer two ways. The tasking
side (`Campaign` + `MissionProfileTable`): a headless tick that fires
per-team air-tasking cycles, gates mission generation on role /
capability / aircraft availability, and publishes `MissionIntent` on
the bus — the same message shape whether the source is the profile
ladder or a decoded save (`emit_flight_intents` turns live saved
flights into intents, B.3). The result side (C1): the
`CampaignResultLedger` — the campaign's write model, closing the
return leg of the loop. C2 makes the ledger THE tasking pool: mission
draws debit it (apply_mission_draw), combat losses net against the
draws, and the reinforcement cadence (the .cmp header's
last_reinforcement anchor, the wire's per-squadron budgets) refills it
— cycles, combat, and resupply deplete ONE pool. C3 gives the forward
leg its brain: `ThreatMap` (ScoreThreatFast's per-cell ownership +
2-bit AD density rings), `AirPathFinder` (the reference's grid A* —
2000-node pool, partial-path fallback, every constant), and
`RouteBuilder` (BuildPathToTarget: ingress corners around SAM rings,
IP, target, turn point, egress, waypoint elimination) — generated
missions fly THEIR OWN routes, airbase → target → airbase, and the
spawner materializes them as they publish (generation-to-spawn).
V-CAMP composes all of it into the live, frame-driven session the
world viewer runs (f4-simulation's `campaign_session.hpp`).

```cpp
#include <f4/campaign/result_ledger.hpp>
#include <f4/simulation/campaign_result_sink.hpp>

// The write model, snapshotted from the same sources the run started
// from (pools + squadron history; a mid-campaign save seeds non-zero).
CampaignResultLedger ledger(adapters.campaign, adapters.teams,
                            adapters.units);

// The sink resolves sim outcomes back to campaign identity (kills,
// bomb impacts → team/squadron/objective) and feeds the ledger.
CampaignResultSink sink(ledger, sim.world());
sink.attach(sim.bus());
// ... run the fight ...
sink.sync_objective_damage();   // objectives' final damage state

// Tasking reacts — and draws from the SAME pool (C2): attach the
// ledger and the availability gates read the one-pool numbers
// (snapshot − draws − non-drawn losses + reinforcements); every
// generated mission debits the ledger where combat losses and
// resupply already live. A fresh ledger changes nothing — pinned.
campaign.set_result_ledger(&ledger);

// The reinforcement cadence (C2, opt-in): anchored on the save's own
// last_reinforcement, refilling deficits from the wire's per-squadron
// budgets. A stale anchor (TestCamp carries 0 against a 38.5M-second
// epoch) fires exactly ONCE — FreeFalcon's catch-up shape.
f4::campaign::CampaignConfig cfg;
cfg.reinforcement_period_sec = 12 * 3600;
f4::campaign::Campaign ladder(adapters.campaign, adapters.teams,
                              adapters.units, profiles, bus, cfg);
ladder.set_result_ledger(&ledger);

// C3: the route planner — generated missions fly their own routes.
// One ThreatMap over the sources (ownership + AD rings, built once —
// static dispositions this slice), the A* and the builder behind it.
// select_target picks the enemy objective (RelType::War — the stance
// vocabulary is an enum, decoded garbage-tolerant); routes shape
// around threat bands, and intents carry waypoints + target.
const f4::campaign::RouteBuilder routes(
    adapters.objectives, adapters.units, adapters.teams,
    /*viewer=*/my_team);
ladder.set_route_planner(&routes, &adapters.objectives);
ladder.tick(24 * 3600);   // cycles fire, draws deplete, resupply refills

// The write-back: pools, squadron counters, objective fstatus into
// the typed WorldState — decode → run → fight → apply → reload.
auto written = f4::campaign::apply_to(ledger, ws);
// written.objectives_written / unmatched_* are LOUD, never silent.

// The artifact: strictly valid JSON, byte-stable, no floats.
std::ofstream out("campaign_result.json") << ledger.to_json();
```

Attribution rides `CampaignOriginComponent` — the sim entity's campaign
identity (flight/squadron/home-airbase VUs + team slot), stamped at
spawn by the campaign bridge (the shared core of every campaign spawn
path). Air kills: victim pool −1, squadron loss, killer aa credit
(unattributed when the killer isn't a campaign aircraft). AG kills
book credit only. Objective damage is final-state sync — f4-weapons
owns the live per-feature ledger; the sink snapshots initial damage at
construction and hands back what changed. All saturating at the wire's
own field limits; no RNG anywhere. Draw/loss NETTING (C2): a drawn
aircraft's death consumes its draw — the pool debits once, the debrief
counts the loss; a parked aircraft's death debits the pool directly.

**Dependencies**: f4-world (IDataSource ONLY — never EntityWorld
components; the ECS resolution lives in f4-simulation's sink),
f4-messaging, f4-json (PRIVATE), f4-io. See
`Docs/CAMPAIGN_LOOP_PLAN.md` (C1 + C2 + C3 + V-CAMP landed, C4–C5 the
roadmap).

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
ctest --test-dir build/f4-install/tests --output-on-failure
ctest --test-dir build/f4-world-viewer/tests --output-on-failure
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
more advanced systems (digi AI, ATO) — and, since V-CAMP, the way to RUN
the campaign loop interactively (see "The live campaign session" below).

### Quick start (install-aware flow — recommended)

```bash
cmake --build build --target f4-world-viewer
./build/f4-world-viewer/f4-world-viewer
```

Then in the app:

1. **File > Set Install Path...** → pick your Falcon 4.0 install directory
   (the one containing `FALCON4.ct` and `terrdata/`). The viewer detects
   theaters, campaigns, and the class table automatically, and caches the
   path to `~/.config/f4-viewer/settings.json` (Linux) /
   `~/Library/Application Support/f4-viewer/` (macOS) /
   `%APPDATA%/F4Viewer/` (Windows). You only do this once.

2. **File > Open Campaign...** → pick a Theater from the dropdown, then a
   Campaign. The viewer runs the in-process converters (`terrain2json` +
   `cam2json` with auto-resolved `FALCON4.ct`) and renders the result.

The install path persists across launches — next time, just open the
viewer and pick a campaign. The last theater + campaign are also
remembered, so re-launching restores your previous session.

### The live campaign session (V-CAMP)

With a campaign open: **Campaign > Start Session...** (or the Campaign
Session window's Start button). The session builds the full Phase-C loop
over the loaded world — the C1 ledger, C2 one-pool tasking, C3 routed
generation — and the save's own flights fly alongside the missions the
ladder generates, in one simulation world. Controls:

- **Play/Pause (Space)** + speed presets **1x / 10x / 60x / 240x** —
  the presets scale wall-clock time; the sim's tick stays at its tuned
  1/60 s (the flight model's discretization holds at every speed). A
  30-minute tasking cycle passes in 30 s at 60x.
- **Campaign clock** (D# HH:MM:SS) — the save's epoch + the loop's own
  clock: one timeline for tasking, reinforcement, and flight.
- **War status** — cycles fired, missions generated, routes
  built/failed, aircraft drawn (the one pool), combat losses,
  reinforcement fires/deliveries, live/airborne counts.
- **Generated missions** — one row per tasked mission (mission, team,
  TOT, target, route waypoints, package size); click a target to
  select + pan to it.
- **Canvas live layer** — the aircraft as they fly (owner colors,
  grounded dimmed), each with its route polyline + numbered waypoints;
  the threat-map overlay (View > Threat map overlay) paints the enemy
  air-defense rings the routes bend around; click a live aircraft to
  inspect it (identity, phase, kinematics, its live flight plan).
- **Write Result JSON** — the C1 ledger as `campaign_result.json`
  (byte-stable, the campaign_qc artifact) next to the world JSON;
  **Write Back** applies the ledger's pools/fstatus into the session's
  WorldState (in-memory; the .cam re-encoder is a future tranche).

Sessions start **paused** — press Space (or Play) to run the clock.
Reset = Stop + Start. The saved-flight spawn cap (default 48 of
TestCamp's 449 flights) keeps the interactive budget; raise it in the
start row if your machine has headroom. Fixture paths (class table,
F-16 config, mission profiles) resolve from the install when one is
configured, else the build tree — the campaign_qc defaults.

### CLI (for scripts / smoke tests)

```bash
# Set install path + load a campaign in one shot
./build/f4-world-viewer/f4-world-viewer \
    --install /path/to/falcon4 \
    --campaign korea save1

# Take a screenshot after 1.5s and exit (headless smoke test)
./build/f4-world-viewer/f4-world-viewer \
    --install /path/to/falcon4 \
    --campaign korea save1 \
    --screenshot out.png

# Focus on a specific region (grid coordinates)
./build/f4-world-viewer/f4-world-viewer save1.world.json --zoom 8 --center 500,500
```

The `--install` flag updates `settings.json` (same as File > Set Install
Path). The `--campaign` flag uses the install-aware loader — it auto-loads
`THEATER.*` and the `.cam` plus `FALCON4.ct` in one call. Both flags can
be combined with the existing positional `world.json terrain.json` args
and `--screenshot` / `--zoom` / `--center`.

### Advanced (manual file picking)

The original four File menu items are preserved under **File > Advanced**
for the dev / un-bundled-fixtures workflow:

- **Open World JSON** — load an existing `*.world.json`
- **Open Terrain JSON** — load an existing `*.terrain.json`
- **Import .cam Archive** — runs `cam2json` in-process, writes a `.world.json`
  next to the source, and loads it
- **Import THEATER.\* Binary** — runs `terrain2json` in-process and loads
  the resulting terrain JSON

All four use native OS file/folder pickers (via `tinyfiledialogs`) — no
more typing paths into a text input. The folder picker (used by Set
Install Path and Import THEATER.*) is new; the old ImGui modal couldn't
do folder selection.

### Features

- Color-coded terrain tiles (water / lowland / hills / mountains / peaks)
- Campaign objectives as type-specific icons (airbase / bridge / city /
  port / ...) colored by owner team, with gold rings on high-priority targets
- Campaign units as squares/diamonds/circles/triangles by class, with
  subtype-specific icons (armor / fighter / carrier / ...) where available
- Click-to-inspect any objective or unit → ImGui panel shows all decoded fields
- Pan (drag), zoom (wheel), fit-to-world (View menu)
- Layer toggles (terrain / objectives / units / grid / legend / routes)
- **Tools > Hex Inspector** — inspect raw bytes of any file in the install
  with format-aware decoder overlays (see below)
- F2 takes a screenshot (saved as `f4_viewer_screenshot.png` in the CWD)

### Hex Inspector (Tools menu)

The Hex Inspector is the primary reverse-engineering tool. Open any file
in the install — `FALCON4.ct`, a `.cam` save, `THEATER.MAP`, an unknown
binary — and get a scrollable hex+ASCII view with decoder annotations
that label what each byte range means.

**Layout:**
```
┌────────────────────────────────────────────────────────────────────┐
│ [Open File...] [path/to/file.cam   197340 bytes   .cam]            │
│ [decoder: Campaign Archive (.cam) ▼]   [Re-decode]                  │
├────────────────────────────┬───────────────────────────────────────┤
│ Annotations (left, 280px)  │  Hex dump + ASCII (right)              │
│  • magic          0x444C.. │  00000000  AE FF 4C 44 ... │ ..L.D... │
│  • manifest_off   197328   │  00000010  ...                      │
│  • subfile:save1.cmp  4420 │  (click a byte to select)            │
│  ...                       │                                       │
├────────────────────────────┴───────────────────────────────────────┤
│ Selection: [0..16]  16 bytes                                        │
│ [Copy as Hex] [Copy as C array] [Copy as Python] [Save As...]      │
└────────────────────────────────────────────────────────────────────┘
```

**Decoders** (auto-selected by file extension / magic bytes, manually
overridable via the dropdown):
- **Campaign Archive (.cam)** — annotates the manifest offset, every
  sub-file's byte range, and the trailing manifest directory
- **Campaign Metadata (.cmp)** — annotates the 8-byte header
  (reserved_skip + decompressed_size) + the LZSS-compressed payload
- **Theater Header (THEATER.MAP)** — annotates magic / width / height /
  ft_to_cell + the first 8 palette entries
- **Class Table (FALCON4.ct)** — annotates num_entities + the first 16
  ClassTableEntry records (entity_type, domain, class, type, stype)
- **Generic** (fallback) — annotates file size, first 16 magic bytes,
  Shannon entropy estimate, ASCII string runs ≥ 4 chars

**Selection + export:** click any byte in the hex dump to select it;
click an annotation to select its whole byte range. Then:
- **Copy as Hex** — space-separated hex bytes (`AE FF 4C 44`)
- **Copy as C array** — `static const unsigned char data[N] = { 0xAE, 0xFF, ... };`
- **Copy as Python** — `data = bytes.fromhex("aeff4c44...")`
- **Save As...** — native save dialog, writes the raw bytes to a file

This is the "extract just the bytes you care about" tool — when you
need to share a hex dump of a specific structure with a collaborator
(or with me), select the range and copy it in your preferred format.

**CLI** (for scripted use / smoke tests):
```bash
./build/f4-world-viewer/f4-world-viewer --hex-inspect /path/to/FALCON4.ct
```
Opens the viewer with the Hex Inspector panel already open and the file
loaded + decoded.

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
