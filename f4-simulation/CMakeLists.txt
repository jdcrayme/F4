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
#   - f4-models         (ModelDatabase, ModelRecord — for VisualModelComponent)
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
    src/combat_bridge.cpp
    src/combat_transcript.cpp
    src/bubble_manager.cpp
)
target_link_libraries(f4-simulation PUBLIC f4_warnings)

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
    f4-models
    f4-recorder
    f4-json
    f4-io
    f4-world           # Phase 2: WorldState, populate_world for spawn_from_campaign_flights
    f4-world-convert   # Phase 2: ClassTable for entity_type → vis_type resolution
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
