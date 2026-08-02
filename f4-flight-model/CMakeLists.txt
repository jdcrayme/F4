cmake_minimum_required(VERSION 3.20)
project(f4-flight-model VERSION 0.1.0 LANGUAGES CXX DESCRIPTION "6-DOF flight dynamics model for the F4 libraries")

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# ---------------------------------------------------------------------------
# f4-flight-model depends on:
#   - f4-data          (for AircraftConfig, table accessors)
#   - f4-math          (for Table2D, Vec3, Quat, filters — transitively via f4-data)
#   - f4-state-machine (for the stall state machine — the first real consumer)
#
# f4-units is NOT a dependency. The flight model uses raw Imperial doubles
# internally to match the coefficient tables exactly. Typed Quantity
# accessors can be added at the host boundary.
# ---------------------------------------------------------------------------
add_library(f4-flight-model STATIC
    src/aerodynamics.cpp
    src/fcs.cpp
    src/engine.cpp
    src/eom.cpp
    src/gear.cpp
    src/flight_model.cpp
    src/stall_state.cpp
)
target_include_directories(f4-flight-model PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)
target_link_libraries(f4-flight-model PUBLIC
    f4-data
    f4-state-machine
)

# Tests
option(F4_FLIGHT_MODEL_BUILD_TESTS "Build f4-flight-model unit tests" ON)
if(F4_FLIGHT_MODEL_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
