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
#include <f4/sensors/rwr.hpp>              // RwrComponent (combat HUD line)
#include <f4/weapons/gun_component.hpp>    // GunComponent (tracer streaks)
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

// Combat-view tuning (bvr_intercept):
//   Missile contrail: one point per rendered frame, newest kept, capped.
//   900 points ~ 15 s of flight at 60 FPS — the AMRAAM flyout is ~35 s, so
//   the trail covers the terminal half where the PN pursuit bends.
//   Missile body: 60-ft cylinder along velocity + 500-ft wire sphere as
//   the tactical marker (true-scale alone is sub-pixel at 10 NM zoom).
static constexpr std::size_t kMaxTrailPoints = 900;
static constexpr float kMissileBodyFt = 60.0f;
static constexpr float kMissileRingFt = 500.0f;

// ── Watched aircraft (Tab cycles; bvr_intercept has two fighters) ──────────

f4::entities::EntityId PlayerApp::Impl::watched_entity() const noexcept {
    if (!sim_initialized) return f4::entities::EntityId{};
    const auto& ids = sim->aircraft_entities();
    if (ids.empty()) return f4::entities::EntityId{};
    const std::size_t i = watched_index < ids.size() ? watched_index : ids.size() - 1;
    return ids[i];
}

void PlayerApp::Impl::cycle_watched() {
    if (!sim_initialized) return;
    const auto n = sim->aircraft_entities().size();
    if (n < 2) return;  // one (or zero) aircraft — nothing to cycle
    watched_index = (watched_index + 1) % n;
    status_msg = "Watching: " + scenario.aircraft[std::min(watched_index,
        scenario.aircraft.size() - 1)].callsign;
}

// ── Camera (delegated to f4::renderer::OrbitCamera) ────────────────────────

void PlayerApp::Impl::handle_camera_input() {
    const ImGuiIO& io = ImGui::GetIO();
    if (!io.WantCaptureKeyboard) {
        if (IsKeyPressed(KEY_F)) fit_to_aircraft();
        if (IsKeyPressed(KEY_C)) { follow_aircraft = !follow_aircraft; status_msg = follow_aircraft ? "Camera: following aircraft" : "Camera: free"; }
        if (IsKeyPressed(KEY_R)) reset_camera();
        if (IsKeyPressed(KEY_TAB)) cycle_watched();
        if (IsKeyPressed(KEY_SPACE)) {
            paused = !paused;
            status_msg = paused ? "Paused" : "Running";
        }
    }
    // Delegate orbit/pan/zoom to OrbitCamera (guards ImGui::WantCaptureMouse internally).
    orbit_cam.handle_input();

    // Follow mode: track the WATCHED aircraft every frame (position only —
    // the user keeps orbit control of yaw/pitch/distance around it).
    if (follow_aircraft && sim_initialized) {
        auto h = f4::entities::EntityHandle(watched_entity(), &sim->world());
        auto* tf = h.get<f4::entities::TransformComponent>();
        if (tf) {
            orbit_cam.set_target(enu_to_raylib_v3(tf->position.x, tf->position.y, tf->position.z));
            orbit_cam.update_from_orbit();
        }
    }
}

