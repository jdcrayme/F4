# f4_raylib_deps.cmake
#
# Shared FetchContent configuration for Raylib + Dear ImGui + rlImGui.
# Used by f4-renderer, f4-world-viewer, f4-models-viewer, and
# f4-scenario-player. Consolidated from 4 duplicated FetchContent
# blocks in the 2026 cleanup pass.
#
# Call include(f4_raylib_deps) from each module's CMakeLists.txt
# after include(FetchContent). This script:
#
#   1. Declares + makes available raylib 5.0 (if not already populated)
#   2. Declares + makes available Dear ImGui v1.91.5 (if not already)
#   3. Declares + makes available rlImGui @ commit 9acdbbf (if not already)
#   4. Sets IMGUI_DIR and RLIMGUI_DIR for downstream use
#
# Guards with FetchContent_GetProperties so it is idempotent across
# multiple includes in the same build tree.

include(FetchContent)

# ── Raylib ──────────────────────────────────────────────────────────────────────
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

# ── Dear ImGui ──────────────────────────────────────────────────────────────────
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

# ── rlImGui (Raylib backend for ImGui) ─────────────────────────────────────────
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
