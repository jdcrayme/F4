cmake_minimum_required(VERSION 3.20)
project(f4-world-convert VERSION 0.1.0 LANGUAGES CXX DESCRIPTION "FreeFalcon .cam campaign archive -> JSON converter")

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Static library: LZSS decompressor + .cam container parser + campaign decoder
# + JSON emitter. No external dependencies beyond the standard library (the
# JSON is emitted by hand to match f4-convert's pattern — nlohmann/json is
# available to f4-data/f4-world on the consuming side).
add_library(f4-world-convert STATIC
    src/lzss.cpp
    src/cam_archive.cpp
    src/campaign_decoder.cpp
    src/objective_decoder.cpp
    src/unit_decoder.cpp
    src/team_decoder.cpp
    src/class_table.cpp
    src/world_json.cpp
)
target_include_directories(f4-world-convert
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)

# CLI tool
add_executable(cam2json cli/cam2json.cpp)
target_link_libraries(cam2json PRIVATE f4-world-convert)

# Copy FALCON4.ct (the class table) next to cam2json so the CLI finds it
# from CWD when run via `cmake --build build --target world`. The class
# table maps entity_type values (100-2000+) to ObjectiveType (1-39) and
# unit subtypes (armor/infantry/fighter/...), so without it objectives
# render as generic circles in the viewer instead of type-specific icons.
set(F4_CT_SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/FALCON4.ct")
if(EXISTS "${F4_CT_SOURCE}")
    add_custom_command(TARGET cam2json POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${F4_CT_SOURCE}"
            "${CMAKE_BINARY_DIR}/FALCON4.ct"
        COMMENT "Copying FALCON4.ct class table to build directory"
    )
endif()

# Tests
option(F4_WORLD_CONVERT_BUILD_TESTS "Build unit tests" ON)
if(F4_WORLD_CONVERT_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
