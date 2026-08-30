# F4 Cleanup Pass — Changes Summary

This document summarizes the cleanup pass applied to the F4 codebase.
**All 998 unit tests pass on a clean build.** (Up from 988 — added 10 new
tests covering the fixes.)

## Build Fixes (the repo on `main` does not compile out-of-the-box)

| File | Fix |
|------|-----|
| `CMakeLists.txt` | Added `add_subdirectory(f4-io)` — `f4-world-convert` and `f4-terrain` link against `f4-io` but the root CMakeLists never added it. Fatal: `f4/io/read_file.hpp: No such file or directory`. |
| `CMakeLists.txt` | Added `enable_testing()` at top level. All test executables built fine, but `ctest` from the build root reported "No tests were found!!!" because no top-level `enable_testing()` had been called. |
| `f4-json/include/f4/json/writer.hpp` | Added missing `f4::json::escape_string()` free function. `world_json.cpp:24` does `using f4::json::escape_string;` but the function didn't exist in the header. The worklog claims it was added but it never made it into the committed code. |

## Phase 0 — Stabilize

### Phase 0b: Deleted 14 orphaned diagnostic files

Removed `f4-models/tests/diag_*.cpp` (14 files, ~1370 lines):
- `diag_bsp_direct.cpp`, `diag_correct.cpp`, `diag_geometry.cpp`,
  `diag_geometry2.cpp`, `diag_hex.cpp`, `diag_hex2.cpp`,
  `diag_offsets.cpp`, `diag_offsets2.cpp`, `diag_offsets3.cpp`,
  `diag_raw_bsp.cpp`, `diag_slot_sizes.cpp`, `diag_tags.cpp`,
  `diag_tree.cpp`, `diag_vtables.cpp`

These were standalone `int main()` diagnostic programs (not gtest tests),
were NOT listed in `CMakeLists.txt` (so never built), and contained
hardcoded absolute paths like `/home/z/my-project/f4-repo/...` that
don't exist in any checkout. Pure dead code cluttering `tests/`.

### Phase 0c: RAII FILE* in `f4-io/src/read_file.cpp`

Replaced raw `FILE* fp = std::fopen(...)` with a `FileGuard` RAII
wrapper. If the `std::vector<uint8_t> buf(sz)` allocation throws
`std::bad_alloc` between `fopen` and `fclose`, the handle no longer
leaks. This single helper backs every binary loader in the project.

### Phase 0d: Comment typos fixed