void PlayerApp::Impl::fit_to_aircraft() {
    if (!sim_initialized) return;
    auto h = f4::entities::EntityHandle(watched_entity(), &sim->world());
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
    // Tranche 0d: vis_type IS the model index (the renderer's cache key).
    // The scenario-player owns its own ModelDatabase (transitional — the
    // final glTF path will use a RuntimeModelCache instead).
    if (!sim_initialized) { meshes_built = true; return; }

    auto h = f4::entities::EntityHandle(sim->aircraft_entity(), &sim->world());
    auto* vis = h.get<f4::simulation::VisualModelComponent>();
    if (!vis || vis->vis_type <= 0) {
        status_msg = "No visual model on aircraft entity";
        meshes_built = true;
        return;
    }

    const int parent_index = vis->vis_type;
    if (model_db.valid()) {
        render_res.build_mesh_for_model(model_db, parent_index);
    }

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

    // When real terrain is loaded, SUPPRESS the flat green ground plane
    // + grid to prevent z-fighting with the terrain mesh. The terrain
    // mesh replaces both — it has its own elevation + the tile-type
    // colors provide visual reference. The axes are kept (they're
    // useful for orientation and don't z-fight with terrain).
    const bool have_chunk_terrain =
        theater_tiles_loaded && world.chunk_set() != nullptr && show_terrain;
    if (have_chunk_terrain) {
        scene.ground.plane = false;
        scene.ground.grid  = false;
    } else if (terrain_loaded && terrain_mesh_built && terrain_mesh.valid) {
        scene.ground.plane = false;
        scene.ground.grid  = false;
    } else {
        scene.ground.grid = show_grid;
    }
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
        // Toggle gating: SCENARIO AIRCRAFT ↔ show_aircraft (any of them —
        // bvr_intercept flies two fighters), static features ↔ show_airport.
        // (Previously "aircraft" meant only the FIRST spawned entity, so
        // toggling "Show airport" hid the bandit — the wrong toggle.)
        const auto& aircraft_ids = sim->aircraft_entities();
        const auto is_scenario_aircraft =
            [&aircraft_ids](f4::entities::EntityId id) {
                return std::find(aircraft_ids.begin(), aircraft_ids.end(), id)
                    != aircraft_ids.end();
            };

        // Tranche 0d: the scenario-player owns its own ModelDatabase
        // (transitional). vis_type IS the parent_index (the cache key).
        scene.model_db = &model_db;

        for (const auto eid : entities) {
            auto h = f4::entities::EntityHandle(eid, &sim->world());
            auto* vis = h.get<f4::simulation::VisualModelComponent>();
            auto* tf  = h.get<f4::entities::TransformComponent>();
            if (!vis || vis->vis_type <= 0 || !tf) continue;

            const bool is_aircraft = is_scenario_aircraft(eid);
            if (is_aircraft && !show_aircraft) continue;
            if (!is_aircraft && !show_airport) continue;

            const int parent_index = vis->vis_type;
            if (parent_index <= 0) continue;

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
        // When the textured theater is loaded, sit the airfield on the
        // SAME surface the terrain renders (the near post level) — the
        // scenario JSON's z can be stale/coarse vs the L2 posts, which
        // buried the base.
        scene.airfield_origin_enu[2] =
            (theater_tiles_loaded && world.theater_loaded())
                ? static_cast<float>(world.near_level().elevation_at_ft(
                      airfield.origin_enu_x, airfield.origin_enu_y))
                : airfield.origin_enu_z;
    }

    // ── Terrain (textured path + untextured fallback) ────────────────
    // The shared WorldView owns the textured chunk set; a single
    // update_frame() sets its lighting + fog uniforms (fog toward the
    // sky color so the far ring melts into the horizon). The single MEA
    // mesh is the fallback for JSON-only terrain.
    if (have_chunk_terrain) {
        world.update_frame(SKY_COLOR);
        scene.terrain_chunk_set = world.chunk_set();
    } else if (terrain_loaded && terrain_mesh_built && terrain_mesh.valid) {
        scene.terrain_mesh = &terrain_mesh;
    }

    // ── Combat view: sample the live missiles' positions (contrails) —
    // BEFORE render so the trails, bodies, and shot lines drawn inside
    // the overlay below use this frame's positions.
    update_missile_trails();

    // ── Scenario-specific 3D overlays (inside the 3D mode) ────────────
    // Taxi route + flight plan + approach + taxi-in + markers + compass
    // + the combat view (missile bodies, contrails, guidance lines).
    // All drawn via shared draw_layout_line / draw_layout_marker primitives
    // (the combat view uses Raylib DrawLine3D/DrawCylinderEx directly).
    scene.overlay_3d = [this](const Camera3D&) { draw_airport(); draw_missiles(); draw_gun_tracers(); };

    f4::renderer::render_world(render_res, scene);

    draw_hud();
    draw_radio();
    draw_combat();
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

        // Aircraft state — the WATCHED aircraft (Tab cycles).
        auto h = f4::entities::EntityHandle(watched_entity(), &sim->world());
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

        // Terrain status (Path B1) — shows whether real terrain is loaded
        // and driving the ground clamp, or if the sim is on flat ground.
        if (terrain_loaded) {
            lines.emplace_back("Terrain: LOADED (real elevation)");
        } else {
            lines.emplace_back("Terrain: none (flat ground)");
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

        // Combat picture of the watched jet (bvr_intercept): RWR state
        // + the live-missile count. "MISSILE LAUNCH" is the RWR's active
        // launch flag — the same trigger the MissileModule defends on.
        if (scenario.combat.enabled) {
            const auto* rwr = h.get<f4::sensors::RwrComponent>();
            const char* rwr_state = "clear";
            if (rwr && rwr->launch_active)      rwr_state = "MISSILE LAUNCH!";
            else if (rwr && rwr->lock_active)   rwr_state = "SPIKE (locked)";
            std::snprintf(buf, sizeof(buf), "RWR: %s   Live missiles: %zu",
                          rwr_state,
                          f4::weapons::count_live_missiles(sim->world()));
            lines.emplace_back(buf);
        }

        if (!scenario.aircraft.empty()) {
            const std::size_t wi =
                watched_index < scenario.aircraft.size() ? watched_index : 0;
            std::snprintf(buf, sizeof(buf), "Callsign: %s   (%s)%s",
                          scenario.aircraft[wi].callsign.c_str(),
                          scenario.aircraft[wi].aircraft_name.c_str(),
                          scenario.aircraft.size() > 1 ? "   [Tab: cycle]" : "");
            lines.emplace_back(buf);
        }
    }

    if (!status_msg.empty()) {
        lines.emplace_back("");
        lines.emplace_back("Status: " + status_msg);
    }

    // Controls hint
    lines.emplace_back("");
    lines.emplace_back("Space: pause/resume   F: focus aircraft   R: reset view   Tab: watched   F3: FCS HUD");

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

    // ── FCS-internals HUD column (Phase 0a observability) ────────────
    // Toggle with F3. Shows the AI's stick/throttle/pedal commands plus
    // the FCS intermediates (aoacmd, pscmd, pstab, pitchIntegral, nzcgs)
    // and the EOM body rates (p, q, r). Critical for diagnosing roll
    // flutter and altitude phugoid live — without this you can't tell
    // whether the AI is commanding the wrong stick or the FCS is mis-
    // shaping a correct input.
    if (show_fcs_hud && sim_initialized) {
        draw_fcs_hud();
    }
}

