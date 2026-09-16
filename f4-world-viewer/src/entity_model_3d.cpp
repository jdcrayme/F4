// f4-world-viewer/src/entity_model_3d.cpp
//
// "3D" tab content for NON-objective selections — squadrons, ground
// units (battalion/brigade/taskforce), flights, and live session
// aircraft. The objective branch lives in ground_layout_3d.cpp; the
// Inspector's 3D tab dispatches between the two by selection kind.
//
// What each selection shows:
//   - Squadron: a parked row of its aircraft type (one per pilot,
//     capped), resolved via the unit's VEHICLE composition →
//     ClassTable::vis_type_for() — CT unit rows carry no vis of their
//     own, the model belongs to the vehicle type the unit operates.
//   - Flight: IN THE WORLD when terrain data exists — its deaggregated
//     aircraft at their real formation positions when live, else an
//     echelon pair at the aggregate's position faced along the engine's
//     course, over the terrain around it (anchored at the flight, the
//     camera tracking it). Falls back to the staged echelon pair
//     without terrain data.
//   - Battalion/Brigade/TaskForce: its VehicleCompositionComponent
//     groups lined up (vehicle entity_type → vis_type per group).
//   - LiveAircraft: the entity's own VisualModelComponent model IN THE
//     WORLD — anchored at its real ENU position and simulated altitude,
//     over the textured terrain around it (the same WorldView/chunk
//     paths the objective view uses, following the aircraft as it
//     moves). Without terrain data it falls back to a staged model on
//     the ground plane; facing is velocity when moving, else the spawn
//     quaternion — the same convention as the map's 3D pass.
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


#include <imgui.h>
#include <raylib.h>
#include <rlgl.h>
#include <rlImGui.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <vector>

namespace f4::viewer {

namespace {

// One staged model: vis type + offset from the staging origin (ENU ft)
// + facing (degrees, CCW around +Z up). dz is the model's ENU altitude
// — 0 for the staged views (they sit on the ground plane); the live
// in-world view sets it to the aircraft's simulated MSL altitude.
struct ModelPlacement {
    int vis_type;
    float dx;
    float dy;
    float facing_deg;
    float dz = 0.0f;
    // ANIM-DOCTOR: per-entity channel values (live aircraft only).
    // Null → the draw path stages the model with parked defaults.
    const f4::anim::AnimValues* anim = nullptr;
};

constexpr float RT_W = 800;   // must match ground_layout_3d.cpp (the
constexpr float RT_H = 600;   // RenderTexture is shared)

constexpr Color BG_COLOR = {22, 24, 30, 255};
constexpr Color GRID_COLOR = {50, 54, 62, 255};

// The terrain meshes sink 5 ft below their true elevation so surface
// geometry doesn't z-fight — the same constant ground_layout_3d.cpp
// renders its terrain with (the two share the view state).
constexpr float GROUND_SINK_FT = -5.0f;

// A unit's OWN class-table row carries no vis — FALCON4.CT gives
// flights and squadrons all-zero vis_type arrays; the model belongs to
// the VEHICLE the unit operates (F-16C vehicle 273 → vis 1052). Resolve
// through the unit's vehicle composition first (the same rule the
// session's spawn paths use), then the unit's own row, then 0.
int unit_aircraft_vis(const f4::entities::EntityHandle& h,
                      const f4::entities::UnitCoreComponent* uc,
                      const f4::world_types::ClassTable& ct) {
    const auto* vc = h.get<f4::entities::VehicleCompositionComponent>();
    if (vc && !vc->groups.empty()) {
        const auto vis = ct.vis_type_for(
            static_cast<uint16_t>(vc->groups.front().vehicle_type), 0);
        if (vis > 0) return vis;
    }
    return ct.vis_type_for(static_cast<uint16_t>(uc->class_table_index), 0);
}

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
    if (!impl_->models_3d_loaded || !impl_->render_res_3d.model_cache.ready() ||
        !impl_->class_table_3d.loaded()) {
        ImGui::TextDisabled(
            "3D models not available — configure an install "
            "(KoreaObj + FALCON4.ct) to view units in 3D.");
        return;
    }

    // --- Gather the cast: title + model placements -----------------------
    std::vector<ModelPlacement> placements;
    char title[192] = "3D";

