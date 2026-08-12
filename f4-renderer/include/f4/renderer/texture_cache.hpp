// f4-renderer/include/f4/renderer/texture_cache.hpp
//
// GPU texture cache: lazily decodes TEX blobs via ModelDatabase and
// uploads RGBA8 data as Texture2D + Material.
//
// Consolidated from 4 duplicated implementations.

#pragma once

#include <raylib.h>
// Undef raylib macros that pollute the namespace
#undef PI
#undef DEG2RAD
#undef RAD2DEG

#include <unordered_map>
#include <vector>

namespace f4::models {
class ModelDatabase;
}  // namespace f4::models

namespace f4::renderer {

/// A single cached texture entry: GPU Texture2D + Material + metadata.
struct TexCacheEntry {
    ::Texture2D texture = {};
    ::Material material = {};
    bool has_alpha = false;
    bool uploaded = false;        ///< true once texture is on GPU
};

/// GPU texture cache. Lazily decodes TEX blobs via ModelDatabase::fetch_texture()
/// and uploads the RGBA8 data as Texture2D + Material.
///
/// Usage:
///   TextureCache cache;
///   cache.upload(db, mesh_entries);    // upload any new textures
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

    /// Upload textures for all mesh entries that have tex_ids not yet in cache.
    /// @param db     Model database to decode textures from
    /// @param tex_ids  Vector of texture IDs to ensure are cached
    void upload(f4::models::ModelDatabase& db, const std::vector<int>& tex_ids);

    /// Upload textures for MeshEntry vector (convenience overload).
    /// Extracts tex_ids from entries and calls upload().
    void upload_for_entries(f4::models::ModelDatabase& db,
                           const std::vector<int>& tex_ids);

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
