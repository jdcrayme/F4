// f4-renderer/include/f4/renderer/runtime_model_cache.hpp
//
// RuntimeModelCache — loads glTF models by vis_type from the
// Data/Models/koreaobj/ tree and caches the built Raylib meshes + PNG
// texture bindings.
//
// This is the Tranche 0d replacement for the KoreaObj ModelDatabase
// path (RENDERER_GLTF_REWIRE_PLAN.md §2.2): the runtime reads
// Data/Models/koreaobj/NNNNN.gltf (produced by `f4import models`, Task
// 53 / Tranche 0c) + textures/NNNNN.png (`f4import textures`) — no
// KoreaObj binary parsing anywhere in the link closure.
//
// vis_type is the FALCON4.CT visType[0] value, which indexes KoreaObj
// models directly — the same cache key the old ModelDatabase path used
// (RenderResources::build_mesh_for_model keyed by parent_index). One
// GPU upload per unique model; entities and features sharing a
// vis_type share one entry.
//
// The parsed GltfDocument is retained on each RuntimeModel: the DOF /
// switch / slot tags (f4 extras, spec §6) live there and are the
// substrate for future animation (gear switches etc.). The current
// pipeline builds static LOD-0 geometry — the same convention as the
// binary path it replaces.

#pragma once

#include <f4/renderer/mesh_builder.hpp>   // MeshEntry, GltfPartData
#include <f4/gltf/anim_map.hpp>           // AnimMap

#include <filesystem>
#include <memory>
#include <unordered_map>
#include <vector>

namespace f4::gltf {
struct GltfDocument;
}  // namespace f4::gltf

namespace f4::renderer {

class TextureCache;

/// One drawable part of an animated model: the uploaded mesh plus the
/// glTF node chain that positions it (AIRCRAFT_ANIMATION_PLAN §5.2).
/// The chain is evaluated per frame against the entity's channel
/// values; switch-branch nodes in the chain gate visibility.
struct RuntimePart {
    MeshEntry entry;
    std::vector<std::size_t> node_chain;
};

/// One loaded runtime model: the parsed glTF document plus the LOD-0
/// Raylib meshes built from it (one MeshEntry per glTF primitive).
///
/// Animated documents (hierarchy-emitted: f4::gltf::has_animation_tags)
/// populate lod0_parts + anim_map instead of lod0_meshes; the draw
/// path picks per model. `animated` is false for flat/legacy exports,
/// which keep drawing through lod0_meshes unchanged.
struct RuntimeModel {
    /// Parsed .gltf (owns the DOF/switch/slot f4-extras tags).
    std::shared_ptr<const f4::gltf::GltfDocument> doc;
    /// Uploaded LOD-0 meshes (STATIC path — one per glTF primitive).
    std::vector<MeshEntry> lod0_meshes;
    /// Uploaded LOD-0 parts (ANIMATED path — chain per part).
    std::vector<RuntimePart> lod0_parts;
    /// Resolved animation tags (channel -> node bindings). Empty for
    /// static models.
    f4::gltf::AnimMap anim_map;
    /// True when the document was hierarchy-emitted and lod0_parts is
    /// the draw surface.
    bool animated = false;
    /// true after the first build attempt — including failures, so the
    /// draw path never retries a missing/corrupt model every frame.
    bool built = false;
};

class RuntimeModelCache {
public:
    RuntimeModelCache() = default;
    ~RuntimeModelCache();

    // Non-copyable (owns GPU handles)
    RuntimeModelCache(const RuntimeModelCache&) = delete;
    RuntimeModelCache& operator=(const RuntimeModelCache&) = delete;

    /// Set the Data/ root (models live at <data_dir>/Models/koreaobj/).
    /// Changing the dir clears the cache (the entries are stale).
    void set_data_dir(const std::filesystem::path& data_dir);

    /// The configured Data/ root.
    const std::filesystem::path& data_dir() const noexcept { return data_dir_; }

    /// True when a Data/ root is configured. The draw paths treat a
    /// not-ready cache as "no models" (draw calls degrade to zero, the
    /// same as a vis_type with no KoreaObj model).
    bool ready() const noexcept { return !data_dir_.empty(); }

    /// Lazily load + build the model for a vis_type. No-op when already
    /// cached (or when a previous attempt failed). Requires the GL
    /// context (mesh upload + PNG texture upload through `textures`).
    void build_model(int vis_type, TextureCache& textures);

    /// Look up a cached model. nullptr if never built (or the build
    /// attempt found no model — those are cached with empty meshes).
    const RuntimeModel* lookup(int vis_type) const;

    /// Unload all GPU meshes and clear the cache. Must be called before
    /// the GL context goes away.
    void unload_all();

private:
    std::filesystem::path data_dir_;
    std::unordered_map<int, RuntimeModel> cache_;
};

} // namespace f4::renderer
