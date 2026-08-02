# F4 — Modern C++ Libraries for Flight Simulation

A ground-up reimplementation of FreeFalcon's core subsystems
(campaign, flight model, AI) as independent, engine-agnostic, testable
C++20 libraries. See `Docs/ARCHITECTURE PROPOSAL.md` for the full design.

## Libraries

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

### f4-flight-model — 6-DOF flight dynamics

Static library implementing the full 6-DOF flight dynamics: atmosphere,
aerodynamics, flight control system, engine, equations of motion, and gear
model. The FlightModel orchestrator ties them together with sub-stepping
and a trim solver.

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
- `flight_model.hpp` — orchestrator with sub-stepping and trim solver

**Tests**: 23 tests (atmosphere model validation, trim convergence, 60-second
stability run, FCS response, throttle response, ground operations, multi-
aircraft init).

## Building

```bash
cmake -B build -S .
cmake --build build
ctest --test-dir build/f4-units/tests --output-on-failure
ctest --test-dir build/f4-math/tests --output-on-failure
ctest --test-dir build/f4-convert/tests --output-on-failure
ctest --test-dir build/f4-data/tests --output-on-failure
ctest --test-dir build/f4-flight-model/tests --output-on-failure
```

Requires CMake 3.20+ and a C++20 compiler (MSVC 19.28+, GCC 10+, Clang 12+).
Tests use [Google Test](https://github.com/google/googletest) v1.14.0,
fetched automatically via CMake's FetchContent.

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
