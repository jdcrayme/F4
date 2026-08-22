// f4-world-viewer/src/ground_layout_3d.cpp
//
// "Ground Layout 3D" tab content — a 3D top-down/perspective view of
// the selected objective's airfield geometry, rendered into an offscreen
// RenderTexture2D via Raylib's BeginMode3D and displayed inside the
// ImGui window via rlImGuiImageSize.
//
// Content-only: the caller (draw_inspector_window) owns the ImGui window
// and tab item. This function draws the 3D viewport (or a placeholder if
// no applicable objective is selected).
//
// Refactored to content-only (INSPECTOR-TABS-1) — no longer opens its
// own ImGui::Begin/End. The lazy model/texture loading and orbit-camera
// update logic now run unconditionally when this tab is selected so the
// 3D state stays consistent with the 2D canvas's feature-mesh pass
// (they share Impl::mesh_cache_3d / texture_cache_3d).
//
// The geometry is built by ground_layout_models.cpp (pure function) and
// cached on Impl::ground_layout_3d_geometry. We rebuild only when the
// selected entity changes (compared by EntityId).
//
// Mouse interaction (only when the ImGui image is hovered):
//   - Left-drag: orbit (yaw + pitch)
//   - Scroll:    zoom (distance)
//
// Coordinate conversion: the layout-to-geometry module emits ENU feet
// (X=East, Y=North, Z=Up). Raylib uses RH Y-up (X=right, Y=up, Z=toward
// viewer). The mapping is:
//     raylib_x =  enu_x
//     raylib_y =  enu_z
//     raylib_z = -enu_y
//
// See Docs/SCENARIO_PLAYER_PLAN.md §5.5 for the convention rationale.

#include "viewer_state.hpp"

#include <f4/entities/entity.hpp>
#include <f4/viewer/enum_text.hpp>
#include <f4/renderer/entity_render.hpp>     // EntityRenderResources, make_entity_render_resources
#include <f4/renderer/layout_draw.hpp>
#include <f4/renderer/scene_draw.hpp>            // draw_airfield_geometry

// f4-models + f4-world-convert headers MUST come before raylib.h because
// Raylib defines `PI` as a preprocessor macro that would otherwise collide
// with any transitive `using PI` declaration. (The scenario-player uses
// the same include-order pattern — see f4-scenario-player/src/renderer.cpp.)
#include <f4/models/model_database.hpp>
#include <f4/models/geometry.hpp>
#include <f4/models/texture.hpp>
#include <f4/world_convert/class_table.hpp>
#include <f4/install/installation.hpp>

#include <imgui.h>
#include <raylib.h>
#include <raymath.h>
#include <rlgl.h>
#include <rlImGui.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <utility>
#include <vector>

