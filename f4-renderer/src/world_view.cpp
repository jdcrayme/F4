// f4-renderer/src/world_view.cpp
//
// WorldView implementation. See world_view.hpp for the design.

#include <f4/renderer/world_view.hpp>

#include <utility>

namespace f4::renderer {

WorldView::~WorldView() { unload(); }

bool WorldView::load_theater(const std::filesystem::path& theater_dir) {
    const auto try_load = [&](const std::filesystem::path& terrain_dir,
                              const std::filesystem::path& texture_dir) {
        return near_level_.load(terrain_dir, near_level_index, geometry_)
            && far_level_.load(terrain_dir, far_level_index, geometry_)
            && near_tiles_.load(texture_dir)
            && far_tiles_.load(texture_dir);
    };

    // Callers disagree on what "theater dir" means: scenario JSONs pass
    // the theater root (terrdata/korea — contains terrain/ + texture/),
    // while f4-install's Theater.dir points at the terrain subdir itself
    // (texture/ is then its sibling). Try the root convention first,
    // then the subdir convention.
    bool ok = try_load(theater_dir / "terrain", theater_dir / "texture");
    if (ok) {
        terrain_dir_ = theater_dir / "terrain";
    } else {
        ok = try_load(theater_dir, theater_dir.parent_path() / "texture");
        if (ok) terrain_dir_ = theater_dir;
    }

    tile_source_.geometry   = &geometry_;
    tile_source_.near_level = &near_level_;
    tile_source_.far_level  = &far_level_;
    tile_source_.near_tiles = &near_tiles_;
    tile_source_.far_tiles  = &far_tiles_;
    return tile_source_.usable();
}

bool WorldView::ensure_gpu(std::string* status_msg) {
    if (gpu_ready_) return terrain_shader_.is_loaded();
    if (!terrain_shader_.ensure(status_msg)) return false;
    tile_cache_.ensure_arrays();
    gpu_ready_ = true;
    return true;
}

bool WorldView::set_view(const f4::terrain::TerrainData& terrain,
                         float center_east_ft, float center_north_ft,
                         float extent_ft, float near_extent_ft,
                         float z_offset_ft) {
    if (!theater_loaded() || !gpu_ready_) return false;

    unload_chunk_set_only();

    TerrainChunkSetConfig cfg;
    cfg.center_east_ft  = center_east_ft;
    cfg.center_north_ft = center_north_ft;
    cfg.extent_ft       = extent_ft;
    cfg.near_extent_ft  = near_extent_ft;
    cfg.z_offset_ft     = z_offset_ft;
    cfg.tiles           = &tile_source_;
    cfg.tile_cache      = &tile_cache_;
    cfg.terrain_shader  = &terrain_shader_;
    chunks_ = build_terrain_chunk_set(terrain, cfg);
    return chunks_.valid;
}

void WorldView::update_frame(Color sky_color) {
    if (!gpu_ready_) return;
    terrain_shader_.set_lighting(sun_direction, light_color, light_intensity,
                                 ambient_color);
    terrain_shader_.set_fog(sky_color, fog_start_ft, fog_end_ft);
}

void WorldView::unload() {
    unload_chunk_set_only();
    tile_cache_.unload();
    gpu_ready_ = false;
}

void WorldView::unload_chunk_set_only() {
    if (chunks_.valid) {
        unload_terrain_chunk_set(chunks_);
    }
}

} // namespace f4::renderer
