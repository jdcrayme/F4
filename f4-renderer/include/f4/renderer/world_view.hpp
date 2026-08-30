// f4-renderer/include/f4/renderer/world_view.hpp
//
// WorldView — one load-a-world → render-a-view path, shared by every
// app (world-viewer, scenario-player).
//
// Owns the complete textured-terrain lifecycle that the apps previously
// hand-rolled in parallel:
//
//   CPU load    load_theater(theater_dir)      THEATER.L*/O* post levels,
//                                             TEXTURE.BIN + texture.zip,
//                                             FArtILES.PAL/.RAW
//   GPU setup   ensure_gpu()                   TerrainShader + tile arrays
//   View build  set_view(terrain, center, ...) textured TerrainChunkSet
//                                             around a view origin
//   Per frame   update_frame(sky_color)        lighting + fog uniforms;
//                                             chunk_set() feeds
//                                             SceneDescription
//   Teardown    unload()                       before the GL context dies
//
// Everything degrades: a theater without binary tile data (JSON-only
// loads, fixture terrain) leaves theater_loaded() false and callers keep
// their untextured TerrainData mesh path.
//
// The TerrainData summary (palette fallback colors, MEA elevation
// fallback) stays owned by the caller — WorldView takes it by reference
// in set_view() and keeps only a pointer for rebuilds; keep it alive
// alongside the view.
//
// C++20.

#pragma once

#include <f4/renderer/terrain_chunks.hpp>
#include <f4/renderer/terrain_shader.hpp>
#include <f4/renderer/terrain_tile_cache.hpp>
#include <f4/renderer/terrain_tile_source.hpp>

#include <f4/terrain/far_tile_db.hpp>
#include <f4/terrain/near_tile_db.hpp>
#include <f4/terrain/post_level.hpp>
#include <f4/terrain/theater_geometry.hpp>
#include <f4/terrain/terrain_data.hpp>

#include <raylib.h>
// Undef raylib macros that pollute the namespace
#undef PI
#undef DEG2RAD
#undef RAD2DEG

#include <filesystem>
#include <string>

namespace f4::renderer {

class WorldView {
public:
    WorldView() = default;

    // unload() touches GPU state; the destructor calls it, but the owner
    // must call unload() explicitly BEFORE the GL context goes away (the
    // dtor is a backstop, not the contract).
    ~WorldView();

    WorldView(const WorldView&) = delete;
    WorldView& operator=(const WorldView&) = delete;
    WorldView(WorldView&&) = delete;
    WorldView& operator=(WorldView&&) = delete;

    // ── CPU load ──────────────────────────────────────────────────────

    /// Load raw theater binaries. Accepts either the theater root
    /// (terrdata/korea — contains terrain/ + texture/) or the terrain
    /// subdir itself (terrdata/korea/terrain — what f4-install's
    /// Theater.dir points at; texture/ is then the sibling). Any missing
    /// piece is non-fatal — the method returns false and callers keep
    /// their untextured terrain path. Throws only on present-but-
    /// malformed files.
    bool load_theater(const std::filesystem::path& theater_dir);

    /// True when every tile component loaded (textured path available).
    [[nodiscard]] bool theater_loaded() const noexcept {
        return tile_source_.usable();
    }

    // ── GPU + view ────────────────────────────────────────────────────

    /// Compile the terrain shader + create the tile arrays. Requires the
    /// GL context. Idempotent; returns false when the shader can't
    /// compile (callers fall back to untextured terrain).
    bool ensure_gpu(std::string* status_msg = nullptr);

    /// (Re)build the textured terrain chunk set around a view origin:
    /// near tiles within near_extent_ft, far tiles out to extent_ft.
    /// Requires the GL context + ensure_gpu() + a TerrainData summary
    /// (must outlive the view — it supplies palette fallback colors and
    /// the elevation fallback). No-op returning false when the theater
    /// isn't loaded.
    bool set_view(const f4::terrain::TerrainData& terrain,
                  float center_east_ft, float center_north_ft,
                  float extent_ft = 250000.0f,
                  float near_extent_ft = 60000.0f,
                  float z_offset_ft = -5.0f);

    /// Per-frame terrain-shader uniforms: sun + ambient + fog ramping
    /// toward `sky_color`. Call once per frame before render_world();
    /// draw_terrain_chunk_set() rebinds the tile samplers itself.
    void update_frame(Color sky_color);

    /// The built chunk set for SceneDescription::terrain_chunk_set.
    /// Null when no textured view is built.
    [[nodiscard]] const TerrainChunkSet* chunk_set() const noexcept {
        return chunks_.valid ? &chunks_ : nullptr;
    }

    // ── Tunables (public by design — apps adjust per context) ─────────

    Vector3 sun_direction = {0.5f, 1.0f, 0.35f};  ///< Raylib Y-up; above horizon.
    Color   light_color   = {255, 250, 235, 255};
    float   light_intensity = 1.0f;
    Color   ambient_color = {80, 80, 90, 255};
    float   fog_start_ft  = 80000.0f;
    float   fog_end_ft    = 240000.0f;

    /// Which post levels to use (stock Korea: near L0..L2, far L3..L5).
    int near_level_index = 2;
    int far_level_index  = 4;

    // ── Diagnostics / advanced access (2D map painting, diag panels) ──

    [[nodiscard]] const f4::terrain::TheaterGeometry& geometry() const noexcept {
        return geometry_;
    }
    [[nodiscard]] const f4::terrain::PostLevel& near_level() const noexcept {
        return near_level_;
    }
    [[nodiscard]] const f4::terrain::PostLevel& far_level() const noexcept {
        return far_level_;
    }
    [[nodiscard]] const f4::terrain::NearTileDB& near_tiles() const noexcept {
        return near_tiles_;
    }
    [[nodiscard]] const f4::terrain::FarTileDB& far_tiles() const noexcept {
        return far_tiles_;
    }
    [[nodiscard]] TerrainTileCache& tile_cache() noexcept { return tile_cache_; }
    [[nodiscard]] TerrainShader& terrain_shader() noexcept { return terrain_shader_; }
    [[nodiscard]] const TerrainTileSource& tile_source() const noexcept {
        return tile_source_;
    }
    /// The terrain directory that actually contained the loaded
    /// THEATER.O*/L* files (empty when nothing loaded). 2D map painters
    /// use this to load the coarsest post level (L5) themselves.
    [[nodiscard]] const std::filesystem::path& terrain_dir() const noexcept {
        return terrain_dir_;
    }

    // ── Teardown ──────────────────────────────────────────────────────

    /// Free all GPU resources (chunk meshes, tile arrays, shader). Call
    /// before the GL context goes away. Safe when nothing was built.
    void unload();

private:
    void unload_chunk_set_only();   // chunk meshes, not the shared arrays

    f4::terrain::TheaterGeometry geometry_ =
        f4::terrain::TheaterGeometry::korea();
    f4::terrain::PostLevel near_level_;
    f4::terrain::PostLevel far_level_;
    f4::terrain::NearTileDB near_tiles_;
    f4::terrain::FarTileDB far_tiles_;
    TerrainTileSource tile_source_;

    TerrainTileCache tile_cache_;
    TerrainShader terrain_shader_;
    TerrainChunkSet chunks_;
    std::filesystem::path terrain_dir_;
    bool gpu_ready_ = false;
};

} // namespace f4::renderer
