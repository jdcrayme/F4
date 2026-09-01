cmake_minimum_required(VERSION 3.20)
project(f4-gltf VERSION 0.1.0 LANGUAGES CXX DESCRIPTION "Minimal glTF 2.0 runtime loader for the F4 asset pipeline")

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Static library: the runtime glTF 2.0 loader. Stage 3 of the asset
# pipeline (ASSET_PIPELINE_SPEC.md §11) — replaces f4-models in the
# runtime link closure. The importer side (f4-import) links f4-models
# to parse KoreaObj and emit glTF; the runtime side links f4-gltf to
# load the glTF back.
#
# Dependencies:
#   - f4-json: shared zero-deps JSON reader (used internally by
#     gltf_loader.cpp; no JSON type is exposed in any f4-gltf header).
#   - f4_warnings: shared warning flags.
add_library(f4-gltf STATIC
    src/gltf_loader.cpp
)
target_link_libraries(f4-gltf PUBLIC f4_warnings f4-json)

target_include_directories(f4-gltf
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)

# The F4_SIDE property marks this as a runtime-side library for the
# boundary enforcement check (ASSET_PIPELINE_SPEC §10).
set_target_properties(f4-gltf PROPERTIES F4_SIDE runtime)

option(F4_GLTF_BUILD_TESTS "Build unit tests" ON)
if(F4_GLTF_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
