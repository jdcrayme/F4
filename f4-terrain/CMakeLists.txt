cmake_minimum_required(VERSION 3.20)
project(f4-terrain VERSION 0.1.0 LANGUAGES CXX DESCRIPTION "FreeFalcon theater terrain data decoder")

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_library(f4-terrain STATIC
    src/terrain_data.cpp
    src/terrain_adapter.cpp
    src/post_level.cpp
    src/far_tile_db.cpp
    src/near_tile_db.cpp
    src/pcx_reader.cpp
)
target_link_libraries(f4-terrain PUBLIC f4_warnings)

target_include_directories(f4-terrain
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)
# f4-terrain uses f4-json for the terrain JSON intermediate format (the
# load_terrain_json / to_terrain_json pair), f4-io for binary file reading,
# and f4-install for the case-insensitive file finder (Stage 2 of the
# asset pipeline folds the duplicated finders into f4-install's public
# API — see f4/install/file_finder.hpp). All three are PRIVATE: they
# are implementation details not exposed in public headers. f4-world
# (which links f4-terrain) declares its own f4-json + f4-assets deps.
target_link_libraries(f4-terrain PRIVATE f4-json f4-io f4-install)

option(F4_TERRAIN_BUILD_TESTS "Build unit tests" ON)
if(F4_TERRAIN_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

# Developer tools compiled against the library (run manually against a
# real install to sanity-check format decoders).
add_executable(dump-terrain-textures tools/dump_terrain_textures.cpp)
target_link_libraries(dump-terrain-textures PRIVATE f4-terrain f4_warnings)

# Paint the 2D map art (L5 posts -> far tiles) + the MEA color map to a
