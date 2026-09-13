cmake_minimum_required(VERSION 3.20)
project(f4-simulation VERSION 0.1.0 LANGUAGES CXX DESCRIPTION "Simulation orchestration: EntityWorld + MessageBus + ModelDatabase + tick loop")

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# ---------------------------------------------------------------------------
# f4-simulation — the orchestration layer proposed in
# Docs/ARCHITECTURE PROPOSAL.md §13. Owns the EntityWorld + MessageBus +
# ModelDatabase + AircraftConfig registry and runs the tick loop. NO
# rendering — that's the executable's job (f4-scenario-player).
#
# This library is where components that depend on BOTH f4-entities and
# f4-models live (e.g. VisualModelComponent — the renderable handle that's
# the ECS equivalent of DrawableBSP* on SimVehicleClass).
#
# Dependencies:
#   - f4-entities       (EntityWorld, Component<T>, TransformComponent)
#   - f4-messaging      (MessageBus — for ATC messages)
#   - f4-flight-model   (FlightModelComponent — the physics, implements IAircraftState)
#   - f4-flight-api     (IAircraftState, IPilotInputSink, PilotInput — brain <-> FM contract)
#   - f4-ai             (BrainComponent, TakeoffModule, StubATC)
#   - f4-data           (AircraftConfig, config_loader)
#   - f4-geo            (WorldPosition, ENU/NED conversions)
#   - f4-math           (Vec3d, etc.)
#   - f4-units          (Quantity, Radians — used by flight::Angle)
#   - f4-state-machine  (StateMachine — used by TakeoffModule)
#   - f4-world-types    (runtime-safe ClassTable JSON loader + layout enums;
#                        Tranche 0d: replaces f4-world-convert for the runtime)
#   - f4-recorder       (FlightRecorder, FlightSnapshot)
#   - f4-json           (Reader — for scenario JSON parsing)
#   - f4-io             (read_file — for scenario file I/O)
# ---------------------------------------------------------------------------

add_library(f4-simulation STATIC
    src/scenario.cpp
    src/simulation.cpp
    src/campaign_bridge.cpp
    src/campaign_spawner.cpp
    src/campaign_result_sink.cpp
    src/campaign_session.cpp
    src/campaign_session_runner.cpp
    src/campaign_war_harness.cpp
    src/bvr_intercept_harness.cpp
    src/wvr_merge_harness.cpp
    src/ground_strike_harness.cpp
    src/combat_bridge.cpp
    src/combat_transcript.cpp
    src/bubble_manager.cpp
)
target_link_libraries(f4-simulation PUBLIC f4_warnings)

# V-THREAD: CampaignSessionRunner owns a std::thread — the library (not
# just its executables) needs the threads library now.
find_package(Threads REQUIRED)
target_link_libraries(f4-simulation PUBLIC Threads::Threads)

target_include_directories(f4-simulation
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)

target_link_libraries(f4-simulation PUBLIC
    f4-entities
    f4-messaging
    f4-flight-model
    f4-flight-api
    f4-ai
    f4-data
    f4-geo
    f4-math
    f4-units
    f4-state-machine
    f4-recorder
    f4-json
    f4-io
    f4-assets          # Task 58 (Tranche 0e): @asset: resolution through the Data/ manifest
    f4-world           # Phase 2: WorldState, populate_world for spawn_from_campaign_flights
    f4-world-types     # Tranche 0d: ClassTable JSON loader + layout enums (replaces f4-world-convert)
    f4-terrain         # Phase 2: transitively required by f4-world/detail/world_state.hpp (TerrainData)
    f4-weapons         # M3 combat chain (COMBAT_CHAIN_PLAN.md): stores, launch, sweep
    f4-sensors         # M3 combat chain: radar, tracks, RWR
    f4-campaign        # B.3: MissionIntent (emit_flight_intents → CampaignSimSpawner loop)
)

add_library(F4::Simulation ALIAS f4-simulation)

