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
# Reuse the same FetchContent blocks as f4-models-viewer (guarded by
# FetchContent_GetProperties so we don't redeclare them).
include(FetchContent)

FetchContent_GetProperties(raylib)
if(NOT raylib_POPULATED)
    set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
    set(BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
    set(BUILD_GAMES    OFF CACHE BOOL "" FORCE)
    set(MACOS_FATLIBRARY OFF CACHE BOOL "" FORCE)
    set(USE_AUDIO OFF CACHE BOOL "" FORCE)
    set(USE_WAYLAND OFF CACHE BOOL "" FORCE)
    set(CMAKE_POLICY_VERSION_MINIMUM 3.5 CACHE STRING "" FORCE)
    FetchContent_Declare(
        raylib
        GIT_REPOSITORY https://github.com/raysan5/raylib.git
        GIT_TAG        5.0
        GIT_SHALLOW    TRUE
    )
    FetchContent_MakeAvailable(raylib)
endif()

set(IMGUI_DISABLE_OBSOLETE_FUNCTIONS OFF CACHE BOOL "" FORCE)
FetchContent_GetProperties(imgui)
if(NOT imgui_POPULATED)
    FetchContent_Declare(
        imgui
        GIT_REPOSITORY https://github.com/ocornut/imgui.git
        GIT_TAG        v1.91.5
        GIT_SHALLOW    TRUE
    )
    FetchContent_MakeAvailable(imgui)
endif()
set(IMGUI_DIR ${imgui_SOURCE_DIR})

FetchContent_GetProperties(rlimgui)
if(NOT rlimgui_POPULATED)
    FetchContent_Declare(
        rlimgui
        GIT_REPOSITORY https://github.com/raylib-extras/rlImGui.git
        GIT_TAG        9acdbbf
    )
    FetchContent_MakeAvailable(rlimgui)
endif()
set(RLIMGUI_DIR ${rlimgui_SOURCE_DIR})

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
configure_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/scenarios/kunsan_parking.json.in
    ${CMAKE_BINARY_DIR}/scenarios/kunsan_parking.json
    @ONLY
)

# ── Tests ──────────────────────────────────────────────────────────────────
option(F4_SCENARIO_PLAYER_BUILD_TESTS "Build f4-scenario-player unit tests" ON)
if(F4_SCENARIO_PLAYER_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
