// f4-renderer/src/runtime_model_cache.cpp
//
// RuntimeModelCache implementation (see runtime_model_cache.hpp).

#include <f4/renderer/runtime_model_cache.hpp>

#include <f4/renderer/texture_cache.hpp>
#include <f4/gltf/gltf_loader.hpp>

#include <raylib.h>

#include <cstdio>
#include <exception>
#include <utility>

namespace f4::renderer {

RuntimeModelCache::~RuntimeModelCache() {
    unload_all();
}

// ── set_data_dir ─────────────────────────────────────────────────────────────

void RuntimeModelCache::set_data_dir(const std::filesystem::path& data_dir) {
    if (data_dir_ == data_dir) return;
    // Different data dir → every cached model (and its GPU meshes) is
    // stale. Unload before repointing.
    unload_all();
    data_dir_ = data_dir;
}

// ── build_model ──────────────────────────────────────────────────────────────

void RuntimeModelCache::build_model(int vis_type, TextureCache& textures) {
    if (vis_type < 0 || data_dir_.empty()) return;
    auto it = cache_.find(vis_type);
    if (it != cache_.end() && it->second.built) return;  // already cached

    RuntimeModel model;

    // NNNNN.gltf — zero-padded to 5 digits (the f4import models layout).
    char name[16];
    std::snprintf(name, sizeof(name), "%05d", vis_type);
    const auto model_dir = data_dir_ / "Models" / "koreaobj";
    const auto gltf_path = model_dir / (std::string(name) + ".gltf");

    auto doc = std::make_shared<f4::gltf::GltfDocument>();
    try {
        doc->load(gltf_path);
    } catch (const std::exception&) {
        // Missing or corrupt model — mark built so the draw path stops
        // retrying (same semantics as the old ModelDatabase path, which
        // marked the cache entry built on parse failure).
        model.built = true;
        model.doc = std::move(doc);
        cache_[vis_type] = std::move(model);
        return;
    }

    // Extract + upload LOD 0 (highest detail — same convention as the
    // old build_mesh_for_model / build_feature_mesh paths).
    auto geoms = extract_gltf_lod_geometry(*doc, 0);
    model.lod0_meshes.reserve(geoms.size());
    for (auto& g : geoms) {
        MeshEntry entry;
        entry.tex_id = g.tex_id;
        entry.texture_uri = std::move(g.texture_uri);
        if (!g.positions.empty()) {
            entry.mesh = build_gltf_mesh(g);
        }
        model.lod0_meshes.push_back(std::move(entry));
    }

    // Upload the referenced textures (PNG files via raylib — no
    // KoreaObj.TEX decoding at runtime). tex ids come from the glTF
    // material chain; the URI resolves relative to the model dir.
    for (const auto& e : model.lod0_meshes) {
        if (e.tex_id >= 0 && !e.texture_uri.empty()) {
            textures.upload_png(e.tex_id, model_dir / e.texture_uri);
        }
    }

    model.built = true;
    cache_[vis_type] = std::move(model);
}

// ── lookup ───────────────────────────────────────────────────────────────────

const RuntimeModel* RuntimeModelCache::lookup(int vis_type) const {
    auto it = cache_.find(vis_type);
    return it != cache_.end() ? &it->second : nullptr;
}

// ── unload_all ───────────────────────────────────────────────────────────────

void RuntimeModelCache::unload_all() {
    for (auto& [id, model] : cache_) {
        for (auto& entry : model.lod0_meshes) {
            if (entry.mesh.vertexCount > 0) {
                UnloadMesh(entry.mesh);
            }
            entry.mesh = {};
        }
        model.lod0_meshes.clear();
        model.built = false;
    }
    cache_.clear();
}

} // namespace f4::renderer
