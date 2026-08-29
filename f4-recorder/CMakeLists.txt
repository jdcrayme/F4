cmake_minimum_required(VERSION 3.20)
project(f4-recorder VERSION 0.1.0 LANGUAGES CXX DESCRIPTION "Flight recording, replay, and LLM-friendly data export")

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# ---------------------------------------------------------------------------
# f4-recorder depends on:
#   - f4-geo      (WorldPosition — stored in FlightSnapshot)
#   - f4-json     (Writer — JSON serialization of recordings)
#
# f4-recorder does NOT depend on:
#   - f4-flight-model (snapshots are plain structs, not FlightModel references)
#   - f4-entities     (entity_id is a uint64, not an EntityHandle)
#   - f4-ai           (ai_mode/ai_state are strings, not module references)
#
# This keeps the recorder lightweight and suitable for headless sim + replay
# without pulling in the full simulation dependency tree.
# ---------------------------------------------------------------------------
add_library(f4-recorder STATIC
    src/flight_recorder.cpp
    src/fcs_trace.cpp
    src/path_geometry.cpp
)
target_link_libraries(f4-recorder PUBLIC f4_warnings)

target_include_directories(f4-recorder
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)

target_link_libraries(f4-recorder PUBLIC
    f4-geo
    f4-json
)

# Tests
option(F4_RECORDER_BUILD_TESTS "Build f4-recorder unit tests" ON)
if(F4_RECORDER_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
