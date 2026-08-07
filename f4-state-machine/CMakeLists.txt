cmake_minimum_required(VERSION 3.20)
project(f4-state-machine VERSION 0.1.0 LANGUAGES CXX DESCRIPTION "Type-safe state machines with transition tables, layered priority, tracing, and serialization")

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Header-only library target.
#
# f4-state-machine has ZERO dependencies on any other f4-* library, and ZERO
# third-party dependencies (no nlohmann/json — serialization emits plain text).
# Tests prove the zero-coupling invariant: every test target links ONLY against
# f4-state-machine. This guarantees the library can be lifted into any project
# without dragging in domain or third-party coupling.
#
# This library is the foundation for every stateful domain system in F4:
#   - f4-flight-model: stall state machine (currently inline — to be migrated)
#   - f4-ai: the 24-mode DigiMode priority ladder (LayeredStateMachine)
#   - f4-ai: 17-state landing SM, 5-state AAR SM, waypoint-progress SM
#   - f4-campaign: mission-request lifecycle, squadron scheduling
#
# Design priority (learned from the F4Flight digi experience, where behavior
# "evolved to a level of complexity where unpredictable behavior was
# impossible to track down"): OBSERVABILITY IS NOT OPTIONAL. Every transition
# is recorded in a bounded trace ring-buffer and emitted as parseable text.
# A misbehaving machine can always be inspected, not just run.
add_library(f4-state-machine INTERFACE)
target_link_libraries(f4-state-machine INTERFACE f4_warnings)

target_include_directories(f4-state-machine INTERFACE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)

# Tests
option(F4_STATE_MACHINE_BUILD_TESTS "Build f4-state-machine unit tests" ON)
if(F4_STATE_MACHINE_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
