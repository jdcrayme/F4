cmake_minimum_required(VERSION 3.20)
project(f4-import VERSION 0.1.0 LANGUAGES CXX DESCRIPTION "Asset pipeline orchestrator: manifest writer, doctor, f4import CLI")

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Static library + CLI: the importer side of the asset pipeline.
#
# f4-import owns the doctor (D1-D9 validation) and the `f4import` CLI
# orchestrator. Stage 1+ adds the import/ensure subcommands that drive
# the converters + write the manifest in one call.
#
# Dependencies:
#   - f4-assets: shared AssetId / Manifest types (P2 — link-time
#     isolation forbids runtime → importer links, not the reverse;
#     f4-import links f4-assets for the schema types).
#   - f4-json: shared JSON reader (used by doctor.cpp to walk world
#     JSON files for the D2 check).
#   - f4_warnings: shared warning flags.

add_library(f4-import STATIC
    src/doctor.cpp
    src/manifest_writer.cpp
    src/gltf_emitter.cpp
)
target_link_libraries(f4-import PUBLIC f4_warnings f4-assets f4-json)
target_link_libraries(f4-import PRIVATE f4-models f4-gltf)

target_include_directories(f4-import
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)

# CLI tool
add_executable(f4import cli/f4import.cpp)
target_link_libraries(f4import PRIVATE f4-import f4-models)

# F4_SIDE marks this as an importer-side library for the boundary
# enforcement check (Stage 5 adds verify_boundary.cmake).
set_target_properties(f4-import f4import PROPERTIES F4_SIDE importer)

option(F4_IMPORT_BUILD_TESTS "Build unit tests" ON)
if(F4_IMPORT_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
