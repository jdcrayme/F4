// f4-renderer/src/feature_mesh.cpp
//
// Implementation of feature_mesh.hpp.
//
// This file consolidates the feature → vis_type → mesh → DrawMesh
// pipeline that was previously inlined (twice) in
// f4-world-viewer/src/ground_layout_3d.cpp and stubbed in
// f4-world-viewer/src/canvas.cpp. The pipeline:
//
//   1. entity_type → vis_type[0]  via f4-world-types ClassTable
//   2. vis_type → glTF load + LOD-0 mesh extraction + upload
//                via RuntimeModelCache (Data/Models/koreaobj/NNNNN.gltf)
//   3. PNG texture upload via TextureCache (lazy, keyed by tex_id)
//   4. Per-feature DrawMesh at Translate(enu_pos) * RotateY(facing)
//
// Tranche 0d (RENDERER_GLTF_REWIRE_PLAN.md): steps 2-3 read the glTF+PNG
// export instead of KoreaObj binary — f4-models is no longer linked.

#include <f4/renderer/feature_mesh.hpp>

#include <f4/renderer/coord_transform.hpp>  // enu_to_raylib
#include <f4/math/constants.hpp>

#include <f4/world_types/class_table.hpp>

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
    // NOTE: class_table is NOT required here — the build path resolves
    // vis_type → model through the RuntimeModelCache alone. (The draw
    // path's vis-type-DIRECT entry — V-3DLIVE's draw_vis_type_mesh,
    // whose callers legitimately hold no table — shares this builder.)
    if (!res.model_cache || !res.texture_cache) {
        return;
    }

    // Lazy load + build; RuntimeModelCache marks the entry built even
    // on failure so we don't retry every frame.
    res.model_cache->build_model(vis_type, *res.texture_cache);
}

// ── draw_vis_type_mesh ─────────────────────────────────────────────────────
//
// V-3DLIVE: the vis-type-DIRECT entry point — everything draw_feature_mesh
// does after its class-table lookup, for callers that ALREADY hold the
// vis type (VisualModelComponent::vis_type — the session entities, whose
// spawn paths resolved the class table against the session's own CT).
// Shares the same RuntimeModelCache and draw path, so a feature and
// a session entity of the same vis type reuse one GPU upload.

DrawStats draw_vis_type_mesh(
    FeatureMeshResources& res,
    int vis_type,
    float enu_x, float enu_y, float enu_z,
    float facing_deg)
{
    DrawStats stats{};
    if (!res.model_cache || !res.texture_cache ||
        !res.lit_shader || !res.default_material) {
        return stats;  // incompletely configured
    }
    if (vis_type <= 0) {
        return stats;  // never resolved / no model
    }

    // Lazily build the mesh (cached by vis_type).
    build_feature_mesh(res, vis_type);

    const RuntimeModel* model = res.model_cache->lookup(vis_type);
    if (!model || model->lod0_meshes.empty()) {
        return stats;  // build failed or yielded no meshes
    }

    // Ensure lit shader + set lighting uniforms.
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

    // Build the model matrix.
    //   - Convert ENU position to Raylib RH Y-up via enu_to_raylib().
    //   - Rotate around the vertical (Raylib +Y, ENU +Z) axis by facing.
    //
    // The facing sign matches the existing footprint code in
    // ground_layout_3d.cpp (which uses facing_rad = facing * (π/180),
    // no negation). The glTF→raylib transform maps the exported
    // meters/Y-up geometry into the same Raylib frame the binary path
    // produced — no extra RotateX(π) needed (was the GLV3D-DIAG-1 /
    // GLV3D-DIAG-2 bug).
    const auto pos_f = enu_to_raylib(enu_x, enu_y, enu_z);
    const Vector3 pos_rh = { pos_f.x, pos_f.y, pos_f.z };
    const float facing_rad = (-facing_deg) * static_cast<float>(f4::math::DEG_TO_RAD);
    const Matrix rot = MatrixRotateY(facing_rad);
    const Matrix model_matrix = MatrixMultiply(rot,
        MatrixTranslate(pos_rh.x, pos_rh.y, pos_rh.z) );

    // Draw each mesh entry. Disable backface culling because
    // FreeFalcon's BSP-derived geometry has inconsistent winding order
    // (same convention as draw_meshes() — see draw_3d.cpp:70-73 for
    // rationale).
    rlDisableBackfaceCulling();
    BeginBlendMode(BLEND_ALPHA);

    for (const auto& me : model->lod0_meshes) {
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

// ── draw_feature_mesh ──────────────────────────────────────────────────────

DrawStats draw_feature_mesh(
    FeatureMeshResources& res,
    uint16_t class_table_index,
    float enu_x, float enu_y, float enu_z,
    float facing_deg)
{
    DrawStats stats{};
    if (!res.model_cache || !res.class_table || !res.texture_cache ||
        !res.lit_shader || !res.default_material) {
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

    // Step 2: everything else (build + draw), shared with
    // draw_vis_type_mesh.
    return draw_vis_type_mesh(res, vis_type,
                              enu_x, enu_y, enu_z, facing_deg);
}

} // namespace f4::renderer
