// f4-renderer/src/terrain_tile_cache.cpp
//
// TerrainTileCache implementation. See terrain_tile_cache.hpp.
//
// The GL calls go through rlgl.h's bundled glad (OpenGL 3.3 core) —
// the same context raylib owns. Texture arrays are created with
// GL_RGBA8 / linear filtering / repeat wrap; UVs stay inside [0,1]
// per tile layer, so wrap mode only matters for the half-texel insets
// the terrain shader's UVs already avoid.

#include <f4/renderer/terrain_tile_cache.hpp>

#include <rlgl.h>

// rlgl.h only pulls glad in under RLGL_IMPLEMENTATION (i.e. inside
// raylib itself), so include the declarations directly. The glad
// function pointers themselves live in raylib's compilation unit and
// resolve at link time; this header supplies the prototypes and the
// GL_* enums.
#include "external/glad.h"

#include <cstring>

namespace f4::renderer {

TerrainTileCache::~TerrainTileCache() { unload(); }

namespace {

constexpr int kInitialLayers = 256;    // First allocation; doubles on growth.
constexpr int kMaxLayers = 32768;      // 32k layers × 128² RGBA ≈ 2 GB cap guard.

void create_array(TerrainTileCache::TileArray& arr, int tile_size, int layers) {
    if (arr.gl_id != 0) return;
    unsigned int id = 0;
    glGenTextures(1, &id);
    glBindTexture(GL_TEXTURE_2D_ARRAY, id);
    glTexImage3D(GL_TEXTURE_2D_ARRAY, 0, GL_RGBA8, tile_size, tile_size, layers,
                 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_S, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_T, GL_REPEAT);
    glBindTexture(GL_TEXTURE_2D_ARRAY, 0);
    arr.gl_id = id;
    arr.tile_size = tile_size;
    arr.layers = layers;
    arr.used = 0;
}

void destroy_array(TerrainTileCache::TileArray& arr) {
    if (arr.gl_id != 0) {
        unsigned int id = arr.gl_id;
        glDeleteTextures(1, &id);
    }
    arr = {};
}

} // namespace

void TerrainTileCache::ensure_arrays() {
    if (far_.gl_id == 0) create_array(far_, 32, kInitialLayers);
    if (near32_.gl_id == 0) create_array(near32_, 32, kInitialLayers);
    if (near64_.gl_id == 0) create_array(near64_, 64, kInitialLayers / 2);
    if (near128_.gl_id == 0) create_array(near128_, 128, kInitialLayers / 4);
}

void TerrainTileCache::unload() {
    destroy_array(far_);
    destroy_array(near32_);
    destroy_array(near64_);
    destroy_array(near128_);
}

int TerrainTileCache::upload_tile(TileArray& arr, uint32_t key,
                                  const uint8_t* rgba, int tile_size) {
    if (rgba == nullptr || tile_size <= 0 || arr.used >= kMaxLayers) return -1;

    if (arr.gl_id == 0 || arr.tile_size != tile_size) {
        destroy_array(arr);
        create_array(arr, tile_size, kInitialLayers);
    } else if (arr.used == arr.layers) {
        // Grow: recreate with double capacity and re-upload retained
        // CPU copies (a few ms every doubling — amortized cheap).
        TileArray grown;
        create_array(grown, tile_size, arr.layers * 2);
        glBindTexture(GL_TEXTURE_2D_ARRAY, grown.gl_id);
        for (int l = 0; l < arr.used; ++l) {
            glTexSubImage3D(GL_TEXTURE_2D_ARRAY, 0, 0, 0, l, tile_size,
                            tile_size, 1, GL_RGBA, GL_UNSIGNED_BYTE,
                            arr.cpu[static_cast<std::size_t>(l)].data());
        }
        glBindTexture(GL_TEXTURE_2D_ARRAY, 0);
        grown.used = arr.used;
        grown.cpu = std::move(arr.cpu);
        grown.layer_of = std::move(arr.layer_of);
        destroy_array(arr);
        arr = std::move(grown);
    }

    const int layer = arr.used;
    glBindTexture(GL_TEXTURE_2D_ARRAY, arr.gl_id);
    glTexSubImage3D(GL_TEXTURE_2D_ARRAY, 0, 0, 0, layer, tile_size, tile_size,
                    1, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
    glBindTexture(GL_TEXTURE_2D_ARRAY, 0);

    arr.cpu.emplace_back(rgba, rgba + static_cast<std::size_t>(tile_size) *
                                       tile_size * 4);
    arr.layer_of[key] = layer;
    arr.used = layer + 1;
    return layer;
}

int TerrainTileCache::far_layer(const f4::terrain::FarTileDB& db, uint32_t index) {
    if (index >= db.tile_count()) return -1;
    if (const auto it = far_.layer_of.find(index); it != far_.layer_of.end())
        return it->second;

    std::vector<uint8_t> rgba;
    if (!db.tile_rgba(index, rgba)) return -1;
    return upload_tile(far_, index, rgba.data(),
                       static_cast<int>(f4::terrain::FarTileDB::TILE_SIZE));
}

int TerrainTileCache::near_layer(const f4::terrain::NearTileDB& db, uint16_t tex_id,
                                 int* tile_size) {
    if (tile_size) *tile_size = 0;
    if (const auto it = near32_.layer_of.find(tex_id); it != near32_.layer_of.end()) {
        if (tile_size) *tile_size = 32;
        return it->second;
    }
    if (const auto it = near64_.layer_of.find(tex_id); it != near64_.layer_of.end()) {
        if (tile_size) *tile_size = 64;
        return it->second;
    }
    if (const auto it = near128_.layer_of.find(tex_id); it != near128_.layer_of.end()) {
        if (tile_size) *tile_size = 128;
        return it->second;
    }

    f4::terrain::NearTileImage img;
    if (!db.tile_rgba(tex_id, img) || img.width != img.height || img.width == 0)
        return -1;

    TileArray* arr = nullptr;
    switch (img.width) {
        case 32:  arr = &near32_;  break;
        case 64:  arr = &near64_;  break;
        case 128: arr = &near128_; break;
        default:  return -1;   // Unexpected tile size — skip texturing.
    }
    const int layer = upload_tile(*arr, tex_id, img.rgba.data(),
                                  static_cast<int>(img.width));
    if (layer >= 0 && tile_size) *tile_size = static_cast<int>(img.width);
    return layer;
}

} // namespace f4::renderer