void PlayerApp::Impl::draw_fcs_hud() {
    auto h = f4::entities::EntityHandle(watched_entity(), &sim->world());
    auto* fm = h.get<f4::flight::FlightModelComponent>();
    if (!fm) return;

    const auto& s = fm->state();
    const auto& fcs = s.fcs;
    const auto& kin = s.kin;
    const auto& aero = s.aero;

    // Place the FCS HUD on the right side of the main HUD, below the
    // radio panel.
    const int x = 12;
    int y = 380;
    const int line_h = 16;
    const int pad = 8;
    const int font_size = 13;

    std::vector<std::string> lines;
    char buf[256];

    lines.emplace_back("─── FCS State (F3 to hide) ───");

    // AI control surface commands (what the brain asked for)
    std::snprintf(buf, sizeof(buf), "pstick: %+0.3f   rstick: %+0.3f",
                  fcs.pshape > 0 ? std::sqrt(fcs.pshape) : -std::sqrt(-fcs.pshape),
                  fcs.rshape > 0 ? std::sqrt(fcs.rshape) : -std::sqrt(-fcs.rshape));
    lines.emplace_back(buf);

    std::snprintf(buf, sizeof(buf), "throttle: %0.3f   speedBrake: %+0.2f",
                  s.engine.rpm > 0.01 ? s.engine.rpm : 0.0,
                  aero.dbrake);
    lines.emplace_back(buf);

    // FCS intermediates
    std::snprintf(buf, sizeof(buf), "aoacmd: %5.2f\u00B0   ptcmd: %+5.2f G",
                  f4::flight::to_degrees(fcs.aoacmd),
                  fcs.ptcmd);
    lines.emplace_back(buf);

    std::snprintf(buf, sizeof(buf), "pscmd: %+6.1f\u00B0/s   pstab: %+6.1f\u00B0/s",
                  fcs.pscmd * 180.0 / 3.14159265358979,
                  fcs.pstab * 180.0 / 3.14159265358979);
    lines.emplace_back(buf);

    std::snprintf(buf, sizeof(buf), "pitchIntegral: %+6.2f",
                  fcs.pitchIntegral.output());
    lines.emplace_back(buf);

    // Aero + loads
    std::snprintf(buf, sizeof(buf), "alpha: %5.2f\u00B0   beta: %5.2f\u00B0   nzcgs: %+5.2f G",
                  f4::flight::to_degrees(aero.alpha),
                  f4::flight::to_degrees(aero.beta),
                  s.loads.nzcgs);
    lines.emplace_back(buf);

    // Body rates (rad/s → deg/s for readability)
    std::snprintf(buf, sizeof(buf), "p: %+6.1f  q: %+6.1f  r: %+6.1f  (\u00B0/s)",
                  kin.p * 180.0 / 3.14159265358979,
                  kin.q * 180.0 / 3.14159265358979,
                  kin.r * 180.0 / 3.14159265358979);
    lines.emplace_back(buf);

    std::snprintf(buf, sizeof(buf), "vs: %+6.0f fpm   qbar: %5.0f",
                  -kin.zdot * 60.0, s.qbar);
    lines.emplace_back(buf);

    int max_w = 0;
    for (const auto& line : lines) {
        const int w = MeasureText(line.c_str(), font_size);
        if (w > max_w) max_w = w;
    }
    const int bg_h = static_cast<int>(lines.size()) * line_h + pad * 2;
    const int bg_w = max_w + pad * 2;

    DrawRectangle(x, y, bg_w, bg_h, { 0, 0, 0, 180 });
    DrawRectangleLines(x, y, bg_w, bg_h, { 100, 200, 255, 120 });

    int line_y = y + pad;
    for (std::size_t i = 0; i < lines.size(); ++i) {
        const Color c = (i == 0) ? Color{120, 200, 255, 255} : RAYWHITE;
        DrawText(lines[i].c_str(), x + pad, line_y, font_size, c);
        line_y += line_h;
    }
}

