// f4-renderer/include/f4/renderer/terrain_mesh.hpp
//
// TerrainMesh — builds and renders a Raylib heightmap mesh from
// f4::terrain::TerrainData around a given ENU center position.
//
// The mesh is a regular grid of N×N vertices, each placed at the
// bilinear-interpolated terrain elevation. Vertex colors come from the
// terrain tile_type palette (water=blue, lowland=tan, etc.) so the mesh
// is self-shaded without textures. The mesh is built ONCE (when the
// terrain or center changes) and cached; subsequent frames just draw it.
//
// Path B1: the scenario player calls build_terrain_mesh() once after
// the terrain is loaded + the airfield center is known, then
// draw_terrain_mesh() each frame inside BeginMode3D. This replaces the
// flat green ground plane with real Korea elevation — the F-16 now
// flies over actual terrain instead of void.
//
// Must be called between BeginMode3D/EndMode3D for draw_terrain_mesh().
// build_terrain_mesh() requires the GL context (rlImGuiSetup has been
// called). C++20.

#pragma once

#include <f4/terrain/terrain_data.hpp>

#include <raylib.h>
// Undef raylib macros that pollute the namespace
#undef PI
#undef DEG2RAD
#undef RAD2DEG

#include <cstdint>

namespace f4::renderer {

/// Configuration for build_terrain_mesh(). Controls the mesh extent,
/// resolution, and placement.
struct TerrainMeshConfig {
    /// ENU center of the mesh (feet). Typically the airfield center.
    float center_east_ft = 0.0f;
    float center_north_ft = 0.0f;

    /// Half-extent of the mesh in each direction (feet). The mesh spans
    /// [center - extent, center + extent] in both east and north.
    /// Default 50000 ft ≈ 9.5 nm half-extent (19 nm square mesh).
    float extent_ft = 50000.0f;

    /// Number of grid cells per side. The mesh has (resolution+1)²
    /// vertices. Default 64 → 4225 vertices, 4096 triangles — cheap.
    /// Bump to 128 for closer shots, down to 32 for far overview.
    ///
    /// HARD CAP: resolution must satisfy (resolution+1)² ≤ 65534, since
    /// Raylib's Mesh.indices is unsigned short. build_terrain_mesh()
    /// clamps resolution to 254 (255² = 65025 verts, max idx 65024) and
    /// logs a warning if the requested value exceeded the cap. Values of
    /// 256 or higher would silently wrap indices and produce "triangles
    /// stretching across the entire terrain" artifacts.
    int resolution = 64;

    /// Vertical exaggeration factor. 1.0 = real elevations. 2.0 = doubled
    /// (useful for flat theaters where real elevation is hard to see).
    float vertical_scale = 1.0f;

    /// Vertical offset (feet, negative = down). Applied AFTER vertical_scale.
    /// Use a small negative value (e.g. -5.0) to sink the terrain slightly
    /// below the airfield geometry so runway/taxiway quads render on top
    /// without z-fighting. 0.0 = terrain at real elevation.
    float z_offset_ft = -5.0f;

    /// Color tinting mode:
    ///   true  → per-vertex color from tile_type palette (water=blue, etc.)
    ///   false → single color (grass green) for all vertices
    bool color_by_tile_type = true;

    /// Optional far-plane override (feet). When >0, draw_terrain_mesh()
    /// calls extend_far_plane() with this value before drawing, so the
    /// terrain is never clipped by Raylib's default RL_CULL_DISTANCE_FAR.
    /// Default 250000 ft covers a 100000-ft terrain extent plus a
    /// 20000-ft camera distance with margin to spare. Set to 0 to
    /// disable the override (use whatever projection the caller set up).
    /// Must be greater than near_plane_ft when nonzero.
    float far_plane_ft = 250000.0f;

    /// Near plane paired with far_plane_ft. Only used when far_plane_ft > 0.
    /// Default 1.0 ft keeps depth precision sane (1:250000 ratio) without
    /// clipping the cockpit/close terrain in front of the camera.
    float near_plane_ft = 1.0f;

    /// Field of view (degrees) used by draw_terrain_mesh() when it
    /// applies the far_plane_ft override. Default 45° matches the
    /// OrbitCamera/FreeCamera default. Set this to your scene camera's
    /// actual fovy to keep the projection consistent.
    float camera_fovy_deg = 45.0f;
};

/// A built terrain mesh + its placement. The mesh is owned by the caller
/// and must be UnloadMesh()'d before the GL context goes away.
struct TerrainMesh {
    Mesh mesh = {};              ///< Raylib mesh (vertices, colors, triangles)
    Model model = {};            ///< Raylib model (mesh + default material)
    bool valid = false;          ///< true after a successful build

    /// The config the mesh was built with. Stored so the caller can
    /// detect config changes and rebuild.
    TerrainMeshConfig config;

    /// Bounding box (ENU feet) of the built mesh. Used for frustum
    /// culling + camera fitting.
    float min_east = 0.0f, max_east = 0.0f;
    float min_north = 0.0f, max_north = 0.0f;
    float min_up = 0.0f, max_up = 0.0f;
};

/// Build a terrain mesh from TerrainData around the given config center.
/// The mesh covers [center - extent, center + extent] in east/north,
/// sampled at (resolution+1)² points. Each vertex's elevation comes
/// from bilinear interpolation of the terrain grid; color comes from
/// the tile_type palette (or a single color if color_by_tile_type=false).
///
/// Requires the GL context (rlImGuiSetup has been called) — UploadMesh()
/// is called internally to push vertices to the GPU.
///
/// \param terrain  The terrain data to sample (must outlive the mesh)
/// \param config   Mesh extent, resolution, placement
/// \return A TerrainMesh with valid=true on success. On failure (e.g.
///         empty terrain), valid=false and the mesh is empty.
TerrainMesh build_terrain_mesh(const f4::terrain::TerrainData& terrain,
                                const TerrainMeshConfig& config);

/// Draw a previously-built terrain mesh. Call inside BeginMode3D/EndMode3D.
/// The mesh is positioned so its vertex (0,0) is at
/// (config.center_east_ft - config.extent_ft, config.center_north_ft - config.extent_ft, 0)
/// in ENU feet — i.e. the mesh floats above the world origin at its
/// correct geographic position.
///
/// No-op if mesh.valid is false.
void draw_terrain_mesh(const TerrainMesh& tm);

/// Free the GPU resources owned by a TerrainMesh. Call before the GL
/// context goes away (or before rebuilding). Safe to call on an
/// uninitialized TerrainMesh (checks valid first).
void unload_terrain_mesh(TerrainMesh& tm);

} // namespace f4::renderer
