cmake_minimum_required(VERSION 3.20)
project(f4-lzss VERSION 0.1.0 LANGUAGES CXX DESCRIPTION "LZSS decompression for Falcon 4.0 (TEX and CAM files)")

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_library(f4-lzss STATIC
    src/lzss.cpp
)
target_link_libraries(f4-lzss PUBLIC f4_warnings)

target_include_directories(f4-lzss
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)

# f4-lzss has zero external dependencies.

option(F4_LZSS_BUILD_TESTS "Build unit tests" ON)
if(F4_LZSS_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
