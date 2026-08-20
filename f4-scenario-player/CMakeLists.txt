cmake_minimum_required(VERSION 3.20)
project(f4-scenario-player VERSION 0.1.0 LANGUAGES CXX DESCRIPTION "Host executable that plays F4 scenarios and renders them with Raylib")

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# ---------------------------------------------------------------------------
# f4-scenario-player — the host executable for the F4 scenario player.
#
# Owns the Simulation (EntityWorld + MessageBus + ModelDatabase + tick
# loop, via f4-simulation) and a Raylib renderer that draws the airport
# geometry + aircraft each frame. See Docs/SCENARIO_PLAYER_PLAN.md.
#
# The library half (f4_scenario_player_lib) is built as a static library
# so the test executable can link against it without recompiling the
# sources. The CLI half (f4-scenario-player) is the user-facing binary.
#
# Dependencies:
#   - f4-simulation (EntityWorld + MessageBus + ModelDatabase + tick loop)
#   - f4-world      (GroundLayoutList types — for Phase 2 real-airbase support)
#   - raylib, imgui, rlimgui (same vendored versions as f4-models-viewer)
# ---------------------------------------------------------------------------

# ── Third-party deps (Raylib + Dear ImGui + rlImGui) ───────────────────────
# Consolidated into a shared cmake module in the 2026 cleanup pass.
include(f4_raylib_deps)

set(IMGUI_SOURCES
    ${IMGUI_DIR}/imgui.cpp
    ${IMGUI_DIR}/imgui_draw.cpp
    ${IMGUI_DIR}/imgui_tables.cpp
    ${IMGUI_DIR}/imgui_widgets.cpp
    ${IMGUI_DIR}/imgui_demo.cpp
)
set(RLIMGUI_SOURCES
    ${RLIMGUI_DIR}/rlImGui.cpp
)

# ── Static library: f4_scenario_player_lib ─────────────────────────────────
add_library(f4_scenario_player_lib STATIC
    src/player_app.cpp
    src/renderer.cpp
    src/airport_geometry.cpp
    src/radio_log.cpp
    ${IMGUI_SOURCES}
    ${RLIMGUI_SOURCES}
)
target_link_libraries(f4_scenario_player_lib PUBLIC f4_warnings)
target_include_directories(f4_scenario_player_lib
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${IMGUI_DIR}
        ${IMGUI_DIR}/backends
        ${RLIMGUI_DIR}
)
target_link_libraries(f4_scenario_player_lib PUBLIC
    f4-simulation
    f4-world
    f4-renderer
)
target_link_libraries(f4_scenario_player_lib PRIVATE raylib)
add_library(F4::ScenarioPlayer ALIAS f4_scenario_player_lib)

# ── CLI tool: f4-scenario-player <scenario.json> ───────────────────────────
add_executable(f4-scenario-player cli/main.cpp)
target_link_libraries(f4-scenario-player PRIVATE f4_scenario_player_lib)
set_target_properties(f4-scenario-player PROPERTIES
    VS_DEBUGGER_WORKING_DIRECTORY "${CMAKE_BINARY_DIR}"
)

# ── Configure the Kunsan scenario JSON at build time ───────────────────────
# Substitutes @F4_SOURCE_DIR@ and @F4_BINARY_DIR@ so the scenario can
# reference KoreaObj.HDR/.LOD/.TEX (in temp/) and f16.json (in
# build/generated_fixtures/) without hardcoding absolute paths.

# Class table for the DIGI mission's feature models: the install's real
# FALCON4.ct when available, else the bundled fixture (fewer rows -> more
# features skipped, but the scenario still runs). Must be set BEFORE the
# configure_file calls below substitute @F4_DIGI_CLASS_TABLE@.
if(F4_INSTALL AND EXISTS "${F4_INSTALL}/terrdata/objects/FALCON4.ct")
    set(F4_DIGI_CLASS_TABLE "${F4_INSTALL}/terrdata/objects/FALCON4.ct")
else()
    set(F4_DIGI_CLASS_TABLE "${CMAKE_SOURCE_DIR}/f4-world-convert/tests/fixtures/FALCON4.ct")
endif()

configure_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/scenarios/kunsan_parking.json.in
    ${CMAKE_BINARY_DIR}/scenarios/kunsan_parking.json
    @ONLY
)
configure_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/scenarios/digi_full_mission.json.in
    ${CMAKE_BINARY_DIR}/scenarios/digi_full_mission.json
    @ONLY
)
# The DIGI mission scenario needs the real world JSON (generated from the
# Falcon install when F4_INSTALL is set).
if(TARGET korea-real-world-json)
    add_dependencies(f4_scenario_player_lib korea-real-world-json)
endif()

# ── Tests ──────────────────────────────────────────────────────────────────
option(F4_SCENARIO_PLAYER_BUILD_TESTS "Build f4-scenario-player unit tests" ON)
if(F4_SCENARIO_PLAYER_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
