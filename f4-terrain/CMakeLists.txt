cmake_minimum_required(VERSION 3.20)
project(f4-terrain VERSION 0.1.0 LANGUAGES CXX DESCRIPTION "FreeFalcon theater terrain data decoder")

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_library(f4-terrain STATIC
    src/terrain_data.cpp
)
target_include_directories(f4-terrain
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)
# f4-terrain uses f4-json for the terrain JSON intermediate format (the
# load_terrain_json / to_terrain_json pair). f4-json is a PUBLIC dep because
# f4-world (which transitively links f4-terrain) consumes the same JSON
# schema and benefits from the same Reader/Writer when extending it.
target_link_libraries(f4-terrain PUBLIC f4-json f4-io)

option(F4_TERRAIN_BUILD_TESTS "Build unit tests" ON)
if(F4_TERRAIN_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
