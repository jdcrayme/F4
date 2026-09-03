cmake_minimum_required(VERSION 3.20)
project(f4-sensors VERSION 0.1.0 LANGUAGES CXX DESCRIPTION "Sensor model: radar detection, track files, RWR")

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# ---------------------------------------------------------------------------
# f4-sensors depends on:
#   - f4-geo       (WorldPosition, BRA geometry; PUBLIC, used in public headers)
#   - f4-math      (Vec3 for velocities/LOS math; PUBLIC, used in headers)
#   - f4-entities  (components: TransformComponent, SignatureComponent base,
#                   RadarSimComponent behavioral; PUBLIC, used in headers)
#   - f4-messaging (MessageBus for track/RWR messages; PUBLIC —
#                   radar_component.hpp + rwr.hpp expose it)
#
# Deliberately NOT a dependency:
#   - f4-ai     (SensorFusion consumes sensors via the M3 adapter; the AI
#                library never appears in this target's dependency graph)
#   - f4-weapons (the missile seeker-source hook is a std::function wired by
#                the host — see COMBAT_CHAIN_PLAN.md M2)
#
# f4-data is a PRIVATE dependency: the SimData signature grids
# (SIGDATA/RCSDAT -> f4::data::SignatureGrid) flow into
# SignatureComponent/TargetSignature as forward-declared pointers, so
# the public headers stay data-free; only detection.cpp and
# signature.cpp need the full grid definition (value_at). Public
# consumers that set rcs_grid include f4/data/signature_data.hpp and
# link f4-data themselves.
# ---------------------------------------------------------------------------
add_library(f4-sensors STATIC
    src/detection.cpp
    src/track_store.cpp
    src/rwr.cpp
    src/radar_component.cpp
    src/signature.cpp
)
target_link_libraries(f4-sensors PUBLIC f4_warnings)

target_include_directories(f4-sensors PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)
target_link_libraries(f4-sensors PUBLIC
    f4-geo
    f4-math
    f4-entities
    f4-messaging
)
target_link_libraries(f4-sensors PRIVATE
    f4-data
)

# Tests
option(F4_SENSORS_BUILD_TESTS "Build f4-sensors unit tests" ON)
if(F4_SENSORS_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
