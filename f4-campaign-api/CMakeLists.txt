# f4-campaign-api — the campaign engine's host contract.
#
# Header-only (CAMP_HOST_PLAN.md §6): the four surfaces (lifecycle,
# queries, commands, events) plus the line protocol, as pure data over
# f4-json. DEPENDENCY RULE: f4-json and the standard library ONLY — a
# host in any language that speaks the JSON wire can implement a client,
# and no engine type leaks into a link closure just to name the
# contract. The engine-side adapter (EngineSessionHost) lives in
# f4-simulation; the reference host (campaignd) is its app.

cmake_minimum_required(VERSION 3.20)
project(f4-campaign-api VERSION 0.1.0 LANGUAGES CXX
        DESCRIPTION "The campaign engine's host contract (CAMP_HOST_PLAN.md)")

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_library(f4-campaign-api INTERFACE)
target_link_libraries(f4-campaign-api INTERFACE f4_warnings)

target_include_directories(f4-campaign-api INTERFACE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)

# f4-json (Reader/Writer) — header-only, dependency-free. The contract's
# canonical byte-stable encodings are built on its Writer (the
# CampaignResultLedger::to_json discipline, generalized).
target_link_libraries(f4-campaign-api INTERFACE f4-json)

# Tests — the vocabulary is pinned BEFORE anything emits it (events) and
# the protocol is proven against a mock session with no engine at all.
option(F4_CAMPAIGN_API_BUILD_TESTS "Build f4-campaign-api unit tests" ON)
if(F4_CAMPAIGN_API_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
