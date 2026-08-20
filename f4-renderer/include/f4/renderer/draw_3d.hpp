// f4-renderer/include/f4/renderer/draw_3d.hpp
//
// 3D drawing helpers: the common pattern of sorting meshes into
// opaque/alpha order, setting up the lit shader, disabling backface
// culling, and drawing each mesh with its material.
//
// Consolidated from 4 duplicated implementations.

#pragma once

// (declared in header; defined in draw_3d.cpp)
// Replaces the projection built by BeginMode3D, whose far plane is the
// rlgl default (1000 units) — far too small for theater-scale scenes in
// feet (a single airfield spans ~9000 ft). Call immediately after
// BeginMode3D. Depth precision stays sane: near 1 ft / far 200,000 ft.
#include <raylib.h>

namespace f4::renderer {
/// Extend the active 3D-mode projection's far plane to `far_ft`.
void extend_far_plane(const Camera3D& camera, float near_ft, float far_ft);
} // namespace f4::renderer


#include <f4/renderer/lit_shader.hpp>
#include <f4/renderer/mesh_builder.hpp>
#include <f4/renderer/texture_cache.hpp>

#include <cstddef>
#include <vector>

namespace f4::renderer {

/// Draw a set of mesh entries with per-mesh materials, handling:
///   - Opaque-before-alpha sort (FreeFalcon's .TEX chroma-key textures
///     need alpha=0 pixels drawn after opaque geometry)
///   - Backface culling disable (FreeFalcon's models were authored
///     without consistent winding order)
///   - Lit shader application (if shader.is_loaded())
///   - Alpha blend mode for transparent pixels
///
/// Must be called inside a BeginMode3D/EndMode3D block.
///
/// @param entries       Mesh entries to draw
/// @param tex_cache     Texture cache for material lookup
/// @param lit_shader    Lit shader (if not loaded, falls back to unlit)
/// @param lighting_enabled  Whether to apply lighting at all
/// @param wireframe     Whether to draw in wireframe mode
/// @param stats         Optional pointer to receive draw call stats
struct DrawStats {
    int draw_calls = 0;
    int meshes_drawn = 0;
    std::size_t vertices_drawn = 0;
};

void draw_meshes(
    const std::vector<MeshEntry>& entries,
    const TextureCache& tex_cache,
    const LitShader& lit_shader,
    bool lighting_enabled,
    bool wireframe = false,
    DrawStats* stats = nullptr);

/// Draw a single mesh with a specific material, handling backface
/// culling disable + alpha blend mode. Useful for non-mesh-entry
/// draw calls (e.g. airport geometry, aircraft with entity transforms).
///
/// Must be called inside a BeginMode3D/EndMode3D block.
void draw_single_mesh(const ::Mesh& mesh, const ::Material& material,
                       const ::Matrix& transform);

/// Draw a grid on the XZ plane.
void draw_grid(float extent, float step);

/// Draw a grid on the XZ plane, offset to an ENU origin (ox=east,
/// oy=north, oz=up in feet) — for scenes whose content is not centered
/// on the world origin (e.g. a grid-referenced airbase).
void draw_grid_at(float extent, float step, float ox, float oy, float oz);

/// Draw RGB coordinate axes at the origin.
void draw_axes(float length);

} // namespace f4::renderer
