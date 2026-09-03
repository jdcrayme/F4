cmake_minimum_required(VERSION 3.20)
project(f4-convert VERSION 0.1.0 LANGUAGES CXX DESCRIPTION "Legacy .dat to JSON aircraft data converter for the F4 libraries")

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# ---------------------------------------------------------------------------
# f4-convert library (parser + JSON I/O).
#
# nlohmann/json is now fetched in cmake/f4_deps.cmake (at the root level)
# so that f4-data can link it too, despite being configured before f4-convert.
#
# Depends on f4-data for the AircraftConfig struct AND for the canonical
# JSON writer/reader (f4-data::writeConfig / loadConfig). f4-convert's
# json_io.cpp is a thin adapter that delegates to f4-data for the actual
# serialization; only diffConfigs() is unique to f4-convert (f4-data does
# no comparison). The previous duplication (two copies of the JSON
# writer/reader that "MUST stay in sync") was eliminated in the 2025
# cleanup pass.
# ---------------------------------------------------------------------------
add_library(f4-convert STATIC
    src/dat_parser.cpp
    src/brain_parser.cpp
    src/formation_parser.cpp
    src/mnvr_parser.cpp
    src/sensor_parser.cpp
    src/signature_parser.cpp
    src/veh_parser.cpp
    src/json_io.cpp
)
target_link_libraries(f4-convert PUBLIC f4_warnings)

target_include_directories(f4-convert PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)
target_link_libraries(f4-convert PUBLIC
    f4-data
PRIVATE
    f4-math
)

# ---------------------------------------------------------------------------
# CLI tools. Each is a thin main() that calls into the f4-convert library.
# Built by default; can be disabled with -DF4_CONVERT_BUILD_CLI=OFF.
# ---------------------------------------------------------------------------
option(F4_CONVERT_BUILD_CLI "Build f4-convert CLI tools" ON)
if(F4_CONVERT_BUILD_CLI)
    add_subdirectory(cli)
endif()

# ---------------------------------------------------------------------------
# Tests.
# ---------------------------------------------------------------------------
option(F4_CONVERT_BUILD_TESTS "Build f4-convert unit tests" ON)
if(F4_CONVERT_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

# ---------------------------------------------------------------------------
# Install the Rosetta Stone as a data file alongside the library.
# This is the contract between f4-convert and f4-data: both load this JSON
# to know what each AuxAeroData key means.
# ---------------------------------------------------------------------------
install(FILES rosetta/auxaero_field_map.json
        DESTINATION share/f4/rosetta)
