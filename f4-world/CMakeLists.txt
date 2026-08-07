cmake_minimum_required(VERSION 3.20)
project(f4-world VERSION 0.1.0 LANGUAGES CXX DESCRIPTION "Typed world state from campaign data, populates f4-entities")

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_library(f4-world STATIC
    src/world_state.cpp
    src/world_loader.cpp
)
target_link_libraries(f4-world PUBLIC f4_warnings)

target_include_directories(f4-world
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)
# f4-world consumes the typed EntityWorld (to populate it), the geo types
# (for future objective/unit positioning), f4-terrain (for the optional
# load_terrain() convenience), and f4-json (for parsing the world-state
# JSON emitted by f4-world-convert). It does NOT depend on f4-world-convert
# itself — the contract between them is JSON, so the library and the
# converter are independently testable.
target_link_libraries(f4-world PUBLIC f4-entities f4-terrain PRIVATE f4-geo f4-json)

# Tests
option(F4_WORLD_BUILD_TESTS "Build unit tests" ON)
if(F4_WORLD_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