    // The live-aircraft "in world" presentation: anchored at the
    // aircraft's REAL ENU position + simulated altitude, over the
    // terrain around it (when terrain data exists). The other selections
    // stay staged on the ground plane at the shared orbit target.
    bool live_world_view = false;
    float live_ax = 0.0f, live_ay = 0.0f;
    float live_ground_z = 0.0f, live_draw_z = 0.0f;

    // Terrain elevation at an ENU position — the same ladder the
    // objective 3D tab uses: the near post level when the theater
    // binaries are loaded (it IS what the textured terrain renders; the
    // MEA summary can be hundreds of feet off in mountains), the 128x128
    // MEA summary as the JSON-only fallback.
    auto terrain_elev_ft = [&](float east_ft, float north_ft) -> float {
        if (impl_->theater_tiles_loaded) {
            return static_cast<float>(
                impl_->world.near_level().elevation_at_ft(east_ft, north_ft));
        }
        if (impl_->terrain_loaded && !impl_->terrain.elevation.empty()) {
            const double theater_size_ft = 1024.0 * 1024.0;
            const double w = static_cast<double>(
                impl_->terrain.header.width > 0
                    ? impl_->terrain.header.width : 128);
            const double ft_per_cell = theater_size_ft / w;
            const uint32_t cell_x = std::min(
                static_cast<uint32_t>(
                    std::max(east_ft / ft_per_cell, 0.0)),
                impl_->terrain.header.width - 1);
            const uint32_t cell_y_raw = std::min(
                static_cast<uint32_t>(
                    std::max(north_ft / ft_per_cell, 0.0)),
                impl_->terrain.header.height - 1);
            const uint32_t cell_y =
                impl_->terrain.header.height - 1 - cell_y_raw;
            return static_cast<float>(
                impl_->terrain.elevation_at(cell_x, cell_y));
        }
        return 0.0f;
    };

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
        live_world_view = impl_->terrain_loaded;
        std::snprintf(title, sizeof(title),
                      "Aircraft (vis type %d)", vmc->vis_type);
        if (live_world_view) {
            live_ax = static_cast<float>(tf->position.x);
            live_ay = static_cast<float>(tf->position.y);
            live_ground_z = terrain_elev_ft(live_ax, live_ay);
            // The model renders at the aircraft's SIMULATED altitude,
            // clamped to the rendered terrain so it can never sit
            // inside a mountain when the post-level elevation and the
            // flight model's ground estimate disagree.
            live_draw_z = std::max(static_cast<float>(tf->position.z),
                                   live_ground_z);
            placements.push_back({vmc->vis_type, 0.0f, 0.0f,
                                  facing_deg_from_transform(tf),
                                  live_draw_z, &vmc->anim_values});
        } else {
            placements.push_back({vmc->vis_type, 0.0f, 0.0f,
                                  facing_deg_from_transform(tf),
                                  0.0f, &vmc->anim_values});
        }
    } else if (impl_->sel_kind == Impl::SelectionKind::Unit) {
        auto h = impl_->unit_handle(impl_->sel_entity);
        auto* uc = h.get<f4::entities::UnitCoreComponent>();
        if (!uc) {
            ImGui::TextDisabled("Selected unit has no unit data.");
            return;
        }
        using f4::entities::UnitClass;
        if (uc->unit_class == UnitClass::Squadron) {
            const auto vis = unit_aircraft_vis(h, uc, impl_->class_table_3d);
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
            const auto vis = unit_aircraft_vis(h, uc, impl_->class_table_3d);
            if (vis <= 0) {
                ImGui::TextDisabled(
                    "Flight aircraft type %d has no 3D model.",
                    uc->class_table_index);
                return;
            }
            std::snprintf(title, sizeof(title),
                          "Flight — %s (vis type %d)",
                          uc->class_name.c_str(), vis);

            // An ACTIVE flight belongs in the world it is flying
            // through: the in-world view anchors at the flight (or its
            // deaggregated aircraft) and draws the terrain around it.
            // Without terrain data the staged ground-plane display
            // below still applies.
            auto* ftf = h.get<f4::entities::TransformComponent>();
            if (ftf && impl_->terrain_loaded) {
                live_world_view = true;
                live_ax = static_cast<float>(ftf->position.x);
                live_ay = static_cast<float>(ftf->position.y);

                // Deaggregated: the flight's OWN aircraft (matched
                // through the origin stamp's flight VU) render at their
                // real formation positions and altitudes. Aggregate:
                // the echelon pair at the flight's position, faced
                // along the engine's current course (the aggregate
                // transform carries no velocity of its own).
                const auto* pb = h.get<f4::entities::PropertyBag>();
                std::uint32_t vu = 0;
                if (pb) {
                    const auto it = pb->ints.find("vu_id_num");
                    if (it != pb->ints.end()) {
                        vu = static_cast<std::uint32_t>(it->second);
                    }
                }
                bool matched_live = false;
                if (vu != 0 && impl_->session) {
                    for (const auto eid : impl_->live_aircraft()) {
                        auto ah = impl_->session_handle(eid);
                        auto* org =
                            ah.get<f4::simulation::CampaignOriginComponent>();
                        auto* atf =
                            ah.get<f4::entities::TransformComponent>();
                        auto* avmc =
                            ah.get<f4::simulation::VisualModelComponent>();
                        if (!org || !atf || !avmc ||
                            org->flight_vu != vu) {
                            continue;
                        }
                        if (!matched_live) {
                            // Anchor on the lead aircraft — the
                            // aggregate's transform freezes once the
                            // aircraft own the truth.
                            live_ax = static_cast<float>(atf->position.x);
                            live_ay = static_cast<float>(atf->position.y);
                            live_ground_z = terrain_elev_ft(live_ax,
                                                            live_ay);
                            live_draw_z = std::max(
                                static_cast<float>(atf->position.z),
                                live_ground_z);
                            matched_live = true;
                        }
                        const float dxa =
                            static_cast<float>(atf->position.x) - live_ax;
                        const float dya =
                            static_cast<float>(atf->position.y) - live_ay;
                        const float gnd = terrain_elev_ft(live_ax + dxa,
                                                          live_ay + dya);
                        placements.push_back({
                            avmc->vis_type > 0 ? avmc->vis_type : vis,
                            dxa, dya,
                            facing_deg_from_transform(atf),
                            std::max(static_cast<float>(atf->position.z),
                                     gnd)});
                        if (placements.size() >= 4) break;  // one cell
                    }
                }
                if (!matched_live) {
                    live_ground_z = terrain_elev_ft(live_ax, live_ay);
                    live_draw_z = std::max(
                        static_cast<float>(ftf->position.z), live_ground_z);
                    float facing = 0.0f;
                    if (impl_->session) {
                        if (auto hdg =
                                impl_->session->flight_heading_rad(vu)) {
                            facing = static_cast<float>(
                                *hdg * 57.29577951308232);
                        } else {
                            facing = facing_deg_from_transform(ftf);
                        }
                    } else {
                        facing = facing_deg_from_transform(ftf);
                    }
                    placements.push_back({vis, 0.0f, 0.0f, facing,
                                          live_draw_z});
                    placements.push_back({vis, 80.0f, -80.0f, facing,
                                          live_draw_z});
                }
            } else {
                // Echelon pair (staged).
                placements.push_back({vis, 0.0f, 0.0f, 0.0f});
                placements.push_back({vis, 80.0f, -80.0f, 0.0f});
            }
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
    // Staged placements sit around the shared orbit target
    // (ground_layout_3d_center_x/y, z=0). The live in-world view anchors
    // at the AIRCRAFT instead — and its orbit target tracks the aircraft
    // every frame (a chase view: yaw/pitch/distance stay where the user
    // put them). The same cached-entity key the objective view uses
    // makes the camera refit whenever the selection (or its kind)
    // changes.
    float max_dx = 0.0f, max_dy = 0.0f;
    for (const auto& pl : placements) {
        max_dx = std::max(max_dx, std::abs(pl.dx));
        max_dy = std::max(max_dy, std::abs(pl.dy));
    }
    const float cx = live_world_view ? live_ax
                                     : impl_->ground_layout_3d_center_x;
    const float cy = live_world_view ? live_ay
                                     : impl_->ground_layout_3d_center_y;
    auto chase_fit = [&]() {
        // fit_to_bbox takes RAYLIB coords (x east, y up, z = -north) —
        // same mapping the objective view's enu_to_rl-based fit lands
        // on. 1200 ft of context radius puts the airframe at a readable
        // size with terrain around it.
        impl_->gl3d_orbit_cam.fit_to_bbox({cx, live_draw_z, -cy},
                                          1200.0f, 2.5f);
    };
    if (impl_->ground_layout_3d_cached_entity != impl_->sel_entity) {
        impl_->ground_layout_3d_cached_entity = impl_->sel_entity;
        if (live_world_view) {
            chase_fit();
        } else {
            const float radius =
                std::max(std::max(max_dx, max_dy) * 2.0f + 100.0f, 200.0f);
            impl_->gl3d_orbit_cam.fit_to_bbox({cx, 0.0f, -cy}, radius, 3.0f);
        }
    }
    if (live_world_view) {
        impl_->gl3d_orbit_cam.set_target({cx, live_draw_z, -cy});
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
        if (live_world_view) {
            chase_fit();
        } else {
            const float radius =
                std::max(std::max(max_dx, max_dy) * 2.0f + 100.0f, 200.0f);
            impl_->gl3d_orbit_cam.fit_to_bbox({cx, 0.0f, -cy}, radius, 3.0f);
        }
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

    // --- Terrain (live in-world view) -------------------------------------
    //
    // The same three paths the objective 3D tab uses (textured WorldView,
    // untextured chunk set, single mesh), keyed to the AIRCRAFT: it
    // moves, so the views rebuild when it drifts out of the last-built
    // near ring. Ownership is handed back and forth with the objective
    // view through the *_cached_entity keys — the entity view clears
    // them after a rebuild so the objective tab rebuilds on its next
    // selection, and it treats a set key as "the objective view owns the
    // current build" and rebuilds here.
    const bool want_terrain = live_world_view && impl_->show_terrain_mesh_3d;
    if (want_terrain) {
        constexpr float REBUILD_DRIFT_FT = 20000.0f;  // ~40% of the
        // near-tile ring — the aircraft stays well inside the detailed
        // terrain between rebuilds.
        const bool drifted =
            !impl_->entity_3d_terrain_valid ||
            std::hypot(live_ax - impl_->entity_3d_terrain_center_east_ft,
                       live_ay - impl_->entity_3d_terrain_center_north_ft) >
                REBUILD_DRIFT_FT;
        if (impl_->theater_tiles_loaded && impl_->world.ensure_gpu()) {
            if (impl_->world.chunk_set() == nullptr || drifted ||
                impl_->world_view_cached_entity.valid()) {
                impl_->world.set_view(impl_->terrain, live_ax, live_ay,
                                      /*extent_ft=*/250000.0f,
                                      /*near_extent_ft=*/50000.0f,
                                      /*z_offset_ft=*/GROUND_SINK_FT);
                impl_->world_view_cached_entity =
                    f4::entities::EntityId{};
                impl_->entity_3d_terrain_valid = true;
                impl_->entity_3d_terrain_center_east_ft = live_ax;
                impl_->entity_3d_terrain_center_north_ft = live_ay;
            }
        }
        if (impl_->world.chunk_set() == nullptr && impl_->use_terrain_chunks) {
            const bool rebuild =
                drifted || !impl_->terrain_chunk_set_3d_built ||
                !impl_->terrain_chunk_set_3d.valid ||
                impl_->terrain_chunk_set_3d_cached_entity.valid();
            if (rebuild) {
                if (impl_->terrain_chunk_set_3d_built) {
                    f4::renderer::unload_terrain_chunk_set(
                        impl_->terrain_chunk_set_3d);
                    impl_->terrain_chunk_set_3d_built = false;
                }
                f4::renderer::TerrainChunkSetConfig tcc;
                tcc.center_east_ft  = live_ax;
                tcc.center_north_ft = live_ay;
                tcc.extent_ft       = 50000.0f;
                tcc.chunks_per_side = 8;
                tcc.chunk_resolution = 32;
                tcc.vertical_scale   = 1.0f;
                tcc.z_offset_ft      = GROUND_SINK_FT;
                tcc.color_by_tile_type = true;
                tcc.far_plane_ft    = 250000.0f;
                tcc.near_plane_ft   = 1.0f;
                tcc.camera_fovy_deg = 45.0f;
                impl_->terrain_chunk_set_3d =
                    f4::renderer::build_terrain_chunk_set(impl_->terrain,
                                                          tcc);
                impl_->terrain_chunk_set_3d_built = true;
                impl_->terrain_chunk_set_3d_cached_entity =
                    f4::entities::EntityId{};
                impl_->entity_3d_terrain_valid = true;
                impl_->entity_3d_terrain_center_east_ft = live_ax;
                impl_->entity_3d_terrain_center_north_ft = live_ay;
            }
        } else if (impl_->world.chunk_set() == nullptr) {
            const bool rebuild =
                drifted || !impl_->terrain_mesh_3d_built ||
                !impl_->terrain_mesh_3d.valid ||
                impl_->terrain_mesh_3d_cached_entity.valid();
            if (rebuild) {
                if (impl_->terrain_mesh_3d_built) {
                    f4::renderer::unload_terrain_mesh(impl_->terrain_mesh_3d);
                    impl_->terrain_mesh_3d_built = false;
                }
                f4::renderer::TerrainMeshConfig tc;
                tc.center_east_ft = live_ax;
                tc.center_north_ft = live_ay;
                tc.extent_ft = 50000.0f;
                tc.resolution = 96;
                tc.vertical_scale = 1.0f;
                tc.z_offset_ft = GROUND_SINK_FT;
                tc.color_by_tile_type = true;
                impl_->terrain_mesh_3d =
                    f4::renderer::build_terrain_mesh(impl_->terrain, tc);
                impl_->terrain_mesh_3d_built = true;
                impl_->terrain_mesh_3d_cached_entity =
                    f4::entities::EntityId{};
                impl_->entity_3d_terrain_valid = true;
                impl_->entity_3d_terrain_center_east_ft = live_ax;
                impl_->entity_3d_terrain_center_north_ft = live_ay;
            }
        }
    }
    const bool terrain_active =
        want_terrain &&
        (impl_->world.chunk_set() != nullptr ||
         (impl_->use_terrain_chunks && impl_->terrain_chunk_set_3d_built &&
          impl_->terrain_chunk_set_3d.valid) ||
         (!impl_->use_terrain_chunks && impl_->terrain_mesh_3d_built &&
          impl_->terrain_mesh_3d.valid));
    // Textured WorldView terrain: per-frame shader uniforms (sun, fog
    // toward the panel's sky color) — same contract as the objective tab.
    if (want_terrain && impl_->world.chunk_set() != nullptr) {
        impl_->world.update_frame(BG_COLOR);
    }

    f4::renderer::SceneDescription scene;
    scene.camera = impl_->gl3d_orbit_cam.camera();
    scene.sky_color = BG_COLOR;
    scene.near_plane = 1.0f;
    scene.far_plane = 250000.0f;
    scene.target = &impl_->ground_layout_3d_target;

    // Terrain first (the textured chunk set wins over the flat plane —
    // drawing both z-fights), then the flat plane + optional grid for
    // the staged views, anchored at the staging origin / the aircraft.
    if (want_terrain) {
        if (impl_->world.chunk_set() != nullptr) {
            scene.terrain_chunk_set = impl_->world.chunk_set();
        } else if (impl_->use_terrain_chunks &&
                   impl_->terrain_chunk_set_3d_built &&
                   impl_->terrain_chunk_set_3d.valid) {
            scene.terrain_chunk_set = &impl_->terrain_chunk_set_3d;
        } else if (impl_->terrain_mesh_3d_built &&
                   impl_->terrain_mesh_3d.valid) {
            scene.terrain_mesh = &impl_->terrain_mesh_3d;
        }
    }
    scene.ground.plane = !terrain_active;
    scene.ground.grid = impl_->ground_layout_3d_show_grid && !terrain_active;
    scene.ground.axes = false;
    scene.ground.origin_enu_x = cx;
    scene.ground.origin_enu_y = cy;
    scene.ground.origin_enu_z =
        live_world_view ? live_ground_z : 0.0f;
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
                &impl_->class_table_3d);
        res.show_ground_layout = false;

        for (const auto& pl : placements) {
            f4::renderer::draw_vis_type_mesh(
                res, pl.vis_type, cx + pl.dx, cy + pl.dy, pl.dz,
                pl.facing_deg, pl.anim);
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
