// f4-scenario-player/src/renderer.cpp
//
// Camera + scene drawing for the scenario player.
//
// The rendering itself is delegated to f4::renderer::render_world() —
// the single world-rendering entry point shared with the world-viewer.
// This file only:
//   1. Owns the orbit camera + keyboard bindings
//   2. Builds the SceneDescription (ground anchor, airfield, entities)
//   3. Extracts VisualModelComponent entities into plain EntityMeshDraw
//      records (f4-renderer must not depend on f4-simulation)
//   4. Draws scenario-specific overlays (taxi route, flight plan,
//      approach, compass rose, HUD, ATC radio) via the overlay hook
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
#include <f4/renderer/scene_draw.hpp>
#include <f4/renderer/world_renderer.hpp>
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
static constexpr Color SKY_COLOR  = {135, 175, 220, 255};  // sky blue

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

// ── Mesh building (delegated to f4::renderer::RenderResources) ────────────
// build_mesh_for_model / upload_textures / the mesh + texture + shader
// caches all live in RenderResources now (shared implementation with the
// world-viewer).

void PlayerApp::Impl::build_aircraft_meshes() {
    // Thin wrapper: ensure the aircraft's mesh is in the cache after GL
    // context creation (draw_entity_meshes builds the rest lazily).
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

    render_res.build_mesh_for_model(db, parent_index);

    meshes_built = true;
    auto it = render_res.mesh_cache.find(parent_index);
    if (it != render_res.mesh_cache.end()) {
        int n_textured = 0;
        for (const auto& me : it->second.meshes) if (me.tex_id >= 0) ++n_textured;
        status_msg = "F-16 loaded: " + std::to_string(it->second.meshes.size()) +
                     " meshes, " + std::to_string(n_textured) + " textured";
    }
}

void PlayerApp::Impl::unload_meshes() {
    render_res.unload_all();
    meshes_built = false;
}

// ── draw_scene ─────────────────────────────────────────────────────────────
// Delegates to f4::renderer::render_world() — see viewer_state.hpp.
//
// The scenario-player's renderer now uses the shared f4::renderer pipeline
// exclusively:
//   - draw_ground(), draw_airfield_geometry(), draw_entity_meshes() all
//     live in f4-renderer and are composed by render_world()
//   - scenario-specific overlays (taxi route, flight plan, approach,
//     taxi-in, compass, markers) use the shared draw_layout_line /
//     draw_layout_marker primitives from layout_draw.hpp
//
// No per-app draw helpers remain — every primitive goes through the
// shared code path so changes to lighting, alpha blending, etc. apply
// uniformly across all viewer apps.

void PlayerApp::Impl::draw_airport() {
    // Scenario-specific overlays only. The runway / taxiway / parking
    // geometry (real OR synthetic) is rendered by render_world() via
    // SceneDescription::airfield — the shared builder runs in both
    // cases. This function draws the navigation aids + scenario markers.
    if (!airport_built || !show_airport) return;

    // Taxi route lines (yellow line strip).
    if (show_taxi_route) {
        for (const auto& l : airfield.taxi_route_lines) {
            f4::renderer::draw_layout_line(l);
        }
    }

    // Flight-plan route: cyan lines at waypoint altitudes + drop lines +
    // waypoint markers — the reference the aircraft's position/orientation
    // is judged against in the air phase.
    if (show_flightplan) {
        for (const auto& l : airfield.flightplan_drop_lines) {
            f4::renderer::draw_layout_line(l);
        }
        for (const auto& l : airfield.flightplan_lines) {
            f4::renderer::draw_layout_line(l);
        }
        for (const auto& m : airfield.flightplan_waypoints) {
            f4::renderer::draw_layout_marker(m);
        }
    }

    // Approach reference: extended centerline + 3-deg glide slope.
    if (show_approach) {
        for (const auto& l : airfield.approach_lines) {
            f4::renderer::draw_layout_line(l);
        }
        for (const auto& m : airfield.approach_markers) {
            f4::renderer::draw_layout_marker(m);
        }
    }

    // Taxi-in route lines (runway exit -> parking).
    if (show_taxi_in) {
        for (const auto& l : airfield.taxi_in_route_lines) {
            f4::renderer::draw_layout_line(l);
        }
    }

    // Scenario markers (parking-spot / hold-short / runway-end). The
    // shared builder also emits a parking marker (green cube) and a
    // runway-end marker (red cube) from the synthesized GroundLayoutLists;
    // the markers below are larger, distinct-color cubes that the user
    // actually sees as the scenario's reference points.
    f4::renderer::draw_layout_marker(airfield.parking_spot);
    f4::renderer::draw_layout_marker(airfield.hold_short);
    f4::renderer::draw_layout_marker(airfield.runway_end);

    // Compass rose.
    if (show_compass) {
        for (const auto& l : airfield.compass_rose) {
            f4::renderer::draw_layout_line(l);
        }
    }
}