# Tests
option(F4_SIMULATION_BUILD_TESTS "Build f4-simulation unit tests" ON)
if(F4_SIMULATION_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

# ============================================================================
# trace_runner — headless scenario runner + CSV trace + landing summary
# (STAB tooling). Not a test: it runs any scenario JSON to mission
# completion (or a tick budget), prints state transitions / go-around
# reasons / touchdown metrics, and writes the FCS CSV trace.
# ============================================================================
add_executable(trace_runner ${CMAKE_SOURCE_DIR}/scripts/trace_runner.cpp)
target_link_libraries(trace_runner PRIVATE f4-simulation)
target_compile_features(trace_runner PRIVATE cxx_std_20)

# The go-around reason tap needs the ATC message types (f4-ai, pulled in
# transitively via f4-simulation). Install alongside the other tools so a
# headless CI can generate §5 acceptance traces in one command.

# ============================================================================
# Default SimData AI data location (brain archetypes + formations): the
# build tree's generated fixtures (f4-convert converts the shipped
# BRAINDAT.brn / FORMDAT.FIL at build time — the same conversion any
# Falcon 4.0\Zips\SimData.zip takes). Scenario JSON can override via
# brain_data_path / formation_library_path; this default makes
# "brain_profile" / "formation" work in any scenario without path
# configuration. Only consulted when a scenario actually references
# archetypes/formations (nothing loads otherwise).
# ============================================================================
target_compile_definitions(f4-simulation PRIVATE
    F4_SIMDATA_DEFAULT_DIR="${F4_GENERATED_FIXTURES_DIR}/simdata")

# ============================================================================
# campaign_qc — the B.3 end-to-end QC harness (tools/campaign_qc.cpp).
# aircraft with saved routes, then a scenario-driven Simulation run that
# writes the replay trace. See the tool's header comment for the artifact
# set (summary JSON + trace + generated scenario).
# ============================================================================
add_executable(campaign_qc tools/campaign_qc.cpp)
target_link_libraries(campaign_qc PRIVATE f4-simulation)
target_compile_features(campaign_qc PRIVATE cxx_std_20)
target_compile_definitions(campaign_qc PRIVATE
    F4_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
    F4_BINARY_DIR="${CMAKE_BINARY_DIR}")
# C2 — the synthetic tasking ladder needs the generated mission-profile
# table (f4-campaign's build-time fixture; the cache var is global).
target_compile_definitions(campaign_qc PRIVATE
    F4_MISSION_PROFILES_JSON="${F4_MISSION_PROFILES_JSON}")
add_dependencies(campaign_qc mission_profiles_fixture)
# The QC's default --config is <bin>/generated_fixtures/f16.json — a
# build artifact (convert_golden_fixtures, f4-convert). Building ONLY the
# campaign_qc target must produce it too (same single-target gap the
# viewer's V-CAMP fix addresses); CLI-gated target, hence the guard.
if(TARGET convert_golden_fixtures)
    add_dependencies(campaign_qc convert_golden_fixtures)
endif()

# ============================================================================
# bvr_intercept_qc — the M4 end-to-end BVR intercept acceptance tool
# (tools/bvr_intercept_qc.cpp). The combat-chain sibling of campaign_qc's
# --war mode: runs a BVR intercept scenario headless through
# BvrInterceptHarness, writes the three artifacts (result + summary +
# diary), and derives the exit-code verdict. See the tool's header comment
# + Docs/COMBAT_CHAIN_M4_PLAN.md §4 for the exit-code table. Links
# f4-simulation only (f4-json comes transitively PUBLIC; the harness owns
# the f4-recorder interaction). No default-fixture compile defs — the tool
# consumes a scenario JSON path directly as argv[1].
# ============================================================================
add_executable(bvr_intercept_qc tools/bvr_intercept_qc.cpp)
target_link_libraries(bvr_intercept_qc PRIVATE f4-simulation)
target_compile_features(bvr_intercept_qc PRIVATE cxx_std_20)

# ============================================================================
# wvr_merge_qc — the M5a end-to-end WVR / guns merge acceptance tool
# (tools/wvr_merge_qc.cpp). The combat-chain sibling of bvr_intercept_qc:
# runs a WVR merge scenario headless through WvrMergeHarness, writes the
# three artifacts (result + summary + diary), and derives the exit-code
# verdict. See the tool's header comment + Docs/COMBAT_CHAIN_M5_PLAN.md
# for the exit-code table. Links f4-simulation only.
# ============================================================================
add_executable(wvr_merge_qc tools/wvr_merge_qc.cpp)
target_link_libraries(wvr_merge_qc PRIVATE f4-simulation)
target_compile_features(wvr_merge_qc PRIVATE cxx_std_20)

# ============================================================================
# ground_strike_qc — the M5b end-to-end air-to-ground strike acceptance
# tool (tools/ground_strike_qc.cpp). The combat-chain sibling of
# bvr_intercept_qc / wvr_merge_qc: runs a strike scenario headless
# through GroundStrikeHarness, composes the strike world (the objective,
# the bomb station, the StrikeModule + mission-plan binding), writes the
# three artifacts (result + summary + diary), and derives the exit-code
# verdict. See the tool's header comment + Docs/COMBAT_CHAIN_M4_PLAN.md
# §6 / Docs/COMBAT_CHAIN_M5_PLAN.md §6 (the deferral this closes) for
# the exit-code table. Links f4-simulation only.
# ============================================================================
add_executable(ground_strike_qc tools/ground_strike_qc.cpp)
target_link_libraries(ground_strike_qc PRIVATE f4-simulation)
target_compile_features(ground_strike_qc PRIVATE cxx_std_20)
