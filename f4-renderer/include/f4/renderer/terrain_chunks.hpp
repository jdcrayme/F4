// f4-renderer/include/f4/renderer/terrain_chunks.hpp
//
// TerrainChunkSet — a chunked terrain mesh for theater-scale rendering.
//
// A TerrainChunkSet tiles a square extent into a grid of small chunk
// meshes, each independently built, frustum-culled, and drawn. This
// replaces the single-mesh approach in terrain_mesh.hpp for callers
// that need:
//   - Higher total vertex count than the 65535-vertex unsigned-short
//     cap allows in a single mesh (chunking sidesteps the cap)
//   - Per-chunk frustum culling (chunks behind the camera aren't drawn)
//   - Future per-chunk LOD (close chunks high-res, far chunks low-res)
//   - Faster rebuilds when the camera moves (only newly-visible chunks
//     build; previously-built chunks are cached)
//
// Each chunk is a regular grid of (chunk_resolution+1)² vertices
// sampled from the terrain's elevation grid via bilinear interpolation.
// Adjacent chunks share edge vertices (duplicated) so there are no T-
// junctions or cracks — at the cost of redundant vertex storage. For
// the default chunk_resolution=32, this is ~1KB per chunk edge, negligible
// vs. the 64-chunk total.
//
// Mesh winding, y-flip, and color sampling conventions match
// build_terrain_mesh() in terrain_mesh.hpp exactly — see that file for
// the rationale. The color sampler uses world_to_cell_clamped() so
// chunks extending beyond the theater boundary correctly sample the
// edge terrain cell (fixing the "coast shows land instead of water" bug
// for objectives near the theater edge).
//
// Must be called between BeginMode3D/EndMode3D for draw_terrain_chunk_set().
// build_terrain_chunk_set() requires the GL context (UploadMesh is called
// internally). C++20.

#pragma once

#include <f4/terrain/terrain_data.hpp>
#include <f4/renderer/terrain_tile_source.hpp>

#include <raylib.h>
// Undef raylib macros that pollute the namespace
#undef PI
#undef DEG2RAD
#undef RAD2DEG

#include <cstdint>
#include <vector>

namespace f4::renderer {

class TerrainTileCache;
class TerrainShader;

/// Configuration for build_terrain_chunk_set(). Controls the overall
/// extent, the chunk grid resolution, and per-chunk mesh resolution.
struct TerrainChunkSetConfig {
    /// ENU center of the entire chunk set (feet). Typically the
    /// selected objective's world position.
    float center_east_ft = 0.0f;
    float center_north_ft = 0.0f;

    /// Half-extent of the entire chunk set (feet). The set spans
    /// [center - extent, center + extent] in both east and north —
    /// a total of 2*extent per side. Default 50000 ft ≈ 9.5 nm
    /// half-extent (19 nm square), matching the world-viewer's 3D tab.
    float extent_ft = 50000.0f;

    /// Number of chunk cells per side (LEGACY vertex-color path only —
    /// the textured path derives its chunk grid from post blocks).
    /// The set has chunks_per_side² chunks total. Default 8 → 64 chunks.
    int chunks_per_side = 8;

    /// Vertices per chunk side (LEGACY vertex-color path only).
    /// Each chunk has (chunk_resolution+1)² vertices. Default 32.
    /// HARD CAP: chunk_resolution ≤ 254 (unsigned-short index cap).
    int chunk_resolution = 32;

    /// Vertical exaggeration factor. 1.0 = real elevations.
    float vertical_scale = 1.0f;

    /// Vertical offset (feet, negative = down). Applied AFTER vertical_scale.
    /// Use a small negative value (e.g. -5.0) to sink terrain below
    /// airfield geometry so runway/taxiway quads render on top without
    /// z-fighting.
    float z_offset_ft = -5.0f;

    /// Color tinting mode (same as TerrainMeshConfig).
    bool color_by_tile_type = true;

    /// Far-plane override (feet). When >0, draw_terrain_chunk_set()
    /// calls extend_far_plane() before drawing. Default 250000 ft.
    /// Set to 0 to disable (use the caller's projection).
    float far_plane_ft = 250000.0f;
    float near_plane_ft = 1.0f;
    float camera_fovy_deg = 45.0f;

    // ── Textured-terrain path (Phase 2) ────────────────────────────────
    //
    // When tiles + tile_cache + terrain_shader are all set (and the
    // tile source is usable), the builder switches to the textured
    // path: the region within near_extent_ft of the center is emitted
    // as post-aligned near-tile quads (per-quad vertices carrying tile
    // UVs + array layer in texcoords2), and the rest of the extent is
    // covered at the far level with far-tile quads. Elevation comes
    // from the respective PostLevel instead of the MEA grid.

