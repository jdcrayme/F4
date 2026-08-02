cmake_minimum_required(VERSION 3.20)
project(f4-world-convert VERSION 0.1.0 LANGUAGES CXX DESCRIPTION "FreeFalcon .cam campaign archive -> JSON converter")

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Static library: LZSS decompressor + .cam container parser + campaign decoder
# + JSON emitter. No external dependencies beyond the standard library (the
# JSON is emitted by hand to match f4-convert's pattern — nlohmann/json is
# available to f4-data/f4-world on the consuming side).
add_library(f4-world-convert STATIC
    src/lzss.cpp
    src/cam_archive.cpp
    src/campaign_decoder.cpp
    src/objective_decoder.cpp
    src/unit_decoder.cpp
    src/team_decoder.cpp
    src/world_json.cpp
)
target_include_directories(f4-world-convert
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)

# CLI tool
add_executable(cam2json cli/cam2json.cpp)
target_link_libraries(cam2json PRIVATE f4-world-convert)

# Tests
option(F4_WORLD_CONVERT_BUILD_TESTS "Build unit tests" ON)
if(F4_WORLD_CONVERT_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
