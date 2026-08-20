// f4-renderer/src/render_resources.cpp
//
// RenderResources implementation. The mesh-build body consolidates the
// (previously 3x-duplicated) KoreaObj LOD-0 extraction + mesh/texture
// upload sequence from the scenario-player and world-viewer apps.

#include <f4/renderer/render_resources.hpp>

#include <f4/models/model_database.hpp>
#include <f4/models/geometry.hpp>

#include <raylib.h>
#include <rlgl.h>   // GetShaderDefault

#include <vector>

namespace f4::renderer {

// ── Destructor ───────────────────────────────────────────────────────────

RenderResources::~RenderResources() {
    unload_all();
}

// ── ensure_default_material ──────────────────────────────────────────────

bool RenderResources::ensure_default_material() {
    if (default_mat_valid_) return true;

    // Compile the lit shader (idempotent; may fail headless, in which
    // case we still build the material with Raylib's unlit default).
    lit_shader.ensure();

    // 1) 1×1 opaque-white fallback texture.
    if (!fallback_white_tex_valid_) {
        Image img = {};
        img.data = RL_MALLOC(4);  // one RGBA8 pixel
        if (!img.data) return false;
        auto* px = static_cast<unsigned char*>(img.data);
        px[0] = 255; px[1] = 255; px[2] = 255; px[3] = 255;
        img.width = 1;
        img.height = 1;
        img.mipmaps = 1;
        img.format = PIXELFORMAT_UNCOMPRESSED_R8G8B8A8;
        fallback_white_tex_ = LoadTextureFromImage(img);
        UnloadImage(img);
        fallback_white_tex_valid_ = (fallback_white_tex_.id != 0);
        if (!fallback_white_tex_valid_) return false;
    }

    // 2) Default material with the white texture bound (so untextured
    // meshes sample (1,1,1,1) — the lit shader's chroma-key discard
    // would hide them otherwise) and the lit shader assigned if it
    // compiled.
    default_mat_ = LoadMaterialDefault();
    default_mat_.maps[MATERIAL_MAP_DIFFUSE].texture = fallback_white_tex_;
    default_mat_.maps[MATERIAL_MAP_DIFFUSE].color = WHITE;
    if (lit_shader.is_loaded()) {
        default_mat_.shader = lit_shader.shader();
    }
    default_mat_valid_ = true;
    return true;
}

// ── build_mesh_for_model ─────────────────────────────────────────────────

void RenderResources::build_mesh_for_model(
    f4::models::ModelDatabase& db, int parent_index)
{
    if (parent_index < 0) return;
    auto it = mesh_cache.find(parent_index);
    if (it != mesh_cache.end() && it->second.built) return;  // already cached

    const auto mark_built = [&]() {
        if (it != mesh_cache.end()) it->second.built = true;
        else mesh_cache[parent_index].built = true;
    };

    const auto* rec = db.model(parent_index);
    if (!rec || rec->lods.empty()) {
        mark_built();
        return;
    }

    const int lod = 0;  // lock to LOD 0 (highest detail) for now
    auto err = db.parse_lod(parent_index, lod);
    if (!err.empty()) {
        mark_built();
        return;
    }

    // Default ModelState: texture_set 0, no DOF/switch animation. Static
    // gear-down geometry is baked at build time (see scenario-player
    // build_mesh_for_model's original note); animating later means
    // invalidating the cache entry.
    f4::models::ModelState default_state;
    default_state.texture_set = 0;
    default_state.n_texture_sets = std::max(1, static_cast<int>(rec->n_texture_sets));

    auto geom = db.extract_model_geometry(parent_index, lod, default_state);
    if (geom.meshes.empty()) {
        mark_built();
        return;
    }

    auto raylib_meshes = build_raylib_meshes(
        geom, db.color_bank(), model_vertex_to_raylib);
    auto mesh_entries = build_mesh_entries(geom, raylib_meshes);

    MeshCacheEntry entry;
    entry.meshes = std::move(mesh_entries);
    entry.built = true;
    mesh_cache[parent_index] = std::move(entry);

    // Upload any new textures referenced by this model's meshes.
    std::vector<int> tex_ids;
    for (const auto& me : mesh_cache[parent_index].meshes) {
        if (me.tex_id >= 0) tex_ids.push_back(me.tex_id);
    }
    if (!tex_ids.empty()) {
        texture_cache.upload(db, tex_ids);
    }
}

// ── unload_all ───────────────────────────────────────────────────────────

void RenderResources::unload_all() {
    texture_cache.unload_all();

    for (auto& [parent_idx, cache_entry] : mesh_cache) {
        for (auto& me : cache_entry.meshes) {
            UnloadMesh(me.mesh);
        }
        cache_entry.meshes.clear();
        cache_entry.built = false;
    }
    mesh_cache.clear();

    // Pure data — no GPU resources, but free the memory.
    airfield_cache.clear();

    // Unset the texture reference on the material BEFORE unloading it,
    // otherwise UnloadMaterial may try to free a texture that's about to
    // be freed separately. Same for the shader: raylib's UnloadMaterial
    // frees any non-default shader assigned to the material — the
    // LitShader member owns that shader and unloads it itself, so detach
    // it first to avoid a double free.
    if (default_mat_valid_) {
        default_mat_.maps[MATERIAL_MAP_DIFFUSE].texture = {};
        default_mat_.shader.id = rlGetShaderIdDefault();
        UnloadMaterial(default_mat_);
        default_mat_ = {};
        default_mat_valid_ = false;
    }
    if (fallback_white_tex_valid_) {
        UnloadTexture(fallback_white_tex_);
        fallback_white_tex_ = {};
        fallback_white_tex_valid_ = false;
    }
}

} // namespace f4::renderer
