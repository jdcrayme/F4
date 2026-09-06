cmake_minimum_required(VERSION 3.20)
project(f4-world-types VERSION 0.1.0 LANGUAGES CXX DESCRIPTION "Runtime-safe world type constants + JSON class table loader (Tranche 0d)")

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# ---------------------------------------------------------------------------
# f4-world-types — the runtime subset of f4-world-convert's theater/class-table
# definitions, extracted into a neutral library that the runtime (f4-simulation,
# f4-renderer) can link WITHOUT pulling in the legacy binary parsers.
#
# What lives here (runtime-safe — no binary parsing):
#   - ObjectiveType / PointType / PointListType enum constants
#   - ClassTableEntry + ClassTable (JSON loader only: load_json / load_auto)
#   - unit_subtype_name() helper
#
# What stays in f4-world-convert (importer-only — binary parsing):
#   - ClassTable::load() (the FALCON4.ct binary decoder)
#   - find_class_table() (install-layout search)
#   - The theater-object struct parsers (ObjectiveClassData, PtHeaderData, ...)
#
# Dependencies: f4-json (JSON reader), f4-io (read_file). NO f4-install,
# NO f4-lzss, NO binary format knowledge.
# ---------------------------------------------------------------------------

add_library(f4-world-types STATIC
    src/class_table.cpp
    src/aii_config.cpp
)
target_link_libraries(f4-world-types PUBLIC f4_warnings)

target_include_directories(f4-world-types
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)
target_link_libraries(f4-world-types PUBLIC f4-json f4-io)

option(F4_WORLD_TYPES_BUILD_TESTS "Build f4-world-types unit tests" ON)
if(F4_WORLD_TYPES_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
