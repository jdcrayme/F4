cmake_minimum_required(VERSION 3.20)
project(f4-world-vis VERSION 0.1.0 LANGUAGES CXX DESCRIPTION "SVG/HTML tile-map renderer for world data")

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_library(f4-world-vis STATIC
    src/svg_map.cpp
)
target_include_directories(f4-world-vis
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)

# CLI tool: reads a world JSON (from cam2json) and emits an HTML map.
add_executable(world2map cli/world2map.cpp)
target_link_libraries(world2map PRIVATE f4-world-vis)

option(F4_WORLD_VIS_BUILD_TESTS "Build unit tests" ON)
if(F4_WORLD_VIS_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
