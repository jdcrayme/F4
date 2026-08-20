// f4-renderer/src/world_renderer.cpp
//
// render_world() implementation (see world_renderer.hpp).

#include <f4/renderer/world_renderer.hpp>

#include <f4/models/model_database.hpp>

#include <raylib.h>

#include <vector>

namespace f4::renderer {

FrameStats render_world(RenderResources& res, const SceneDescription& s) {
    FrameStats stats{};

    if (s.target) BeginTextureMode(*s.target);

    ClearBackground(s.sky_color);

    BeginMode3D(s.camera);
    // BeginMode3D's projection far plane is the rlgl default (1000 units),
    // far too small for theater-scale scenes in feet.
    extend_far_plane(s.camera, s.near_plane, s.far_plane);

    draw_ground(s.ground);

    // Cull center: the camera position, expressed in ENU feet
    // (raylib → ENU is (x, -z, y)).
    const float cull_e = s.camera.position.x;
    const float cull_n = -s.camera.position.z;
    const float cull_u = s.camera.position.y;
    const bool culling = s.cull_radius_ft > 0.0f;
    const float cull_r2 = s.cull_radius_ft * s.cull_radius_ft;

    // ── Component-dispatch entities ─────────────────────────────────
    if (s.world && !s.entities.empty()) {
        EntityRenderResources eres =
            make_entity_render_resources(res, s.model_db, s.class_table);
        eres.show_features = s.draw_feature_models;
        eres.airfield_toggles = s.airfield_toggles;

        for (const auto eid : s.entities) {
            auto h = f4::entities::EntityHandle(eid, s.world);
            if (!h.valid()) continue;
            ++stats.entities_total;

            if (culling) {
                auto* tf = h.get<f4::entities::TransformComponent>();
                if (tf) {
                    const float dx = static_cast<float>(tf->position.x) - cull_e;
                    const float dy = static_cast<float>(tf->position.y) - cull_n;
                    const float dz = static_cast<float>(tf->position.z) - cull_u;
                    if (dx * dx + dy * dy + dz * dz > cull_r2) continue;
                }
            }

            const auto st = RenderEntity(eres, h);
            stats.draw.draw_calls   += st.draw_calls;
            stats.draw.meshes_drawn += st.meshes_drawn;
            stats.draw.vertices_drawn += st.vertices_drawn;
            if (st.draw_calls > 0) ++stats.entities_drawn;
        }
    }

    // ── VisualModelComponent entity meshes ──────────────────────────
    if (!s.entity_meshes.empty() && s.model_db) {
        const auto st = draw_entity_meshes(
            res, *s.model_db, s.entity_meshes,
            cull_e, cull_n, cull_u, s.cull_radius_ft);
        stats.draw.draw_calls   += st.draw_calls;
        stats.draw.meshes_drawn += st.meshes_drawn;
        stats.draw.vertices_drawn += st.vertices_drawn;
        stats.entity_meshes_drawn = st.meshes_drawn;
    }

    // ── Standalone airfield ─────────────────────────────────────────
    if (s.airfield) {
        const auto st = draw_airfield_geometry(
            *s.airfield, s.airfield_toggles,
            s.airfield_origin_enu[0], s.airfield_origin_enu[1],
            s.airfield_origin_enu[2]);
        stats.draw.draw_calls   += st.draw_calls;
        stats.draw.meshes_drawn += st.meshes_drawn;
        stats.draw.vertices_drawn += st.vertices_drawn;
    }

    // ── App overlays, still inside the 3D mode ──────────────────────
    if (s.overlay_3d) s.overlay_3d(s.camera);

    // Label candidates are collected before EndMode3D (projection state),
    // drawn in the 2D pass below.
    std::vector<LayoutLabel2D> labels;
    if (s.airfield_labels && s.airfield) {
        collect_layout_labels(
            s.camera, *s.airfield,
            /*show_parking=*/s.airfield_toggles.parking,
            GetRenderWidth(), GetRenderHeight(),
            s.airfield_origin_enu[0], s.airfield_origin_enu[1],
            s.airfield_origin_enu[2], labels);
    }

    EndMode3D();

    // 2D label pass (inside the texture target when rendering offscreen).
    if (!labels.empty()) {
        draw_layout_labels(labels);
    }

    if (s.target) EndTextureMode();

    return stats;
}

} // namespace f4::renderer
