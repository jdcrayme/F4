// f4-renderer/src/feature_mesh.cpp
//
// Implementation of feature_mesh.hpp.
//
// This file consolidates the feature → vis_type → mesh → DrawMesh
// pipeline that was previously inlined (twice) in
// f4-world-viewer/src/ground_layout_3d.cpp and stubbed in
// f4-world-viewer/src/canvas.cpp. The pipeline:
//
//   1. entity_type → vis_type[0]  via ClassTable
//   2. vis_type → ModelRecord + parse_lod + extract_model_geometry
//                via ModelDatabase
//   3. ModelGeometry → Raylib ::Mesh[]  via build_raylib_meshes
//   4. ::Mesh[] → MeshEntry[]  via build_mesh_entries (pairs each mesh
//                with its tex_id for per-mesh material lookup)
//   5. Texture upload via TextureCache (lazy, keyed by tex_id)
//   6. Per-feature DrawMesh at Translate(enu_pos) * RotateY(facing)
//
// The mesh cache (keyed by vis_type) and texture cache (keyed by tex_id)
// are owned by the caller (typically ViewerApp::Impl) so they persist
// across frames and across selections — building a KoreaObj mesh is
// expensive (~1-10ms per model), so caching is essential.

#include <f4/renderer/feature_mesh.hpp>

#include <f4/renderer/coord_transform.hpp>  // enu_to_raylib
#include <f4/renderer/mesh_builder.hpp>     // build_raylib_meshes, build_mesh_entries

#include <f4/models/geometry.hpp>
#include <f4/models/model_record.hpp>

#include <raylib.h>
#include <raymath.h>
#include <rlgl.h>

#include <cmath>
#include <utility>
#include <vector>

