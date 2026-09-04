// f4-world-viewer/src/entity_model_3d.cpp
//
// "3D" tab content for NON-objective selections — squadrons, ground
// units (battalion/brigade/taskforce), flights, and live session
// aircraft. The objective branch lives in ground_layout_3d.cpp; the
// Inspector's 3D tab dispatches between the two by selection kind.
//
// What each selection shows (all as real KoreaObj vis-type models on a
// ground plane, staged at a fixed world position, orbit camera shared
// with the objective view):
//   - Squadron: a parked row of its aircraft type (one per pilot,
//     capped), resolved via UnitCoreComponent::class_table_index →
//     ClassTable::vis_type_for() — the exact lookup the session's
//     parked-aircraft spawn uses.
//   - Flight: two of its aircraft in echelon.
//   - Battalion/Brigade/TaskForce: its VehicleCompositionComponent
//     groups lined up (vehicle entity_type → vis_type per group).
//   - LiveAircraft: the entity's own VisualModelComponent model with
//     its current facing (velocity when moving, else the spawn
//     quaternion — the same convention as the map's 3D pass).
//
// Shares Impl::ground_layout_3d_target, Impl::gl3d_orbit_cam, and
// Impl::render_res_3d with the objective view (only one 3D tab draws
// per frame, so sharing is safe); the camera refits when the selection
// changes, keyed on the same Impl::ground_layout_3d_cached_entity the
// objective view uses.

#include "viewer_state.hpp"

#include <f4/entities/entity.hpp>
#include <f4/simulation/campaign_origin.hpp>
#include <f4/simulation/visual_model_component.hpp>
#include <f4/renderer/entity_render.hpp>
#include <f4/renderer/feature_mesh.hpp>
#include <f4/renderer/scene_draw.hpp>

// f4-models + f4-world-convert headers MUST come before raylib.h (the
// PI macro collision — see ground_layout_3d.cpp).
#include <f4/models/model_database.hpp>
#include <f4/world_convert/class_table.hpp>

#include <imgui.h>
#include <raylib.h>
#include <rlgl.h>
#include <rlImGui.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <vector>

