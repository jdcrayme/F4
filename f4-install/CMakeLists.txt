cmake_minimum_required(VERSION 3.20)
project(f4-install VERSION 0.1.0 LANGUAGES CXX DESCRIPTION "Locates and describes a Falcon 4.0 / FreeFalcon installation on disk")

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Static library: install layout locator. Owns the knowledge of where
# FALCON4.ct, sim/, terrdata/, and campaign/ live, so neither the viewer
# nor any CLI tool has to duplicate the search heuristics.
#
# Zero external dependencies beyond the standard library — f4-install is
# intentionally platform-portable (must work on Windows, macOS, Linux)
# and lives below f4-world-convert in the dependency graph so the
# converter can call into it without creating a cycle.
add_library(f4-install STATIC
    src/installation.cpp
    src/theater.cpp
    src/campaign.cpp
)
target_link_libraries(f4-install PUBLIC f4_warnings)

target_include_directories(f4-install
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)

# Tests
option(F4_INSTALL_BUILD_TESTS "Build unit tests" ON)
if(F4_INSTALL_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
