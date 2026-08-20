// f4-scenario-player/src/renderer.cpp
//
// Camera + scene drawing for the scenario player. Mirrors the structure
// of f4-models-viewer's camera3d.cpp + canvas3d.cpp, but adapted for the
// larger-scale world (camera distances in hundreds of feet, not tens)
// and the ENU coordinate frame.
//
// CRITICAL: Raylib defines `PI` as a preprocessor macro (raylib.h:110),
// which collides with `f4::math::PI` brought in by f4-flight-model's
// constants.hpp. We must include all f4-flight-model headers BEFORE
// raylib.h so the `using f4::math::PI;` declaration resolves against
// the real constant, not the macro. (Once the macro is defined, the
// `using` declaration breaks with a confusing parse error.)

#include "viewer_state.hpp"

#include <f4/simulation/visual_model_component.hpp>
#include <f4/ai/brain_component.hpp>
#include <f4/entities/entity.hpp>
#include <f4/flight/flight_model_component.hpp>
#include <f4/flight/angle.hpp>
#include <f4/models/model_database.hpp>
#include <f4/renderer/draw_3d.hpp>
#include <f4/renderer/layout_draw.hpp>
#include <f4/renderer/mesh_builder.hpp>
#include <f4/renderer/coord_transform.hpp>

// Now safe to include Raylib (PI macro won't break the flight headers).
#include <imgui.h>
#include <raylib.h>
#include <raymath.h>
#include <rlgl.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

