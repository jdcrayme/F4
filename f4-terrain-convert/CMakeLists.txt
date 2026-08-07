cmake_minimum_required(VERSION 3.20)
project(f4-terrain-convert VERSION 0.1.0 LANGUAGES CXX DESCRIPTION "THEATER.* binary -> terrain JSON converter")

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Static library: thin wrapper around f4-terrain's load() + save_terrain_json().
# Kept as a separate library so the sim (which only consumes the JSON) doesn't
# link the binary parser.
add_library(f4-terrain-convert STATIC
    src/terrain_converter.cpp
)
target_link_libraries(f4-terrain-convert PUBLIC f4_warnings)

target_include_directories(f4-terrain-convert
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)
target_link_libraries(f4-terrain-convert PRIVATE f4-terrain)

# CLI tool: terrain2json <terrain_dir> [out.json]
add_executable(terrain2json cli/terrain2json.cpp)
target_link_libraries(terrain2json PRIVATE f4-terrain-convert)