void PlayerApp::Impl::draw_scene() {
    f4::renderer::SceneDescription scene;
    scene.camera = orbit_cam.camera();
    scene.sky_color = SKY_COLOR;

    // Scene anchor: a grid-referenced airbase lives at its objective
    // center (grid×1024 ft, hundreds of thousands of ft from origin) —
    // the ground plane, grid, and axes follow it, not the world origin.
    if (scenario.has_airbase_source) {
        scene.ground.origin_enu_x = static_cast<float>(scenario.layout_center.x);
        scene.ground.origin_enu_y = static_cast<float>(scenario.layout_center.y);
        scene.ground.origin_enu_z = static_cast<float>(scenario.layout_center.z);
    } else if (!scenario.aircraft.empty()) {
        const auto& p = scenario.aircraft.front().parking_spot;
        scene.ground.origin_enu_x = static_cast<float>(p.x);
        scene.ground.origin_enu_y = static_cast<float>(p.y);
        scene.ground.origin_enu_z = static_cast<float>(p.z);
    }
    scene.ground.grid = show_grid;
    scene.ground.axes = show_axes;

    // ── VisualModelComponent entities → plain EntityMeshDraw records ──
    //
    // VisualModelComponent lives in f4-simulation, which f4-renderer
    // must not depend on — so we extract position + quaternion +
    // KoreaObj model index here. Meshes build lazily inside
    // draw_entity_meshes() the first time a model appears.
    if (sim_initialized && (show_aircraft || show_airport)) {
        const auto entities =
            sim->world().with_component<f4::simulation::VisualModelComponent>();
        const auto primary_aircraft_id = sim->aircraft_entity();

        auto& db = const_cast<f4::models::ModelDatabase&>(sim->model_db());
        const auto* base = db.model(0);
        scene.model_db = &db;

        for (const auto eid : entities) {
            auto h = f4::entities::EntityHandle(eid, &sim->world());
            auto* vis = h.get<f4::simulation::VisualModelComponent>();
            auto* tf  = h.get<f4::entities::TransformComponent>();
            if (!vis || !vis->model_record || !tf) continue;

            // Toggle gating: aircraft ↔ show_aircraft, features ↔
            // show_airport (everything that isn't the primary aircraft).
            const bool is_aircraft = (eid.value == primary_aircraft_id.value);
            if (is_aircraft && !show_aircraft) continue;
            if (!is_aircraft && !show_airport) continue;

            const int parent_index = base
                ? static_cast<int>(vis->model_record - base) : -1;
            if (parent_index < 0) continue;

            f4::renderer::EntityMeshDraw emd;
            emd.enu_x = static_cast<float>(tf->position.x);
            emd.enu_y = static_cast<float>(tf->position.y);
            emd.enu_z = static_cast<float>(tf->position.z);
            emd.qw = static_cast<float>(tf->qw);
            emd.qx = static_cast<float>(tf->qx);
            emd.qy = static_cast<float>(tf->qy);
            emd.qz = static_cast<float>(tf->qz);
            emd.parent_index = parent_index;
            scene.entity_meshes.push_back(emd);
        }
    }

    // ── Airfield geometry (real OR synthetic — both built via the shared
    //    f4::renderer::build_airfield_geometry_3d()). render_world() draws
    //    it via SceneDescription::airfield, offset by airfield.origin_enu_*.
    //    This unifies the synthetic and real-layout paths through one
    //    draw_airfield_geometry() call.
    if (airport_built && show_airport && !airfield.geometry.empty) {
        scene.airfield = &airfield.geometry;
        scene.airfield_origin_enu[0] = airfield.origin_enu_x;
        scene.airfield_origin_enu[1] = airfield.origin_enu_y;
        scene.airfield_origin_enu[2] = airfield.origin_enu_z;
    }

    // ── Scenario-specific 3D overlays (inside the 3D mode) ────────────
    // Taxi route, flight-plan route, approach reference, taxi-in route,
    // markers, compass rose. All drawn via shared draw_layout_line /
    // draw_layout_marker primitives.
    scene.overlay_3d = [this](const Camera3D&) { draw_airport(); };

    f4::renderer::render_world(render_res, scene);

    draw_hud();
    draw_radio();
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
