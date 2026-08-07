cmake_minimum_required(VERSION 3.20)
project(f4-flight-model VERSION 0.1.0 LANGUAGES CXX DESCRIPTION "6-DOF flight dynamics model for the F4 libraries")

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# ---------------------------------------------------------------------------
# f4-flight-model depends on:
#   - f4-data          (for AircraftConfig, table accessors)
#   - f4-math          (for Table2D, Vec3, Quat, filters — explicit PUBLIC dep,
#                       used in public headers)
#   - f4-state-machine (for the stall state machine — the first real consumer)
#   - f4-messaging     (for the optional MessageBus integration — the first
#                       real consumer of the bus. The dependency is PUBLIC
#                       because the FlightModel's public API exposes
#                       set_message_bus(MessageBus*) and messages.hpp is a
#                       public header.)
#   - f4-units         (for the strong-typed Angle / AngularRate wrappers
#                       used by AircraftState. The dependency is PUBLIC
#                       because angle.hpp is a public header and
#                       AircraftState's fields are visible to consumers.)
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
    f4-math
    f4-state-machine
    f4-messaging
    f4-units
)

# Tests
option(F4_FLIGHT_MODEL_BUILD_TESTS "Build f4-flight-model unit tests" ON)
if(F4_FLIGHT_MODEL_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
