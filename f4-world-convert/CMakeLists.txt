cmake_minimum_required(VERSION 3.20)
project(f4-world-convert VERSION 0.1.0 LANGUAGES CXX DESCRIPTION "FreeFalcon .cam campaign archive -> JSON converter")

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Static library: LZSS decompressor adapter + .cam container parser + campaign
# decoder + JSON emitter. Dependencies:
#   - f4-lzss: canonical LZSS decompression (f4::lzss::decompress). The
#     local lzss.cpp is now a thin adapter that delegates to f4-lzss
#     instead of duplicating the algorithm.
#   - f4-install: find_class_table() delegates install-aware class-table
#     resolution to f4::install::Installation.
#   - f4-io: shared Cursor binary reader + read_file helper.
#   - f4-json: shared escape_string() used by world_json.cpp (eliminates
#     the local json_escape duplicate).
# nlohmann/json is available to f4-data/f4-world on the consuming side
# (the world_json emitter is hand-rolled to match f4-convert's pattern).
add_library(f4-world-convert STATIC
    src/lzss.cpp
    src/cam_archive.cpp
    src/campaign_decoder.cpp
    src/cmp_encoder.cpp
    src/campaign_json.cpp
    src/campaign_saver.cpp
    src/save_writeback.cpp
    src/cam_writer.cpp
    src/objective_decoder.cpp
    src/objective_encoder.cpp
    src/unit_decoder.cpp
    src/unit_encoder.cpp
    src/team_decoder.cpp
    src/team_encoder.cpp
    src/class_table.cpp
    src/theater_data.cpp
    src/world_json.cpp
    src/scenario_pack.cpp
    src/campaign_initializer.cpp
)
target_link_libraries(f4-world-convert PUBLIC f4_warnings)

target_include_directories(f4-world-convert
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)
target_link_libraries(f4-world-convert PRIVATE f4-lzss f4-install f4-io f4-json)

# CLI tool
add_executable(cam2json cli/cam2json.cpp)
target_link_libraries(cam2json PRIVATE f4-world-convert f4-assets f4-io)

# CLI tool: json2cam — reassemble a .cam from a world JSON produced by
# `cam2json --preserve-subfiles` (the save-write round-trip). The JSON
# parsing lives in the library (cam_from_world_json); the CLI is a thin
# read -> assemble -> write shell.
add_executable(json2cam cli/json2cam.cpp)
target_link_libraries(json2cam PRIVATE f4-world-convert f4-io)

# CLI tool: ct2json — convert FALCON4.ct (binary class table) to open JSON.
# Tranche 0a.1 (NO_BINARY_RUNTIME_PLAN.md): eliminates the binary .ct from
# the runtime. ClassTable::load_json() is the consumer-side counterpart.
add_executable(ct2json cli/ct2json.cpp)
target_link_libraries(ct2json PRIVATE f4-world-convert f4-json f4-io f4-assets)

# CLI tool: wcd2json — convert Falcon4.WCD (binary campaign weapon-class
# table) to open JSON. The real-data tier's weapon export
# (COMBAT_CHAIN_PLAN.md §5): f4-weapons' wcd_weapon_data loader is the
# runtime consumer.
add_executable(wcd2json cli/wcd2json.cpp)
target_link_libraries(wcd2json PRIVATE f4-world-convert f4-json f4-io f4-assets)

# CLI tool: campinit — CAMP-INIT-1's create-from-parameters face. Generates
# a fresh .cam (Task-70 encoder stack + CamWriter) and its world JSON (the
# existing reader's own projection) from a scenario pack.
add_executable(campinit cli/campinit.cpp)
target_link_libraries(campinit PRIVATE f4-world-convert f4-io)
target_compile_definitions(campinit PRIVATE
    F4_CAMPINIT_CT_1="${CMAKE_CURRENT_SOURCE_DIR}/../../Data/Classes/falcon4.ct.json"
    F4_CAMPINIT_CT_2="${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/falcon4.ct.json")

# ── CAMP-INIT-1 generated war fixtures ────────────────────────────────
# The committed packs (tests/fixtures/packs/) generated at build time into
# generated_world_fixtures/ — the same discipline as f4-campaign's
# mission_profiles_fixture. f4-simulation's war-harness test consumes the
# generated WORLD JSON (which is the decode of the generated .cam — the
# full loop closes at build time, not by a second projection).
set(F4_CAMPINIT_PACKS_DIR "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/packs")
set(F4_GENERATED_INIT_DIR "${CMAKE_BINARY_DIR}/generated_world_fixtures")
file(MAKE_DIRECTORY "${F4_GENERATED_INIT_DIR}")
set(F4_CAMPINIT_FIXTURES
    "${F4_GENERATED_INIT_DIR}/campinit_small.world.json"
    "${F4_GENERATED_INIT_DIR}/campinit_medium.world.json"
    "${F4_GENERATED_INIT_DIR}/campinit_large.world.json"
    "${F4_GENERATED_INIT_DIR}/campinit_twinwars.world.json")
set(F4_CAMPINIT_FIXTURES_DIR "${F4_GENERATED_INIT_DIR}"
    CACHE PATH "Directory of the campinit-generated war fixtures" FORCE)
foreach(pack_name small medium large twinwars)
    add_custom_command(
        OUTPUT  "${F4_GENERATED_INIT_DIR}/campinit_${pack_name}.world.json"
                "${F4_GENERATED_INIT_DIR}/campinit_${pack_name}.cam"
        COMMAND campinit --pack "${F4_CAMPINIT_PACKS_DIR}/${pack_name}.pack.json"
                --out-cam "${F4_GENERATED_INIT_DIR}/campinit_${pack_name}.cam"
                --out-world "${F4_GENERATED_INIT_DIR}/campinit_${pack_name}.world.json"
        DEPENDS campinit "${F4_CAMPINIT_PACKS_DIR}/${pack_name}.pack.json"
        COMMENT "CAMP-INIT-1: generating campinit_${pack_name} (.cam + world JSON)"
        VERBATIM)
endforeach()
add_custom_target(campinit_fixtures ALL
    DEPENDS ${F4_CAMPINIT_FIXTURES})

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