namespace f4::viewer {

namespace {

// One staged model: vis type + offset from the staging origin (ENU ft)
// + facing (degrees, CCW around +Z up).
struct ModelPlacement {
    int vis_type;
    float dx;
    float dy;
    float facing_deg;
};

constexpr float RT_W = 800;   // must match ground_layout_3d.cpp (the
constexpr float RT_H = 600;   // RenderTexture is shared)

constexpr Color BG_COLOR = {22, 24, 30, 255};
constexpr Color GRID_COLOR = {50, 54, 62, 255};

// Facing for a session entity's transform: velocity → compass when
// moving, else the spawn compass quaternion (identical to the map's
// V-3DLIVE pass in canvas.cpp).
float facing_deg_from_transform(const f4::entities::TransformComponent* tf) {
    if (!tf) return 0.0f;
    const double v2 = tf->vx * tf->vx + tf->vy * tf->vy;
    if (v2 > 400.0) {  // > 20 ft/s ground speed
        const double rad = std::atan2(tf->vx, tf->vy);
        return static_cast<float>(rad * 57.29577951308232);
    }
    double rad = -2.0 * std::atan2(tf->qz, tf->qw);
    while (rad < 0.0) rad += 6.283185307179586;
    while (rad >= 6.283185307179586) rad -= 6.283185307179586;
    return static_cast<float>(rad * 57.29577951308232);
}

} // namespace

// ---------------------------------------------------------------------------
// Public method — ViewerApp::draw_entity_model_3d
// ---------------------------------------------------------------------------

void ViewerApp::draw_entity_model_3d() {
    if (impl_->sel_kind == Impl::SelectionKind::None ||
        !impl_->sel_entity.valid()) {
        ImGui::TextDisabled(
            "Select a squadron, ground unit, or aircraft to view it in 3D.");
        return;
    }

    // Models + class table — shared with the objective 3D tab (lazily
    // loaded once; ~50-150ms on first use).
    if (!impl_->models_3d_load_attempted) {
        impl_->ensure_models_3d_loaded();
    }
    if (!impl_->models_3d_loaded || !impl_->model_db_3d.has_value() ||
        !impl_->class_table_3d.loaded()) {
        ImGui::TextDisabled(
            "3D models not available — configure an install "
            "(KoreaObj + FALCON4.ct) to view units in 3D.");
        return;
    }

    // --- Gather the cast: title + model placements -----------------------
    std::vector<ModelPlacement> placements;
    char title[192] = "3D";

    if (impl_->sel_kind == Impl::SelectionKind::LiveAircraft) {
        auto h = impl_->session_handle(impl_->sel_entity);
        auto* tf = h.get<f4::entities::TransformComponent>();
        auto* vmc = h.get<f4::simulation::VisualModelComponent>();
        if (!tf || !vmc || vmc->vis_type <= 0) {
            ImGui::TextDisabled(
                "Selected aircraft has no 3D model (vis_type %d).",
                vmc ? vmc->vis_type : 0);
            return;
        }
        std::snprintf(title, sizeof(title),
                      "Aircraft (vis type %d)", vmc->vis_type);
        placements.push_back({vmc->vis_type, 0.0f, 0.0f,
                              facing_deg_from_transform(tf)});
    } else if (impl_->sel_kind == Impl::SelectionKind::Unit) {
        auto h = impl_->handle(impl_->sel_entity);
        auto* uc = h.get<f4::entities::UnitCoreComponent>();
        if (!uc) {
            ImGui::TextDisabled("Selected unit has no unit data.");
            return;
        }
        using f4::entities::UnitClass;
        if (uc->unit_class == UnitClass::Squadron) {
            const auto vis = impl_->class_table_3d.vis_type_for(
                static_cast<uint16_t>(uc->class_table_index), 0);
            if (vis <= 0) {
                ImGui::TextDisabled(
                    "Squadron aircraft type %d has no 3D model "
                    "(vis_type 0 in the class table).",
                    uc->class_table_index);
                return;
            }
            auto* sq = h.get<f4::entities::SquadronComponent>();
            // One parked airframe per pilot, capped at 6 — enough to
            // read the squadron's size without a 24-airframe pile.
            const std::size_t n = std::clamp(
                sq ? sq->pilots.size() : std::size_t{2},
                std::size_t{1}, std::size_t{6});
            std::snprintf(title, sizeof(title),
                          "Squadron — %s (%d aircraft, vis type %d)",
                          uc->class_name.c_str(), static_cast<int>(n),
                          vis);
            for (std::size_t i = 0; i < n; ++i) {
                // Two parking rows (like the ramp: 4 spots, ~100 ft
                // apart along the row, 120 ft between rows).
                const std::size_t row = i / 4, col = i % 4;
                placements.push_back({vis,
                                      static_cast<float>(col) * 100.0f,
                                      static_cast<float>(row) * 120.0f,
                                      0.0f});
            }
        } else if (uc->unit_class == UnitClass::Flight) {
            const auto vis = impl_->class_table_3d.vis_type_for(
                static_cast<uint16_t>(uc->class_table_index), 0);
            if (vis <= 0) {
                ImGui::TextDisabled(
                    "Flight aircraft type %d has no 3D model.",
                    uc->class_table_index);
                return;
            }
            std::snprintf(title, sizeof(title),
                          "Flight — %s (vis type %d)",
                          uc->class_name.c_str(), vis);
            // Echelon pair.
            placements.push_back({vis, 0.0f, 0.0f, 0.0f});
            placements.push_back({vis, 80.0f, -80.0f, 0.0f});
        } else if (uc->unit_class == UnitClass::Battalion ||
                   uc->unit_class == UnitClass::Brigade ||
                   uc->unit_class == UnitClass::TaskForce) {
            auto* vc = h.get<f4::entities::VehicleCompositionComponent>();
            if (!vc || vc->groups.empty()) {
                ImGui::TextDisabled(
                    "Ground unit has no vehicle composition to show "
                    "(campaign-level units carry no model of their own "
                    "— models exist at the vehicle level).");
                return;
            }
            std::snprintf(title, sizeof(title),
                          "%s — %zu vehicle group(s)",
                          uc->class_name.c_str(), vc->groups.size());
            // One line per group, up to 3 vehicles per group, groups
            // spaced along +X (east).
            float gx = 0.0f;
            int shown_groups = 0;
            for (const auto& grp : vc->groups) {
                if (shown_groups >= 8) break;
                const auto vis = impl_->class_table_3d.vis_type_for(
                    static_cast<uint16_t>(grp.vehicle_type), 0);
                if (vis <= 0) continue;
                const int n = std::clamp(grp.live_count > 0
                                             ? grp.live_count
                                             : grp.count,
                                         1, 3);
                for (int k = 0; k < n; ++k) {
                    placements.push_back({vis, gx,
                                          static_cast<float>(k) * 45.0f,
                                          0.0f});
                }
                gx += 140.0f;
                ++shown_groups;
            }
            if (placements.empty()) {
                ImGui::TextDisabled(
                    "None of the unit's vehicle types resolved to a 3D "
                    "model in the class table.");
                return;
            }
        } else {
            ImGui::TextDisabled(
                "Packages have no 3D model — select an element flight.");
            return;
        }
    } else {
        ImGui::TextDisabled("Nothing 3D-viewable selected.");
        return;
    }

    // --- Stage extent + camera refit (once per selection) -----------------
    //
    // Placements are staged around the shared orbit target
    // (ground_layout_3d_center_x/y, z=0). The same cached-entity key the
    // objective view uses makes the camera refit whenever the selection
    // (or its kind) changes.
    float max_dx = 0.0f, max_dy = 0.0f;
    for (const auto& pl : placements) {
        max_dx = std::max(max_dx, std::abs(pl.dx));
        max_dy = std::max(max_dy, std::abs(pl.dy));
    }
    const float cx = impl_->ground_layout_3d_center_x;
    const float cy = impl_->ground_layout_3d_center_y;
    if (impl_->ground_layout_3d_cached_entity != impl_->sel_entity) {
        impl_->ground_layout_3d_cached_entity = impl_->sel_entity;
        const float radius =
            std::max(std::max(max_dx, max_dy) * 2.0f + 100.0f, 200.0f);
        // fit_to_bbox takes RAYLIB coords (z = -north) — same mapping the
        // objective view's enu_to_rl-based fit lands on.
        impl_->gl3d_orbit_cam.fit_to_bbox({cx, 0.0f, -cy}, radius, 3.0f);
    }

    impl_->gl3d_orbit_cam.update_from_orbit();

    ImGui::TextUnformatted(title);
    ImGui::SameLine();
    ImGui::TextDisabled("(%d model%s)", static_cast<int>(placements.size()),
                        placements.size() == 1 ? "" : "s");
    if (!impl_->ground_layout_3d_show_grid) {
        ImGui::SameLine();
    }
    ImGui::Checkbox("Grid", &impl_->ground_layout_3d_show_grid);
    ImGui::SameLine();
    if (ImGui::Button("Reset View")) {
        const float radius =
            std::max(std::max(max_dx, max_dy) * 2.0f + 100.0f, 200.0f);
        impl_->gl3d_orbit_cam.fit_to_bbox({cx, 0.0f, -cy}, radius, 3.0f);
    }

    // --- Render into the shared offscreen target --------------------------
    const ImVec2 avail = ImGui::GetContentRegionAvail();
    const int img_w = std::max(static_cast<int>(avail.x), 64);
    const int img_h = std::max(static_cast<int>(avail.y) - 20, 64);

    if (!impl_->ground_layout_3d_target_valid) {
        if (impl_->ground_layout_3d_target.id != 0) {
            UnloadRenderTexture(impl_->ground_layout_3d_target);
            impl_->ground_layout_3d_target = {0};
        }
        impl_->ground_layout_3d_target = LoadRenderTexture(RT_W, RT_H);
        SetTextureFilter(impl_->ground_layout_3d_target.texture,
                         TEXTURE_FILTER_BILINEAR);
        impl_->ground_layout_3d_target_valid = true;
    }

    f4::renderer::SceneDescription scene;
    scene.camera = impl_->gl3d_orbit_cam.camera();
    scene.sky_color = BG_COLOR;
    scene.near_plane = 1.0f;
    scene.far_plane = 250000.0f;
    scene.target = &impl_->ground_layout_3d_target;

    // Flat ground plane + optional grid, anchored at the staging origin.
    scene.ground.plane = true;
    scene.ground.grid = impl_->ground_layout_3d_show_grid;
    scene.ground.axes = false;
    scene.ground.origin_enu_x = cx;
    scene.ground.origin_enu_y = cy;
    scene.ground.origin_enu_z = 0.0f;
    scene.ground.grid_extent =
        std::max(std::max(max_dx, max_dy) * 3.0f + 200.0f, 400.0f);
    scene.ground.grid_step = scene.ground.grid_extent > 4000.0f ? 1000.0f
                             : scene.ground.grid_extent > 1000.0f ? 500.0f
                                                                  : 100.0f;
    scene.ground.grid_color = GRID_COLOR;
    scene.ground.plane_color = BG_COLOR;

    scene.overlay_3d = [this, &placements, &cx, &cy](const Camera3D&) {
        rlDisableBackfaceCulling();
        rlSetBlendMode(BLEND_ALPHA);

        f4::renderer::EntityRenderResources res =
            f4::renderer::make_entity_render_resources(
                impl_->render_res_3d,
                &*impl_->model_db_3d,
                &impl_->class_table_3d);
        res.show_ground_layout = false;

        for (const auto& pl : placements) {
            f4::renderer::draw_vis_type_mesh(
                res, pl.vis_type, cx + pl.dx, cy + pl.dy, 0.0f,
                pl.facing_deg);
        }

        rlEnableBackfaceCulling();
    };

    f4::renderer::render_world(impl_->render_res_3d, scene);

    // --- Display + mouse interaction (same contract as the objective view)
    rlImGuiImageRect(&impl_->ground_layout_3d_target.texture, img_w, img_h,
        Rectangle{0, 0,
                  (float)impl_->ground_layout_3d_target.texture.width,
                  -(float)impl_->ground_layout_3d_target.texture.height});

    const bool img_hovered = ImGui::IsItemHovered();
    if (img_hovered) {
        const Vector2 wheel = GetMouseWheelMoveV();
        if (wheel.y != 0.0f) {
            const float factor = std::exp(-wheel.y * 0.03f);
            impl_->gl3d_orbit_cam.set_distance(
                std::clamp(impl_->gl3d_orbit_cam.distance() * factor,
                           50.0f, 50000.0f));
        }
    }
    static bool s_dragging_entity_3d = false;
    if (img_hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        s_dragging_entity_3d = true;
    }
    if (s_dragging_entity_3d &&
        ImGui::IsMouseReleased(ImGuiMouseButton_Left)) {
        s_dragging_entity_3d = false;
    }
    if (s_dragging_entity_3d) {
        const ImVec2 delta = ImGui::GetIO().MouseDelta;
        const float deg_per_px = 0.2865f;  // ~0.005 rad/px
        impl_->gl3d_orbit_cam.set_yaw(
            impl_->gl3d_orbit_cam.yaw() - delta.x * deg_per_px);
        impl_->gl3d_orbit_cam.set_pitch(
            std::clamp(impl_->gl3d_orbit_cam.pitch() + delta.y * deg_per_px,
                       -89.0f, 89.0f));
    }

    impl_->gl3d_orbit_cam.update_from_orbit();

    ImGui::TextDisabled("yaw: %.0f°   pitch: %.0f°   d: %.0f ft",
                        impl_->gl3d_orbit_cam.yaw(),
                        impl_->gl3d_orbit_cam.pitch(),
                        impl_->gl3d_orbit_cam.distance());
    ImGui::TextDisabled("drag = orbit, scroll = zoom");
}

} // namespace f4::viewer
