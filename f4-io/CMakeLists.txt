cmake_minimum_required(VERSION 3.20)
project(f4-io VERSION 0.1.0 LANGUAGES CXX DESCRIPTION "Binary I/O primitives (Cursor + read_file) shared by f4-world-convert and f4-terrain")

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Static library: consolidated binary-reader primitives.
#
# Two consolidation targets live here:
#   * f4::io::Cursor  — header-only sequential byte reader with a sticky
#                       error flag (replaces 6 anonymous-namespace Cursor
#                       structs across f4-world-convert + f4-terrain).
#   * f4::io::read_file — C-style FILE* loader returning std::vector<uint8_t>
#                       (replaces 3 standalone helpers + 1 inlined copy).
#
# Zero f4-* dependencies. Only the standard library is used (<cstdint>,
# <cstring>, <vector>, <filesystem>, <cstdio>, <stdexcept>, <string>).
# Sits BELOW f4-world-convert and f4-terrain in the dependency graph
# (the two consumers that link it PUBLIC); see /home/z/my-project/work/io_survey.md
# for the layering analysis.
add_library(f4-io STATIC
    src/read_file.cpp
    src/zip_reader.cpp
)
target_link_libraries(f4-io PUBLIC f4_warnings)

target_include_directories(f4-io
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)

# Tests
option(F4_IO_BUILD_TESTS "Build unit tests" ON)
if(F4_IO_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
