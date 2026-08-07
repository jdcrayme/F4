cmake_minimum_required(VERSION 3.20)
project(f4-units VERSION 0.1.0 LANGUAGES CXX DESCRIPTION "Compile-time physical quantity types for flight simulation")

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Header-only library target
add_library(f4-units INTERFACE)
target_link_libraries(f4-units INTERFACE f4_warnings)

target_include_directories(f4-units INTERFACE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)

# Tests
option(F4_UNITS_BUILD_TESTS "Build unit tests" ON)
if(F4_UNITS_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()