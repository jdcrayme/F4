// f4-renderer/include/f4/renderer/texture_cache.hpp
//
// GPU texture cache: lazily loads PNG textures and uploads them as
// Texture2D + Material, keyed by KoreaObj texture bank id.
//
// Tranche 0d: the texture source is the exported PNG set
// (Data/Models/koreaobj/textures/NNNNN.png — produced by
// `f4import textures`, Tranche 0c) instead of KoreaObj.TEX decoding.
// The bank id (the cache key) is unchanged, so the draw paths'
// MeshEntry.tex_id → lookup(tex_id) → material flow is unchanged.
//
// Consolidated from 4 duplicated implementations.

#pragma once

#include <raylib.h>
// Undef raylib macros that pollute the namespace
#undef PI
#undef DEG2RAD
#undef RAD2DEG

#include <filesystem>
#include <unordered_map>

namespace f4::renderer {

/// A single cached texture entry: GPU Texture2D + Material + metadata.
struct TexCacheEntry {
    ::Texture2D texture = {};
    ::Material material = {};
    bool has_alpha = false;
    bool uploaded = false;        ///< true once texture is on GPU
};

/// GPU texture cache. Lazily loads PNG files and uploads them as
/// Texture2D + Material.
///
/// Usage:
///   TextureCache cache;
///   cache.upload_png(tex_id, png_path);  // upload any new textures
///   // ... later, in draw loop:
///   auto* ce = cache.lookup(tex_id);
///   if (ce && ce->uploaded) DrawMesh(mesh, ce->material, transform);
///
/// Consolidated from 4 duplicated implementations across the viewer apps.
class TextureCache {
public:
    TextureCache() = default;
    ~TextureCache();

    // Non-copyable
    TextureCache(const TextureCache&) = delete;
    TextureCache& operator=(const TextureCache&) = delete;

    /// Load a PNG from disk (raylib LoadImage path) and upload it to the
    /// GPU, cached under the KoreaObj texture bank id. No-op when the
    /// id is already cached (or previously failed). A missing/corrupt
    /// file is cached as not-uploaded so the draw path doesn't retry.
    /// Requires the GL context.
    void upload_png(int tex_id, const std::filesystem::path& png_path);

    /// Inject a pre-built entry (GPU texture already uploaded by the
    /// caller). Generic escape hatch for tools that decode textures
    /// themselves (e.g. f4-models-viewer's legacy KoreaObj.TEX path —
    /// the decoder lives in the importer-side tool, not here).
    /// Overwrites any existing entry for the id; takes ownership of the
    /// GPU texture for cleanup (unload_all()).
    void insert(int tex_id, const TexCacheEntry& entry);

    /// Look up a cached texture by tex_id. Returns nullptr if not cached.
    const TexCacheEntry* lookup(int tex_id) const;
    TexCacheEntry* lookup(int tex_id);

    /// Check if a texture is in the cache (even if upload failed).
    bool contains(int tex_id) const;

    /// Check if a texture has alpha (for sorting).
    bool has_alpha(int tex_id) const;

    /// Unload all textures from GPU and clear the cache.
    void unload_all();

    /// Get the underlying map (for iteration in draw loops).
    const std::unordered_map<int, TexCacheEntry>& map() const { return cache_; }

private:
    std::unordered_map<int, TexCacheEntry> cache_;
};

} // namespace f4::renderer
