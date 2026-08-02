cmake_minimum_required(VERSION 3.20)
project(f4-convert VERSION 0.1.0 LANGUAGES CXX DESCRIPTION "Legacy .dat to JSON aircraft data converter for the F4 libraries")

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# ---------------------------------------------------------------------------
# Fetch nlohmann/json single-header (used for JSON serialization).
# This is the standard C++ JSON library and eliminates the entire class of
# escaping/parsing bugs that F4Flight's hand-rolled writer had.
# ---------------------------------------------------------------------------
include(FetchContent)
FetchContent_Declare(
    nlohmann_json
    GIT_REPOSITORY https://github.com/nlohmann/json.git
    GIT_TAG v3.11.3
)
FetchContent_MakeAvailable(nlohmann_json)

# ---------------------------------------------------------------------------
# f4-convert library (parser + JSON I/O).
#
# Depends on f4-data for the AircraftConfig struct (which f4-data owns).
# The JSON I/O code in f4-convert/src/json_io.cpp is a COPY of f4-data's
# config_loader.cpp — they must stay in sync. Both copies exist because
# f4-convert cannot depend on f4-data at build time for the JSON writer
# (that would be fine), but f4-convert also needs to produce JSON that
# f4-data can read, so the format must be identical. The round-trip tests
# in both libraries verify this.
# ---------------------------------------------------------------------------
add_library(f4-convert STATIC
    src/dat_parser.cpp
    src/json_io.cpp
)
target_include_directories(f4-convert PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)
target_link_libraries(f4-convert PUBLIC
    f4-data
    nlohmann_json::nlohmann_json
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
