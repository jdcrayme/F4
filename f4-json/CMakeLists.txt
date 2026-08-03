cmake_minimum_required(VERSION 3.20)
project(f4-json VERSION 0.1.0 LANGUAGES CXX DESCRIPTION "Minimal dependency-free JSON reader/writer for F4 libraries")

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Static library: dependency-free JSON reader + writer.
#
# Replaces the three duplicated hand-rolled implementations that lived in
# f4-world/src/world_state.cpp, f4-terrain/src/terrain_data.cpp, and
# f4-world-viewer/src/settings.cpp. Header-only today (src/f4_json.cpp is
# an empty TU kept for ABI-surface parity with f4-messaging); linking
# against a real static target rather than an INTERFACE one keeps the
# dependency graph uniform.
#
# Dependencies: standard library only. f4-json sits BELOW f4-world,
# f4-terrain, and f4-world-viewer in the §3 dependency graph and must not
# pull in any domain library or any third-party JSON implementation
# (nlohmann/json stays in f4-convert / f4-data where its richer API is
# actually needed).
add_library(f4-json STATIC
    src/f4_json.cpp
)
target_include_directories(f4-json
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)

# Tests
option(F4_JSON_BUILD_TESTS "Build unit tests" ON)
if(F4_JSON_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
