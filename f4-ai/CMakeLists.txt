cmake_minimum_required(VERSION 3.20)
project(f4-ai VERSION 0.1.0 LANGUAGES CXX DESCRIPTION "Engine-agnostic AI brain (sensor fusion, tactic modules)")

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# ---------------------------------------------------------------------------
# f4-ai depends on:
#   - f4-entities      (EntityWorld, TransformComponent, tag queries)
#   - f4-messaging     (MessageBus — for RWRContactMessage and similar)
#   - f4-geo           (WorldPosition, to_bra, BRA geometry)
#   - f4-state-machine (LayeredStateMachine for the DigiMode priority ladder)
#   - f4-flight-model  (AircraftState — read by tactic modules)
#   - f4-data          (AircraftConfig — for aircraft-specific limits)
#   - f4-math          (Table2D, filters — used by autopilot PIDs)
#
# f4-geo, f4-entities, f4-messaging are pulled in transitively by
# f4-flight-model, but we list them explicitly to make the dependency
# contract visible and to keep the link graph stable if f4-flight-model
# ever drops one of them.
# ---------------------------------------------------------------------------
add_library(f4-ai STATIC
    src/sensor_fusion.cpp
    src/takeoff_module.cpp
)
target_link_libraries(f4-ai PUBLIC f4_warnings)

target_include_directories(f4-ai
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)

target_link_libraries(f4-ai PUBLIC
    f4-flight-model
    f4-entities
    f4-messaging
    f4-state-machine
    f4-geo
    f4-data
    f4-math
    f4-recorder
)

# Tests
option(F4_AI_BUILD_TESTS "Build f4-ai unit tests" ON)
if(F4_AI_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
