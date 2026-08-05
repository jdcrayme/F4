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
)
target_include_directories(f4-models
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)
# f4-models uses f4-json for JSON export of model data
target_link_libraries(f4-models PUBLIC f4-json)

option(F4_MODELS_BUILD_TESTS "Build unit tests" ON)
if(F4_MODELS_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
