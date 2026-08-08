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
)

add_library(F4::Simulation ALIAS f4-simulation)

# Tests
option(F4_SIMULATION_BUILD_TESTS "Build f4-simulation unit tests" ON)
if(F4_SIMULATION_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
