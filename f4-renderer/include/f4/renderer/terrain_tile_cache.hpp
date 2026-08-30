// f4-renderer/include/f4/renderer/terrain_tile_cache.hpp
//
// TerrainTileCache — GPU storage for terrain tile art.
//
// Terrain tiles are drawn through a sampler2DArray-based shader (see
// terrain_shader.hpp): one texture array per tile-size family —
//   far:  32x32 tiles, layer key = far-tile index (from FArtILES.RAW)
//   near: 32/64/128-px tiles, layer key = near texID (set/tile/res packed)
// Raylib has no texture-array API, so the arrays are created directly
// through rlgl/OpenGL 3.3 (the same context raylib owns).
//
// Layers are uploaded lazily as the terrain builders request tiles; an
// array grows (recreate + re-upload from retained CPU copies) when full.
// Only tiles actually referenced by built terrain reach the GPU — the
// full Korea far database is 53k tiles (218 MB as RGBA) but a view
// typically references a few thousand.
//
// Requires the GL context for any layer/ensure call. One instance per
// RenderResources (shared across a viewer's views).
//
// C++20.

#pragma once

#include <f4/terrain/far_tile_db.hpp>
#include <f4/terrain/near_tile_db.hpp>

#include <cstdint>
#include <unordered_map>
#include <vector>

namespace f4::renderer {

class TerrainTileCache {
public:
    /// One lazily-grown GL_TEXTURE_2D_ARRAY of square RGBA8 tiles.
    struct TileArray {
        unsigned int gl_id = 0;        ///< 0 until first ensure/upload
        int tile_size = 0;             ///< Pixels per side (32/64/128).
        int layers = 0;                ///< Allocated layer count.
        int used = 0;                  ///< Uploaded layer count.
        std::vector<std::vector<uint8_t>> cpu;      ///< Uploaded RGBA copies (for growth re-upload).
        std::unordered_map<uint32_t, int> layer_of; ///< Key → layer index.
    };

    TerrainTileCache() = default;
    ~TerrainTileCache();

    // Non-copyable AND non-movable: the destructor unloads GL texture
    // ids, so a (default-)moved cache would destroy textures its new
    // owner still uses. Hold it by pointer or in place.
    TerrainTileCache(const TerrainTileCache&) = delete;
    TerrainTileCache& operator=(const TerrainTileCache&) = delete;
    TerrainTileCache(TerrainTileCache&&) = delete;
    TerrainTileCache& operator=(TerrainTileCache&&) = delete;

    /// Layer index for far tile `index` (uploads on first request).
    /// Returns -1 when the tile can't be decoded (caller falls back to
    /// vertex color). Requires the GL context.
    int far_layer(const f4::terrain::FarTileDB& db, uint32_t index);

    /// Layer index for near texID `tex_id` (resolves art through the
    /// NearTileDB, uploads on first request). Returns -1 when no art.
    /// When `tile_size` is non-null it receives the resolved art's
    /// dimension (32/64/128) — the caller needs it to tag vertices with
    /// the right sampler family. Requires the GL context.
    int near_layer(const f4::terrain::NearTileDB& db, uint16_t tex_id,
                   int* tile_size = nullptr);

    /// The four arrays, in the order the terrain shader binds them
    /// (far, near32, near64, near128). Arrays with no uploads yet have
    /// gl_id == 0; bind_arrays() creates 1-layer placeholders so every
    /// sampler is always valid.
    TileArray& far() noexcept { return far_; }
    TileArray& near32() noexcept { return near32_; }
    TileArray& near64() noexcept { return near64_; }
    TileArray& near128() noexcept { return near128_; }
    const TileArray& far() const noexcept { return far_; }
    const TileArray& near32() const noexcept { return near32_; }
    const TileArray& near64() const noexcept { return near64_; }
    const TileArray& near128() const noexcept { return near128_; }

    /// Ensure all four arrays exist (creating 1-layer placeholders for
    /// empty families). Idempotent. Requires the GL context.
    void ensure_arrays();

    /// Free all GPU arrays. Call before the GL context goes away.
    void unload();

    /// Total uploaded layers across arrays (diagnostics).
    [[nodiscard]] int total_layers() const noexcept {
        return far_.used + near32_.used + near64_.used + near128_.used;
    }

private:
    /// Upload `rgba` (tile_size² * 4 bytes) into `arr` under `key`,
    /// growing the array if full. Returns the layer index or -1.
    int upload_tile(TileArray& arr, uint32_t key,
                    const uint8_t* rgba, int tile_size);

    TileArray far_;
    TileArray near32_;
    TileArray near64_;
    TileArray near128_;
};

} // namespace f4::renderer