// ============================================================================
// ATC radio transcript — top-right panel
// ============================================================================

void PlayerApp::Impl::draw_radio() {
    if (!show_radio || !sim_initialized) { last_radio_h = 0; return; }

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
    if (max_w == 0) { last_radio_h = 0; return; }

    const int pad = 8;
    const int line_h = 17;
    const int shown = static_cast<int>(n - first);
    const int bg_w = max_w + pad * 2;
    const int bg_h = shown * line_h + pad * 2 + 18;  // + header line
    const int x = window_w - bg_w - 12;
    const int y = 12;
    last_radio_h = bg_h;   // anchors the COMBAT panel below this one

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

// ============================================================================
// Combat view (bvr_intercept) — contrails, missile bodies, guidance lines
// ============================================================================
//
// Missiles are ECS entities like the jets, but they carry no
// VisualModelComponent (no KoreaObj record is bound at launch) — so they
// are NOT drawn by render_world()'s mesh path. They get a procedural
// draw here instead: a small bright cylinder body along the velocity
// vector + a wire-sphere tactical marker + the contrail sampled by
// update_missile_trails() + a thin red line to the assigned target that
// makes the proportional-navigation pursuit visible.

void PlayerApp::Impl::update_missile_trails() {
    if (!sim_initialized) { missile_trails.clear(); return; }

    // Sample the live missiles' positions (one point per rendered frame —
    // cosmetic, so speed > 1x stretches the sampled spacing, which reads
    // naturally as "faster"). Skipped while paused (no movement —
    // appending would pile duplicate points).
    std::vector<std::uint64_t> live;
    if (scenario.combat.enabled && show_combat && !paused) {
        for (const auto eid :
             sim->world().with_component<f4::weapons::MissileComponent>()) {
            auto h = f4::entities::EntityHandle(eid, &sim->world());
            auto* tf = h.get<f4::entities::TransformComponent>();
            if (!tf) continue;
            live.push_back(eid.value);
            auto& trail = missile_trails[eid.value];
            trail.points.push_back(
                enu_to_raylib_v3(tf->position.x, tf->position.y, tf->position.z));
            if (trail.points.size() > kMaxTrailPoints) {
                trail.points.erase(trail.points.begin());
            }
        }
    }

    // Drop the trails of missiles that are gone (swept after
    // detonation/expiry) — also clears everything when combat is toggled
    // off, since `live` is empty then.
    for (auto it = missile_trails.begin(); it != missile_trails.end();) {
        if (std::find(live.begin(), live.end(), it->first) == live.end()) {
            it = missile_trails.erase(it);
        } else {
            ++it;
        }
    }
}

void PlayerApp::Impl::draw_missiles() {
    if (!sim_initialized || !show_combat || !scenario.combat.enabled) return;

    for (const auto eid :
         sim->world().with_component<f4::weapons::MissileComponent>()) {
        auto h = f4::entities::EntityHandle(eid, &sim->world());
        auto* tf = h.get<f4::entities::TransformComponent>();
        auto* mc = h.get<f4::weapons::MissileComponent>();
        if (!tf || !mc) continue;

        const Vector3 pos =
            enu_to_raylib_v3(tf->position.x, tf->position.y, tf->position.z);

        // Guidance line to the assigned target — the PN pursuit made
        // visible (fades as the seeker closes: shorter line, tighter bend).
        if (mc->target_id != 0) {
            auto tgt = f4::entities::EntityHandle(
                f4::entities::EntityId{mc->target_id}, &sim->world());
            if (const auto* ttf = tgt.get<f4::entities::TransformComponent>()) {
                const Vector3 tpos = enu_to_raylib_v3(
                    ttf->position.x, ttf->position.y, ttf->position.z);
                DrawLine3D(pos, tpos, Color{255, 70, 70, 70});
            }
        }

        // Contrail (fading white — newest segments bright, oldest faint).
        if (const auto it = missile_trails.find(eid.value);
            it != missile_trails.end()) {
            const auto& pts = it->second.points;
            for (std::size_t i = 1; i < pts.size(); ++i) {
                const float age =
                    static_cast<float>(pts.size() - i) /
                    static_cast<float>(pts.size());  // 0 = new, 1 = old
                const unsigned char alpha =
                    static_cast<unsigned char>(200 * (1.0f - age) + 25);
                DrawLine3D(pts[i - 1], pts[i],
                           Color{235, 235, 235, alpha});
            }
        }

        // Body: thin bright cylinder along the velocity vector (true-ish
        // scale — visible once you zoom in on the merge).
        const Vector3 vel = enu_to_raylib_v3(tf->vx, tf->vy, tf->vz);
        if (Vector3Length(vel) > 1.0f) {
            const Vector3 tail = Vector3Subtract(
                pos, Vector3Scale(Vector3Normalize(vel), kMissileBodyFt));
            DrawCylinderEx(tail, pos, 3.0f, 1.0f, 8, Color{255, 235, 120, 220});
        }

        // Tactical marker: wire sphere big enough to see at BVR zoom.
        DrawSphereWires(pos, kMissileRingFt, 10, 10, Color{255, 240, 130, 160});
    }
}

// ── Gun tracers (Steps 11-12) ──────────────────────────────────────────
//
// Every live tracer point in every GunComponent's stream draws as a short
// bright streak back along its velocity (the classic tracer look — a round
// at 3,400 ft/s is a point, the STREAK is what the eye sees). Fades with
// age so a burst reads as a string of rounds, not a solid beam.

void PlayerApp::Impl::draw_gun_tracers() {
    if (!sim_initialized || !show_combat || !scenario.combat.enabled) return;

    constexpr float kTracerStreakFt = 300.0f;   // visual streak length

    for (const auto eid :
         sim->world().with_component<f4::weapons::GunComponent>()) {
        auto h = f4::entities::EntityHandle(eid, &sim->world());
        auto* gun = h.get<f4::weapons::GunComponent>();
        if (!gun) continue;

        for (const auto& t : gun->stream.tracers()) {
            const Vector3 pos =
                enu_to_raylib_v3(t.position.x, t.position.y, t.position.z);
            const Vector3 vel =
                enu_to_raylib_v3(t.velocity.x, t.velocity.y, t.velocity.z);
            if (Vector3Length(vel) < 1.0f) continue;
            const Vector3 tail = Vector3Subtract(
                pos, Vector3Scale(Vector3Normalize(vel), kTracerStreakFt));

            // Fade by age (2 s lifetime): fresh rounds bright amber,
            // expiring ones fade out.
            const float life =
                static_cast<float>(t.age_s) / 2.0f;   // 0 = new
            const unsigned char alpha = static_cast<unsigned char>(
                230.0f * (1.0f - life) + 20.0f);
            DrawLine3D(tail, pos, Color{255, 200, 60, alpha});
        }
    }
}

// ============================================================================
// COMBAT transcript — brevity panel under the ATC radio (top-right)
// ============================================================================
//
// Draws the CombatTranscript ring (the M4 observability piece maintained
// by f4-simulation): radar contacts, spikes, FOX calls, splash. Severity
// drives the color: Info = white, Warning = amber, Kill = red.

void PlayerApp::Impl::draw_combat() {
    if (!show_combat || !sim_initialized) return;

    constexpr std::size_t MAX_SHOWN = 10;
    const std::size_t n = combat_log.size();
    if (n == 0) return;
    const std::size_t first = n > MAX_SHOWN ? n - MAX_SHOWN : 0;

    // Measure the widest line so the panel fits its content.
    int max_w = 0;
    for (std::size_t i = first; i < n; ++i) {
        const auto* e = combat_log.at(i);
        if (!e) continue;
        char line[320];
        std::snprintf(line, sizeof(line), "T+%06.1f  %s: %s", e->time_s,
                      e->speaker.c_str(), e->text.c_str());
        const int w = MeasureText(line, 13);
        if (w > max_w) max_w = w;
    }
    if (max_w == 0) return;

    const int pad = 8;
    const int line_h = 17;
    const int shown = static_cast<int>(n - first);
    const int bg_w = max_w + pad * 2;
    const int bg_h = shown * line_h + pad * 2 + 18;  // + header line
    // Anchor under the ATC panel when it's visible; top-right otherwise.
    const int x = window_w - bg_w - 12;
    const int y = 12 + last_radio_h + 8;

    DrawRectangle(x, y, bg_w, bg_h, {10, 0, 0, 175});
    DrawRectangleLines(x, y, bg_w, bg_h, {255, 90, 90, 90});
    DrawText("COMBAT", x + pad, y + pad, 13, {255, 120, 120, 255});

    int line_y = y + pad + 18;
    for (std::size_t i = first; i < n; ++i) {
        const auto* e = combat_log.at(i);
        if (!e) continue;
        char line[320];
        std::snprintf(line, sizeof(line), "T+%06.1f  %s: %s", e->time_s,
                      e->speaker.c_str(), e->text.c_str());
        Color c = {235, 235, 235, 255};                      // Info = white
        if (e->severity ==
            f4::simulation::CombatTranscript::Severity::Warning) {
            c = Color{255, 205, 90, 255};                    // Warning = amber
        } else if (e->severity ==
                   f4::simulation::CombatTranscript::Severity::Kill) {
            c = Color{255, 95, 95, 255};                     // Kill = red
        }
        DrawText(line, x + pad, line_y, 13, c);
        line_y += line_h;
    }
}

} // namespace f4::scenario_player
