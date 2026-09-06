// f4-renderer/src/texture_cache.cpp
//
// GPU texture cache implementation (PNG loading path).

#include <f4/renderer/texture_cache.hpp>

#include <raylib.h>

#include <cstring>
#include <filesystem>

namespace f4::renderer {

// ── Destructor ────────────────────────────────────────────────────────────────

TextureCache::~TextureCache() {
    unload_all();
}

// ── upload_png ───────────────────────────────────────────────────────────────

void TextureCache::upload_png(int tex_id, const std::filesystem::path& png_path) {
    if (tex_id < 0) return;
    if (cache_.count(tex_id)) return;  // already cached (or previously failed)

    const auto fail = [this, tex_id]() {
        // Cache as failed so we don't retry every frame.
        TexCacheEntry ce;
        ce.uploaded = false;
        cache_[tex_id] = ce;
    };

    if (!std::filesystem::exists(png_path)) {
        fail();
        return;
    }

    // Load via LoadImage (not LoadTexture directly) so we can detect
    // alpha — the draw paths sort alpha-blended meshes last, and the
    // old DecodedTexture path provided that bit. PNGs without an alpha
    // channel decode to a 3-byte-per-pixel format, which is the
    // has_alpha=false case.
    Image img = LoadImage(png_path.string().c_str());
    if (img.data == nullptr || img.width <= 0 || img.height <= 0) {
        fail();
        return;
    }

    bool has_alpha = false;
    if (img.format == PIXELFORMAT_UNCOMPRESSED_R8G8B8A8) {
        const unsigned char* px = static_cast<const unsigned char*>(img.data);
        const std::size_t n = static_cast<std::size_t>(img.width) *
                              static_cast<std::size_t>(img.height);
        for (std::size_t i = 0; i < n; ++i) {
            if (px[i * 4 + 3] != 255) {
                has_alpha = true;
                break;
            }
        }
    }

    // Upload to GPU.
    Texture2D tex = LoadTextureFromImage(img);
    UnloadImage(img);
    if (tex.id == 0) {
        fail();
        return;
    }

    // Create a material with this texture bound.
    Material mat = LoadMaterialDefault();
    mat.maps[MATERIAL_MAP_DIFFUSE].texture = tex;
    mat.maps[MATERIAL_MAP_DIFFUSE].color = WHITE;

    TexCacheEntry ce;
    ce.texture = tex;
    ce.material = mat;
    ce.has_alpha = has_alpha;
    ce.uploaded = true;
    cache_[tex_id] = ce;
}

// ── insert ───────────────────────────────────────────────────────────────────

void TextureCache::insert(int tex_id, const TexCacheEntry& entry) {
    if (tex_id < 0) return;
    // Free any previous GPU state the new entry replaces.
    auto it = cache_.find(tex_id);
    if (it != cache_.end() && it->second.uploaded) {
        UnloadTexture(it->second.texture);
        it->second.material.maps[MATERIAL_MAP_DIFFUSE].texture = {};
    }
    cache_[tex_id] = entry;
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