namespace f4::scenario_player {

// ── Constants ──────────────────────────────────────────────────────────────
// Colors
static constexpr Color SKY_COLOR  = {135, 175, 220, 255};  // sky blue
static constexpr Color GROUND_COLOR = {50, 70, 35, 255};   // green grass

// ── Camera (delegated to f4::renderer::OrbitCamera) ────────────────────────

void PlayerApp::Impl::handle_camera_input() {
    const ImGuiIO& io = ImGui::GetIO();
    if (!io.WantCaptureKeyboard) {
        if (IsKeyPressed(KEY_F)) fit_to_aircraft();
        if (IsKeyPressed(KEY_C)) { follow_aircraft = !follow_aircraft; status_msg = follow_aircraft ? "Camera: following aircraft" : "Camera: free"; }
        if (IsKeyPressed(KEY_R)) reset_camera();
        if (IsKeyPressed(KEY_SPACE)) {
            paused = !paused;
            status_msg = paused ? "Paused" : "Running";
        }
    }
    // Delegate orbit/pan/zoom to OrbitCamera (guards ImGui::WantCaptureMouse internally).
    orbit_cam.handle_input();

    // Follow mode: track the aircraft every frame (position only — the
    // user keeps orbit control of yaw/pitch/distance around it).
    if (follow_aircraft && sim_initialized) {
        auto h = f4::entities::EntityHandle(sim->aircraft_entity(), &sim->world());
        auto* tf = h.get<f4::entities::TransformComponent>();
        if (tf) {
            orbit_cam.set_target(enu_to_raylib_v3(tf->position.x, tf->position.y, tf->position.z));
            orbit_cam.update_from_orbit();
        }
    }
}

void PlayerApp::Impl::fit_to_aircraft() {
    if (!sim_initialized) return;
    auto h = f4::entities::EntityHandle(sim->aircraft_entity(), &sim->world());
    auto* tf = h.get<f4::entities::TransformComponent>();
    if (!tf) return;
    const Vector3 target = enu_to_raylib_v3(tf->position.x, tf->position.y, tf->position.z);
    orbit_cam.set_target(target);
    orbit_cam.set_distance(80.0f);  // close enough to see the F-16 in detail
    orbit_cam.update_from_orbit();
}

void PlayerApp::Impl::reset_camera() {
    orbit_cam.reset();
    // Default target: the parking spot (scenario aircraft's spawn position).
    if (sim_initialized && !scenario.aircraft.empty()) {
        const auto& p = scenario.aircraft.front().parking_spot;
        orbit_cam.set_target(enu_to_raylib_v3(p.x, p.y, p.z));
    }
    orbit_cam.update_from_orbit();
}

// ── Lit shader (delegated to f4::renderer::LitShader) ────────────────────
// No more inline GLSL source or manual LoadShaderFromMemory here.
// LitShader::ensure() compiles lazily on first call; set_lighting() sets
// uniforms per-frame.

// ── Mesh building (delegated to f4::renderer::build_raylib_meshes) ────────
// The manual per-vertex conversion loop and resolve_vertex_color() have
// been consolidated into f4-renderer's mesh_builder.cpp.

void PlayerApp::Impl::build_aircraft_meshes() {
    // Phase 2A: this is now a thin wrapper that ensures the aircraft's
    // mesh is in the cache. The actual mesh-building logic lives in
    // build_mesh_for_model(), shared between aircraft and airfield features.
    if (!sim_initialized) { meshes_built = true; return; }

    auto h = f4::entities::EntityHandle(sim->aircraft_entity(), &sim->world());
    auto* vis = h.get<f4::simulation::VisualModelComponent>();
    if (!vis || !vis->model_record) {
        status_msg = "No visual model on aircraft entity";
        meshes_built = true;
        return;
    }

    auto& db = const_cast<f4::models::ModelDatabase&>(sim->model_db());
    const auto* base = db.model(0);
    const int parent_index = base ? static_cast<int>(vis->model_record - base) : -1;
    if (parent_index < 0) {
        status_msg = "Cannot resolve model index from pointer";
        meshes_built = true;
        return;
    }

    build_mesh_for_model(parent_index);

    meshes_built = true;
    auto it = mesh_cache.find(parent_index);
    if (it != mesh_cache.end()) {
        int n_textured = 0;
        for (const auto& me : it->second.meshes) if (me.tex_id >= 0) ++n_textured;
        status_msg = "F-16 loaded: " + std::to_string(it->second.meshes.size()) +
                     " meshes, " + std::to_string(n_textured) + " textured";
    }
}

void PlayerApp::Impl::build_mesh_for_model(int parent_index) {
    // Phase 2A: build (or skip if already cached) the Raylib Mesh objects
    // for one KoreaObj model. The result is stored in mesh_cache[parent_index]
    // so multiple entities sharing the same vis_type reuse one upload.
    //
    // Requires the GL context (UploadMesh). Called lazily from
    // draw_visual_entities() the first time an entity with a previously-
    // unseen parent_index is encountered, and eagerly from
    // build_aircraft_meshes() at startup for the primary aircraft.
    if (parent_index < 0) return;
    auto it = mesh_cache.find(parent_index);
    if (it != mesh_cache.end() && it->second.built) return;  // already cached

    auto& db = const_cast<f4::models::ModelDatabase&>(sim->model_db());
    const auto* rec = db.model(parent_index);
    if (!rec || rec->lods.empty()) {
        if (it != mesh_cache.end()) it->second.built = true;
        else mesh_cache[parent_index].built = true;
        return;
    }
    const int lod = 0;  // lock to LOD 0 (highest detail) for now
    auto err = db.parse_lod(parent_index, lod);
    if (!err.empty()) {
        if (it != mesh_cache.end()) it->second.built = true;
        else mesh_cache[parent_index].built = true;
        return;
    }

    // Use a default ModelState (texture_set=0, no switches). The aircraft's
    // per-instance gear switch is baked into the extracted geometry at
    // build time, so a future phase that needs to animate the gear will
    // need to invalidate and rebuild the cache entry. For Phase 2A we
    // accept static gear (already down at spawn, stays down during taxi).
    f4::models::ModelState default_state;
    default_state.texture_set = 0;
    default_state.n_texture_sets = std::max(1, static_cast<int>(rec->n_texture_sets));

    auto geom = db.extract_model_geometry(parent_index, lod, default_state);
    if (geom.meshes.empty()) {
        mesh_cache[parent_index].built = true;
        return;
    }

    const auto& cb = db.color_bank();

    // Delegate mesh construction to f4-renderer.
    auto raylib_meshes = f4::renderer::build_raylib_meshes(geom, cb, f4::renderer::model_vertex_to_raylib);
    auto mesh_entries = f4::renderer::build_mesh_entries(geom, raylib_meshes);

    MeshCacheEntry entry;
    entry.meshes = std::move(mesh_entries);
    entry.built = true;
    mesh_cache[parent_index] = std::move(entry);

    // Upload any new textures referenced by this model's meshes.
    upload_textures();
}

void PlayerApp::Impl::upload_textures() {
    if (!sim_initialized) return;
    auto& db = const_cast<f4::models::ModelDatabase&>(sim->model_db());

    // Collect all tex_ids from every cached mesh entry.
    std::vector<int> tex_ids;
    for (const auto& [parent_idx, cache_entry] : mesh_cache) {
        for (const auto& me : cache_entry.meshes) {
            if (me.tex_id >= 0) tex_ids.push_back(me.tex_id);
        }
    }

    // Delegate to f4-renderer's TextureCache.
    texture_cache.upload(db, tex_ids);
}

void PlayerApp::Impl::unload_meshes() {
    texture_cache.unload_all();
    // Phase 2A: walk every model in the mesh cache.
    for (auto& [parent_idx, cache_entry] : mesh_cache) {
        std::vector<::Mesh> meshes;
        meshes.reserve(cache_entry.meshes.size());
        for (auto& me : cache_entry.meshes) {
            meshes.push_back(me.mesh);
        }
        f4::renderer::unload_meshes(meshes);
        cache_entry.meshes.clear();
        cache_entry.built = false;
    }
    mesh_cache.clear();
    meshes_built = false;
}

// ── draw_scene ─────────────────────────────────────────────────────────────
// Grid and axes drawing delegated to f4::renderer::draw_grid() and
// f4::renderer::draw_axes().

// Helper: convert an ENU WorldPosition to a Raylib Vector3.
static inline Vector3 to_rh(const f4::geo::WorldPosition& p) {
    return enu_to_raylib_v3(p.x, p.y, p.z);
}

// Helper: convert a Raylib Color from float RGBA (0..1) to ::Color.
// Reads r, g, blue, a_ (the struct uses `blue` instead of `b` to avoid
// collision with GeoLine::b which is the segment endpoint).
static inline Color to_raylib_color(const float c[4]) {
    return Color{
        static_cast<unsigned char>(c[0] * 255.0f),  // r
        static_cast<unsigned char>(c[1] * 255.0f),  // g
        static_cast<unsigned char>(c[2] * 255.0f),  // blue
        static_cast<unsigned char>(c[3] * 255.0f)   // a_
    };
}

// Draw a flat quad (assumed coplanar) on the ground. Raylib has no
// DrawQuad3D, so we draw it as two triangles via rlgl primitives.
static void draw_quad_3d(const GeoQuad& q) {
    const Color c = to_raylib_color(&q.r);
    // Two triangles: (p0,p1,p2) and (p0,p2,p3)
    DrawTriangle3D(to_rh(q.p[0]), to_rh(q.p[1]), to_rh(q.p[2]), c);
    DrawTriangle3D(to_rh(q.p[0]), to_rh(q.p[2]), to_rh(q.p[3]), c);
}

// Draw a small cube at a marker position.
static void draw_marker(const GeoMarker& m) {
    const Vector3 c = to_rh(m.center);
    const float s = m.size_ft;
    const Color col = to_raylib_color(&m.r);
    DrawCube(c, s, s, s, col);
    DrawCubeWires(c, s, s, s, BLACK);
}

void PlayerApp::Impl::draw_airport() {
    if (!airport_built || !show_airport) return;

    if (airport.has_real_layout) {
        // Real campaign layout: draw the shared f4-renderer geometry,
        // translated from objective-local to world ENU. (Culling was
        // disabled by draw_visual_entities for the BSP models; enable it
        // around these authored CCW quads is unnecessary — winding-agnostic
        // under the disabled state.)
        const float ox = static_cast<float>(airport.layout_origin_x);
        const float oy = static_cast<float>(airport.layout_origin_y);
        const float oz = static_cast<float>(airport.layout_origin_z);
        const auto& rl = airport.real_layout;
        for (const auto& q : rl.runway_surfaces)
            f4::renderer::draw_layout_quad(q, ox, oy, oz);
        for (const auto& q : rl.threshold_bars)
            f4::renderer::draw_layout_quad(q, ox, oy, oz);
        for (const auto& q : rl.centerline_dashes)
            f4::renderer::draw_layout_quad(q, ox, oy, oz);
        for (const auto& q : rl.taxiway_strips)
            f4::renderer::draw_layout_quad(q, ox, oy, oz);
        for (const auto& l : rl.taxiway_centerlines)
            f4::renderer::draw_layout_line(l, ox, oy, oz);
        for (const auto& m : rl.runway_ends)
            f4::renderer::draw_layout_marker(m, ox, oy, oz);
        for (const auto& m : rl.parking_spots)
            f4::renderer::draw_layout_marker(m, ox, oy, oz);
    } else {
        // Synthetic airfield (hand-authored scenario).
        // Runway surface
        draw_quad_3d(airport.runway_surface);

        // Threshold bars
        for (const auto& b : airport.threshold_bars) {
            draw_quad_3d(b);
        }

        // Centerline dashes
        for (const auto& d : airport.centerline_dashes) {
            draw_quad_3d(d);
        }
    }

    // Taxi route lines
    if (show_taxi_route) {
        for (const auto& l : airport.taxi_route_lines) {
            const Color c = to_raylib_color(&l.r);
            DrawLine3D(to_rh(l.a), to_rh(l.b), c);
        }
    }

    // Flight-plan route: cyan lines at waypoint altitudes + drop lines +
    // waypoint markers — the reference the aircraft's position/orientation
    // is judged against in the air phase.
    if (show_flightplan) {
        for (const auto& l : airport.flightplan_drop_lines) {
            const Color c = to_raylib_color(&l.r);
            DrawLine3D(to_rh(l.a), to_rh(l.b), c);
        }
        for (const auto& l : airport.flightplan_lines) {
            const Color c = to_raylib_color(&l.r);
            DrawLine3D(to_rh(l.a), to_rh(l.b), c);
        }
        for (const auto& m : airport.flightplan_waypoints) {
            draw_marker(m);
        }
    }

    // Approach reference: extended centerline + 3-deg glide slope.
    if (show_approach) {
        for (const auto& l : airport.approach_lines) {
            const Color c = to_raylib_color(&l.r);
            DrawLine3D(to_rh(l.a), to_rh(l.b), c);
        }
        for (const auto& m : airport.approach_markers) {
            draw_marker(m);
        }
    }

    // Taxi-in route lines (runway exit -> parking).
    if (show_taxi_in) {
        for (const auto& l : airport.taxi_in_route_lines) {
            const Color c = to_raylib_color(&l.r);
            DrawLine3D(to_rh(l.a), to_rh(l.b), c);
        }
    }

    // Markers
    draw_marker(airport.parking_spot);
    draw_marker(airport.hold_short);
    draw_marker(airport.runway_end);

    // Compass rose
    if (show_compass) {
        for (const auto& l : airport.compass_rose) {
            const Color c = to_raylib_color(&l.r);
            DrawLine3D(to_rh(l.a), to_rh(l.b), c);
        }
    }
}

void PlayerApp::Impl::draw_aircraft() {
    // Phase 2A: legacy entry point — draws only the primary aircraft.
    // Kept for compatibility; the real work now happens in draw_visual_entities().
    if (!show_aircraft || !sim_initialized) return;
    draw_visual_entities();
}

void PlayerApp::Impl::draw_visual_entities() {
    // Phase 2A: walk every entity that has a VisualModelComponent and draw
    // it. This unifies the aircraft and airfield-feature draw paths — both
    // are just entities with a TransformComponent + VisualModelComponent,
    // and the renderer doesn't care whether they also have a FlightModelComponent.
    //
    // The mesh cache is keyed by parent_index (the KoreaObj model number),
    // so multiple entities sharing the same vis_type reuse one GPU upload.
    // The cache is built lazily here — the first time we see a new
    // parent_index, build_mesh_for_model() uploads it.
    if (!sim_initialized) return;
    if (!show_aircraft && !show_airport) return;  // both toggles off → skip

    // Collect all VMC-bearing entities. with_component<>() returns a fresh
    // vector each call (O(N) walk over the world), so we do it once per
    // frame, not once per mesh.
    const auto entities = sim->world().with_component<f4::simulation::VisualModelComponent>();
    if (entities.empty()) return;

    auto& db = const_cast<f4::models::ModelDatabase&>(sim->model_db());
    const auto* base = db.model(0);

    // Set up lighting once per frame (shared across all entities).
    bool lighting_active = false;
    if (lit_shader.ensure(&status_msg)) {
        lighting_active = true;
        lit_shader.set_lighting(light_direction, light_color, light_intensity, ambient_color);
    }

    // CRITICAL: Disable backface culling (same as f4-models-viewer).
    // FreeFalcon's models were authored without consistent winding.
    rlDisableBackfaceCulling();
    BeginBlendMode(BLEND_ALPHA);

    Material default_mat = LoadMaterialDefault();
    default_mat.maps[MATERIAL_MAP_DIFFUSE].color = WHITE;
    if (lighting_active) default_mat.shader = lit_shader.shader();

    // Determine which entity is the "primary aircraft" so we can apply the
    // show_aircraft toggle only to it (features are gated by show_airport).
    const auto primary_aircraft_id = sim->aircraft_entity();

    for (const auto eid : entities) {
        auto h = f4::entities::EntityHandle(eid, &sim->world());
        auto* vis = h.get<f4::simulation::VisualModelComponent>();
        auto* tf  = h.get<f4::entities::TransformComponent>();
        if (!vis || !vis->model_record || !tf) continue;

        // Toggle gating: aircraft ↔ show_aircraft, features ↔ show_airport.
        // The primary aircraft's entity ID matches aircraft_entity(); all
        // other VMC-bearing entities are features.
        const bool is_aircraft = (eid.value == primary_aircraft_id.value);
        if (is_aircraft && !show_aircraft) continue;
        if (!is_aircraft && !show_airport) continue;

        // Resolve parent_index from the model_record pointer.
        const int parent_index = base ? static_cast<int>(vis->model_record - base) : -1;
        if (parent_index < 0) continue;

        // Lazy mesh build: if this model isn't cached yet, build it now.
        // This handles features that weren't pre-built at startup.
        build_mesh_for_model(parent_index);

        auto cache_it = mesh_cache.find(parent_index);
        if (cache_it == mesh_cache.end() || cache_it->second.meshes.empty()) continue;

        // Convert ENU position to Raylib RH Y-up.
        const Vector3 pos_rh = enu_to_raylib_v3(tf->position.x, tf->position.y, tf->position.z);

        // Convert ENU quaternion to Raylib RH Y-up quaternion.
        // Uses the shared enu_quat_to_raylib() from f4-renderer/coord_transform.hpp.
        const auto q_rh_components = f4::renderer::enu_quat_to_raylib(
            tf->qw, tf->qx, tf->qy, tf->qz);
        Quaternion q_rh = {
            q_rh_components.x,
            q_rh_components.y,
            q_rh_components.z,
            q_rh_components.w
        };

        const Matrix model_matrix = MatrixMultiply(
            QuaternionToMatrix(q_rh),
            MatrixTranslate(pos_rh.x, pos_rh.y, pos_rh.z)
        );

        for (const auto& me : cache_it->second.meshes) {
            if (me.mesh.triangleCount <= 0) continue;
            const Material* mat_to_use = &default_mat;
            if (me.tex_id >= 0) {
                auto* tex_entry = texture_cache.lookup(me.tex_id);
                if (tex_entry && tex_entry->uploaded) {
                    mat_to_use = &tex_entry->material;
                    if (lighting_active) {
                        const_cast<Material*>(mat_to_use)->shader = lit_shader.shader();
                    }
                }
            }
            DrawMesh(me.mesh, *mat_to_use, model_matrix);
        }
    }

    EndBlendMode();
    rlEnableBackfaceCulling();
}

void PlayerApp::Impl::draw_hud() {
    if (!show_hud) return;

    const int x = 12;
    int y = 12;
    const int line_h = 18;
    const int pad = 8;

    std::vector<std::string> lines;
    char buf[256];

    std::snprintf(buf, sizeof(buf), "Scenario: %s", scenario.name.c_str());
    lines.emplace_back(buf);

    if (sim_initialized) {
        std::snprintf(buf, sizeof(buf), "Tick: %llu   Sim time: %.1fs",
                      static_cast<unsigned long long>(sim->tick_count()),
                      sim->sim_time_s());
        lines.emplace_back(buf);
        std::snprintf(buf, sizeof(buf), "State: %s   FPS: %d",
                      paused ? "PAUSED" : "RUNNING", GetFPS());
        lines.emplace_back(buf);

        // Aircraft state
        auto h = f4::entities::EntityHandle(sim->aircraft_entity(), &sim->world());
        auto* fm = h.get<f4::flight::FlightModelComponent>();
        if (fm) {
            const auto& s = fm->state();
            std::snprintf(buf, sizeof(buf), "KCAS: %.1f   AGL: %.0fft",
                          s.vcas, -s.kin.z - s.gear.groundZ_ft);
            lines.emplace_back(buf);
            std::snprintf(buf, sizeof(buf), "Hdg: %.0f\u00B0   Pitch: %.1f\u00B0   Roll: %.1f\u00B0",
                          f4::flight::to_degrees(s.kin.psi),
                          f4::flight::to_degrees(s.kin.theta),
                          f4::flight::to_degrees(s.kin.phi));
            lines.emplace_back(buf);
            std::snprintf(buf, sizeof(buf), "Gear: %s   On ground: %s",
                          s.aero.gearPos > 0.5 ? "DOWN" : "UP",
                          !s.gear.inAir ? "yes" : "no");
            lines.emplace_back(buf);
        }

        // AI mission state — which module is flying and where it is in
        // its state machine (the "what is it doing" line).
        if (auto* brain = h.get<f4::ai::BrainComponent>(); brain) {
            std::snprintf(buf, sizeof(buf), "AI: %s | %s | phase %s",
                          brain->mode_name().c_str(),
                          brain->state_name().c_str(),
                          brain->phase_name());
            lines.emplace_back(buf);
        }

        if (!scenario.aircraft.empty()) {
            std::snprintf(buf, sizeof(buf), "Callsign: %s   (%s)",
                          scenario.aircraft.front().callsign.c_str(),
                          scenario.aircraft.front().aircraft_name.c_str());
            lines.emplace_back(buf);
        }
    }

    if (!status_msg.empty()) {
        lines.emplace_back("");
        lines.emplace_back("Status: " + status_msg);
    }

    // Controls hint
    lines.emplace_back("");
    lines.emplace_back("Space: pause/resume   F: focus aircraft   R: reset view");

    int max_w = 0;
    for (const auto& line : lines) {
        const int w = MeasureText(line.c_str(), 14);
        if (w > max_w) max_w = w;
    }
    const int bg_h = static_cast<int>(lines.size()) * line_h + pad * 2;
    const int bg_w = max_w + pad * 2;

    DrawRectangle(x, y, bg_w, bg_h, { 0, 0, 0, 180 });
    DrawRectangleLines(x, y, bg_w, bg_h, { 255, 255, 255, 80 });

    int line_y = y + pad;
    for (const auto& line : lines) {
        DrawText(line.c_str(), x + pad, line_y, 14, RAYWHITE);
        line_y += line_h;
    }
}

void PlayerApp::Impl::draw_scene() {
    // Background sky + ground
    ClearBackground(SKY_COLOR);

    BeginMode3D(orbit_cam.camera());
    // Theater-scale scene (feet): the default 1000-unit far plane clips
    // the airfield layout at any wide camera distance.
    f4::renderer::extend_far_plane(orbit_cam.camera(), 1.0f, 250000.0f);

    // Scene anchor: a grid-referenced airbase lives at its objective
    // center (grid×1024 ft, hundreds of thousands of ft from origin) —
    // the ground plane and grid must follow it, not the world origin.
    float cx = 0.0f, cy = 0.0f, cz = 0.0f;  // ENU feet
    if (scenario.has_airbase_source) {
        cx = static_cast<float>(scenario.layout_center.x);
        cy = static_cast<float>(scenario.layout_center.y);
        cz = static_cast<float>(scenario.layout_center.z);
    } else if (!scenario.aircraft.empty()) {
        const auto& p = scenario.aircraft.front().parking_spot;
        cx = static_cast<float>(p.x);
        cy = static_cast<float>(p.y);
        cz = static_cast<float>(p.z);
    }
    const Vector3 anchor = enu_to_raylib_v3(cx, cy, cz);

    // Ground plane + grid sit 2/1 ft below the layout plane so they never
    // z-fight with the runway/taxiway surfaces drawn at the same elevation.
    DrawPlane({anchor.x, anchor.y - 2.0f, anchor.z}, {20000, 20000}, GROUND_COLOR);

    if (show_grid) {
        f4::renderer::draw_grid_at(10000.0f, 1000.0f, cx, cy, cz - 1.0f);
    }

    if (show_axes) {
        // RGB axes at the parking spot (or origin)
        Vector3 origin = {0, 0, 0};
        if (sim_initialized && !scenario.aircraft.empty()) {
            const auto& p = scenario.aircraft.front().parking_spot;
            origin = enu_to_raylib_v3(p.x, p.y, p.z);
        }
        // Draw translated axes: move to origin, draw, then move back.
        // f4::renderer::draw_axes() draws at the world origin, so we
        // use DrawLine3D with offset for now (preserving original behavior).
        DrawLine3D(origin, {origin.x + 50, origin.y, origin.z}, RED);
        DrawLine3D(origin, {origin.x, origin.y + 50, origin.z}, GREEN);
        DrawLine3D(origin, {origin.x, origin.y, origin.z + 50}, BLUE);
    }

    draw_airport();
    draw_visual_entities();

    EndMode3D();

    draw_hud();
    draw_radio();
}

// ============================================================================
// ATC radio transcript — top-right panel
// ============================================================================

void PlayerApp::Impl::draw_radio() {
    if (!show_radio || !sim_initialized) return;

    // Show the most recent transmissions (newest at the bottom).
    constexpr std::size_t MAX_SHOWN = 9;
    const std::size_t n = radio_log.size();
    const std::size_t first = n > MAX_SHOWN ? n - MAX_SHOWN : 0;

    // Measure the widest line so the panel fits its content.
    int max_w = 0;
    for (std::size_t i = first; i < n; ++i) {
        const auto* e = radio_log.at(i);
        if (!e) continue;
        char line[320];
        std::snprintf(line, sizeof(line), "T+%06.1f  %s: %s", e->time_s,
                      e->from_atc ? "TWR" : "EAGLE1", e->text.c_str());
        const int w = MeasureText(line, 13);
        if (w > max_w) max_w = w;
    }
    if (max_w == 0) return;

    const int pad = 8;
    const int line_h = 17;
    const int shown = static_cast<int>(n - first);
    const int bg_w = max_w + pad * 2;
    const int bg_h = shown * line_h + pad * 2 + 18;  // + header line
    const int x = window_w - bg_w - 12;
    const int y = 12;

    DrawRectangle(x, y, bg_w, bg_h, {0, 0, 0, 170});
    DrawRectangleLines(x, y, bg_w, bg_h, {255, 255, 255, 70});
    DrawText("ATC", x + pad, y + pad, 13, {200, 200, 210, 255});

    int line_y = y + pad + 18;
    for (std::size_t i = first; i < n; ++i) {
        const auto* e = radio_log.at(i);
        if (!e) continue;
        char line[320];
        std::snprintf(line, sizeof(line), "T+%06.1f  %s: %s", e->time_s,
                      e->from_atc ? "TWR" : "EAGLE1", e->text.c_str());
        DrawText(line, x + pad, line_y, 13,
                 e->from_atc ? Color{140, 230, 140, 255}   // tower = green
                             : Color{235, 235, 235, 255}); // pilot = white
        line_y += line_h;
    }
}

} // namespace f4::scenario_player
