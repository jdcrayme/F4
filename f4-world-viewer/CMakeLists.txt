cmake_minimum_required(VERSION 3.20)
project(f4-world-viewer VERSION 0.1.0 LANGUAGES CXX DESCRIPTION "Interactive Raylib + Dear ImGui viewer for F4 world data")

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# ============================================================================
# Third-party dependencies (Raylib + Dear ImGui + rlImGui bindings).
# Consolidated into a shared cmake module in the 2026 cleanup pass.
# ============================================================================
include(f4_raylib_deps)

# CRITICAL FIX (was the "multi-feature layouts render nothing" bug):
# Raylib's default RL_CULL_DISTANCE_FAR is 1000.0, which is fine for tiny
# model-viewer scenes (cam distance 80-250 ft) but WAY too small for the
# Ground Layout 3D panel. Airfield bbox diagonals routinely exceed 7000 ft
# (e.g. objective 125 "02_20 Airbase 2" has diag ~7471 ft), and
# default_orbit_for_bbox sets camera distance = max(diag * 1.5, 500) ≈
# 11200 ft — well beyond the 1000 ft default far clip. The result: every
# 3D draw call inside BeginMode3D is clipped out, and the rendered
# RenderTexture2D ends up as solid background color (only the 2D overlay
# text/labels render, because BeginMode3D's frustum doesn't apply to 2D
# draws). Single-feature layouts (Town, Depot) sometimes appeared to
# render because their bbox is small enough that the camera distance stays
# under 1000 ft.
#
# The far-clip is hard-coded in raylib's src/config.h (NOT guarded by
# #ifndef, so a -D compile flag is silently re-defined to 1000.0). We
# must patch config.h directly after FetchContent. 100000 ft covers the
# largest theater-scale objective.
file(READ "${raylib_SOURCE_DIR}/src/config.h" _raylib_config)
string(REPLACE
    "#define RL_CULL_DISTANCE_FAR              1000.0"
    "#define RL_CULL_DISTANCE_FAR          100000.0"
    _raylib_config "${_raylib_config}")
file(WRITE "${raylib_SOURCE_DIR}/src/config.h" "${_raylib_config}")

# tinyfiledialogs — native OS file/folder pickers on Windows, macOS, Linux.
# Replaces the ad-hoc ImGui text-input modal we shipped originally. Single
# .c file, no external deps (uses Win32 dialog on Windows, zenity/kdialog
# on Linux, AppleScript on macOS).
#
# Vendored under third_party/tinyfiledialogs/ rather than FetchContent'd
# because the upstream repo (SourceForge git) doesn't tag releases and
# we want a known-good snapshot. v3.21.3 (Feb 2026) — Zlib license.
# Update by replacing the .c/.h files from a fresh clone of:
#   git clone http://git.code.sf.net/p/tinyfiledialogs/code
set(TINYFILEDIALOGS_DIR ${CMAKE_CURRENT_SOURCE_DIR}/third_party/tinyfiledialogs)

# Build Dear ImGui sources directly into our viewer library (avoids dealing
# with imgui's own CMake target which pulls in many backends we don't need).
set(IMGUI_SOURCES
    ${IMGUI_DIR}/imgui.cpp
    ${IMGUI_DIR}/imgui_draw.cpp
    ${IMGUI_DIR}/imgui_tables.cpp
    ${IMGUI_DIR}/imgui_widgets.cpp
)
set(RLIMGUI_SOURCES
    ${RLIMGUI_DIR}/rlImGui.cpp
)

# tinyfiledialogs is a single .c file. We compile it with C11 and link it
# directly into the viewer library — no separate target needed.
set(TINYFILEDIALOGS_SOURCES
    ${TINYFILEDIALOGS_DIR}/tinyfiledialogs.c
)