namespace f4::viewer {
// The geometry builder + draw primitives moved to f4-renderer; keep the
// short names used throughout this file.
using f4::renderer::AirfieldGeometry3D;
using f4::renderer::build_airfield_geometry_3d;
using f4::renderer::draw_layout_quad;
using f4::renderer::draw_layout_line;
using f4::renderer::draw_layout_marker;

namespace {

// ---------------------------------------------------------------------------
// Constants
// ---------------------------------------------------------------------------

// Offscreen render target size. Larger = sharper but slower. 800x600 is
// a good balance for an embedded panel — the user can resize the ImGui
// window, and rlImGuiImageSize scales the texture to fit.
constexpr int RT_W = 800;
constexpr int RT_H = 600;

// Background + grid colors.
constexpr Color BG_COLOR     = { 22,  24,  30, 255};
constexpr Color GRID_COLOR   = { 50,  54,  62, 255};
constexpr Color ORIGIN_COLOR = {255, 255,   0, 200};

// ---------------------------------------------------------------------------
// Coordinate helpers — thin wrappers around f4::renderer functions
// ---------------------------------------------------------------------------
// f4::renderer::enu_to_raylib / model_vertex_to_raylib return Float3;
// Raylib draw calls need Vector3. These wrappers convert for convenience.

inline Vector3 enu_to_rl(float enu_x, float enu_y, float enu_z) {
    auto f = f4::renderer::enu_to_raylib(enu_x, enu_y, enu_z-10);
    return { f.x, f.y, f.z };
}

inline Vector3 model_vertex_to_rl(float x, float y, float z) {
    auto f = f4::renderer::model_vertex_to_raylib(x, y, z);
    return { f.x, f.y, f.z };
}

// resolve_vertex_color and lit shader source are now provided by
// f4::renderer (mesh_builder.hpp + lit_shader.hpp). No local duplicates.

// ---------------------------------------------------------------------------
// Drawing — flat ground quads (runway surface, taxiway strip, threshold
// bars, building footprints). Corners are CCW from above with +Z up.
// ---------------------------------------------------------------------------


// ---------------------------------------------------------------------------
// Drawing — line segments (centerline dashes, taxiway centerlines)
// ---------------------------------------------------------------------------


// ---------------------------------------------------------------------------
// Drawing — labeled markers
// ---------------------------------------------------------------------------


// ---------------------------------------------------------------------------
// Drawing — ground grid for orientation reference
// ---------------------------------------------------------------------------

void draw_ground_grid(float center_x, float center_y,
                      float extent_ft, float step_ft) {
    for (float gx = std::ceil((-extent_ft + center_x) / step_ft) * step_ft;
         gx <= extent_ft + center_x; gx += step_ft) {
        const Vector3 a = enu_to_rl(gx, center_y - extent_ft, 0.0f);
        const Vector3 b = enu_to_rl(gx, center_y + extent_ft, 0.0f);
        DrawLine3D(a, b, GRID_COLOR);
    }
    for (float gy = std::ceil((-extent_ft + center_y) / step_ft) * step_ft;
         gy <= extent_ft + center_y; gy += step_ft) {
        const Vector3 a = enu_to_rl(center_x - extent_ft, gy, 0.0f);
        const Vector3 b = enu_to_rl(center_x + extent_ft, gy, 0.0f);
        DrawLine3D(a, b, GRID_COLOR);
    }
    // Origin marker (objective center, z=0).
    const Vector3 o = enu_to_rl(center_x, center_y, 0.0f);
    DrawLine3D({o.x - 8, o.y, o.z}, {o.x + 8, o.y, o.z}, ORIGIN_COLOR);
    DrawLine3D({o.x, o.y, o.z - 8}, {o.x, o.y, o.z + 8}, ORIGIN_COLOR);
}

// ---------------------------------------------------------------------------
// Drawing — labels (project 3D marker position to 2D screen, draw text
// in the overlay pass after EndMode3D)
// ---------------------------------------------------------------------------



} // anonymous namespace

// ---------------------------------------------------------------------------
// Impl method definitions — KoreaObj model loading
// ---------------------------------------------------------------------------
//
// Only ensure_models_3d_loaded() (asset discovery) remains app-specific.
// The mesh/texture caches, default material, and cleanup are owned by
// f4::renderer::RenderResources (Impl::render_res_3d), shared with the
// 2D canvas feature-mesh pass and the 3D world mode.

bool ViewerApp::Impl::ensure_models_3d_loaded() {
    if (models_3d_load_attempted) return models_3d_loaded;
    models_3d_load_attempted = true;

    // Resolve KoreaObj.HDR + .LOD + .TEX + Falcon4.ct from the install.
    if (!install.has_value()) {
        models_3d_error = "No installation configured — set the install path to load feature models.";
        return false;
    }
    const auto& inst = *install;
    if (inst.terrdata_dir().empty()) {
        models_3d_error = "Installation has no terrdata/ — cannot locate KoreaObj files.";
        return false;
    }

    // Locate KoreaObj.HDR/.LOD (or .DXH/.DXL) under the install root.
    auto [hdr_path, lod_path] =
        f4::models::ModelDatabase::find_koreaobj_files(inst.root());
    if (hdr_path.empty() || lod_path.empty()) {
        // Try terrdata/ root as fallback (some installs nest objects there).
        const auto& td = inst.terrdata_dir();
        auto [hdr2, lod2] = f4::models::ModelDatabase::find_koreaobj_files(td);
        if (!hdr2.empty() && !lod2.empty()) {
            hdr_path = hdr2;
            lod_path = lod2;
        }
    }
    if (hdr_path.empty() || lod_path.empty()) {
        models_3d_error = "KoreaObj.HDR/.LOD not found under install root.";
        return false;
    }

    // Locate the .TEX file (try install root first, then terrdata/).
    auto tex_path = f4::models::ModelDatabase::find_tex_file(inst.root());
    if (tex_path.empty()) {
        tex_path = f4::models::ModelDatabase::find_tex_file(inst.terrdata_dir());
    }

    // Load the class table (FALCON4.ct) — needed to map FeatureEntryState.index
    // (entity_type) → vis_type[0] (KoreaObj model index).
    const auto& ct_path = inst.class_table();
    if (ct_path.empty()) {
        models_3d_error = "FALCON4.ct not found — cannot resolve feature entity_type to model.";
        return false;
    }
    try {
        class_table_3d.load(ct_path);
    } catch (const std::exception& e) {
        models_3d_error = std::string("Failed to load FALCON4.ct: ") + e.what();
        return false;
    }

    // Load the model database.
    model_db_3d.emplace();
    auto err = model_db_3d->load(hdr_path, lod_path);
    if (!err.empty()) {
        model_db_3d.reset();
        models_3d_error = "ModelDatabase::load failed: " + err;
        return false;
    }
    if (!tex_path.empty()) {
        auto tex_err = model_db_3d->load_tex(tex_path);
        if (!tex_err.empty()) {
            // Textures are optional — features will render with vertex
            // colors only. Don't treat this as a hard failure.
            // (Note: many FF installs don't ship KoreaObj.TEX at all —
            //  the textures live in the theater's texture atlas instead.)
        }
    }

    models_3d_loaded = true;
    return true;
}

// ---------------------------------------------------------------------------
// Public method — ViewerApp::draw_ground_layout_3d
// ---------------------------------------------------------------------------

void ViewerApp::draw_ground_layout_3d() {
    // Only meaningful when an objective with ground_layout OR features is
    // selected. When the user is on this tab without an applicable selection,
    // show a placeholder so the tab stays stable (doesn't disappear).
    if (impl_->sel_kind != Impl::SelectionKind::Objective ||
        !impl_->sel_entity.valid()) {
        ImGui::TextDisabled("Select an objective to view its 3D layout.");
        return;
    }
    auto h = impl_->handle(impl_->sel_entity);
    auto* gl = h.get<f4::entities::GroundLayoutComponent>();
    auto* fs = h.get<f4::entities::FeatureSetComponent>();
    auto* ot = h.get<f4::entities::ObjectiveTypeComponent>();
    const bool has_layout = gl && !gl->layouts.empty();
    const bool has_features = fs && !fs->features.empty();
    if (!has_layout && !has_features) {
        ImGui::TextDisabled("Selected objective has no ground layout or features.");
        return;
    }

    // Lazily load KoreaObj models + FALCON4.ct the first time we have a
    // feature-bearing objective selected. The load is ~50-150ms; once
    // loaded, subsequent calls are no-ops. On failure, we silently fall
    // back to flat footprint rendering (the toggle just won't do
    // anything visible).
    if (has_features && impl_->ground_layout_3d_show_models &&
        !impl_->models_3d_load_attempted) {
        impl_->ensure_models_3d_loaded();
    }
    const bool models_ready = impl_->models_3d_loaded &&
                              impl_->model_db_3d.has_value() &&
                              impl_->class_table_3d.loaded();

    // Local lambda for fitting orbit camera to bbox.
    auto default_orbit_for_bbox = [&](const AirfieldGeometry3D& g) {
        if (g.empty) return;
        const float cx = (g.min_x + g.max_x) * 0.5f;
        const float cy = (g.min_y + g.max_y) * 0.5f;
        const float w = g.max_x - g.min_x;
        const float h = g.max_y - g.min_y;
        const float diag = std::sqrt(w * w + h * h);
        impl_->ground_layout_3d_center_x = cx;
        impl_->ground_layout_3d_center_y = cy;
        // Use OrbitCamera.fit_to_bbox to set camera position.
        const Vector3 center = enu_to_rl(cx, cy, 0.0f);
        const float radius = diag * 0.5f;
        impl_->gl3d_orbit_cam.fit_to_bbox(center, std::max(radius, 250.0f), 3.0f);
    };

    // Rebuild the geometry when the selection changes. We compare by
    // EntityId — the geometry is purely a function of the layout/features
    // data, which doesn't mutate at runtime (the user can't edit it).
    if (impl_->ground_layout_3d_cached_entity != impl_->sel_entity) {
        impl_->ground_layout_3d_geometry = build_airfield_geometry_3d(
            gl ? gl->layouts : std::vector<f4::entities::GroundLayoutList>{},
            (fs && impl_->ground_layout_3d_show_features) ? &fs->features : nullptr);
        impl_->ground_layout_3d_cached_entity = impl_->sel_entity;
        default_orbit_for_bbox(impl_->ground_layout_3d_geometry);
    }
    // If the user toggled "show features" since the last rebuild, force
    // a rebuild (cheap — pure function).
    if (fs && !fs->features.empty() &&
        (impl_->ground_layout_3d_geometry.feature_footprints.empty() ==
         impl_->ground_layout_3d_show_features)) {
        impl_->ground_layout_3d_geometry = build_airfield_geometry_3d(
            gl ? gl->layouts : std::vector<f4::entities::GroundLayoutList>{},
            impl_->ground_layout_3d_show_features ? &fs->features : nullptr);
        impl_->ground_layout_3d_cached_entity = impl_->sel_entity;
    }
    const auto& g = impl_->ground_layout_3d_geometry;
    if (g.empty) {
        ImGui::TextDisabled("(no geometry built from this objective's layout)");
        return;  // nothing to render
    }

    // Update the orbit camera BEFORE the render pass.
    impl_->gl3d_orbit_cam.update_from_orbit();
    // Header — class name + counts.
    if (ot && !ot->class_name.empty()) {
        ImGui::TextUnformatted(ot->class_name.c_str());
    } else {
        ImGui::TextUnformatted("Objective");
    }
    ImGui::SameLine();
    ImGui::TextDisabled("(%d runways, %d parking, %d taxi strips, %d features)",
        static_cast<int>(g.runway_surfaces.size()),
        static_cast<int>(g.parking_spots.size()),
        static_cast<int>(g.taxiway_strips.size()),
        static_cast<int>(g.feature_footprints.size()));

    // Layer toggles (compact, on one line).
    ImGui::Checkbox("Runway",   &impl_->ground_layout_3d_show_runway);
    ImGui::SameLine();
    ImGui::Checkbox("Taxiways", &impl_->ground_layout_3d_show_taxiways);
    ImGui::SameLine();
    ImGui::Checkbox("Parking",  &impl_->ground_layout_3d_show_parking);
    ImGui::SameLine();
    ImGui::Checkbox("Features", &impl_->ground_layout_3d_show_features);
    ImGui::SameLine();
    ImGui::Checkbox("3D Models", &impl_->ground_layout_3d_show_models);
    ImGui::SameLine();
    ImGui::Checkbox("Labels",   &impl_->ground_layout_3d_show_labels);
    ImGui::SameLine();
    ImGui::Checkbox("Grid",     &impl_->ground_layout_3d_show_grid);

    // Reset-view button.
    ImGui::SameLine();
    if (ImGui::Button("Reset View")) {
        default_orbit_for_bbox(g);
    }

    // Model-loading status indicator (only shown when 3D Models is on
    // AND we have features). Helps the user understand why nothing is
    // rendering as 3D — common cases: no install set, KoreaObj.HDR not
    // found, FALCON4.ct missing.
    if (impl_->ground_layout_3d_show_models && has_features) {
        if (!impl_->install.has_value()) {
            ImGui::TextDisabled("[3D models: no install set — using footprints]");
        } else if (!models_ready) {
            const char* err = impl_->models_3d_error.empty()
                ? "(loading…)" : impl_->models_3d_error.c_str();
            ImGui::TextDisabled("[3D models: %s]", err);
        } else {
            const int cached = static_cast<int>(impl_->render_res_3d.mesh_cache.size());
            const int textures = static_cast<int>(impl_->render_res_3d.texture_cache.map().size());
            ImGui::TextDisabled("[3D models: %d cached, %d textures]",
                                cached, textures);
            // Per-frame diagnostic — shows where features are being
            // dropped in the pipeline. The numbers reset every frame so
            // they always reflect the CURRENT selection. If "drawn" is 0
            // but "total" is non-zero, the bug is in the feature → vis_type
            // → mesh pipeline (visible in the skipped counters).
            ImGui::TextDisabled(
                "[features: %d total | drawn=%d | no_vistype=%d | no_mesh=%d | placeholder=%d]",
                impl_->diag_3d_features_total,
                impl_->diag_3d_features_drawn,
                impl_->diag_3d_features_no_vistype,
                impl_->diag_3d_features_no_mesh,
                impl_->diag_3d_features_skipped_placeholder);
            ImGui::TextDisabled("[meshes drawn: %d, triangles: %d]",
                                impl_->diag_3d_meshes_drawn,
                                impl_->diag_3d_triangles_drawn);
        }
    }

    // Available content region for the embedded image.
    const ImVec2 avail = ImGui::GetContentRegionAvail();
    const int img_w = std::max(static_cast<int>(avail.x), 64);
    const int img_h = std::max(static_cast<int>(avail.y) - 20, 64);  // -20 for footer

    // Lazily allocate the RenderTexture2D on first use.
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

    // --- Render the 3D scene into the RenderTexture2D --------------------
    BeginTextureMode(impl_->ground_layout_3d_target);
    {
        ClearBackground(BG_COLOR);
        BeginMode3D(impl_->gl3d_orbit_cam.camera());
        {
            // CRITICAL: disable backface culling. Our ground quads have
            // arbitrary winding (the layout data doesn't guarantee CCW),
            // and we draw both sides anyway. Mirrors the f4-models-viewer
            // approach for FreeFalcon's .LOD meshes.
            rlDisableBackfaceCulling();
            rlSetBlendMode(BLEND_ALPHA);

            // --- Diagnostic: origin axis triad (always drawn) ---------------
            //
            // Draw a small RGB axis triad at the camera target so the user
            // can ALWAYS see SOMETHING in the viewport — even if no
            // features/runways render. If the triad is invisible, the bug
            // is in the camera/projection (BeginMode3D not setting up
            // matrices correctly). If the triad is visible but nothing
            // else is, the bug is in the feature/mesh pipeline.
            //
            // +X = red (East in ENU), +Y = green (Up), +Z = blue (-North).
            // Length scales with bbox size so it stays proportional.
            {
                const float axis_len = std::max(
                    (g.max_x - g.min_x) * 0.05f,
                    (g.max_y - g.min_y) * 0.05f);
                const float al = std::max(axis_len, 50.0f);
                const Vector3 o = enu_to_rl(
                    impl_->ground_layout_3d_center_x,
                    impl_->ground_layout_3d_center_y, 0.0f);
                DrawLine3D(o, {o.x + al, o.y, o.z}, Color{220, 60, 60, 255});
                DrawLine3D(o, {o.x, o.y + al, o.z}, Color{60, 220, 60, 255});
                DrawLine3D(o, {o.x, o.y, o.z + al}, Color{60, 100, 220, 255});
            }

            // Ground grid (orientation reference).
            if (impl_->ground_layout_3d_show_grid) {
                const float ext = std::max(
                    (g.max_x - g.min_x) * 0.6f,
                    (g.max_y - g.min_y) * 0.6f);
                const float step = ext > 4000.0f ? 1000.0f :
                                   ext > 1000.0f ?  500.0f : 100.0f;
                draw_ground_grid(impl_->ground_layout_3d_center_x,
                                 impl_->ground_layout_3d_center_y,
                                 ext, step);
            }

            // Airfield geometry — drawn via the shared f4::renderer function
            // (scene_draw.cpp). The per-layer toggles mirror the View-menu
            // checkboxes: runway + markers, taxiways, parking, helipads.
            // Feature footprints are drawn only when 3D models are off (or
            // unavailable); otherwise the KoreaObj meshes draw below.
            {
                f4::renderer::AirfieldDrawToggles toggles;
                toggles.runway   = impl_->ground_layout_3d_show_runway;
                toggles.markers  = impl_->ground_layout_3d_show_runway;
                toggles.taxiways = impl_->ground_layout_3d_show_taxiways;
                toggles.parking  = impl_->ground_layout_3d_show_parking;
                toggles.helipads = true;
                toggles.features = !has_features ||
                                    !impl_->ground_layout_3d_show_models ||
                                    !models_ready;
                f4::renderer::draw_airfield_geometry(g, toggles);
            }

            // --- Real KoreaObj 3D feature models ------------------------
            //
            // For each FeatureEntryState on the selected objective, resolve
            // its vis_type via FALCON4.ct (entity_type → vis_type[0]),
            // build the Raylib Mesh for that KoreaObj parent_index (cached),
            // and DrawMesh it at the feature's offset_xyz rotated by
            // facing degrees around the vertical axis.
            //
            // The mesh cache is keyed by vis_type, so multiple features
            // of the same type (e.g. three hangars of type 169) share
            // one GPU upload. Textures are cached by tex_id across all
            // meshes — shared across features and across selections.

            // Reset per-frame diagnostic counters (always, even when we
            // won't draw models — so the panel shows 0/0/0 instead of
            // stale values from a previous selection).
            impl_->diag_3d_features_total = 0;
            impl_->diag_3d_features_skipped_placeholder = 0;
            impl_->diag_3d_features_no_vistype = 0;
            impl_->diag_3d_features_no_mesh = 0;
            impl_->diag_3d_features_drawn = 0;
            impl_->diag_3d_meshes_drawn = 0;
            impl_->diag_3d_triangles_drawn = 0;

            if (impl_->ground_layout_3d_show_models && models_ready && fs) {
                // Compile the lit shader on first use (via f4::renderer::LitShader).
                // Also build the cached default material (1x1 white fallback
                // texture + lit shader) for untextured meshes.
                //
                // draw_feature_mesh() will re-ensure both per-call (idempotent),
                // but doing it once here lets us bail out early if the shader
                // or material can't be built (avoids spamming DrawMesh with
                // an uninitialized material).
                if (!impl_->render_res_3d.ensure_default_material()) {
                    // Failed to build the fallback texture — can't safely
                    // draw meshes (the shader would discard them all).
                    // Skip the model pass; the grid + axis triad + flat
                    // geometry are still visible so the user knows the
                    // panel is alive.
                } else {
                    // Build the resource bundle once for the whole loop
                    // from the shared RenderResources instance. The mesh
                    // cache, texture cache, lit shader, default material,
                    // and lighting state all live on render_res_3d and
                    // are shared with the 2D canvas feature-mesh pass —
                    // so models loaded by either view are free for the other.
                    f4::renderer::EntityRenderResources res =
                        f4::renderer::make_entity_render_resources(
                            impl_->render_res_3d,
                            &*impl_->model_db_3d,
                            &impl_->class_table_3d);

                    // Walk features and draw each one's model.
                    impl_->diag_3d_features_total =
                        static_cast<int>(fs->features.size());
                    for (const auto& f : fs->features) {
                        // Skip empty placeholder features (the bridge emits
                        // these when the FED entry is unused).
                        if (f.index == 0 && f.offset_x == 0.0f && f.offset_y == 0.0f) {
                            ++impl_->diag_3d_features_skipped_placeholder;
                            continue;
                        }

                        // Convert FeatureEntryState.index (descriptionIndex)
                        // to entity_type. See the long comment in
                        // f4-renderer/src/feature_mesh.cpp + the historical
                        // note below: FeatureEntryState.index is a 0-based
                        // descriptionIndex into the class table, NOT an
                        // entity_type. ClassTable::vis_type_for() takes
                        // entity_type = descriptionIndex + VU_LAST_ENTITY_TYPE
                        // (=100). Passing f.index verbatim used to land at
                        // descriptionIndex-100 — i.e. a completely different
                        // class table entry — which is why power plants
                        // rendered as upside-down B-52s.
                        //
                        // (Historical note preserved here because the
                        // diagnostic counters below still need to distinguish
                        // "no vis_type" from "no mesh". The pipeline comment
                        // now also lives in f4-renderer/src/feature_mesh.cpp.)
                        constexpr uint16_t VU_LAST_ENTITY_TYPE = 100;
                        const uint16_t entity_type = static_cast<uint16_t>(
                            VU_LAST_ENTITY_TYPE +
                            static_cast<uint16_t>(f.index));

                        // Peek at vis_type so we can classify skips for
                        // the per-frame diagnostic. draw_feature_mesh() also
                        // looks up vis_type internally (cheap — hash lookup),
                        // so the redundancy is fine.
                        const auto vis_type =
                            impl_->class_table_3d.vis_type_for(entity_type, 0);
                        if (vis_type <= 0) {
                            ++impl_->diag_3d_features_no_vistype;
                            continue;
                        }

                        const auto stats = f4::renderer::draw_feature_mesh(
                            res, entity_type,
                            f.offset_x, f.offset_y, f.offset_z,
                            static_cast<float>(f.facing));

                        if (stats.meshes_drawn > 0) {
                            ++impl_->diag_3d_features_drawn;
                            impl_->diag_3d_meshes_drawn += stats.meshes_drawn;
                            // Triangle count: not exposed by DrawStats (only
                            // vertices), but vertices ≈ 3 × triangles for
                            // typical meshes. Close enough for the panel's
                            // "drawn / total" diagnostic.
                            impl_->diag_3d_triangles_drawn +=
                                static_cast<int>(stats.vertices_drawn) / 3;
                        } else {
                            ++impl_->diag_3d_features_no_mesh;
                        }
                    }
                }
            }

            rlEnableBackfaceCulling();
        }
        EndMode3D();

        // 2D overlay — labels projected from 3D positions.
        if (impl_->ground_layout_3d_show_labels) {
            std::vector<f4::renderer::LayoutLabel2D> labels;
            f4::renderer::collect_layout_labels(impl_->gl3d_orbit_cam.camera(), g,
                           impl_->ground_layout_3d_show_parking,
                           RT_W, RT_H, 0.0f, 0.0f, 0.0f, labels);
            f4::renderer::draw_layout_labels(labels);
        }
    }
    EndTextureMode();

    // --- Display the rendered texture in the ImGui window -----------------
    //
    // rlImGuiImageSize takes a Texture* (not RenderTexture*) and a width/
    // height. We pass the RenderTexture's .texture field and the
    // content-region size so the image scales to fit the window.
    rlImGuiImageSize(&impl_->ground_layout_3d_target.texture, img_w, img_h);

    // --- Mouse input (orbit + zoom) — only when the image is hovered ------
    //
    // We use ImGui::IsItemHovered() to detect hover on the last-drawn
    // image. The orbit camera input is handled manually (updating yaw/pitch/
    // distance via accessors) rather than via OrbitCamera::handle_input()
    // because we need to restrict input to when the 3D panel image is hovered.
    const bool img_hovered = ImGui::IsItemHovered();
    if (img_hovered) {
        const Vector2 wheel = GetMouseWheelMoveV();
        if (wheel.y != 0.0f) {
            // Log-scale zoom.
            const float factor = std::exp(-wheel.y * 0.03f);
            impl_->gl3d_orbit_cam.set_distance(
                std::clamp(impl_->gl3d_orbit_cam.distance() * factor,
                           50.0f, 50000.0f));
        }
    }
    static bool s_dragging_3d = false;
    if (img_hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        s_dragging_3d = true;
    }
    if (s_dragging_3d && ImGui::IsMouseReleased(ImGuiMouseButton_Left)) {
        s_dragging_3d = false;
    }
    if (s_dragging_3d) {
        const ImVec2 delta = ImGui::GetIO().MouseDelta;
        // OrbitCamera stores yaw/pitch in degrees; convert pixel delta.
        const float deg_per_px = 0.2865f;  // ~0.005 rad/px
        impl_->gl3d_orbit_cam.set_yaw(
            impl_->gl3d_orbit_cam.yaw() + delta.x * deg_per_px);
        impl_->gl3d_orbit_cam.set_pitch(
            std::clamp(impl_->gl3d_orbit_cam.pitch() - delta.y * deg_per_px,
                       -89.0f, 89.0f));
    }

    // Re-update orbit camera so drag this frame is reflected next frame.
    impl_->gl3d_orbit_cam.update_from_orbit();

    // Footer — bbox + camera info.
    ImGui::TextDisabled("bbox: %.0f x %.0f ft   yaw: %.0f°   pitch: %.0f°   d: %.0f ft",
                        g.max_x - g.min_x, g.max_y - g.min_y,
                        impl_->gl3d_orbit_cam.yaw(),
                        impl_->gl3d_orbit_cam.pitch(),
                        impl_->gl3d_orbit_cam.distance());
    ImGui::TextDisabled("drag = orbit, scroll = zoom");
    // (No ImGui::End() here — caller owns the window.)
}

} // namespace f4::viewer