    /// Decoded theater post/tile data. null → legacy vertex-color path.
    const TerrainTileSource* tiles = nullptr;

    /// GPU tile cache for layer allocation (non-owning; must outlive
    /// the chunk set and be unloaded before the GL context goes away).
    TerrainTileCache* tile_cache = nullptr;

    /// Textured-terrain shader (non-owning; must outlive the chunk set).
    TerrainShader* terrain_shader = nullptr;

    /// Half-extent (feet) of the near-textured region around the
    /// center. Beyond it, out to extent_ft, the far level is drawn.
    float near_extent_ft = 60000.0f;

    /// Extra z bias (feet, negative = lower) for far-region quads so
    /// the near region wins depth tests where the two overlap (the
    /// levels sample elevation differently at the seam).
    float far_z_bias_ft = -20.0f;
};

/// One chunk's GPU resources + bounding box. Owned by TerrainChunkSet.
struct TerrainChunk {
    Mesh mesh = {};              ///< Raylib mesh (vertices, colors, triangles)
    Model model = {};            ///< Raylib model (mesh + default material)
    bool valid = false;          ///< true after a successful build
    bool textured = false;       ///< built by the textured path (arrays+shader)

    /// ENU bounding box (feet) — used for frustum culling.
    float min_east = 0.0f, max_east = 0.0f;
    float min_north = 0.0f, max_north = 0.0f;
    float min_up = 0.0f, max_up = 0.0f;

    /// Grid coordinates of this chunk in the chunk grid.
    /// chunk (cx, cy) where cx,cy ∈ [0, chunks_per_side).
    int chunk_x = 0;
    int chunk_y = 0;

    /// Textured path: quads that fell back to palette vertex color
    /// because their tile couldn't be resolved (diagnostics).
    int untextured_quads = 0;
};

/// A built set of terrain chunks + the config they were built with.
/// The caller owns this and must call unload_terrain_chunk_set() before
/// the GL context goes away.
struct TerrainChunkSet {
    std::vector<TerrainChunk> chunks;  ///< chunks_per_side² entries
    bool valid = false;                ///< true after a successful build
    bool textured = false;             ///< true when built by the textured path

    TerrainChunkSetConfig config;

    /// Overall ENU bounding box (feet) — union of all chunks.
    float min_east = 0.0f, max_east = 0.0f;
    float min_north = 0.0f, max_north = 0.0f;
    float min_up = 0.0f, max_up = 0.0f;

    /// Per-frame stats updated by draw_terrain_chunk_set(). Useful for
    /// the HUD's diagnostic counter.
    int chunks_total = 0;       ///< total chunks in the set
    int chunks_visible = 0;    ///< chunks drawn this frame (after culling)

    /// Build stats for the textured path (diagnostics): quads emitted
    /// per region and quads that fell back to vertex color because a
    /// tile couldn't be resolved.
    int near_quads = 0;
    int far_quads = 0;
    int quads_untextured = 0;
};

/// Build a chunked terrain mesh from TerrainData around the given config center.
///
/// Each chunk covers a (2*extent/chunks_per_side)² ft sub-region, sampled
/// at (chunk_resolution+1)² points. Vertices in adjacent chunks are
/// duplicated (no shared vertices) so there are no T-junctions.
///
/// Requires the GL context — UploadMesh() is called per chunk.
///
/// \param terrain  The terrain data to sample (must outlive the chunk set)
/// \param config   Extent, chunk grid, per-chunk resolution, placement
/// \return A TerrainChunkSet with valid=true on success. On failure
///         (empty terrain, bad config), valid=false and chunks is empty.
TerrainChunkSet build_terrain_chunk_set(
    const f4::terrain::TerrainData& terrain,
    const TerrainChunkSetConfig& config);

/// Draw a previously-built chunk set with per-chunk frustum culling.
/// Call inside BeginMode3D/EndMode3D.
///
/// Chunks whose bounding box falls outside the camera's view frustum
/// are skipped — for a typical orbit camera looking at one objective,
/// ~50% of chunks are culled, halving draw calls.
///
/// The chunk set's far_plane_ft config (if >0) is applied via
/// extend_far_plane() before drawing — same self-healing behavior as
/// draw_terrain_mesh().
///
/// Updates tm.chunks_visible for diagnostic display.
void draw_terrain_chunk_set(TerrainChunkSet& tm, const Camera3D& camera);

/// Free the GPU resources owned by a TerrainChunkSet. Call before the
/// GL context goes away (or before rebuilding). Safe to call on an
/// uninitialized set (checks valid first).
void unload_terrain_chunk_set(TerrainChunkSet& tm);

} // namespace f4::renderer
