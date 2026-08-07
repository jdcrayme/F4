cmake_minimum_required(VERSION 3.20)
project(f4-math VERSION 0.1.0 LANGUAGES CXX DESCRIPTION "Numerical mathematics for flight simulation: tables, interpolation, integrators, filters, vectors, quaternions, solvers")

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Header-only library target.
#
# f4-math has ZERO dependencies on any other f4-* library. Tests prove this:
# every test target links ONLY against f4-math (never f4-units, never f4-data,
# never any domain library). This guarantees the library can be lifted into
# any project without dragging in domain coupling.
add_library(f4-math INTERFACE)
target_link_libraries(f4-math INTERFACE f4_warnings)

target_include_directories(f4-math INTERFACE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)

# Tests
option(F4_MATH_BUILD_TESTS "Build f4-math unit tests" ON)
if(F4_MATH_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
