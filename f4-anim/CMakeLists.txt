cmake_minimum_required(VERSION 3.20)
project(f4-anim VERSION 0.1.0 LANGUAGES CXX DESCRIPTION "Semantic animation channels, rig, and blink patterns for F4 models")

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Static library: pure C++20, no domain or third-party dependencies.
#
# f4-anim sits at the bottom of the animation stack (see
# Docs/AIRCRAFT_ANIMATION_PLAN.md §3.1): the converter (f4-import),
# the glTF evaluator (f4-gltf), and the sim/viewer layers all consume
# the channel vocabulary and the DOF value processing from here. It
# must never link anything beyond the standard library — that is what
# keeps the animation contract testable headlessly and portable.
add_library(f4-anim STATIC
    src/channels.cpp
    src/rig.cpp
)
target_link_libraries(f4-anim PUBLIC f4_warnings)

target_include_directories(f4-anim
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)

# The advisory channel registry (channels.json) ships with the library
# so the doctor and tooling can find it next to the headers.
add_library(F4::Anim ALIAS f4-anim)

# Tests
option(F4_ANIM_BUILD_TESTS "Build unit tests" ON)
if(F4_ANIM_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
