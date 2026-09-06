cmake_minimum_required(VERSION 3.20)
project(f4-assets VERSION 0.1.0 LANGUAGES CXX DESCRIPTION "Runtime asset root, manifest reader, and capability/staleness API")

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Static library: the runtime's single point of access to `Data/`.
#
# f4-assets defines AssetId, the manifest schema v1, and the asset root
# resolver. Runtime libraries link this instead of any importer. Importer
# targets (f4-import) ALSO link this for the shared schema types.
#
# Dependencies:
#   - f4-json: shared zero-deps JSON reader/writer (used internally by
#     manifest.cpp and asset_root.cpp; no JSON type is exposed in any
#     f4-assets header).
#   - f4_warnings: shared warning flags.
add_library(f4-assets STATIC
    src/asset_id.cpp
    src/hash.cpp
    src/manifest.cpp
    src/asset_root.cpp
)
target_link_libraries(f4-assets PUBLIC f4_warnings f4-json)

target_include_directories(f4-assets
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)

# The F4_SIDE property marks this as a runtime-side library for the
# boundary enforcement check (ASSET_PIPELINE_SPEC §10 — Stage 5 adds
# `cmake/verify_boundary.cmake`).
set_target_properties(f4-assets PROPERTIES F4_SIDE runtime)

option(F4_ASSETS_BUILD_TESTS "Build unit tests" ON)
if(F4_ASSETS_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
