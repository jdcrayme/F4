cmake_minimum_required(VERSION 3.20)
project(f4-world-viewer VERSION 0.1.0 LANGUAGES CXX DESCRIPTION "Interactive Raylib + Dear ImGui viewer for F4 world data")

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# ============================================================================
# Third-party dependencies (Raylib + Dear ImGui + rlImGui bindings).
# Fetched at configure time via FetchContent. On Windows this Just Works
# with Visual Studio 17 2022 generator; no vcpkg or manual install needed.
# ============================================================================
include(FetchContent)

# Raylib — turn off everything we don't need to keep build times reasonable.
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(BUILD_GAMES    OFF CACHE BOOL "" FORCE)
set(MACOS_FATLIBRARY OFF CACHE BOOL "" FORCE)
set(USE_AUDIO OFF CACHE BOOL "" FORCE)
# PLATFORM_DESKTOP is the default; we don't need PLATFORM_WEB / PLATFORM_ANDROID.
set(USE_WAYLAND OFF CACHE BOOL "" FORCE)
# Raylib 5.0's CMakeLists requires CMake 3.5+, but its top-level
# cmake_minimum_required() predates the policy CMake 4.x enforces. Override
# the policy version so the subproject configures cleanly.
set(CMAKE_POLICY_VERSION_MINIMUM 3.5 CACHE STRING "" FORCE)
FetchContent_Declare(
    raylib
    GIT_REPOSITORY https://github.com/raysan5/raylib.git
    GIT_TAG        5.0
    GIT_SHALLOW    TRUE
)
FetchContent_MakeAvailable(raylib)

# Dear ImGui — core + Raylib backend bindings from raylib-extras.
# Pin ImGui to v1.91.5 (last release before the 1.92 font/texture system
# rewrite). rlImGui must be pinned to a commit that predates the 1.92
# migration — see commit dc7479b on rlImGui main.
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

# rlImGui — pinned to commit 9acdbbf (uses ImTextureID, compatible with
# ImGui 1.91.x). The 1.92 migration that introduced ImTextureData landed
# in commit dc7479b.
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
    ${IMGUI_DIR}/imgui_demo.cpp
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
#                         (TEMPORARY diagnostic tool for the static-data
#                          parsing milestone; see snapshot.hpp)
#   canvas.cpp          — handle_input / draw_canvas (Raylib 2D render)
#   imgui_panels.cpp    — draw_imgui (menus, layers, legend, inspector, modals)
#                         + open_file_dialog (legacy text-input modal)
# Plus the pre-existing hex/ (Hex Inspector) + file_dialog.cpp (tinyfiledialogs
# wrapper) + settings.cpp (persisted settings JSON).
# ============================================================================
add_library(f4_world_viewer STATIC
    src/viewer_app.cpp
    src/symbols.cpp
    src/camera.cpp
    src/file_ops.cpp
    src/install_flow.cpp
    src/diagnostics.cpp
    src/snapshot.cpp
    src/canvas.cpp
    src/imgui_panels.cpp
    src/inspector_panel.cpp
    src/ground_layout_view.cpp
    src/campaign_and_teams_view.cpp
    src/file_dialog.cpp
    src/settings.cpp
    src/hex/hex_model.cpp
    src/hex/decoders.cpp
    src/hex/hex_inspector.cpp
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
    f4-json
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
