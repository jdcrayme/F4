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
FetchContent_Declare(
    imgui
    GIT_REPOSITORY https://github.com/ocornut/imgui.git
    GIT_TAG        v1.91.5
    GIT_SHALLOW    TRUE
)
FetchContent_GetProperties(imgui)
if(NOT imgui_POPULATED)
    FetchContent_Populate(imgui)
    set(IMGUI_DIR ${imgui_SOURCE_DIR})
endif()

# rlImGui — pinned to commit 9acdbbf (uses ImTextureID, compatible with
# ImGui 1.91.x). The 1.92 migration that introduced ImTextureData landed
# in commit dc7479b.
FetchContent_Declare(
    rlimgui
    GIT_REPOSITORY https://github.com/raylib-extras/rlImGui.git
    GIT_TAG        9acdbbf
)
FetchContent_GetProperties(rlimgui)
if(NOT rlimgui_POPULATED)
    FetchContent_Populate(rlimgui)
    set(RLIMGUI_DIR ${rlimgui_SOURCE_DIR})
endif()

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

# ============================================================================
# Static library: rendering logic (terrain tiles, objective icons, inspector
# panel). Linked by the CLI exe and (future) by editor embeds.
# ============================================================================
add_library(f4_world_viewer STATIC
    src/viewer_app.cpp
    ${IMGUI_SOURCES}
    ${RLIMGUI_SOURCES}
)
target_include_directories(f4_world_viewer
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
    PRIVATE
        ${IMGUI_DIR}
        ${IMGUI_DIR}/backends
        ${RLIMGUI_DIR}
)
target_link_libraries(f4_world_viewer PUBLIC
    f4-terrain
    f4-world
    f4-world-convert
    f4-terrain-convert
)
target_link_libraries(f4_world_viewer PRIVATE raylib)

# Visual Studio: set the working directory to the build dir so relative
# fixture paths resolve correctly when running via F5.
set_target_properties(f4_world_viewer PROPERTIES
    VS_DEBUGGER_WORKING_DIRECTORY "${CMAKE_BINARY_DIR}"
)

# Alias for namespaced consumption.
add_library(F4::WorldViewer ALIAS f4_world_viewer)

# ============================================================================
# CLI tool: f4-world-viewer [world.json] [terrain.json]
# ============================================================================
add_executable(f4-world-viewer cli/main.cpp)
target_link_libraries(f4-world-viewer PRIVATE f4_world_viewer)
set_target_properties(f4-world-viewer PROPERTIES
    VS_DEBUGGER_WORKING_DIRECTORY "${CMAKE_BINARY_DIR}"
    VS_DEBUGGER_COMMAND_ARGUMENTS "save1.world.json korea.terrain.json"
)
