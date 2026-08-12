// f4-renderer/src/texture_cache.cpp
//
// GPU texture cache implementation.

#include <f4/renderer/texture_cache.hpp>
#include <f4/renderer/mesh_builder.hpp>

#include <f4/models/model_database.hpp>
#include <f4/models/texture.hpp>

#include <raylib.h>

#include <cstring>
#include <vector>

namespace f4::renderer {

// ── Destructor ────────────────────────────────────────────────────────────────

TextureCache::~TextureCache() {
    unload_all();
}

// ── upload ────────────────────────────────────────────────────────────────────

void TextureCache::upload(f4::models::ModelDatabase& db, const std::vector<int>& tex_ids) {
    for (int tex_id : tex_ids) {
        if (tex_id < 0) continue;
        if (cache_.count(tex_id)) continue;  // already cached

        // Decode the texture (lazy, cached in ModelDatabase)
        const auto* decoded = db.fetch_texture(tex_id);
        if (!decoded || !decoded->valid()) {
            // Mark as cached-but-failed so we don't retry
            TexCacheEntry ce;
            ce.uploaded = false;
            cache_[tex_id] = ce;
            continue;
        }

        // Create a Raylib Image from the RGBA8 pixel data
        Image img = {};
        img.data = RL_MALLOC(decoded->width * decoded->height * 4);
        if (!img.data) {
            TexCacheEntry ce;
            ce.uploaded = false;
            cache_[tex_id] = ce;
            continue;
        }
        std::memcpy(img.data, decoded->rgba.data(),
                     static_cast<std::size_t>(decoded->width * decoded->height * 4));
        img.width = decoded->width;
        img.height = decoded->height;
        img.mipmaps = 1;
        img.format = PIXELFORMAT_UNCOMPRESSED_R8G8B8A8;

        // Upload to GPU
        Texture2D tex = LoadTextureFromImage(img);

        // Create a material with this texture bound
        Material mat = LoadMaterialDefault();
        mat.maps[MATERIAL_MAP_DIFFUSE].texture = tex;
        mat.maps[MATERIAL_MAP_DIFFUSE].color = WHITE;

        TexCacheEntry ce;
        ce.texture = tex;
        ce.material = mat;
        ce.has_alpha = decoded->has_alpha;
        ce.uploaded = true;
        cache_[tex_id] = ce;

        // Free the CPU-side image (GPU copy is retained)
        UnloadImage(img);
    }
}

// ── upload_for_entries ────────────────────────────────────────────────────────

void TextureCache::upload_for_entries(f4::models::ModelDatabase& db,
                                       const std::vector<int>& tex_ids) {
    upload(db, tex_ids);
}

// ── lookup ────────────────────────────────────────────────────────────────────

const TexCacheEntry* TextureCache::lookup(int tex_id) const {
    auto it = cache_.find(tex_id);
    return it != cache_.end() ? &it->second : nullptr;
}

TexCacheEntry* TextureCache::lookup(int tex_id) {
    auto it = cache_.find(tex_id);
    return it != cache_.end() ? &it->second : nullptr;
}

// ── contains ──────────────────────────────────────────────────────────────────

bool TextureCache::contains(int tex_id) const {
    return cache_.count(tex_id) > 0;
}

// ── has_alpha ─────────────────────────────────────────────────────────────────

bool TextureCache::has_alpha(int tex_id) const {
    auto it = cache_.find(tex_id);
    return it != cache_.end() && it->second.uploaded && it->second.has_alpha;
}

// ── unload_all ────────────────────────────────────────────────────────────────

void TextureCache::unload_all() {
    for (auto& [id, ce] : cache_) {
        if (ce.uploaded) {
            UnloadTexture(ce.texture);
            ce.material.maps[MATERIAL_MAP_DIFFUSE].texture = {};
        }
    }
    cache_.clear();
}

} // namespace f4::renderer
