cmake_minimum_required(VERSION 3.20)
project(f4-geo VERSION 0.1.0 LANGUAGES CXX DESCRIPTION "Strong-typed coordinate frames and geodesy for flight simulation")

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Header-only library target. No external dependencies beyond the standard
# library — geodesy is self-contained, which keeps it maximally portable and
# testable. (f4-units quantities can be used alongside these types by callers
# but are not required.)
add_library(f4-geo INTERFACE)
target_include_directories(f4-geo INTERFACE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)

# Tests
option(F4_GEO_BUILD_TESTS "Build unit tests" ON)
if(F4_GEO_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