# ============================================================================
# Static library: rendering logic (terrain tiles, objective icons, inspector
# panel). Linked by the CLI exe and (future) by editor embeds.
#
# Source layout (after the god-file split — see viewer_state.hpp for the
# per-concern breakdown):
#   viewer_app.cpp      — lifecycle (ctor/dtor/run) + set_initial_camera
#                         + schedule_screenshot
#   symbols.cpp         — procedural symbol drawing (replaces icons.cpp —
#                         no PNG assets, see symbols.hpp)
#   camera.cpp          — Impl world<->screen transforms + fit_to_world
#   file_ops.cpp        — load_*_json / import_cam_archive / import_terrain_binary
#   install_flow.cpp    — set_install_path* / open_campaign_dialog /
#                         load_campaign_from_install / install_diagnostics_text
#                         / open_install_diagnostics / open_hex_inspector_with_file
#                         / open_snapshot_dialog / snapshot_install_files
#   diagnostics.cpp     — build_install_diagnostics / build_campaign_load_error
#   snapshot.cpp        — build_install_snapshot / write_install_snapshot
#   canvas.cpp          — handle_input / draw_canvas (Raylib 2D render)
#   imgui_panels.cpp    — draw_imgui (menus, layers, legend, inspector, modals)
#   ground_layout_view.cpp   — 2D ImGui "Ground Layout" panel (top-down
#                               line+dot rendering, per-list type colors).
#   ground_layout_3d.cpp     — "Ground Layout 3D" ImGui panel: renders the
#                               AirfieldGeometry3D into an offscreen
#                               RenderTexture2D via BeginMode3D, displays
#                               via rlImGuiImageSize, captures mouse input
#                               for orbit/zoom. The geometry builder +
#                               per-layer draw live in f4-renderer
#                               (ground_layout_models.cpp + scene_draw.cpp).
# Plus the pre-existing hex/ (Hex Inspector) + file_dialog.cpp (tinyfiledialogs
# wrapper) + settings.cpp (persisted settings JSON).
# ============================================================================
add_library(f4_world_viewer STATIC
    src/viewer_app.cpp
    src/camera.cpp
    src/file_ops.cpp
    src/install_flow.cpp
    src/diagnostics.cpp
    src/snapshot.cpp
    src/canvas.cpp
    src/imgui_panels.cpp
    src/inspector_panel.cpp
    src/ground_layout_view.cpp
    src/ground_layout_3d.cpp
    src/campaign_and_teams_view.cpp
    src/file_dialog.cpp
    src/settings.cpp
    src/replay_mode.cpp
    src/replay_view.cpp
    src/hex/hex_model.cpp
    src/hex/decoders.cpp
    src/hex/hex_inspector.cpp
    src/class_table_browser.cpp
    src/symbol_creator.cpp
    ${IMGUI_SOURCES}
    ${RLIMGUI_SOURCES}
    ${TINYFILEDIALOGS_SOURCES}
)
target_link_libraries(f4_world_viewer PUBLIC f4_warnings)
target_include_directories(f4_world_viewer
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${IMGUI_DIR}
        ${IMGUI_DIR}/backends
        ${RLIMGUI_DIR}
        ${TINYFILEDIALOGS_DIR}
)
target_link_libraries(f4_world_viewer PUBLIC
    f4-terrain
    f4-world
    f4-world-convert
    f4-terrain-convert
    f4-install
    f4-models
    f4-json
    f4-renderer
    f4-recorder
)
target_link_libraries(f4_world_viewer PRIVATE raylib)
# tinyfiledialogs needs to be compiled as C (it's a .c file). Set the
# language explicitly on the source so CMake doesn't try to compile it
# as C++ just because the rest of the target is C++.
set_source_files_properties(${TINYFILEDIALOGS_SOURCES} PROPERTIES LANGUAGE C)

# Visual Studio: set the working directory to the build dir so relative
# fixture paths resolve correctly when running via F5.
set_target_properties(f4_world_viewer PROPERTIES
    VS_DEBUGGER_WORKING_DIRECTORY "${CMAKE_BINARY_DIR}"
)

# Alias for namespaced consumption.
add_library(F4::WorldViewer ALIAS f4_world_viewer)

# Tests — settings module only (no raylib dep). The full viewer is
# exercised via the screenshot smoke test in cli/main.cpp's --screenshot
# path, run by hand or by future CI.
option(F4_WORLD_VIEWER_BUILD_TESTS "Build unit tests" ON)
if(F4_WORLD_VIEWER_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

# ============================================================================
# CLI tool: f4-world-viewer [world.json] [terrain.json]
# ============================================================================
add_executable(f4-world-viewer cli/main.cpp)
target_link_libraries(f4-world-viewer PRIVATE f4_world_viewer)
set_target_properties(f4-world-viewer PROPERTIES
    VS_DEBUGGER_WORKING_DIRECTORY "${CMAKE_BINARY_DIR}"
    VS_DEBUGGER_COMMAND_ARGUMENTS "save1.world.json korea.terrain.json"
)

# NOTE: The viewer no longer ships or loads any PNG icon assets. Every
# objective and unit is drawn procedurally from raylib primitives — see
# src/symbols.cpp. The assets/icons/ directory is kept in the source tree
# for historical reference only and is NOT copied to the build directory.