namespace f4::renderer {

// ── build_feature_mesh ──────────────────────────────────────────────────────

void build_feature_mesh(FeatureMeshResources& res, int vis_type) {
    if (vis_type <= 0) return;
    if (!res.model_db || !res.class_table || !res.texture_cache ||
        !res.mesh_cache) {
        return;
    }

    // Already cached (or previously failed) — don't rebuild.
    auto it = res.mesh_cache->find(vis_type);
    if (it != res.mesh_cache->end() && it->second.built) return;

    auto& db = *res.model_db;
    const auto* rec = db.model(vis_type);
    if (!rec || rec->lods.empty()) {
        // Mark as built so we don't retry every frame.
        if (it != res.mesh_cache->end()) it->second.built = true;
        else (*res.mesh_cache)[vis_type].built = true;
        return;
    }

    // Lock to LOD 0 (highest detail) — same convention as the 3D panel.
    const int lod = 0;
    auto err = db.parse_lod(vis_type, lod);
    if (!err.empty()) {
        if (it != res.mesh_cache->end()) it->second.built = true;
        else (*res.mesh_cache)[vis_type].built = true;
        return;
    }

    // Default ModelState: texture_set=0, no DOFs/switches active.
    // Matches the existing 3D-panel behavior.
    f4::models::ModelState default_state;
    default_state.texture_set = 0;
    default_state.n_texture_sets = std::max(1, static_cast<int>(rec->n_texture_sets));

    auto geom = db.extract_model_geometry(vis_type, lod, default_state);
    if (geom.meshes.empty()) {
        (*res.mesh_cache)[vis_type].built = true;
        return;
    }

    // Build Raylib meshes + mesh entries (consolidated path).
    auto raylib_meshes = build_raylib_meshes(
        geom, db.color_bank(), model_vertex_to_raylib);
    auto entries = build_mesh_entries(geom, raylib_meshes);

    FeatureMeshCacheEntry cache_entry;
    cache_entry.meshes = std::move(entries);
    cache_entry.built = true;
    (*res.mesh_cache)[vis_type] = std::move(cache_entry);

    // Upload any new textures via the shared TextureCache. Tex_ids come
    // from the geometry's per-mesh material assignment.
    std::vector<int> tex_ids;
    for (const auto& me : (*res.mesh_cache)[vis_type].meshes) {
        if (me.tex_id >= 0) tex_ids.push_back(me.tex_id);
    }
    if (!tex_ids.empty()) {
        res.texture_cache->upload(db, tex_ids);
    }
}

// ── draw_feature_mesh ──────────────────────────────────────────────────────

DrawStats draw_feature_mesh(
    FeatureMeshResources& res,
    uint16_t class_table_index,
    float enu_x, float enu_y, float enu_z,
    float facing_deg)
{
    DrawStats stats{};
    if (!res.model_db || !res.class_table || !res.texture_cache ||
        !res.lit_shader || !res.mesh_cache || !res.default_material) {
        return stats;  // incompletely configured
    }

    // Step 1: entity_type → vis_type[0] via the class table.
    // The caller is responsible for converting FeatureEntryState.index
    // (a descriptionIndex) to entity_type (descriptionIndex + 100) before
    // calling — this matches ClassTable::vis_type_for()'s convention.
    const auto vis_type = res.class_table->vis_type_for(class_table_index, 0);
    if (vis_type <= 0) {
        return stats;  // no model for this entity_type
    }

    // Step 2: lazily build the mesh (cached by vis_type).
    build_feature_mesh(res, vis_type);

    auto cache_it = res.mesh_cache->find(vis_type);
    if (cache_it == res.mesh_cache->end() || cache_it->second.meshes.empty()) {
        return stats;  // build failed or yielded no meshes
    }

    // Step 3: ensure lit shader + set lighting uniforms.
    // Idempotent — caller may have already done this, but doing it again
    // per-call is cheap (just uniform uploads) and avoids relying on
    // caller-side setup ordering.
    const bool lighting_active = res.lit_shader->ensure();
    if (lighting_active) {
        res.lit_shader->set_lighting(
            res.light_direction,
            res.light_color,
            res.light_intensity,
            res.ambient_color);
    }

    // Step 4: build the model matrix.
    //   - Convert ENU position to Raylib RH Y-up via enu_to_raylib().
    //   - Rotate around the vertical (Raylib +Y, ENU +Z) axis by facing.
    //
    // The facing sign matches the existing footprint code in
    // ground_layout_3d.cpp (which uses facing_rad = facing * (π/180),
    // no negation). The model_vertex_to_raylib transform already maps
    // FreeFalcon's -Z-up BSP convention to Raylib's +Y-up — no extra
    // RotateX(π) needed (was the GLV3D-DIAG-1 / GLV3D-DIAG-2 bug).
    constexpr float kPi = 3.14159265358979323846f;
    const auto pos_f = enu_to_raylib(enu_x, enu_y, enu_z);
    const Vector3 pos_rh = { pos_f.x, pos_f.y, pos_f.z };
    const float facing_rad = (-facing_deg) * (kPi / 180.0f);
    //const Matrix trn = MatrixTranslate(pos_rh.x, pos_rh.y, pos_rh.z);
    const Matrix rot = MatrixRotateY(facing_rad);
    const Matrix model_matrix = MatrixMultiply(rot,
        MatrixTranslate(pos_rh.x, pos_rh.y, pos_rh.z) );

    // Step 5: draw each mesh entry. Disable backface culling because
    // FreeFalcon's BSP models have inconsistent winding order (same
    // convention as draw_meshes() — see draw_3d.cpp:70-73 for rationale).
    rlDisableBackfaceCulling();
    BeginBlendMode(BLEND_ALPHA);

    for (const auto& me : cache_it->second.meshes) {
        if (me.mesh.triangleCount <= 0) continue;

        const ::Material* mat_to_use = res.default_material;
        if (me.tex_id >= 0) {
            auto* ce = res.texture_cache->lookup(me.tex_id);
            if (ce && ce->uploaded) {
                mat_to_use = &ce->material;
            }
        }
        DrawMesh(me.mesh, *mat_to_use, model_matrix);
        ++stats.draw_calls;
        ++stats.meshes_drawn;
        stats.vertices_drawn += static_cast<std::size_t>(me.mesh.vertexCount);
    }

    EndBlendMode();
    rlEnableBackfaceCulling();

    return stats;
}

} // namespace f4::renderer
