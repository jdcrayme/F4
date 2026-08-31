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
#   feature_mesh     — entity_type → vis_type → mesh → DrawMesh (used by
#                     the 2D canvas and the 3D ground-layout panel to draw
#                     real KoreaObj feature models at FeatureEntryState
#                     offsets)
#   symbols          — procedural MIL-STD-2525-style symbol drawing
#   symbol_library   — data-driven symbol vocabulary (lines + polygons
#                     as point lists, persisted to JSON via f4-json).
#                     The eventual refactor of symbols.cpp will load
#                     this library into a dictionary and render via
#                     draw_library_symbol() for a given key.
#   entity_render    — entity-level rendering: RenderEntity() inspects
#                     components and dispatches to feature_mesh, symbols,
#                     etc. RenderEntityIcon() renders 2D map symbols
#   render_resources — RenderResources: owns the per-viewer GPU caches
#                     (lit shader, textures, meshes, default material,
#                     airfield geometry) + lazy mesh building
#   scene_draw       — scene composition: draw_ground (flat plane/grid),
#                     draw_entity_meshes (VisualModelComponent path),
#                     draw_airfield_geometry
#   world_renderer   — render_world(): the single entry point that renders
#                     a chunk of the world from a SceneDescription camera
#   world_camera     — world-space camera helpers: make_perspective_camera,
#                     make_topdown_ortho_camera, FreeCamera controller
#
# Dependencies:
#   PUBLIC:  f4-models (for ModelGeometry, ColorBank, ModelDatabase types)
#            f4-entities (for UnitClass used by symbols)
#            f4-world-convert (for ClassTable used by feature_mesh)
#            raylib (public headers include <raylib.h>, <raymath.h>)
#   PRIVATE: imgui (for symbols ImGui draw-list path + orbit_camera input guard)
#            f4-json (for symbol_library JSON I/O — Reader/Writer only)
# ============================================================================

# ── Third-party deps (Raylib + Dear ImGui) ────────────────────────────────────
# Consolidated into a shared cmake module in the 2026 cleanup pass.
include(f4_raylib_deps)

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
    src/feature_mesh.cpp
    src/symbols.cpp
    src/symbol_library.cpp
    src/entity_render.cpp
    src/ground_layout_models.cpp
    src/render_resources.cpp
    src/scene_draw.cpp
    src/world_renderer.cpp
    src/world_camera.cpp
    src/terrain_mesh.cpp
    src/terrain_chunks.cpp
    src/terrain_tile_cache.cpp
    src/terrain_shader.cpp
    src/world_view.cpp
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
        f4-world-convert
        f4-terrain          # Path B1: terrain_mesh.hpp includes f4/terrain/terrain_data.hpp
        raylib
    PRIVATE
        f4-json
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

# Developer tool: average-color probe for debugging screenshots.
add_executable(png_probe tools/png_probe.cpp)
target_link_libraries(png_probe PRIVATE f4_warnings raylib)

# Paint the 2D map art (L5 posts -> far tiles) + the MEA color map to a
# PNG for visual comparison against real geography.
add_executable(paint-map-check tools/paint_map_check.cpp)
target_link_libraries(paint-map-check PRIVATE f4-terrain f4_warnings raylib)