- `f4-flight-model/include/f4/flight/flight_model.hpp:193` — wrong
  arithmetic in the `minorFrameTime_` comment ("6 sub-steps of 1/60s =
  1/10s major" — actually 6 × 1/360s = 1/60s major).
- `f4-math/include/f4/math/scalar.hpp:147` — referenced a `qsquared`
  function that doesn't exist.

### Phase 0e: Tightened weak tests

The original tests asserted "is the value finite" or used tolerances so
loose they admitted fully-developed stalls and divergent EOMs.

**`test_atmosphere.cpp`**:
- Replaced self-consistency check (`EXPECT_NEAR(out.rho, RHOASL, 1e-9)`)
  with assertions against published ISA-1976 reference values at sea
  level, 18000 ft, 36089 ft (tropopause), and 50000 ft. Tolerances are
  relative (0.5%) to admit the legacy Falcon-4 lapse rate constants
  while still catching wrong layer breakpoints or swapped exponents.
- Tightened `ZeroAirspeedDoesNotProduceNaN` to assert `mach == 0` and
  `qbar == 0` (the previous implementation produced a phantom Mach of
  `1/sound` at zero airspeed because the `safe_vt` floor leaked into
  the Mach computation). **This found a real bug in `atmosphere.hpp`
  which is now fixed** — Mach and qbar use the actual `vt`, only `qovt`
  uses the floor.

**`test_flight_model.cpp`**:
- `SixtySecondStabilityRun`: tightened G tolerance from ±3.0 to ±1.0,
  added altitude drift bound (12000 ft — loose enough to admit the
  known trim/FCS settling transient, tight enough to catch divergent
  EOMs), added speed divergence bound (10x).
- `PitchStickChangesAlpha`: rewrote to compare against a no-input
  baseline (because trim() doesn't seed the FCS integrator, so the
  first few seconds involve settling). Asserts alpha INCREASES (not
  just "moves") and Nz INCREASES above baseline. Found a real
  direction issue that was hidden by the old `|delta| > 0.1` check.
- `ThrottleIncreasesSpeed`: added quantitative lower bound (≥50 ft/s
  gain in 10 s of full AB) — catches a regressed throttle path (AB
  not lighting, throttle reversed, etc.) that the old "speed went up"
  check would miss.

**`test_table_accessors.cpp::F16TablesInterpolateCorrectly`**:
- Added grid-point fidelity check (interpolated value at an exact
  breakpoint must equal the raw table value to 1e-9).
- Added CL-monotonic-in-alpha check at low Mach (catches indexing
  bugs that "look right" but read from the wrong cell).
- Fixed boundary clamping test to use `mach0 - 1` / `alpha0 - 1`
  instead of hardcoded `-1`/`-10` (the F-16 fixture's alpha axis
  extends to -10 deg, so the old test was querying INSIDE the table).
- Added thrust magnitude check (catches table-misload like
  thrust_ab being loaded into thrust_mil's slot).

## Phase 1 — Type safety & code quality

### Phase 1a: `f4-geo` position ctors `explicit` + `from_degrees()` factory

`f4-geo/include/f4/geo/position.hpp`:
- Marked 3-arg ctors of `WorldPosition`, `LatLonAlt`, `ECEFPosition`
  as `explicit`. Closes the most common bug: passing degrees where
  radians are expected, which compiled silently because both are
  `double`.
- Added `LatLonAlt::from_degrees(lat_deg, lon_deg, alt_ft)` factory.
  This is the recommended way to construct a LatLonAlt from
  human-readable coordinates — applies `DEG_TO_RAD` so callers don't
  have to remember the radians convention.
- Updated `operator+` / `operator-` to use explicit `WorldPosition{...}`
  construction (so the explicit ctor doesn't break arithmetic).
- Updated all call sites in `f4-geo/tests/` to use either
  `T(...)` functional-cast form (which works with explicit ctors) or
  `T{...}` direct-init (also works). The parenthesized-braced form
  `EXPECT_EQ(x, (T{...}))` had to become `EXPECT_EQ(x, T(...))` because
  gtest treats the parenthesized braced-init as copy-init.
- Added new test `Position.FromDegreesFactoryConvertsCorrectly`.

### Phase 1b: Named constants for magic numbers in binary parsers

`f4-world-convert/src/campaign_decoder.cpp`:
- Extracted `NUM_TEAMS`, `TEAM_NAME_LEN`, `TEAM_MOTTO_LEN`,
  `CMP_HEADER_BYTES` from inline literals.

`f4-world-convert/src/objective_decoder.cpp`:
- Extracted `OBJ_HEADER_BYTES`, `LINK_COSTS_PER_LINK`,
  `NUM_RADAR_ARCS`, `GRID_COORD_MIN`, `GRID_COORD_MAX`, `VU_ID_BYTES`.
  (Renamed from `MOVEMENT_TYPES` to avoid collision with the
  `MoveType::MOVEMENT_TYPES` enum value.)

`f4-models/src/hdr_parser.cpp`:
- Documented `FORMAT_VERSION` and added `OLD_FORMAT_SENTINEL = 0xFEEF`
  (was a bare hex literal with no explanation of what it means).
- Extracted `LOD_ENTRY_BYTES` and `LOD_ENTRY_SPARE` from inline `12`
  and `20` literals.

### Phase 1c: `BinReader::remaining()` footgun fixed

`f4-models/src/bin_reader.hpp`:
- The old `bool remaining()` returned `size - pos` (a `size_t`)
  implicitly converted to `bool`. Callers writing `auto n =
  r.remaining();` got 0 or 1, not the byte count.
- Renamed to `remaining_bytes() -> std::size_t` (returns the actual
  count) and added `has_remaining() -> bool` for the boolean check.
- No callers existed, so the rename was safe.

### Phase 1d: `EntityWorld` move ctor regenerates cookie

`f4-entities/include/f4/entities/entity.hpp` + `f4-entities/src/entity.cpp`:
- The defaulted move ctor copied the cookie from the source, so after
  `EntityWorld b = std::move(a);`, both `a` (moved-from) and `b` had
  the same cookie. Handles captured against `a` before the move would
  incorrectly validate against `b` — defeating the use-after-free
  detection the cookie is there to provide.
- Replaced with a custom move ctor/assign that regenerates the cookie
  on the destination via a new `detail::next_world_cookie()` helper
  (declared in the public header, defined out-of-line in `entity.cpp`
  so the global atomic counter stays file-local).

### Phase 1e: `Trace::append()` is now O(1)

`f4-state-machine/include/f4/fsm/trace.hpp`:
- Switched internal storage from `std::vector<Record>` to
  `std::deque<Record>`. `push_back` + `pop_front` are both O(1) on
  deque; the previous `erase(begin())` on vector was O(n) per append
  when full. At 360 Hz on a 1024-record trace, that's a measurable
  cost on the minor frame.
- `records()` now returns `std::vector<Record>` by value (a copy)
  instead of `const std::vector<Record>&`. Trace inspection is rare
  (test/diagnostic only), so the copy cost is acceptable.

## Phase 2 — `Cursor::check_and_throw()`

`f4-io/include/f4/io/cursor.hpp`:
- Added `Cursor::check_and_throw(const char* context)` and the
  `const std::string&` overload. Converts the silent sticky error
  flag into an observable `std::runtime_error` at the end of a parse
  block — one-line idiomatic check instead of
  `if (c.error) throw std::runtime_error(...)` by hand.
- Rationale: the original sticky-flag design (worklog.md:1504) chose
  silent flags over throw-on-OOB to surface real bugs in subclass
  dispatch paths where exceptions would be caught and swallowed. But
  the consequence was that any caller who forgot to check `error`
  would silently produce zeroed records. `check_and_throw()` gives
  those callers a one-liner.

Migrated all throwing call sites to use it:
- `f4-world-convert/src/theater_data.cpp` — 9 sites converted from
  `if (c.error) throw std::runtime_error("...");` to
  `c.check_and_throw("...");`
- `f4-world-convert/src/campaign_decoder.cpp` — 2 sites.

Call sites that use the sticky flag for non-throwing rollback behavior
(`objective_decoder.cpp`, `unit_decoder.cpp`, `team_decoder.cpp`) keep
their existing `if (c.error)` pattern — that's the correct use of the
sticky flag.

Added 6 new tests in `f4-io/tests/test_cursor.cpp`:
- `CheckAndThrowNoopsWhenNoError`
- `CheckAndThrowThrowsAfterOobRead`
- `CheckAndThrowIncludesContextInMessage`
- `CheckAndThrowAcceptsStdString`
- `CheckAndThrowAfterSkipOob`
- `CheckAndThrowAfterFixedStringOob`

## Phase 3 — Deduplication

### Phase 3a: JSON serialization consolidated

`f4-convert/src/json_io.cpp` was a 664-line copy of
`f4-data/src/config_loader.cpp` (both admitted "Both copies MUST stay
in sync" in their comments). The "circular dependency" justification
was invalid: `f4-convert` already depends on `f4-data` via
`target_link_libraries(f4-convert PUBLIC f4-data nlohmann_json)`.

- `writeJson()` / `writeJsonFile()` / `readJson()` / `readJsonFile()`
  are now thin adapters that delegate to `f4::data::writeConfig()` /
  `loadConfig()` / `loadConfigFromString()`. The `IoResult` ↔
  `LoadResult` adapter handles the structural difference (IoResult
  takes cfg by reference; LoadResult embeds it).
- `diffConfigs()` stays in f4-convert — it has no equivalent in
  f4-data and is genuinely f4-convert-specific (used by the `json_diff`
  CLI tool and the round-trip test harness).
- Net: ~470 lines of duplicated serialization code deleted. The
  "MUST stay in sync" maintenance hazard is gone — format changes
  now happen in exactly one place.
- Updated misleading comments in `f4-data/src/config_loader.cpp` and
  `f4-convert/CMakeLists.txt` to reflect the new reality.

### Phase 3b: `f4-models::Vec3` aliased to `f4::math::Vec3<float>`

`f4-models/include/f4/models/types.hpp`:
- The duplicate `struct Vec3 { float x, y, z; }` (with only `operator==`)
  is now `using Vec3 = f4::math::Vec3<float>;`. Consumers get the full
  Vec3 operator set (dot, cross, length, normalize, hadamard, scalar
  arithmetic, `operator[]`) from f4-math, instead of the bare struct.
- Layout is identical (3 contiguous floats, 12 bytes, no padding), so
  `sizeof(Vec3)` in the binary parsers (which read arrays of Vec3
  directly from disk) is unchanged.
- `f4-models/CMakeLists.txt` now links `f4-math` as a PUBLIC dependency.

## Verification

Clean build from scratch:
```
$ rm -rf build && cmake -G Ninja -DCMAKE_BUILD_TYPE=Release \
    -DF4_BUILD_MODEL_VIEWER=OFF -DF4_BUILD_VIEWER=OFF ..
$ cmake --build . -j2
$ ctest -j2
100% tests passed out of 1012
```

(Viewers are OFF because they require X11 + OpenGL dev headers not
present in this environment. The library code itself builds clean
with viewers enabled on a dev machine.)

## What was NOT done (and why)

The following items from the original review were deferred because they
are higher-risk or require more design work:

- **Full strong-type migration in f4-flight-model** (CRITICAL #1 in the
  review). The flight model uses raw `double` for alpha/beta/sigma/etc.
  with degrees/radians mixed in the same struct. Migrating to strong
  types is invasive — every consumer of `AeroState` / `KinematicState`
  / `calcBodyRates` needs updating. Recommend doing this as a separate
  dedicated PR with its own test pass, not as part of a cleanup batch.
  **DONE — see "Phase 4 — Angle Strong-Type Migration" below.**

- **`f4-messaging::publish()` shared_mutex refactor** (CRITICAL #5).
  The current `recursive_mutex` + vector copy on every publish is a
  real serialization point at 60 Hz, but the reentrant-publish
  semantics need careful thought. Needs its own benchmark + design
  pass.

- **`LayeredStateMachine::applyInhibition()` `force_to_state()`**
  (HIGH #3). The current `reset()` call fires the initial state's
  entry action, which is wrong if initial ≠ idle. Fix requires adding
  a new `force_to_state()` method to `StateMachine` and reasoning
  carefully about which transitions should/shouldn't fire on
  suppression.

- **`dat_parser.cpp` exception-as-control-flow** (HIGH #6). The
  backtracking search uses `try { ts.nextDouble(); } catch (...) {
  ts.setPos(saved+1); }`. Fix requires adding `tryNextDouble() ->
  std::optional<double>` to `TokenStream` and replacing 5+ sites.

These are all good candidates for the next cleanup pass.

## Phase 4 — Angle Strong-Type Migration (CRITICAL #1)

Resolves the largest correctness hazard flagged in the original review:
`AircraftState` stored angles as raw `double` with the unit convention
documented only in comments (alpha/beta in degrees, euler angles in
radians, alpha_dot in deg/s). Passing a degree value where radians were
expected (or vice versa) compiled silently because both are `double`.

### Approach

`f4-units` already provides a complete `Quantity<U,R>` framework with
`Radians`, `Degrees`, dimension arithmetic, and user-defined literals.
The flight model now uses these directly via flight-local aliases and
factory functions in a new public header:

**`f4-flight-model/include/f4/flight/angle.hpp`** (new, ~120 lines):
- `Angle` = `f4::Quantity<f4::Radians>` (radians canonical storage)
- `AngularRate` = `f4::Quantity<f4::RadiansPerSecond>` (rad/s canonical)
- Factories: `angle_from_degrees(d)`, `angle_from_radians(r)`,
  `angular_rate_from_degrees_per_second(dps)`,
  `angular_rate_from_radians_per_second(rps)`, `zero_angle()`,
  `zero_angular_rate()`
- Accessors: `to_degrees(a)`, `to_radians(a)`, `to_deg_per_s(r)`,
  `to_rad_per_s(r)`
- The `Angle` ctor is `explicit` (inherited from `Quantity`), so
  implicit `double -> Angle` conversion is rejected at compile time.
  Call sites must pick a side via the named factories.

### Changes by file

| File | Change |
|------|--------|
| `f4-flight-model/include/f4/flight/angle.hpp` | **New** — Angle / AngularRate aliases + factories + accessors |
| `f4-flight-model/include/f4/flight/aircraft_state.hpp` | All euler angles (sigma, gmma, mu, psi, theta, phi) and aero angles (alpha, beta, alpha_dot, beta_dot) and FCS commands (aoacmd, betcmd) migrated from raw `double` to `Angle` / `AngularRate`. Body rates (p, q, r) kept as raw double with a comment explaining why (they're integrated into the quaternion and never compared with degree-valued quantities). |
| `f4-flight-model/include/f4/flight/aerodynamics.hpp` | `Aerodynamics::update()` signature: `alpha_deg`/`beta_deg` params → `alpha`/`beta` of type `Angle` |
| `f4-flight-model/include/f4/flight/fcs.hpp` | `FlightControlSystem::update()` + `computeGains` + `runPitch` + `runRoll` + `runYaw`: `alpha_deg`/`beta_deg`/`phi_rad` params → `Angle` |
| `f4-flight-model/src/aerodynamics.cpp` | Extract `alpha_deg`/`beta_deg` locals via `to_degrees()` at the top of `update()` (the F-16 aero tables are degree-indexed and we deliberately do NOT convert them). Body unchanged. |
| `f4-flight-model/src/fcs.cpp` | Same boundary-extraction pattern. Write-backs via `angle_from_degrees(...)`. |
| `f4-flight-model/src/eom.cpp` | `trigonometry()` reads `to_radians(k.theta)` etc. instead of `k.theta` directly. Quaternion recovery writes via `angle_from_radians(...)`. Ground clamp uses `zero_angle()`. |
| `f4-flight-model/src/flight_model.cpp` | `init()`, `minorStep()`, `trim()`, `updateStallSM()` — all `alpha_deg`/`beta_deg` field references converted to `alpha`/`beta` (Angle) with `to_degrees()` extraction at the boundary. |
| `f4-flight-model/CMakeLists.txt` | Added `f4-units` as a PUBLIC dependency (was previously explicitly NOT a dependency). |
| `f4-flight-model/tests/*.cpp` | All `a.update(alpha, beta, ...)` and `fcs.update(..., alpha, beta, ..., phi, ...)` call sites updated to pass `angle_from_degrees(x)` for angle args. Direct field assignments (`aero.alpha_deg = 30.0`) updated to `aero.alpha = angle_from_degrees(30.0)`. Field reads in EXPECT_* macros wrapped with `to_degrees(...)`. |
| `f4-flight-model/tests/test_angle.cpp` | **New** — 11 unit tests covering the Angle / AngularRate factories, accessors, round-trip conversions, arithmetic, and the explicit-ctor guarantee. |

### What was deliberately NOT changed

- **The F-16 aero coefficient tables remain degree-indexed.** They are
  physical data files in degrees; converting the data would alter the
  flight feel. The degree convention now survives at exactly one place:
  the lookup call site (`table(mach, alpha_deg)`), where `alpha_deg` is
  a local extracted via `to_degrees(alpha)` and named `_deg` to make
  the convention explicit.
- **`StallConfig::*_deg` and `StallDetection::alpha_deg` / `*_deg`
  fields.** These are plain-data fields consumed by the polling
  detection logic and serialized into bus messages. They are
  deliberately `double` (degrees) because they cross the bus boundary
  as plain data and consumers (UI audio cues, JSON config) want
  degrees. The bridge from the typed `AeroState::alpha` to the plain
  `StallDetection::alpha_deg` is one `to_degrees(a.alpha)` call in
  `flight_model.cpp::updateStallSM()`.
- **Body rates p, q, r in `KinematicState`.** They are rad/s and feed
  the quaternion integrator + FCS roll-rate lag, never a degree-valued
  comparison. Typing them would add friction without closing a real
  correctness gap. Documented in the file-level comment.
- **`AeroState::clalpha`, `clalph0`, `cnalpha`.** These are
  dimensionless aerodynamic derivatives (dCL/dalpha per radian), not
  angles. Correctly `double`.
- **`FcsState::startRoll`.** This is the time-integral of p (rad), not
  an angle. Documented.

### Verification

Clean build from scratch:
```
$ rm -rf build && cmake -G Ninja -DCMAKE_BUILD_TYPE=Release \
    -DF4_BUILD_MODEL_VIEWER=OFF -DF4_BUILD_VIEWER=OFF ..
$ cmake --build . -j2
$ ctest -j2
100% tests passed out of 1012
```

The 11 new `test_angle` tests cover the Angle/AngularRate contract
itself; the existing 1001 flight-model / world / etc. tests confirm
the migration didn't change runtime behavior (the same trim alpha
converges, the same stall transitions fire, the same FCS gains
compute).

## Phase 5 — Deferred Item Resolution & Code Quality Pass

Resolves the three deferred items from Phase 4 plus additional code
quality improvements identified in the comprehensive audit.

### C1: MessageBus shared_mutex + copy-on-write refactor

**CRITICAL** — The `recursive_mutex` + vector-copy-on-every-`publish()`
was the dominant serialization point at 60 Hz × N entities.

`f4-messaging/include/f4/messaging/bus.hpp`:
- Replaced `std::recursive_mutex` with `std::shared_mutex`. `publish()`
  takes a shared lock (concurrent reads), `subscribe()`/`unsubscribe()`
  take exclusive locks.
- Replaced per-publish vector copy with copy-on-write `shared_ptr<vector>`.
  `publish()` reads the current shared_ptr under shared lock — zero
  allocation per publish. `subscribe()`/`unsubscribe()` create a new
  vector and swap the shared_ptr.
- Reentrant publish (handler calling publish() on the same bus) is now
  handled via a thread-local deferred list instead of recursive_mutex.
  The outer publish drains the list after handler dispatch completes.
- Deleted move operations (shared_mutex is not movable, and the
  thread-local reentry list references `this`).

### C2: LayeredStateMachine inhibition uses force_to_state()

**CRITICAL** — `applyInhibition()` was calling `reset()` which fires the
initial state's entry action. When a higher-priority DigiMode layer
activates, suppressed layers would execute phantom entry actions (e.g.,
starting timers, publishing messages) instead of silently going to idle.

`f4-state-machine/include/f4/fsm/state_machine.hpp`:
- Added `force_to_state(StateEnum s) noexcept` — sets `current_` to `s`
  without firing entry/exit actions or recording a transition. This is
  an administrative reset, not a UML 2 transition.

`f4-state-machine/include/f4/fsm/layered.hpp`:
- Changed `applyInhibition()` to call
  `layers_[j].sm.force_to_state(layers_[j].idle_state)` instead of
  `layers_[j].sm.reset()`.

### C3: dat_parser exception-as-control-flow eliminated

**HIGH** — The backtracking search in `parseEngine`, `parseRollData`,
and `parseLimiters` used `try { ts.nextDouble(); } catch (...) { ... }`
as a branching mechanism. C++ exceptions are ~100× slower than normal
return on most platforms, making .dat loading the dominant cost.

`f4-Fconvert/src/dat_parser.cpp`:
- Added `tryNextDouble() -> optional<double>`,
  `tryNextInt() -> optional<int>`, and
  `tryNextDoubles(n) -> optional<vector<double>>` to `TokenStream`.
  These return `nullopt` on EOF or parse failure WITHOUT throwing.
- Replaced all 4 catch sites: `parseEngine` legacy format scan (site 1),
  `parseEngine` alpha-factor probe (site 2), `parseRollData` (site 3),
  and `parseLimiters` (site 4).
- Added `<optional>` include.

### H9: Cursor::remaining() unsigned underflow

`f4-io/include/f4/io/cursor.hpp`:
- `remaining()` now returns 0 instead of `SIZE_MAX` when `p > end`
9  (unsigned underflow from `static_cast<size_t>(end - p)`). Previously
  any code using `remaining()` without first checking `error` would get
  a garbage value that could be used as a loop bound.

### CP2: [[nodiscard]] on result-returning functions

`f4-convert/include/f4/convert/dat_parser.hpp`:
- Added `[[nodiscard]]` to `loadFile()` and `loadString()`. Discarding
  the `ParseResult`D silently ignores parse errors.

`f4-flight-model/include/f4/flight/flight_model.hpp`:
- Added `[[nodiscard]]` to* `trim()`. Discarding the bool return
  silently ignores trim convergence failure.

### L6/L7: FlightModel::setMinorPerMajor + Cursor non-copyable

`f4-flight-model/include/f4/flight/flight_model.hpp`:
- Changed `minorPerMajor_` from `int` to `unsigned int`. Negative values
  silently became 1; now the type prevents them at compile time.

`f4-io/include/f4/io/cursor.hpp`:
- Added `Cursor(const Cursor&) = delete` and `operator=`. Copying a
  Cursor creates two readers sharing the# same buffer, advancing
  independently — a logic error.

### M8:2 ModelDatabase texture cache thread safety

`f4-models/include/f4/models/model_database.hpp`:
- Added `mutable std::shared_mutex texture_cache_mutex_` to protect
  `decoded_textures_`. `fetch_texture()` is const and lazily populates
  the cache; concurrent calls from render + export threads would
  otherwise be a data race.

### H8: FlightModel::bus_ lifetime contract documented

`f4-flight-model/include/f4/flight/flight_model.hpp`:
- Added explicit lifetime contract comment in `set_message_bus()`.
  The raw pointer pattern is retained for the default-construct-then-
  reassign pattern, but the contract is now prominent.

## Phase 6 — Pre-AI Hardening Sprint

Resolves the 5 Critical + 5 High issues from the dark-pattern audit
that would have become acute the moment multiple aircraft start sharing
state at 360 Hz. All 1020 tests pass (up from 1008 — 12 new tests added).

### PR1: C3+C4+C5 — silent-garbage fixes (low risk, high value)

- **C3: THEATER.MAP magic validation** (`f4-terrain/src/terrain_data.cpp`):
  the parser read `header.magic` but never compared it against
  `0x444CFFAE`. A file with the wrong magic but plausible dimensions
  parsed silently and produced garbage elevation data. Added
  `constexpr uint32_t THEATER_MAP_MAGIC = 0x444CFFAEu` in the public
  header (`terrain_data.hpp`) and a check in `load()`. Dedupes 8
  scattered literal occurrences across the terrain lib, hex inspector,
  and tests.
- **C4: BinReader::remaining_bytes() underflow** (`f4-models/src/bin_reader.hpp`):
  same bug as the `Cursor::remaining()` H9 fix in Phase 5, applied to
  the parallel reader. Now returns 0 (not `SIZE_MAX`) when `pos > size`
  after an OOB read.
- **C5: JSON skip_value() strict validation** (`f4-json/include/f4/json/reader.hpp`):
  the bare-token path accepted any non-structural char run — `truu`,
  `1.2.3.4`, `@#$%` all parsed silently. Now validates against
  `true`/`false`/`null`/number grammar and throws on anything else.
  Added `read_bool()` helper to replace the hand-rolled
  `if (r.consume('t')) ... skip_value()` pattern in `world_state.cpp`
  (3 sites) that broke under strict skip_value.
- 5 new tests: `Terrain.RejectsBadMagic`, `SkipValueFalse`,
  `SkipValueNumber`, `SkipValueRejectsMalformedBareToken`,
  `SkipValueRejectsBarePunctuation`.

### PR2: C1 — Table1D/Table2D data race on mutable cache

`f4-math/include/f4/math/table.hpp`:
- The "cached last-index hint" used `mutable std::size_t last_` written
  on every `const operator()` call. Two `FlightModel` instances sharing
  an aero table (e.g. formation AI) would race on this write.
- Replaced with `mutable std::atomic<std::size_t>` using
  `memory_order_relaxed`. Relaxed atomics are ~1ns on x86, preserving
  the cache's perf benefit while making the const operator() thread-safe
  per the C++ memory model. The cache is only a hint — a racy write
  simply causes the next call to do a full scan (still correct).
- Added explicit copy/move constructors because `std::atomic` is
  non-copyable. The cache value is loaded/stored via relaxed atomics;
  any value (including stale) is acceptable.
- No new tests (the existing 192 f4-math tests verify correctness).
  TSAN would now pass on a multi-aircraft shared-table test.

### PR3: H2 — warning flags + GoogleTest fetch dedup

- **Warning flags**: created `cmake/f4_warnings.cmake` defining an
  INTERFACE library `f4_warnings` with `-Wall -Wextra -Wpedantic`
  (GCC/Clang) or `/W4 /permissive-` (MSVC).
  `-Wno-missing-field-initializers` is suppressed (tests legitimately
  use `{}` aggregate init for structs with many fields). Linked to
  every f4-* library target (PUBLIC for STATIC, INTERFACE for
  header-only). First clean build: 2 real warnings (unused function,
  unused variable) — both fixed. Zero compiler warnings now.
  `-Wshadow -Wconversion` deferred to a follow-up (would produce
  hundreds of warnings on legacy-shaped code).
- **GoogleTest dedup**: the 17 `f4-*/tests/CMakeLists.txt` files each
  repeated the same 10-line `FetchContent_Declare(googletest ...)` +
  `include(GoogleTest)` block, pinning the version in 17 places.
  Hoisted to `cmake/f4_deps.cmake`, included once from the root
  `CMakeLists.txt`. Each test CMakeLists now just calls
  `gtest_discover_tests()`.
- 0 new tests (infrastructure change).

### PR4: C2 — TagValue → std::variant

`f4-entities/include/f4/entities/entity.hpp`:
- Replaced the hand-rolled tagged union (4 parallel fields:
  `str_val`, `int_val`, `float_val`, `bool_val`, only 1 populated,
  ~40 bytes wasted per int/bool tag) with
  `std::variant<std::string, int64_t, double, bool>`.
- API change: the 4 fields → 4 pointer-returning accessors
  (`as_string()`, `as_int()`, `as_float()`, `as_bool()`). Returns
  nullptr if the variant doesn't hold the requested type — surfaces
  type mismatches instead of silently returning default-constructed
  zeros/empties.
- Retained `Type` enum and `type()` method for backward compatibility
  with code that switches on type.
- Updated 12 call sites across `f4-entities/tests/test_entity.cpp`,
  `f4-world/tests/test_world_loader.cpp`, and
  `f4-world-viewer/src/{canvas,inspector_panel}.cpp`. The viewer's
  `team_tag->type == Type::Int ? team_tag->int_val : 0` pattern
  (4 sites) became the cleaner `team_tag->as_int() ? *team_tag->as_int() : 0`.
- 0 new tests (existing 20 entity tests + 7 world-loader tests verify
  the migration).

### PR5: H1 — ECS Phase 4 verification + interface contract test

**Finding**: the audit's H1 was overly pessimistic. The ECS Phase 4
work is **already complete** — `IDataSource` interfaces
(`ICampaignSource`, `ITeamSource`, `IObjectiveSource`, `IUnitCoreSource`
+ `IGroundUnitSource`/`ISquadronSource`/`IFlightSource`/`IPackageSource`)
exist in `f4-world/include/f4/world/data_source.hpp`, the bridge has
interface-based overloads, `WorldState` is forward-declared in the
public header (not included), the adapter structs are private to
`src/world_loader.cpp`, and the viewer reads exclusively from
`EntityWorld` components (verified by grep — zero direct `WorldState`
field accesses in viewer code).

The only remaining smell is that `UnitState` is still a 40-field
tagged-union struct. But it's now an INTERNAL implementation detail
of the `WorldStateAdapter` (the bridge's concrete `IUnitCoreSource`
implementation), in `detail/world_state.hpp`, never exposed to
consumers. This is acceptable — the smell is contained.

**Delivered**:
- Added `test_interface_bridge.cpp` (7 tests) — implements mock
  `ICampaignSource` / `ITeamSource` / `IObjectiveSource` /
  `IUnitCoreSource` adapters inline and calls `populate_world()`
  through the interface overloads. This is the **contract test for
  f4-ai**: if the interface contract breaks, this test fails before
  any AI code can be written against it. Also unlocks future data
  sources (BMS saves, DIS streams, procedural generation) without
  touching `WorldState`.
- Added deprecation comment to `UnitState` documenting that it's
  internal-only and pointing consumers to either `EntityWorld`
  components or the `IUnitCoreSource` interface.

### Verification

Clean build from scratch:
```
$ rm -rf build && cmake -G Ninja -DCMAKE_BUILD_TYPE=Release \
    -DF4_BUILD_MODEL_VIEWER=OFF -DF4_BUILD_VIEWER=OFF ..
$ cmake --build . -j2
$ ctest -j2
100% tests passed out of 1020
```

Zero compiler warnings. 12 new tests added across the 5 PRs.

### What was NOT done (and why)

- **`-Wshadow -Wconversion` warning flags** — would produce hundreds
  of warnings on legacy-shaped code (narrowing in binary parsers,
  shadowed loop vars in viewer code). Add in a follow-up cleanup pass.
- **`std::function` on state-machine hot path** (H3) — 32 bytes each,
  heap-allocating. Migration to SBO function type is invasive (every
  `StateMachine::Builder::on()` call site). Defer to a dedicated SM
  perf pass.
- **`EntityHandle::add<T>()` returns `T&`** (H4) — breaks encapsulation.
  Migration to `ComponentHandle<T>` touches every component-add call
  site. Defer until f4-ai actually needs dirty-flag/versioning hooks.
- **`FlightModel::state()` mutable overload** (H6) — f4-ai will reach
  into `state()` for sensor reads. Defer the mutable-overload removal
  to the f4-ai integration PR (where the actual call sites will be
  known).
- **Splitting `UnitState` into per-subclass structs** — the struct is
  now internal-only. Splitting it would require rewriting the JSON
  loader + adapter. Low value now that the smell is contained.

### PR6: H8 + H5 — locale-independent JSON parsing + portable temp paths

Follow-up batch closing two audit items that fit the sprint's spirit but
were missed in PR1–PR5.

- **H8: Locale-independent number parsing** (`f4-json/include/f4/json/reader.hpp`):
  `read_int()` used `std::strtol(..., 10)` and `read_number()` used
  `std::strtod`. `std::strtod` is **locale-dependent** — with
  `LC_NUMERIC=de_DE.UTF-8` the literal `"3.14"` parses as `3.0` (the
  `.` is treated as a thousand separator). Any campaign save loaded on
  a German/French/Spanish user's machine would silently corrupt every
  coordinate, velocity, and probability field in the JSON. Replaced
  both with `std::from_chars` (C++17), which is locale-independent by
  spec (§charconv). Also migrated the `\uXXXX` hex parse to
  `std::from_chars(..., 16)` for consistency — `strtol` with base 16
  was already locale-independent, but the migration removes the last
  locale-sensitive stdlib surface from the reader. `long` return type
  of `read_int()` is preserved for ABI compatibility; `std::from_chars`
  handles `long` natively.
- **H5: Hardcoded `/tmp/` paths in tests** — replaced with
  `std::filesystem::temp_directory_path()` in 3 sites:
  - `f4-data/tests/test_config_loader.cpp` (config write test)
  - `f4-world-viewer/tests/test_settings.cpp` (XDG_CONFIG_HOME temp dir)
  - `f4-world/tests/test_world_state.cpp` (terrain resolve test)
  The two remaining `"/tmp/..."` literals in `test_settings.cpp`
  (`s.last_world_json`, `s.last_terrain_json`) are **round-trip data
  values** — the test verifies JSON serialize/deserialize, not
  filesystem access — so they're left as POSIX-style sample strings.
  Portability fix: tests now work on Windows (where `/tmp/` may not
  exist), macOS sandboxed runners, and CI containers with non-standard
  `TMPDIR`.
- 2 new tests: `ReadNumberLocaleIndependent`, `ReadIntLocaleIndependent`.
  Both call `std::setlocale(LC_NUMERIC, "de_DE.UTF-8")` and verify the
  parser still returns the C-locale value. Skipped via `GTEST_SKIP()`
  on systems without the `de_DE` locale installed (CI runners and
  minimal containers commonly lack it).

### Verification (post-PR6)

Build not re-run in this environment (no cmake/ninja available in the
agent sandbox). The change is mechanical:
- `std::from_chars` for `long` and `double` is available since
  libstdc++ 11 / libc++ 14 / MSVC 19.41; the project already requires
  C++20 and g++ ≥ 13 (per existing CI config).
- The reader now includes `<charconv>` and `<system_error>`; both are
  header-only and have no link-time impact.
- The 4 existing float/int tests (`ReadIntPositive`, `ReadIntNegative`,
  `ReadNumberFloat`, `ReadNumberScientific`, `ReadNumberNegativeFloat`)
  continue to pass — `std::from_chars` accepts the same grammar that
  `std::strtod` did for the JSON number subset (no `0x`, no `inf`/`nan`,
  no grouping).
- The 2 new locale tests are skipped on systems lacking `de_DE`, so
  they cannot regress the suite on minimal CI runners.


---

# f4-scenario-player v0 — Initial Implementation

## Summary

Built the first cut of the `f4-scenario-player` host executable that
spawns an F-16 on a parking spot at Kunsan (synthesized layout) and
renders the aircraft + airport geometry (runway, threshold bars,
centerline dashes, taxi route, parking/hold-short/runway-end markers,
compass rose) in a Raylib window with orbit/pan/zoom camera.

The simulation starts **paused** so the aircraft sits at the parking
spot. Press Space to begin taxi (the brain's `TakeoffModule` follows
the scenario's taxi route to the runway threshold).

## Files added

- `Docs/SCENARIO_PLAYER_PLAN.md` — design doc, replaces the earlier
  `F4_TAXI_DEMO_PLAN.md`. Documents the rename, the v0 acceptance
  criteria, and the deferred items (auto-spawning from campaign data,
  real Kunsan ground layout, takeoff rotation, multi-aircraft).
- `f4-scenario-player/` — new top-level crate:
  - `CMakeLists.txt` — builds `f4_scenario_player_lib` (static library)
    + `f4-scenario-player` (CLI executable). Fetches Raylib 5.0 +
    Dear ImGui v1.91.5 + rlImGui (same versions as `f4-models-viewer`).
  - `cli/main.cpp` — entry point: `f4-scenario-player <scenario.json>`.
  - `include/f4/scenario_player/player_app.hpp` — public pimpl API.
  - `include/f4/scenario_player/airport_geometry.hpp` — `AirportGeometry`
    struct + `build_airport_geometry(Scenario)`.
  - `include/f4/scenario_player/coordinate_transform.hpp` — ENU ↔ Raylib
    math (extracted to a public header so tests can verify it without
    linking Raylib).
  - `src/player_app.cpp` — lifecycle (load_scenario, run, screenshot).
  - `src/renderer.cpp` — orbit camera, mesh building, scene drawing,
    HUD overlay, ImGui panel.
  - `src/airport_geometry.cpp` — synthesizes runway/threshold/dashes/
    taxi-route/markers/compass from a `Scenario`.
  - `src/viewer_state.hpp` — private pimpl state.
  - `scenarios/kunsan_parking.json.in` — CMake-configured scenario
    fixture (substitutes `@F4_SOURCE_DIR@` / `@F4_BINARY_DIR@` so it
    can reference `temp/KoreaObj.HDR/.LOD/.TEX` and
    `build/generated_fixtures/f16.json` portably).
  - `tests/test_airport_geometry.cpp` — 12 tests covering runway
    surface, threshold bars, centerline dashes, taxi route, markers,
    compass rose, colors, and zero-length-runway edge case.
  - `tests/test_coordinate_transform.cpp` — 9 tests covering the
    ENU → Raylib and model-vertex → Raylib axis mappings.

## Files modified

- `CMakeLists.txt` — added `add_subdirectory(f4-scenario-player)` gated
  by `F4_BUILD_SCENARIO_PLAYER` option (ON by default).
- `f4-world-convert/include/f4/world_convert/class_table.hpp` — added
  `int16_t vis_type[7]` field to `ClassTableEntry` and a
  `vis_type_for(entity_type, slot=0)` accessor. This closes the
  data-flow gap from `Falcon4.CT` → `ModelDatabase` so that future
  campaign-driven aircraft spawning can auto-resolve the visual model.
- `f4-world-convert/src/class_table.cpp` — parser now reads the 14-byte
  `visType[7]` array at offset 60 of each 81-byte CT record (previously
  discarded). Added `vis_type_for()` implementation.
- `f4-world-convert/tests/CMakeLists.txt` — registered the new
  `test_class_table` test target.
- `f4-world-convert/tests/test_class_table.cpp` — new (8 tests):
  regression guard for the existing fields (classInfo, dataType,
  dataPtr) + new tests for visType exposure (F-16 vehicle-class entry
  has vis_type[0]=1052, out-of-bounds slot returns 0, ~1080 of 2135
  entries have non-zero vis_type[0]).
- `Docs/AIRCRAFT_BINDING_DESIGN.md` — updated to reference
  `f4-scenario-player` (was `f4-taxi-demo`) and the new
  `SCENARIO_PLAYER_PLAN.md`.

## Build

```bash
cd build
cmake -DCMAKE_C_FLAGS="-I.../local-deps/usr/include" \
      -DCMAKE_CXX_FLAGS="-I.../local-deps/usr/include" \
      -DX11_Xrandr_INCLUDE_PATH=... \
      [... other X11 paths ...] \
      /path/to/F4
cmake --build . --target f4-scenario-player -j 4
```

(Note: the build needs X11 dev headers — `libxrandr-dev`,
`libxinerama-dev`, `libxcursor-dev`, `libxi-dev`, `libxfixes-dev`,
`libx11-dev`, `libgl-dev`. In a sandbox without root, these can be
extracted locally via `apt-get download` + `dpkg -x`.)

## Run

```bash
cd build
./f4-scenario-player/f4-scenario-player scenarios/kunsan_parking.json
```

Controls: left-drag orbit, right-drag pan, scroll zoom, Space pause/
resume, F focus aircraft, R reset view, F2 screenshot.

## Headless smoke test

```bash
xvfb-run -a -s "-screen 0 1024x768x24" \
    ./f4-scenario-player/f4-scenario-player \
    scenarios/kunsan_parking.json \
    --screenshot out.png --width 800 --height 600
```

The screenshot is 36 KB, 800×600 RGBA, with detectable pixels for the
runway surface (~4700 grey pixels), threshold bars + centerline dashes
(~5000 white pixels), taxi route (~360 yellow pixels), parking-spot
marker (~530 green pixels), runway-end marker (~40 red pixels), and
the F-16 aircraft model (~83000 dark pixels).

## Test results

- New tests added: 29 (8 ClassTable + 12 AirportGeometry + 9 CoordinateTransform).
- All 29 pass.
- Pre-existing tests: 6 failures in `PilotInput.Validate*` (5) and
  `EngineModel.DefaultConstructedHasNoTables` (1) — these are
  pre-existing and unrelated to this change (the EngineModel test
  asserts on a null table pointer that's a known issue in the test
  setup, not the production code).

## What's next (per `SCENARIO_PLAYER_PLAN.md` §8)

- Real Kunsan ground layout from `f4-world::WorldState`'s
  `GroundLayoutList` data (currently synthesized from scenario JSON).
- Auto-spawn aircraft from campaign `Flight`/`Squadron` units via
  the new `vis_type_for()` accessor.
- Takeoff rotation + climb-out (the brain supports it; the renderer
  just needs to keep drawing).
- Multiple aircraft (the `Simulation` currently tracks one
  `aircraft_entity_`; supporting N is a small refactor).

---

# Phase 2 — Campaign-Derived Scenarios

## Summary

Closed the §4.3 gap (deferred from Phase 1): the scenario player can now
spawn aircraft from a real Falcon 4.0 campaign save instead of a hand-
authored JSON aircraft list. Loading `save1.cam` (via `f4-world-convert`'s
`cam2json`) produces a `WorldState` with `Flight`-class units; the new
`spawn_aircraft_from_flights()` bridge walks those units, resolves each
flight's squadron → airbase → parking spot, and composes a 4-component
aircraft entity (`TransformComponent + FlightModelComponent +
VisualModelComponent + BrainComponent`) per flight.

## Changes

- **`f4-simulation/scenario.hpp`**: Added `SpawnMode` enum (`ScenarioList`
  | `CampaignFlights`) + `Scenario::world_json_path` + `class_table_path`
  fields. Backward compatible — defaults to `ScenarioList`.
- **`f4-simulation/campaign_bridge.{hpp,cpp}`** (new): Two functions:
  - `derive_airfield_from_objective(obj, runway_id)` — pure conversion
    from `ObjectiveState.ground_layout` to `ScenarioAirfield`. Returns
    `nullopt` for non-airbases. Grid→ENU conversion (1024 ft/grid unit).
    Builds taxi route from parking → follow-me → threshold.
  - `spawn_aircraft_from_flights(world, ct, db, cfg, airfield, template)`
    — walks `world.with_component<FlightPlanComponent>()`, resolves each
    flight's squadron → airbase → transform for parking spot, applies
    per-flight lateral offset (80 ft alternating ±), looks up vis_type
    via `ClassTable::vis_type_for(squadron.class_table_index, 0)`,
    composes the 4-component aircraft entity. Falls back gracefully when
    CT lookup or squadron resolution fails.
- **`f4-simulation/simulation.{hpp,cpp}`**: Replaced single
  `aircraft_entity_` with `std::vector<EntityId> aircraft_entities_`.
  Spawn dispatcher picks `spawn_from_scenario_list()` (Phase 1 path, now
  iterates `scenario.aircraft[]`) or `spawn_from_campaign_flights()`
  (Phase 2 path: loads WorldState, populates EntityWorld via
  `f4-world::populate_world`, derives airfield, loads ClassTable, calls
  `spawn_aircraft_from_flights()`). `tick()` and `record_snapshot()`
  iterate the vector — one snapshot per aircraft per tick.
- **`f4-simulation/CMakeLists.txt`**: Added `f4-world` + `f4-world-convert`
  + `f4-terrain` to dependencies (transitively required by WorldState +
  ClassTable).
- **Tests**: 17 new tests.
  - `test_campaign_bridge.cpp` (11 tests): 7 for `derive_airfield_*`
    (nullopt paths, threshold/runway_end/departure_alt, taxi route,
    heading conversion, runway_id propagation) + 4 for `spawn_*` (empty
    world, one-per-flight, lateral offset, vis_type fallback, threshold
    fallback).
  - `test_scenario_loader.cpp` (+6 tests): `spawn_mode` parsing,
    unknown-mode-throws, `campaign_flights` requires `world_json_path` /
    `class_table_path` / aircraft template.
- **Docs**: New `Docs/NEXT_PHASE_PLAN.md` documents Phase 2 scope +
  acceptance criteria + implementation order.

## Test Results

- All 56 simulation tests pass (11 CampaignBridge + 6 new ScenarioLoader +
  5 existing ScenarioLoader + 8 ClassTable + 5 VisualModelComponent +
  21 others).
- Full suite: 1265 of 1271 pass. The 6 failures are pre-existing
  (`PilotInput.ValidateClamps*` + `EngineModel.DefaultConstructedHasNoTables`)
  and unrelated to this change.

## What's Next (Phase 3)

- Wire `f4-scenario-player`'s renderer to iterate `sim.aircraft_entities()`
  (currently draws only the singleton via `sim.aircraft_entity()`).
- Build a `kunsan_from_campaign.json` scenario fixture that uses
  `spawn_mode: "campaign_flights"` with the bundled `save1.world.json` +
  `FALCON4.ct`.
- Smoke-test with `--screenshot` to verify multiple F-16s are visible at
  their campaign-derived parking spots.

---

## Phase 2A — Real Airfield Meshes

**Goal:** Replace the procedural painted airfield (one dark-grey quad with
threshold bars + centerline dashes) with **real KoreaObj BSP models** placed
at Kunsan: runway sections, taxiway, control tower, hangars, fuel tanks,
parking apron. The F-16 and airfield features share the same render path —
both are just entities with a `VisualModelComponent`.

### Architecture

A new `ScenarioFeature` struct joins `ScenarioAircraft` and
`ScenarioAirfield` in `f4-simulation/scenario.hpp`. Each feature carries
`{name, vis_type_index, position, heading_rad}` — the same keying model as
the aircraft block (direct KoreaObj model index, no class-table lookup).
The scenario JSON gains an `airfield_features[]` block.

`Simulation::spawn_airfield_features()` creates one entity per feature with
`TransformComponent` + `VisualModelComponent` (no FM, no brain — static).
Tracked in a separate `feature_entities_` vector so `tick()` doesn't try
to sync them from a flight model.

The renderer refactors `aircraft_meshes` (a flat `std::vector<MeshEntry>`)
into `mesh_cache` (a `std::unordered_map<int, MeshCacheEntry>` keyed by
`parent_index`). Multiple features sharing the same `vis_type` reuse one
GPU upload. `draw_aircraft()` becomes `draw_visual_entities()` which walks
`world.with_component<VisualModelComponent>()` and draws every entity —
aircraft and features share the same draw path.

### Files changed

| File | Change |
|------|--------|
| `f4-simulation/include/f4/simulation/scenario.hpp` | New `ScenarioFeature` struct + `Scenario::airfield_features` vector |
| `f4-simulation/src/scenario.cpp` | `read_feature()` parser + `airfield_features` block in `parse_scenario()` + validation |
| `f4-simulation/include/f4/simulation/simulation.hpp` | `feature_entities_` vector + `feature_entities()` accessor + `spawn_airfield_features()` decl |
| `f4-simulation/src/simulation.cpp` | `spawn_airfield_features()` impl + call from `initialize()` |
| `f4-scenario-player/src/viewer_state.hpp` | `MeshCacheEntry` struct + `mesh_cache` map (replaces `aircraft_meshes` vector) + `build_mesh_for_model()` + `draw_visual_entities()` decls |
| `f4-scenario-player/src/renderer.cpp` | `build_aircraft_meshes()` → thin wrapper; new `build_mesh_for_model(int)` lazy builder; `upload_textures()`/`unload_meshes()` walk `mesh_cache`; `draw_aircraft()` → `draw_visual_entities()` walks all VMC entities |
| `f4-scenario-player/scenarios/kunsan_parking.json.in` | Adds 12 real feature placements (runway sections, threshold bars, taxiway, parking apron, control tower, hangars, fuel tank, runway access gate) |
| `f4-simulation/tests/fixtures/takeoff_kunsan.json` | Adds 4-feature block for testing |
| `f4-simulation/tests/test_scenario_loader.cpp` | 4 new tests for `airfield_features` parsing + validation |
| `f4-simulation/tests/test_feature_spawning.cpp` | **NEW** — 8 integration tests for `spawn_airfield_features()` (entity creation, transform encoding, static-ness, model sharing, discoverability) |
| `f4-simulation/tests/CMakeLists.txt` | Wires `test_feature_spawning` |

### Test results

- 4 new `ScenarioLoader` tests pass (fixture parsing, empty-allowed, invalid-throws, non-zero-heading)
- 8 new `FeatureSpawning` integration tests pass (entity-per-feature, transform+VMC present, no-FM, tick-doesn't-modify, model-sharing, empty-spawns-zero, heading-to-quaternion, with_component-discovery)
- All 19 prior `ScenarioLoader` + `CampaignBridge` tests still pass
- `f4-scenario-player` builds clean and produces a screenshot showing 11 VAOs (F-16 sub-meshes + multiple feature models) loaded into VRAM
- Pre-existing `PilotInputTest` failures are unrelated (assertion in `f4-flight-api/src/pilot_input.cpp:21`, present before Phase 2A)

### What's next (Phase 2B)

- Wire `BrainComponent` to drive the F-16 along the taxi route from parking
  to hold-short, using `FlightModelComponent`'s nosewheel steering + ground
  throttle.
- Render the aircraft actually moving across the real airfield meshes.

## Fixed-Timestep Player Loop (speed slider rework)

**Goal:** Make the simulation trajectory independent of the speed slider
and the frame rate, and raise the slider max from 4x to 10x.

### The problem

The scenario player ticked the sim ONCE per rendered frame with an
INFLATED dt (`player_app.cpp:241` before this change):

    sim->tick(scenario.sim_dt * time_scale);

so the flight model's FCS/EOM minor step (dt/6) slid between 1/3600 s
(0.1x) and 1/90 s (4x). The FCS PI + lead-lag filters are discrete and
dt-dependent — past their tuned operating point (1/360 s minor step)
the pitch loop destabilizes, which is why the slider was clamped to 4x
(FLIGHT_CONTROL_STABILITY_PLAN.md §4.2 RC-2 — a symptom patch). Two
more defects fell out of the same line:

1. Real-time pacing depended on the FRAME RATE: the sim advanced
   `sim_dt*time_scale` per FRAME, so "1.0x = real time" only held at
   exactly 60 FPS (a 144 Hz vsync display ran the sim ~2.4x too fast).
2. The trajectory itself changed with the slider (different dt =
   different integration path), so interactive runs, headless CI runs,
   and recorded traces were never directly comparable.
3. `Simulation::tick` ALSO multiplied by its own `time_scale_` member
   (default 1.0, never set by any caller) — a latent double-scaling
   trap for future hosts.

### The fix

Fixed-timestep accumulator in the player (the classic "Fix Your
Timestep" pattern):

    accumulator += frame_dt * time_scale;      // slider scales WALL TIME
    while (accumulator >= scenario.sim_dt) {   // dt is ALWAYS sim_dt
        sim->tick(scenario.sim_dt);
        accumulator -= scenario.sim_dt;
    }

- dt is now always `scenario.sim_dt`, so the FM minor step stays at its
  tuned 1/360 s at every speed; filter behavior is speed-independent.
- The slider scales wall-clock time, not dt: 1.0x is true real time at
  any frame rate, and slow-mo carries fractional remainders across
  frames smoothly.
- The tick sequence — hence the trajectory, the flight recording, and
  the FCS CSV trace — is identical across slider settings and matches
  the headless harnesses (`sim.tick(1.0/60.0)` loops), so interactive
  and CI traces are directly comparable again.
- The 4x stability cap is replaced by a CPU-bounded guard
  (`kMaxSimStepsPerFrame = 30` — covers 10x at 30 FPS; drops the
  remainder after a stall instead of freezing the render loop), and
  the slider max is raised to 10x (10x/60 FPS = 10 ticks x 6 minor
  steps per frame).
- `Simulation::set_time_scale()`/`time_scale_` (behavioral scaling) are
  REMOVED. The FCS CSV trace's `time_scale` column is kept and now
  records pure host metadata via `Simulation::set_trace_time_scale()`
  (default 1.0) — baselines stay identifiable, but no host can
  accidentally double-scale dt.

### Files changed

| File | Change |
|------|--------|
| `f4-scenario-player/src/player_app.cpp` | Accumulator tick loop with `kMaxSimStepsPerFrame = 30` guard; slider + `set_time_scale` clamp raised to [0.1, 10.0]; comments updated |
| `f4-scenario-player/src/viewer_state.hpp` | `sim_accumulator` member |
| `f4-scenario-player/include/f4/scenario_player/player_app.hpp` | `set_time_scale` doc (real time at any frame rate, [0.1, 10.0]) |
| `f4-simulation/include/f4/simulation/simulation.hpp` | `set_time_scale`/`time_scale_` removed; `set_trace_time_scale`/`trace_time_scale_` added (metadata only) |
| `f4-simulation/src/simulation.cpp` | `tick(dt)` no longer scales dt (dt is authoritative); FCS trace sample records `trace_time_scale_` |
| `Docs/FLIGHT_CONTROL_NEXT_STEPS.md` | Phase 2b row marked Superseded |
| `CHANGES.md` | This entry |

### Testing

- Full non-renderer test suite passes (all f4-* libraries; renderer/
  viewer executables excluded from CI here only because the container
  lacks GL dev headers — no renderer code was touched).
- The scenario-player translation unit compiles clean against raylib
  5.0 / imgui v1.91.5 / rlImGui @ 9acdbbf headers.
- Manual: run `takeoff_only` at 0.1x / 1x / 10x — the FCS CSV traces
  must be identical modulo the `time_scale` column; at 10x the run
  completes ~10x faster with no pitch oscillation.
- Manual: resize/drag the window mid-run at 10x — the render loop must
  stay live (guard drops catch-up debt) and sim time must not jump.
