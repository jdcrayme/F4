// f4-renderer/src/scene_draw.cpp
//
// Scene composition implementation (see scene_draw.hpp).

#include <f4/renderer/scene_draw.hpp>

#include <f4/gltf/anim_map.hpp>
#include <f4/gltf/gltf_loader.hpp>

#include <raylib.h>
#include <raymath.h>
#include <rlgl.h>

#include <cmath>
#include <vector>

namespace f4::renderer {

// ---------------------------------------------------------------------------
// draw_ground
// ---------------------------------------------------------------------------

void draw_ground(const GroundConfig& cfg) {
    // Flat colored ground plane, anchored at the ENU origin and offset
    // below the layout elevation so it never z-fights with the runway /
    // taxiway surfaces drawn there.
    if (cfg.plane) {
        const auto c = enu_to_raylib(
            cfg.origin_enu_x, cfg.origin_enu_y,
            cfg.origin_enu_z + cfg.plane_z_offset);
        DrawPlane(Vector3{c.x, c.y, c.z},
                  Vector2{cfg.plane_size, cfg.plane_size}, cfg.plane_color);
    }

    if (cfg.grid) {
        draw_grid_at(cfg.grid_extent, cfg.grid_step,
                     cfg.origin_enu_x, cfg.origin_enu_y,
                     cfg.origin_enu_z + cfg.grid_z_offset);
    }

    if (cfg.axes) {
        const auto o = enu_to_raylib(
            cfg.origin_enu_x, cfg.origin_enu_y, cfg.origin_enu_z);
        const Vector3 origin = {o.x, o.y, o.z};
        const float L = cfg.axis_length;
        // Raylib-space axes: +X (East), +Y (Up), +Z (-North).
        DrawLine3D(origin, {origin.x + L, origin.y, origin.z},
                   Color{220, 60, 60, 255});
        DrawLine3D(origin, {origin.x, origin.y + L, origin.z},
                   Color{60, 220, 60, 255});
        DrawLine3D(origin, {origin.x, origin.y, origin.z + L},
                   Color{60, 60, 220, 255});
    }
}

// ---------------------------------------------------------------------------
// Animated model application (AIRCRAFT_ANIMATION_PLAN.md §5.2)
//
// A hierarchy-emitted model draws as PARTS: each part's node chain is
// composed per frame from the entity's channel values (dof nodes)
// and switch visibility (sw branch nodes). Static nodes contribute
// their authored transform. All matrix math lives here — the f4-gltf
// side stays pure double math via eval_tagged_local().
// ---------------------------------------------------------------------------

namespace {

/// Compose one chain node's local matrix for this frame.
Matrix anim_node_local_matrix(const f4::gltf::GltfDocument& doc,
                              std::size_t node_index,
                              const f4::anim::AnimValues* anim,
                              const f4::gltf::AnimNode* anim_node) {
    // The driving value: bound channel from the instance's AnimValues,
    // 0 (= the authored default pose) when unbound or no instance
    // state.
    float value = 0.0f;
    if (anim_node && anim_node->kind == "dof" && anim &&
        !anim_node->channel.empty()) {
        if (auto ch = f4::anim::channel_from_name(anim_node->channel)) {
            value = (*anim)[*ch];
        }
    }

    double t[3], q[4], s[3];
    f4::gltf::eval_tagged_local(doc, node_index, value, t, q, s);

    const Matrix rot = QuaternionToMatrix(Quaternion{
        static_cast<float>(q[0]), static_cast<float>(q[1]),
        static_cast<float>(q[2]), static_cast<float>(q[3])});
    const Matrix scale = MatrixScale(static_cast<float>(s[0]),
                                     static_cast<float>(s[1]),
                                     static_cast<float>(s[2]));
    const Matrix trans = MatrixTranslate(static_cast<float>(t[0]),
                                         static_cast<float>(t[1]),
                                         static_cast<float>(t[2]));
    // glTF node semantics: local = T · R · S.
    return MatrixMultiply(trans, MatrixMultiply(rot, scale));
}

/// Switch visibility along a part's chain: every sw branch node gates
/// the part through FreeFalcon's bitmask rule. UNBOUND switches (no
/// channel) stay visible — the legacy show-all convention, so geometry
/// never silently vanishes on models the rig doesn't command.
bool anim_part_visible(const f4::renderer::RuntimeModel& model,
                       const f4::renderer::RuntimePart& part,
                       const f4::anim::AnimValues* anim) {
    for (const auto node : part.node_chain) {
        const auto* an = model.anim_map.find_by_node(node);
        if (!an || an->kind != "sw") continue;
        if (an->channel.empty() || !anim) continue;
        auto ch = f4::anim::channel_from_name(an->channel);
        if (!ch) continue;  // unknown channel name — treat as unbound
        const float mask = (*anim)[*ch];
        if (!f4::gltf::switch_mask_visible(mask, an->sw_child, an->reversed)) {
            return false;
        }
    }
    return true;
}

/// Compose a part's full node-chain matrix (identity for empty chains).
///
/// Composition ORDER: outermost chain node pushed first with
/// m = m·local, giving M_o·M_m·…·M_i — the innermost frame is applied
/// to the vertex first, matching the flat extractor's accumulated
/// transform stack (compose(accum, local) — first-pushed outermost in
/// application). Verified by the rest-pose parity tests: nested-DOF
/// chains (F-16 gear assemblies) reproduce the flat bake to 0.0 m.
Matrix anim_part_matrix(const f4::renderer::RuntimeModel& model,
                        const f4::renderer::RuntimePart& part,
                        const f4::anim::AnimValues* anim) {
    Matrix m = MatrixIdentity();
    for (const auto node : part.node_chain) {
        const auto* an = model.anim_map.find_by_node(node);
        const Matrix local = anim_node_local_matrix(*model.doc, node, anim, an);
        m = MatrixMultiply(m, local);
    }
    return m;
}

} // namespace

// Public: shared animated draw loop (parts × material selection ×
// DrawMesh), also used by feature_mesh.cpp's draw_vis_type_mesh for
// staged animated models. Takes the FeatureMeshResources base so both
// the entity path (RenderResources&) and the feature path can call it.
DrawStats draw_animated_model(FeatureMeshResources& res,
                              const RuntimeModel& model,
                              const Matrix& model_matrix,
                              const f4::anim::AnimValues* anim,
                              bool lighting_active) {
    DrawStats stats{};
    if (!model.doc) return stats;  // no document — nothing to evaluate
    const Material* default_mat = res.default_material;

    for (const auto& part : model.lod0_parts) {
        if (part.entry.mesh.triangleCount <= 0) continue;
        if (!anim_part_visible(model, part, anim)) continue;

        const Matrix part_matrix =
            MatrixMultiply(anim_part_matrix(model, part, anim), model_matrix);

        const Material* mat_to_use = default_mat;
        if (part.entry.tex_id >= 0 && res.texture_cache) {
            auto* tex_entry = res.texture_cache->lookup(part.entry.tex_id);
            if (tex_entry && tex_entry->uploaded) {
                mat_to_use = &tex_entry->material;
                if (lighting_active && res.lit_shader) {
                    const_cast<Material*>(mat_to_use)->shader =
                        res.lit_shader->shader();
                }
            }
        }
        if (!mat_to_use) continue;

        DrawMesh(part.entry.mesh, *mat_to_use, part_matrix);
        ++stats.draw_calls;
        ++stats.meshes_drawn;
        stats.vertices_drawn +=
            static_cast<std::size_t>(part.entry.mesh.vertexCount);
    }
    return stats;
}

// ---------------------------------------------------------------------------
// draw_entity_meshes
// ---------------------------------------------------------------------------

DrawStats draw_entity_meshes(
    RenderResources& res,
    const std::vector<EntityMeshDraw>& entities,
    float cull_enu_x, float cull_enu_y, float cull_enu_z,
    float cull_radius_ft)
{
    DrawStats total{};
    if (entities.empty()) return total;

    const bool culling = cull_radius_ft > 0.0f;
    const float cull_r2 = cull_radius_ft * cull_radius_ft;

    // Lighting once for the whole pass.
    bool lighting_active = false;
    if (res.lit_shader.ensure()) {
        lighting_active = true;
        res.lit_shader.set_lighting(res.light_direction, res.light_color,
                                    res.light_intensity, res.ambient_color);
    }

    // FreeFalcon models were authored without consistent winding order —
    // same convention as draw_meshes() / draw_feature_mesh().
    rlDisableBackfaceCulling();
    BeginBlendMode(BLEND_ALPHA);

    res.ensure_default_material();
    const Material* default_mat = res.default_material_valid()
        ? &res.default_material() : nullptr;

    for (const auto& ent : entities) {
        if (ent.parent_index < 0) continue;

        if (culling) {
            const float dx = ent.enu_x - cull_enu_x;
            const float dy = ent.enu_y - cull_enu_y;
            const float dz = ent.enu_z - cull_enu_z;
            if (dx * dx + dy * dy + dz * dz > cull_r2) continue;
        }

        // Lazy mesh build on first encounter (glTF + PNG via
        // RuntimeModelCache; no-op when the Data dir isn't configured).
        res.build_mesh_for_model(ent.parent_index);

        const RuntimeModel* model = res.model_cache.lookup(ent.parent_index);
        if (!model) {
            continue;
        }
        // NOTE: no lod0_meshes.empty() check here — animated
        // (hierarchy-emitted) models carry their geometry in
        // lod0_parts and an empty lod0_meshes; the branches below
        // pick the right surface per model.

        const auto pos_f = enu_to_raylib(ent.enu_x, ent.enu_y, ent.enu_z);
        const Vector3 pos_rh = {pos_f.x, pos_f.y, pos_f.z};

        const auto q_rh = enu_quat_to_raylib(ent.qw, ent.qx, ent.qy, ent.qz);
        const Quaternion q = {q_rh.x, q_rh.y, q_rh.z, q_rh.w};

        const Matrix model_matrix = MatrixMultiply(
            QuaternionToMatrix(q),
            MatrixTranslate(pos_rh.x, pos_rh.y, pos_rh.z));

        // Animated (hierarchy-emitted) models draw through the parts
        // path with the entity's per-instance channel values; static
        // models keep the flat loop below untouched.
        if (model->animated && !model->lod0_parts.empty()) {
            // RenderResources carries value members; the shared animated
            // loop takes the pointer-style FeatureMeshResources view.
            FeatureMeshResources base;
            base.model_cache    = &res.model_cache;
            base.texture_cache  = &res.texture_cache;
            base.lit_shader     = &res.lit_shader;
            base.default_material = res.default_material_valid()
                ? &res.default_material() : nullptr;
            const auto st = draw_animated_model(base, *model, model_matrix,
                                                ent.anim, lighting_active);
            total.draw_calls     += st.draw_calls;
            total.meshes_drawn   += st.meshes_drawn;
            total.vertices_drawn += st.vertices_drawn;
            continue;
        }

        for (const auto& me : model->lod0_meshes) {
            if (me.mesh.triangleCount <= 0) continue;

            const Material* mat_to_use = default_mat;
            if (me.tex_id >= 0) {
                auto* tex_entry = res.texture_cache.lookup(me.tex_id);
                if (tex_entry && tex_entry->uploaded) {
                    mat_to_use = &tex_entry->material;
                    if (lighting_active) {
                        const_cast<Material*>(mat_to_use)->shader =
                            res.lit_shader.shader();
                    }
                }
            }
            if (!mat_to_use) continue;

            DrawMesh(me.mesh, *mat_to_use, model_matrix);
            ++total.draw_calls;
            ++total.meshes_drawn;
            total.vertices_drawn +=
                static_cast<std::size_t>(me.mesh.vertexCount);
        }
    }

    EndBlendMode();
    rlEnableBackfaceCulling();

    return total;
}

// ---------------------------------------------------------------------------
// draw_airfield_geometry
// ---------------------------------------------------------------------------

DrawStats draw_airfield_geometry(
    const AirfieldGeometry3D& geom,
    const AirfieldDrawToggles& toggles,
    float origin_x, float origin_y, float origin_z)
{
    DrawStats stats{};
    if (geom.empty) return stats;

    const float ox = origin_x, oy = origin_y, oz = origin_z;

    if (toggles.runway) {
        for (const auto& q : geom.runway_surfaces)
            draw_layout_quad(q, ox, oy, oz);
        for (const auto& q : geom.threshold_bars)
            draw_layout_quad(q, ox, oy, oz);
        for (const auto& q : geom.centerline_dashes)
            draw_layout_quad(q, ox, oy, oz);
    }

    if (toggles.markers) {
        for (const auto& m : geom.runway_ends)
            draw_layout_marker(m, ox, oy, oz);
    }

    if (toggles.taxiways) {
        for (const auto& q : geom.taxiway_strips)
            draw_layout_quad(q, ox, oy, oz);
        for (const auto& l : geom.taxiway_centerlines)
            draw_layout_line(l, ox, oy, oz);
    }

    if (toggles.parking) {
        for (const auto& m : geom.parking_spots)
            draw_layout_marker(m, ox, oy, oz);
    }

    // Helipads are few and always relevant.
    if (toggles.helipads) {
        for (const auto& m : geom.helipads)
            draw_layout_marker(m, ox, oy, oz);
    }

    // Feature footprints — flat fallback for when 3D models are off.
    if (toggles.features) {
        for (const auto& q : geom.feature_footprints)
            draw_layout_quad(q, ox, oy, oz);
    }

    stats.draw_calls =
        static_cast<int>(geom.runway_surfaces.size() +
                         geom.threshold_bars.size() +
                         geom.centerline_dashes.size() +
                         geom.taxiway_strips.size() +
                         geom.taxiway_centerlines.size() +
                         geom.feature_footprints.size());
    stats.meshes_drawn = stats.draw_calls;
    return stats;
}

} // namespace f4::renderer
