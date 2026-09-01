cmake_minimum_required(VERSION 3.20)
project(f4-models VERSION 0.1.0 LANGUAGES CXX DESCRIPTION "Falcon 4.0 3D model database parser (KoreaObj.HDR/LOD/DXH/DXL)")

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_library(f4-models STATIC
    src/hdr_parser.cpp
    src/bsp_parser.cpp
    src/dx_parser.cpp
    src/poly_parser.cpp
    src/geometry_extractor.cpp
    src/geometry.cpp
    src/model_database.cpp
    src/model_json.cpp
    src/tex_reader.cpp
)
target_link_libraries(f4-models PUBLIC f4_warnings)

target_include_directories(f4-models
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)
# f4-models uses f4-json for JSON export of model data, f4-math for
# the Vec3 / Mat3x3 types (f4::models::Vec3 is now an alias for
# f4::math::Vec3<float> — the previous duplicate definition in types.hpp
# was removed in the 2025 cleanup pass to avoid the two types drifting
# apart as f4-math::Vec3 gains operators), and f4-install for the
# case-insensitive file finder with name variants (Stage 2 of the asset
# pipeline folds the duplicated finders into f4-install's public API —
# see f4/install/file_finder.hpp).
target_link_libraries(f4-models PUBLIC f4-math PRIVATE f4-json f4-lzss f4-install)

option(F4_MODELS_BUILD_TESTS "Build unit tests" ON)
if(F4_MODELS_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

option(F4_MODELS_BUILD_DIAG "Build diagnostic CLI tools (require game data)" OFF)
if(F4_MODELS_BUILD_DIAG)
    add_subdirectory(diag)
endif()
