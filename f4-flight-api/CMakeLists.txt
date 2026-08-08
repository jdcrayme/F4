# f4-flight-api — Flight model API interfaces.
#
# A lightweight library defining the contract between the AI brain
# (f4-ai) and the flight model (f4-flight-model). Contains:
#   - PilotInput (moved from aircraft_state.hpp)
#   - IAircraftState (read-only aircraft state for AI)
#   - IPilotInputSink (write interface for control input)
#
# This library has NO dependencies beyond the C++ standard library,
# so consumers like f4-ai don't pull in f4-data, f4-math, etc.

cmake_minimum_required(VERSION 3.20)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_library(f4-flight-api STATIC
    src/pilot_input.cpp
)

target_include_directories(f4-flight-api PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(f4-flight-api PUBLIC f4_warnings)

# Tests
option(F4_FLIGHT_API_BUILD_TESTS "Build f4-flight-api unit tests" ON)
if(F4_FLIGHT_API_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
