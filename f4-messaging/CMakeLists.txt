cmake_minimum_required(VERSION 3.20)
project(f4-messaging VERSION 0.1.0 LANGUAGES CXX DESCRIPTION "Type-safe message bus with explicit thread boundaries")

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Static library: type-indexed handler dispatch + cross-thread queue.
#
# The library is header-only today (src/bus.cpp is an empty TU kept for
# ABI-surface parity with f4-entities / f4-install), but linking against
# a real static target rather than an INTERFACE one keeps the dependency
# graph uniform and lets us add a non-template trace recorder later
# without churning consumers.
#
# Dependencies: standard library only. f4-messaging sits below f4-ai,
# f4-campaign, and f4-simulation in the §3 dependency graph and must not
# pull in any domain library. Tests enforce this: every test target
# links ONLY against f4-messaging.
add_library(f4-messaging STATIC
    src/bus.cpp
)
target_link_libraries(f4-messaging PUBLIC f4_warnings)

target_include_directories(f4-messaging
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)

# Tests
option(F4_MESSAGING_BUILD_TESTS "Build unit tests" ON)
if(F4_MESSAGING_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
