cmake_minimum_required(VERSION 3.20)
project(f4-avionics VERSION 0.1.0 LANGUAGES CXX DESCRIPTION "Engine-agnostic avionics logic: INS, steerpoint navigation, page/view models")

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Header-only library target. Depends on:
#   f4-flight-api    — the IAircraftState seam (the INS consumes aircraft truth)
#   f4-geo           — WorldPosition + BRA/to_bra for the navigation reads
#   f4-state-machine — the alignment SM (transitions are data; tests pin them)
# Nothing else. Like f4-geo, the library is maximally portable and testable;
# rendering and input devices stay outside (AVIONICS_PLAN.md's charter).
add_library(f4-avionics INTERFACE)
target_link_libraries(f4-avionics INTERFACE f4_warnings)

target_include_directories(f4-avionics INTERFACE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)
target_link_libraries(f4-avionics INTERFACE
    f4-flight-api
    f4-geo
    f4-state-machine
)

# Tests
option(F4_AVIONICS_BUILD_TESTS "Build f4-avionics unit tests" ON)
if(F4_AVIONICS_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
