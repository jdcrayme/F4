cmake_minimum_required(VERSION 3.20)
project(f4-renderer VERSION 0.1.0 LANGUAGES CXX DESCRIPTION "Shared Raylib rendering library for F4 viewer apps")

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# ============================================================================
# f4-renderer — shared rendering library
#
# Consolidates the 4x-duplicated rendering code from f4-models-viewer,
# f4-scenario-player, and f4-world-viewer into a single library:
#
#   coord_transform  — LH Y-up / ENU → RH Y-up conversion (pure math)
#   orbit_camera     — spherical orbit camera (Config + input handling)
#   lit_shader       — RAII Lambertian shader (GLSL 330)
#   mesh_builder     — ModelGeometry → Raylib ::Mesh conversion
#   texture_cache    — GPU texture upload/cache
#   draw_3d          — opaque/alpha sort + DrawMesh loop
#   symbols          — procedural MIL-STD-2525-style symbol drawing
#
# Dependencies:
#   PUBLIC:  f4-models (for ModelGeometry, ColorBank, ModelDatabase types)
#            f4-entities (for UnitClass used by symbols)
#            raylib (public headers include <raylib.h>, <raymath.h>)
#   PRIVATE: imgui (for symbols ImGui draw-list path + orbit_camera input guard)
# ============================================================================

# ── Third-party deps (Raylib + Dear ImGui) ────────────────────────────────────
# Reuse the same FetchContent blocks as f4-models-viewer (guarded so we
# don't redeclare them).
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

# ============================================================================
# Static library: f4-renderer
# ============================================================================
add_library(f4-renderer STATIC
    src/coord_transform_dummy.cpp   # coord_transform is header-only; this exists so CMake has at least one .cpp
    src/orbit_camera.cpp
    src/lit_shader.cpp
    src/mesh_builder.cpp
    src/texture_cache.cpp
    src/draw_3d.cpp
    src/symbols.cpp
)
target_link_libraries(f4-renderer PUBLIC f4_warnings)
target_include_directories(f4-renderer
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${IMGUI_DIR}
        ${IMGUI_DIR}/backends
)
target_link_libraries(f4-renderer
    PUBLIC
        f4-models
        f4-entities
        raylib
)

add_library(F4::Renderer ALIAS f4-renderer)

# ============================================================================
# Tests
# ============================================================================
option(F4_RENDERER_BUILD_TESTS "Build f4-renderer unit tests" ON)
if(F4_RENDERER_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
