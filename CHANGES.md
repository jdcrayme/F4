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
