cmake_minimum_required(VERSION 3.20)
project(f4-campaign VERSION 0.1.0 LANGUAGES CXX DESCRIPTION
        "Headless dynamic campaign: mission profiles, tasking tick, attrition")

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Static library: the campaign dynamics layer (Architecture Proposal
# Phase 4). Dependencies follow the f4-ai boundary discipline — domain
# libs only (f4-entities, f4-world, f4-messaging, f4-geo, f4-math as the
# slices need them) plus the base-layer f4-json (profile loader) and
# f4-io (read_file). NOTHING here links f4-world-convert, f4-models, or
# any viewer: the campaign reads the typed WorldState through the same
# interfaces everything else uses.
add_library(f4-campaign STATIC
    src/mission_type.cpp
    src/mission_profile.cpp
    src/campaign.cpp
    src/result_ledger.cpp
    src/world_writeback.cpp
)
target_link_libraries(f4-campaign PUBLIC f4_warnings)

target_include_directories(f4-campaign
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)
# Domain boundary: f4-world (IDataSource interfaces ONLY — the library never
# touches EntityWorld internals or f4-world-convert), f4-messaging (intent
# publishing), and the base-layer f4-json + f4-io.
target_link_libraries(f4-campaign PUBLIC f4-world f4-messaging
                                  PRIVATE f4-json f4-io)

# ── Build-time MissionProfiles.json generation ──────────────────────────────
# The FreeFalcon MissionData[] table is build-time generated JSON (the
# f4-convert golden-fixture discipline): the generator self-validates its
# output through the library's loader, so a table that fails validation
# never reaches the build. Tests and future consumers load it from
# F4_GENERATED_FIXTURES_DIR (root CMakeLists).
add_executable(mission_profiles_gen tools/profiles_gen.cpp)
target_link_libraries(mission_profiles_gen PRIVATE f4-campaign f4-json f4-io)

set(F4_GENERATED_CAMPAIGN_DIR "${CMAKE_BINARY_DIR}/generated_campaign")
file(MAKE_DIRECTORY "${F4_GENERATED_CAMPAIGN_DIR}")
# Own directory, NOT F4_GENERATED_FIXTURES_DIR: that directory is
# f4-data's aircraft-config contract surface (its fixture-loader test
# validates EVERY *.json there as an aircraft config — MissionProfiles.json
# correctly fails that validation and must not sit next to them).
# FORCE: this path is owned by f4-campaign, and a cache entry pointing at
# the old generated_fixtures location must not survive a reconfigure.
set(F4_MISSION_PROFILES_JSON
    "${F4_GENERATED_CAMPAIGN_DIR}/MissionProfiles.json"
    CACHE PATH "Path to the generated MissionProfiles.json"
    FORCE)

add_custom_command(
    OUTPUT  "${F4_MISSION_PROFILES_JSON}"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${F4_GENERATED_CAMPAIGN_DIR}"
    COMMAND mission_profiles_gen "${F4_MISSION_PROFILES_JSON}"
    DEPENDS mission_profiles_gen
    COMMENT "Generating MissionProfiles.json (41 FreeFalcon mission types)"
    VERBATIM
)
add_custom_target(mission_profiles_fixture ALL
    DEPENDS "${F4_MISSION_PROFILES_JSON}")

# Tests
option(F4_CAMPAIGN_BUILD_TESTS "Build unit tests" ON)
if(F4_CAMPAIGN_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
