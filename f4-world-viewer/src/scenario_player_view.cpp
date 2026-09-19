// f4-world-viewer/src/scenario_player_view.cpp
//
// The world viewer's "scenario" mode — the consolidated f4-scenario-player.
//
//   load_scenario()           — parse + validate a scenario JSON, build the
//                                Simulation, load terrain + textured theater,
//                                build the airfield overlays, point the
//                                shared RenderResources at Data/Models.
//   handle_scenario_input()   — orbit/pan/zoom + F/C/R/Tab/Space/F3 keys.
//   scenario_advance(dt)      — the in-frame fixed-timestep tick loop
//                                (drains wall*dt*scale in whole sim_dt ticks).
//   draw_scenario()           — build SceneDescription -> render_world() (the
//                                shared f4-renderer entry point) + HUD + ATC
//                                radio + COMBAT panel + missile contrails.
//   draw_scenario_panel()     — the ImGui "Scenario Player" control window.
//   run_harness()             — the --harness headless BVR-intercept QC path
//                                (no GL context, no render loop).
//
// The rendering is delegated to f4::renderer::render_world() -- the SAME
// entry point the world viewer's 3D Ground Layout panel + entity-model 3D
// tab use. The scenario mode BORROWS impl.render_res_3d (the glTF model
// cache, lit shader, texture cache) instead of owning its own -- the single
// biggest code/GPU savings of the consolidation.
//
// CRITICAL: f4-flight-model headers must be included BEFORE raylib.h
// (Raylib's PI macro breaks `using f4::math::PI;` in f4/flight/constants.hpp).
// viewer_state.hpp includes scenario_player_state.hpp (which orders them
// correctly) before raylib.h, so this TU is safe.

#include "viewer_state.hpp"

#include <f4/simulation/simulation.hpp>
#include <f4/simulation/visual_model_component.hpp>
#include <f4/simulation/bvr_intercept_harness.hpp>
#include <f4/simulation/combat_transcript.hpp>
#include <f4/entities/entity.hpp>
#include <f4/flight/flight_model_component.hpp>
#include <f4/flight/angle.hpp>
#include <f4/ai/brain_component.hpp>
#include <f4/sensors/rwr.hpp>
#include <f4/weapons/missile_battery.hpp>
#include <f4/weapons/gun_component.hpp>
#include <f4/assets/asset_root.hpp>
#include <f4/json/f4_json.hpp>
#include <f4/renderer/draw_3d.hpp>
#include <f4/renderer/layout_draw.hpp>
#include <f4/renderer/scene_draw.hpp>
#include <f4/renderer/world_renderer.hpp>
#include <f4/renderer/coord_transform.hpp>

#include <rlImGui.h>
#include <imgui.h>
#include <raylib.h>
#include <raymath.h>
#include <rlgl.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace f4::viewer {

namespace {

// ── Constants ──────────────────────────────────────────────────────────────
constexpr Color SKY_COLOR = {135, 175, 220, 255};  // sky blue

// Combat-view tuning (bvr_intercept).
constexpr std::size_t kMaxTrailPoints = 900;
constexpr float kMissileBodyFt = 60.0f;
constexpr float kMissileRingFt = 500.0f;

// JSON string literal emitter for run_harness's summary + diary writers.
void write_json_string_(f4::json::Writer& w, const std::string& s) {
    w.string(s);
}

// ── File-local helpers (take ViewerApp::Impl by reference) ─────────────────
//
// These are the scenario player's old PlayerApp::Impl methods, ported to
// operate on the world viewer's Impl -- reading scenario-specific state
// from impl.scenario_player and shared resources from impl directly.

using Impl = ViewerApp::Impl;

// Forward declarations (some helpers are used before their definition below).
void sp_fit_to_aircraft(Impl& impl);
void sp_reset_camera(Impl& impl);
void sp_cycle_watched(Impl& impl);
void sp_build_aircraft_meshes(Impl& impl);
void sp_draw_airport(Impl& impl);
void sp_draw_hud(Impl& impl);
void sp_draw_fcs_hud(Impl& impl);
void sp_draw_radio(Impl& impl);
void sp_draw_combat(Impl& impl);
void sp_update_missile_trails(Impl& impl);
void sp_draw_missiles(Impl& impl);
void sp_draw_gun_tracers(Impl& impl);

// ── Watched aircraft (Tab cycles; bvr_intercept has two fighters) ──────────

f4::entities::EntityId sp_watched_entity(const Impl& impl) {
    const auto& sp = impl.scenario_player;
    if (!sp.sim_initialized) return f4::entities::EntityId{};
    const auto& ids = sp.sim->aircraft_entities();
    if (ids.empty()) return f4::entities::EntityId{};
    const std::size_t i = sp.watched_index < ids.size() ? sp.watched_index
                                                         : ids.size() - 1;
    return ids[i];
}

void sp_cycle_watched(Impl& impl) {
    auto& sp = impl.scenario_player;
    if (!sp.sim_initialized) return;
    const auto n = sp.sim->aircraft_entities().size();
    if (n < 2) return;  // one (or zero) aircraft -- nothing to cycle
    sp.watched_index = (sp.watched_index + 1) % n;
    impl.status_msg = "Watching: " +
        sp.scenario.aircraft[std::min(sp.watched_index,
            sp.scenario.aircraft.size() - 1)].callsign;
}

// ── Camera (delegated to f4::renderer::OrbitCamera) ────────────────────────

void sp_handle_camera_input(Impl& impl) {
    auto& sp = impl.scenario_player;
    const ImGuiIO& io = ImGui::GetIO();
    if (!io.WantCaptureKeyboard) {
        if (IsKeyPressed(KEY_F)) sp_fit_to_aircraft(impl);
        if (IsKeyPressed(KEY_C)) {
            sp.follow_aircraft = !sp.follow_aircraft;
            impl.status_msg = sp.follow_aircraft ? "Camera: following aircraft"
                                                : "Camera: free";
        }
        if (IsKeyPressed(KEY_R)) sp_reset_camera(impl);
        if (IsKeyPressed(KEY_TAB)) sp_cycle_watched(impl);
        if (IsKeyPressed(KEY_SPACE)) {
            sp.paused = !sp.paused;
            impl.status_msg = sp.paused ? "Paused" : "Running";
        }
        if (IsKeyPressed(KEY_F3)) sp.show_fcs_hud = !sp.show_fcs_hud;
    }
    // Delegate orbit/pan/zoom to OrbitCamera (guards ImGui::WantCaptureMouse).
    sp.orbit_cam.handle_input();

    // Follow mode: track the WATCHED aircraft every frame (position only --
    // the user keeps orbit control of yaw/pitch/distance around it).
    if (sp.follow_aircraft && sp.sim_initialized) {
        auto h = f4::entities::EntityHandle(sp_watched_entity(impl),
                                            &sp.sim->world());
        auto* tf = h.get<f4::entities::TransformComponent>();
        if (tf) {
            sp.orbit_cam.set_target(scenario_enu_to_raylib_v3(
                tf->position.x, tf->position.y, tf->position.z));
            sp.orbit_cam.update_from_orbit();
        }
    }
}

void sp_fit_to_aircraft(Impl& impl) {
    auto& sp = impl.scenario_player;
    if (!sp.sim_initialized) return;
    auto h = f4::entities::EntityHandle(sp_watched_entity(impl), &sp.sim->world());
    auto* tf = h.get<f4::entities::TransformComponent>();
    if (!tf) return;
    const Vector3 target = scenario_enu_to_raylib_v3(
        tf->position.x, tf->position.y, tf->position.z);
    sp.orbit_cam.set_target(target);
    sp.orbit_cam.set_distance(80.0f);  // close enough to see the F-16 in detail
    sp.orbit_cam.update_from_orbit();
}

void sp_reset_camera(Impl& impl) {
    auto& sp = impl.scenario_player;
    sp.orbit_cam.reset();
    // Default target: the parking spot (scenario aircraft's spawn position).
    if (sp.sim_initialized && !sp.scenario.aircraft.empty()) {
        const auto& p = sp.scenario.aircraft.front().parking_spot;
        sp.orbit_cam.set_target(scenario_enu_to_raylib_v3(p.x, p.y, p.z));
    }
    sp.orbit_cam.update_from_orbit();
}

// ── Mesh building (delegated to the shared RenderResources) ────────────────

void sp_build_aircraft_meshes(Impl& impl) {
    auto& sp = impl.scenario_player;
    if (!sp.sim_initialized) { sp.meshes_built = true; return; }

    auto h = f4::entities::EntityHandle(sp.sim->aircraft_entity(), &sp.sim->world());
    auto* vis = h.get<f4::simulation::VisualModelComponent>();
    if (!vis || vis->vis_type <= 0) {
        impl.status_msg = "No visual model on aircraft entity";
        sp.meshes_built = true;
        return;
    }

    impl.render_res_3d.build_mesh_for_model(vis->vis_type);

    sp.meshes_built = true;
    const auto* model = impl.render_res_3d.model_cache.lookup(vis->vis_type);
    if (model && !model->lod0_meshes.empty()) {
        int n_textured = 0;
        for (const auto& me : model->lod0_meshes) if (me.tex_id >= 0) ++n_textured;
        impl.status_msg = "Aircraft loaded: " +
            std::to_string(model->lod0_meshes.size()) + " meshes, " +
            std::to_string(n_textured) + " textured (glTF)";
    } else if (impl.render_res_3d.model_cache.ready()) {
        impl.status_msg = "Aircraft vis_type " + std::to_string(vis->vis_type) +
            " has no glTF export in Data/Models/koreaobj";
    }
}

/// Build the scenario's terrain mesh + textured-theater chunk set now that
/// the GL context exists (deferred from load_scenario because UploadMesh
/// requires the context). Idempotent -- guarded by the *_built flags. This
/// is the consolidated equivalent of the player's run()-prefix terrain
/// build; it runs once on the first scenario frame.
void sp_ensure_gl_resources_built(Impl& impl) {
    auto& sp = impl.scenario_player;

    // Aircraft meshes (glTF cache -- shared with the rest of the viewer).
    if (!sp.meshes_built) {
        sp_build_aircraft_meshes(impl);
    }

    // Terrain (Path B1 + Phase 2 textured path).
    if (sp.terrain_loaded && !sp.terrain_mesh_built) {
        float center_e = 0.0f, center_n = 0.0f;
        if (sp.scenario.has_airbase_source) {
            center_e = static_cast<float>(sp.scenario.layout_center.x);
            center_n = static_cast<float>(sp.scenario.layout_center.y);
        } else if (!sp.scenario.aircraft.empty()) {
            center_e = static_cast<float>(sp.scenario.aircraft.front().parking_spot.x);
            center_n = static_cast<float>(sp.scenario.aircraft.front().parking_spot.y);
        }

        bool built_textured = false;
        if (sp.theater_tiles_loaded && sp.world.ensure_gpu()) {
            sp.world_gpu_initialized = true;
            built_textured = sp.world.set_view(
                sp.terrain, center_e, center_n,
                /*extent_ft=*/250000.0f,      // far ring reaches the horizon
                /*near_extent_ft=*/60000.0f,  // near tiles around the airbase
                /*z_offset_ft=*/-5.0f);       // sink below airfield geometry
            if (const auto* cs = sp.world.chunk_set()) {
                std::fprintf(stderr,
                    "terrain: textured chunks n=%d near_quads=%d far_quads=%d "
                    "untextured=%d tile_layers=%d\n",
                    cs->chunks_total, cs->near_quads, cs->far_quads,
                    cs->quads_untextured,
                    sp.world.tile_cache().total_layers());
            }
        }
        sp.terrain_mesh_built = true;   // either path -- suppress re-entry

        if (!built_textured) {
            // Legacy Path B1 single mesh (vertex colors).
            f4::renderer::TerrainMeshConfig tc;
            tc.center_east_ft = center_e;
            tc.center_north_ft = center_n;
            tc.extent_ft = 100000.0f;  // ~19 nm half-extent (38 nm square)
            tc.resolution = 128;       // 16641 vertices, 16384 triangles
            tc.vertical_scale = 1.0f;
            tc.z_offset_ft = -5.0f;    // sink below airfield geometry to avoid z-fight
            tc.color_by_tile_type = true;
            sp.terrain_mesh = f4::renderer::build_terrain_mesh(sp.terrain, tc);
        }
    }

    // Reset the camera to look at the parking spot (once).
    if (!sp.initial_camera_set) {
        sp_reset_camera(impl);
        sp.initial_camera_set = true;
    }
    if (sp.camera_distance_override > 0.0) {
        sp.orbit_cam.set_distance(static_cast<float>(sp.camera_distance_override));
        sp.orbit_cam.update_from_orbit();
    }
}

// ── draw_airport (scenario-specific 3D overlays) ───────────────────────────

void sp_draw_airport(Impl& impl) {
    auto& sp = impl.scenario_player;
    if (!sp.airport_built || !sp.show_airport) return;

    if (sp.show_taxi_route) {
        for (const auto& l : sp.airfield.taxi_route_lines)
            f4::renderer::draw_layout_line(l);
    }
    if (sp.show_flightplan) {
        for (const auto& l : sp.airfield.flightplan_drop_lines)
            f4::renderer::draw_layout_line(l);
        for (const auto& l : sp.airfield.flightplan_lines)
            f4::renderer::draw_layout_line(l);
        for (const auto& m : sp.airfield.flightplan_waypoints)
            f4::renderer::draw_layout_marker(m);
    }
    if (sp.show_approach) {
        for (const auto& l : sp.airfield.approach_lines)
            f4::renderer::draw_layout_line(l);
        for (const auto& m : sp.airfield.approach_markers)
            f4::renderer::draw_layout_marker(m);
    }
    if (sp.show_taxi_in) {
        for (const auto& l : sp.airfield.taxi_in_route_lines)
            f4::renderer::draw_layout_line(l);
    }
    f4::renderer::draw_layout_marker(sp.airfield.parking_spot);
    f4::renderer::draw_layout_marker(sp.airfield.hold_short);
    f4::renderer::draw_layout_marker(sp.airfield.runway_end);
    if (sp.show_compass) {
        for (const auto& l : sp.airfield.compass_rose)
            f4::renderer::draw_layout_line(l);
    }
}

// ── draw_scene (the 3D world, via the shared render_world) ─────────────────

void sp_draw_scene(Impl& impl) {
    auto& sp = impl.scenario_player;
    f4::renderer::SceneDescription scene;
    scene.camera = sp.orbit_cam.camera();
    scene.sky_color = SKY_COLOR;

    // Scene anchor: a grid-referenced airbase lives at its objective center.
    if (sp.scenario.has_airbase_source) {
        scene.ground.origin_enu_x = static_cast<float>(sp.scenario.layout_center.x);
        scene.ground.origin_enu_y = static_cast<float>(sp.scenario.layout_center.y);
        scene.ground.origin_enu_z = static_cast<float>(sp.scenario.layout_center.z);
    } else if (!sp.scenario.aircraft.empty()) {
        const auto& p = sp.scenario.aircraft.front().parking_spot;
        scene.ground.origin_enu_x = static_cast<float>(p.x);
        scene.ground.origin_enu_y = static_cast<float>(p.y);
        scene.ground.origin_enu_z = static_cast<float>(p.z);
    }

    // Suppress the flat ground plane + grid when real terrain is present
    // (prevents z-fighting with the terrain mesh).
    const bool have_chunk_terrain =
        sp.theater_tiles_loaded && sp.world.chunk_set() != nullptr && sp.show_terrain;
    if (have_chunk_terrain) {
        scene.ground.plane = false;
        scene.ground.grid  = false;
    } else if (sp.terrain_loaded && sp.terrain_mesh_built && sp.terrain_mesh.valid) {
        scene.ground.plane = false;
        scene.ground.grid  = false;
    } else {
        scene.ground.grid = sp.show_grid;
    }
    scene.ground.axes = sp.show_axes;

    // VisualModelComponent entities -> plain EntityMeshDraw records.
    // f4-renderer must not depend on f4-simulation, so we extract position +
    // quaternion + KoreaObj model index here.
    if (sp.sim_initialized && (sp.show_aircraft || sp.show_airport)) {
        const auto entities =
            sp.sim->world().with_component<f4::simulation::VisualModelComponent>();
        const auto& aircraft_ids = sp.sim->aircraft_entities();
        const auto is_scenario_aircraft =
            [&aircraft_ids](f4::entities::EntityId id) {
                return std::find(aircraft_ids.begin(), aircraft_ids.end(), id)
                    != aircraft_ids.end();
            };

        for (const auto eid : entities) {
            auto h = f4::entities::EntityHandle(eid, &sp.sim->world());
            auto* vis = h.get<f4::simulation::VisualModelComponent>();
            auto* tf  = h.get<f4::entities::TransformComponent>();
            if (!vis || vis->vis_type <= 0 || !tf) continue;

            const bool is_aircraft = is_scenario_aircraft(eid);
            if (is_aircraft && !sp.show_aircraft) continue;
            if (!is_aircraft && !sp.show_airport) continue;

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
            emd.anim = &vis->anim_values;
            scene.entity_meshes.push_back(emd);
        }
    }

    // Airfield geometry (real OR synthetic -- both via the shared builder).
    if (sp.airport_built && sp.show_airport && !sp.airfield.geometry.empty) {
        scene.airfield = &sp.airfield.geometry;
        scene.airfield_origin_enu[0] = sp.airfield.origin_enu_x;
        scene.airfield_origin_enu[1] = sp.airfield.origin_enu_y;
        scene.airfield_origin_enu[2] =
            (sp.theater_tiles_loaded && sp.world.theater_loaded())
                ? static_cast<float>(sp.world.near_level().elevation_at_ft(
                      sp.airfield.origin_enu_x, sp.airfield.origin_enu_y))
                : sp.airfield.origin_enu_z;
    }

    // Terrain (textured path + untextured fallback).
    if (have_chunk_terrain) {
        sp.world.update_frame(SKY_COLOR);
        scene.terrain_chunk_set = sp.world.chunk_set();
    } else if (sp.terrain_loaded && sp.terrain_mesh_built && sp.terrain_mesh.valid) {
        scene.terrain_mesh = &sp.terrain_mesh;
    }

    // Sample the live missiles' positions (contrails) BEFORE render.
    sp_update_missile_trails(impl);

    // Scenario-specific 3D overlays (inside the 3D mode): taxi route, flight
    // plan, approach, taxi-in, markers, compass + the combat view.
    Impl* impl_ptr = &impl;
    scene.overlay_3d = [impl_ptr](const Camera3D&) {
        sp_draw_airport(*impl_ptr);
        sp_draw_missiles(*impl_ptr);
        sp_draw_gun_tracers(*impl_ptr);
    };

    f4::renderer::render_world(impl.render_res_3d, scene);

    sp_draw_hud(impl);
    sp_draw_radio(impl);
    sp_draw_combat(impl);
}

// ── HUD ────────────────────────────────────────────────────────────────────

void sp_draw_hud(Impl& impl) {
    auto& sp = impl.scenario_player;
    if (!sp.show_hud) return;

    const int x = 12;
    int y = 12;
    const int line_h = 18;
    const int pad = 8;

    std::vector<std::string> lines;
    char buf[256];

    std::snprintf(buf, sizeof(buf), "Scenario: %s", sp.scenario.name.c_str());
    lines.emplace_back(buf);

    if (sp.sim_initialized) {
        std::snprintf(buf, sizeof(buf), "Tick: %llu   Sim time: %.1fs",
                      static_cast<unsigned long long>(sp.sim->tick_count()),
                      sp.sim->sim_time_s());
        lines.emplace_back(buf);
        std::snprintf(buf, sizeof(buf), "State: %s   FPS: %d",
                      sp.paused ? "PAUSED" : "RUNNING", GetFPS());
        lines.emplace_back(buf);

        auto h = f4::entities::EntityHandle(sp_watched_entity(impl), &sp.sim->world());
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

        if (sp.terrain_loaded) {
            lines.emplace_back("Terrain: LOADED (real elevation)");
        } else {
            lines.emplace_back("Terrain: none (flat ground)");
        }

        if (auto* brain = h.get<f4::ai::BrainComponent>(); brain) {
            std::snprintf(buf, sizeof(buf), "AI: %s | %s | phase %s",
                          brain->mode_name().c_str(),
                          brain->state_name().c_str(),
                          brain->phase_name());
            lines.emplace_back(buf);
        }

        if (sp.scenario.combat.enabled) {
            const auto* rwr = h.get<f4::sensors::RwrComponent>();
            const char* rwr_state = "clear";
            if (rwr && rwr->launch_active)      rwr_state = "MISSILE LAUNCH!";
            else if (rwr && rwr->lock_active)   rwr_state = "SPIKE (locked)";
            std::snprintf(buf, sizeof(buf), "RWR: %s   Live missiles: %zu",
                          rwr_state,
                          f4::weapons::count_live_missiles(sp.sim->world()));
            lines.emplace_back(buf);
        }

        if (!sp.scenario.aircraft.empty()) {
            const std::size_t wi =
                sp.watched_index < sp.scenario.aircraft.size() ? sp.watched_index : 0;
            std::snprintf(buf, sizeof(buf), "Callsign: %s   (%s)%s",
                          sp.scenario.aircraft[wi].callsign.c_str(),
                          sp.scenario.aircraft[wi].aircraft_name.c_str(),
                          sp.scenario.aircraft.size() > 1 ? "   [Tab: cycle]" : "");
            lines.emplace_back(buf);
        }
    }

    if (!impl.status_msg.empty()) {
        lines.emplace_back("");
        lines.emplace_back("Status: " + impl.status_msg);
    }

    lines.emplace_back("");
    lines.emplace_back("Space: pause/resume   F: focus aircraft   R: reset view   Tab: watched   F3: FCS HUD");

    int max_w = 0;
    for (const auto& line : lines) {
        const int w = MeasureText(line.c_str(), 14);
        if (w > max_w) max_w = w;
    }
    const int bg_h = static_cast<int>(lines.size()) * line_h + pad * 2;
    const int bg_w = max_w + pad * 2;

    DrawRectangle(x, y, bg_w, bg_h, {0, 0, 0, 180});
    DrawRectangleLines(x, y, bg_w, bg_h, {255, 255, 255, 80});

    int line_y = y + pad;
    for (const auto& line : lines) {
        DrawText(line.c_str(), x + pad, line_y, 14, RAYWHITE);
        line_y += line_h;
    }
    (void)y;

    if (sp.show_fcs_hud && sp.sim_initialized) {
        sp_draw_fcs_hud(impl);
    }
}

void sp_draw_fcs_hud(Impl& impl) {
    auto& sp = impl.scenario_player;
    auto h = f4::entities::EntityHandle(sp_watched_entity(impl), &sp.sim->world());
    auto* fm = h.get<f4::flight::FlightModelComponent>();
    if (!fm) return;

    const auto& s = fm->state();
    const auto& fcs = s.fcs;
    const auto& kin = s.kin;
    const auto& aero = s.aero;

    const int x = 12;
    int y = 380;
    const int line_h = 16;
    const int pad = 8;
    const int font_size = 13;

    std::vector<std::string> lines;
    char buf[256];

    lines.emplace_back("\u2500\u2500\u2500 FCS State (F3 to hide) \u2500\u2500\u2500");

    std::snprintf(buf, sizeof(buf), "pstick: %+0.3f   rstick: %+0.3f",
                  fcs.pshape > 0 ? std::sqrt(fcs.pshape) : -std::sqrt(-fcs.pshape),
                  fcs.rshape > 0 ? std::sqrt(fcs.rshape) : -std::sqrt(-fcs.rshape));
    lines.emplace_back(buf);

    std::snprintf(buf, sizeof(buf), "throttle: %0.3f   speedBrake: %+0.2f",
                  s.engine.rpm > 0.01 ? s.engine.rpm : 0.0,
                  aero.dbrake);
    lines.emplace_back(buf);

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

    std::snprintf(buf, sizeof(buf), "alpha: %5.2f\u00B0   beta: %5.2f\u00B0   nzcgs: %+5.2f G",
                  f4::flight::to_degrees(aero.alpha),
                  f4::flight::to_degrees(aero.beta),
                  s.loads.nzcgs);
    lines.emplace_back(buf);

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

    DrawRectangle(x, y, bg_w, bg_h, {0, 0, 0, 180});
    DrawRectangleLines(x, y, bg_w, bg_h, {100, 200, 255, 120});

    int line_y = y + pad;
    for (std::size_t i = 0; i < lines.size(); ++i) {
        const Color c = (i == 0) ? Color{120, 200, 255, 255} : RAYWHITE;
        DrawText(lines[i].c_str(), x + pad, line_y, font_size, c);
        line_y += line_h;
    }
}

// ── ATC radio transcript (top-right panel) ─────────────────────────────────

void sp_draw_radio(Impl& impl) {
    auto& sp = impl.scenario_player;
    if (!sp.show_radio || !sp.sim_initialized) { sp.last_radio_h = 0; return; }

    constexpr std::size_t MAX_SHOWN = 9;
    const std::size_t n = sp.radio_log.size();
    const std::size_t first = n > MAX_SHOWN ? n - MAX_SHOWN : 0;

    int max_w = 0;
    for (std::size_t i = first; i < n; ++i) {
        const auto* e = sp.radio_log.at(i);
        if (!e) continue;
        char line[320];
        std::snprintf(line, sizeof(line), "T+%06.1f  %s: %s", e->time_s,
                      e->from_atc ? "TWR" : "EAGLE1", e->text.c_str());
        const int w = MeasureText(line, 13);
        if (w > max_w) max_w = w;
    }
    if (max_w == 0) { sp.last_radio_h = 0; return; }

    const int pad = 8;
    const int line_h = 17;
    const int shown = static_cast<int>(n - first);
    const int bg_w = max_w + pad * 2;
    const int bg_h = shown * line_h + pad * 2 + 18;  // + header line
    const int x = impl.window_w - bg_w - 12;
    const int y = 12;
    sp.last_radio_h = bg_h;   // anchors the COMBAT panel below this one

    DrawRectangle(x, y, bg_w, bg_h, {0, 0, 0, 170});
    DrawRectangleLines(x, y, bg_w, bg_h, {255, 255, 255, 70});
    DrawText("ATC", x + pad, y + pad, 13, {200, 200, 210, 255});

    int line_y = y + pad + 18;
    for (std::size_t i = first; i < n; ++i) {
        const auto* e = sp.radio_log.at(i);
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

// ── Combat view: contrails, missile bodies, guidance lines ────────────────

void sp_update_missile_trails(Impl& impl) {
    auto& sp = impl.scenario_player;
    if (!sp.sim_initialized) { sp.missile_trails.clear(); return; }

    std::vector<std::uint64_t> live;
    if (sp.scenario.combat.enabled && sp.show_combat && !sp.paused) {
        for (const auto eid :
             sp.sim->world().with_component<f4::weapons::MissileComponent>()) {
            auto h = f4::entities::EntityHandle(eid, &sp.sim->world());
            auto* tf = h.get<f4::entities::TransformComponent>();
            if (!tf) continue;
            live.push_back(eid.value);
            auto& trail = sp.missile_trails[eid.value];
            trail.points.push_back(
                scenario_enu_to_raylib_v3(tf->position.x, tf->position.y, tf->position.z));
            if (trail.points.size() > kMaxTrailPoints) {
                trail.points.erase(trail.points.begin());
            }
        }
    }

    for (auto it = sp.missile_trails.begin(); it != sp.missile_trails.end();) {
        if (std::find(live.begin(), live.end(), it->first) == live.end()) {
            it = sp.missile_trails.erase(it);
        } else {
            ++it;
        }
    }
}

void sp_draw_missiles(Impl& impl) {
    auto& sp = impl.scenario_player;
    if (!sp.sim_initialized || !sp.show_combat || !sp.scenario.combat.enabled) return;

    for (const auto eid :
         sp.sim->world().with_component<f4::weapons::MissileComponent>()) {
        auto h = f4::entities::EntityHandle(eid, &sp.sim->world());
        auto* tf = h.get<f4::entities::TransformComponent>();
        auto* mc = h.get<f4::weapons::MissileComponent>();
        if (!tf || !mc) continue;

        const Vector3 pos =
            scenario_enu_to_raylib_v3(tf->position.x, tf->position.y, tf->position.z);

        if (mc->target_id != 0) {
            auto tgt = f4::entities::EntityHandle(
                f4::entities::EntityId{mc->target_id}, &sp.sim->world());
            if (const auto* ttf = tgt.get<f4::entities::TransformComponent>()) {
                const Vector3 tpos = scenario_enu_to_raylib_v3(
                    ttf->position.x, ttf->position.y, ttf->position.z);
                DrawLine3D(pos, tpos, Color{255, 70, 70, 70});
            }
        }

        if (const auto it = sp.missile_trails.find(eid.value);
            it != sp.missile_trails.end()) {
            const auto& pts = it->second.points;
            for (std::size_t i = 1; i < pts.size(); ++i) {
                const float age =
                    static_cast<float>(pts.size() - i) /
                    static_cast<float>(pts.size());
                const unsigned char alpha =
                    static_cast<unsigned char>(200 * (1.0f - age) + 25);
                DrawLine3D(pts[i - 1], pts[i], Color{235, 235, 235, alpha});
            }
        }

        const Vector3 vel = scenario_enu_to_raylib_v3(tf->vx, tf->vy, tf->vz);
        if (Vector3Length(vel) > 1.0f) {
            const Vector3 tail = Vector3Subtract(
                pos, Vector3Scale(Vector3Normalize(vel), kMissileBodyFt));
            DrawCylinderEx(tail, pos, 3.0f, 1.0f, 8, Color{255, 235, 120, 220});
        }

        DrawSphereWires(pos, kMissileRingFt, 10, 10, Color{255, 240, 130, 160});
    }
}

void sp_draw_gun_tracers(Impl& impl) {
    auto& sp = impl.scenario_player;
    if (!sp.sim_initialized || !sp.show_combat || !sp.scenario.combat.enabled) return;

    constexpr float kTracerStreakFt = 300.0f;   // visual streak length

    for (const auto eid :
         sp.sim->world().with_component<f4::weapons::GunComponent>()) {
        auto h = f4::entities::EntityHandle(eid, &sp.sim->world());
        auto* gun = h.get<f4::weapons::GunComponent>();
        if (!gun) continue;

        for (const auto& t : gun->stream.tracers()) {
            const Vector3 pos =
                scenario_enu_to_raylib_v3(t.position.x, t.position.y, t.position.z);
            const Vector3 vel =
                scenario_enu_to_raylib_v3(t.velocity.x, t.velocity.y, t.velocity.z);
            if (Vector3Length(vel) < 1.0f) continue;
            const Vector3 tail = Vector3Subtract(
                pos, Vector3Scale(Vector3Normalize(vel), kTracerStreakFt));

            const float life = static_cast<float>(t.age_s) / 2.0f;   // 0 = new
            const unsigned char alpha = static_cast<unsigned char>(
                230.0f * (1.0f - life) + 20.0f);
            DrawLine3D(tail, pos, Color{255, 200, 60, alpha});
        }
    }
}

// ── COMBAT transcript (brevity panel under the ATC radio) ─────────────────

void sp_draw_combat(Impl& impl) {
    auto& sp = impl.scenario_player;
    if (!sp.show_combat || !sp.sim_initialized) return;

    constexpr std::size_t MAX_SHOWN = 10;
    const std::size_t n = sp.combat_log.size();
    if (n == 0) return;
    const std::size_t first = n > MAX_SHOWN ? n - MAX_SHOWN : 0;

    int max_w = 0;
    for (std::size_t i = first; i < n; ++i) {
        const auto* e = sp.combat_log.at(i);
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
    const int bg_h = shown * line_h + pad * 2 + 18;
    const int x = impl.window_w - bg_w - 12;
    const int y = 12 + sp.last_radio_h + 8;

    DrawRectangle(x, y, bg_w, bg_h, {10, 0, 0, 175});
    DrawRectangleLines(x, y, bg_w, bg_h, {255, 90, 90, 90});
    DrawText("COMBAT", x + pad, y + pad, 13, {255, 120, 120, 255});

    int line_y = y + pad + 18;
    for (std::size_t i = first; i < n; ++i) {
        const auto* e = sp.combat_log.at(i);
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

} // namespace

// ============================================================================
// ViewerApp public + private scenario API (defined here, declared in
// viewer_app.hpp). These delegate to the file-local sp_* helpers above.
// ============================================================================

bool ViewerApp::scenario_active() const noexcept {
    return impl_->scenario_player.active();
}

void ViewerApp::set_paused(bool paused) noexcept {
    impl_->scenario_player.paused = paused;
}

void ViewerApp::set_time_scale(double scale) noexcept {
    // Clamp to [0.1, 10.0]. The slider scales WALL-CLOCK time fed into the
    // fixed-timestep accumulator -- never the per-tick dt -- so FCS filter
    // stability no longer depends on it.
    if (scale > 0.0) {
        impl_->scenario_player.time_scale = std::clamp(scale, 0.1, 10.0);
    }
}

void ViewerApp::set_follow_camera(bool follow) noexcept {
    impl_->scenario_player.follow_aircraft = follow;
}

void ViewerApp::set_recording(const std::filesystem::path& trace_path,
                               int record_every) {
    auto& sp = impl_->scenario_player;
    sp.record_override = true;
    sp.record_override_path = trace_path;
    sp.record_override_every = record_every;
    if (!trace_path.empty()) {
        if (auto parent = trace_path.parent_path(); !parent.empty()) {
            std::filesystem::create_directories(parent);
        }
    }
}

void ViewerApp::set_camera_distance(double dist_ft) noexcept {
    impl_->scenario_player.camera_distance_override = dist_ft;
}

void ViewerApp::load_scenario(const std::filesystem::path& json_path) {
    auto& sp = impl_->scenario_player;

    // Remember the path for run_harness (the harness re-loads the scenario
    // fresh from disk).
    sp.scenario_json_path = json_path;

    // Load and validate the scenario JSON (resolves asset paths).
    sp.scenario = f4::simulation::load_scenario(json_path);

    // SHOWCASE-1: the CLI's --record override beats the template's own
    // record fields (applied before the Simulation snapshots the scenario).
    if (sp.record_override) {
        sp.scenario.record = true;
        if (!sp.record_override_path.empty()) {
            sp.scenario.record_path = sp.record_override_path;
        }
        if (sp.record_override_every > 0) {
            sp.scenario.record_every = sp.record_override_every;
        }
    }

    // Build the simulation. The asset dir is the scenario file's parent.
    const auto asset_dir = json_path.parent_path();
    sp.sim = std::make_unique<f4::simulation::Simulation>(sp.scenario, asset_dir);
    sp.sim->initialize();
    sp.sim_initialized = true;

    // Tranche 0d: the model source is the glTF export tree
    // (Data/Models/koreaobj -- RuntimeModelCache). Point the SHARED
    // RenderResources at it (the same cache the 3D Ground Layout panel uses).
    {
        std::filesystem::path data_dir = sp.scenario.data_dir;
        if (data_dir.empty()) {
            if (auto root = f4::assets::AssetRoot::discover()) {
                data_dir = root->data_dir();
            }
        }
        if (!data_dir.empty() &&
            std::filesystem::exists(data_dir / "Models" / "koreaobj")) {
            impl_->render_res_3d.set_model_data_dir(data_dir);
        } else {
            std::fprintf(stderr,
                "ScenarioPlayer: no glTF models under Data/Models/koreaobj -- "
                "aircraft will render without meshes. Run `f4import models "
                "--install <root> --data Data --all` to export them.\n");
        }
    }

    // Adopt the DERIVED scenario (initialize() resolves airbase_source: real
    // runway/taxi/parking layout, runway-frame waypoints, parking:"auto").
    sp.scenario = sp.sim->scenario();

    // Observe the ATC traffic + combat traffic for the overlays.
    sp.radio_log.attach(*sp.sim);
    sp.combat_log.attach(*sp.sim);

    // Load terrain (Path B1). Register a TerrainDataAdapter with the sim so
    // the FM's ground clamp follows real Korea elevation.
    if (!sp.scenario.terrain_json_path.empty()) {
        try {
            sp.terrain.load_terrain_json(sp.scenario.terrain_json_path);
            sp.terrain_loaded = true;
            sp.sim->set_terrain_source(&sp.terrain_adapter);
            impl_->status_msg = "Terrain loaded: " +
                std::to_string(sp.terrain.header.width) + "x" +
                std::to_string(sp.terrain.header.height) + " grid";
        } catch (const std::exception& e) {
            impl_->status_msg = std::string("Terrain load failed: ") + e.what();
        }
    }

    // Textured theater data (Phase 2): the scenario's own WorldView loads the
    // raw post levels + tile databases from scenario.theater_dir.
    if (!sp.scenario.theater_dir.empty()) {
        try {
            sp.theater_tiles_loaded = sp.world.load_theater(sp.scenario.theater_dir);
            if (!sp.theater_tiles_loaded) {
                impl_->status_msg =
                    "Theater tiles incomplete -- using untextured terrain";
            }
        } catch (const std::exception& e) {
            sp.theater_tiles_loaded = false;
            impl_->status_msg = std::string("Theater tile load failed: ") + e.what();
        }
    }

    // Build the airfield geometry from the SIMULATION's (derived) scenario.
    sp.airfield = build_airfield_overlays(sp.sim->scenario());
    sp.airport_built = true;

    // Reset the camera to point at the parking spot. (The GL-context-dependent
    // mesh/terrain build is deferred to the first scenario frame.)
    sp_reset_camera(*impl_);
    sp.initial_camera_set = false;  // re-set on first frame after GL context

    impl_->status_msg = "Scenario '" + sp.scenario.name + "' loaded.";
}

void ViewerApp::handle_scenario_input() {
    sp_ensure_gl_resources_built(*impl_);
    sp_handle_camera_input(*impl_);
}

void ViewerApp::scenario_advance(double dt) {
    auto& sp = impl_->scenario_player;
    // Fixed-timestep tick loop ("Fix Your Timestep" accumulator). The
    // accumulator collects WALL-CLOCK seconds scaled by the speed slider and
    // is drained in whole scenario.sim_dt ticks -- dt is ALWAYS sim_dt, so
    // the flight model's minor step stays at its tuned 1/360 s and the FCS's
    // discrete filters run at their designed operating point at every speed.
    if (!sp.paused && dt > 0.0) {
        sp.sim_accumulator += dt * sp.time_scale;
        constexpr int kMaxSimStepsPerFrame = 30;
        int steps = 0;
        while (sp.sim_accumulator >= sp.scenario.sim_dt &&
               steps < kMaxSimStepsPerFrame) {
            sp.sim->tick(sp.scenario.sim_dt);
            sp.sim_accumulator -= sp.scenario.sim_dt;
            ++steps;
        }
        if (steps == kMaxSimStepsPerFrame) {
            sp.sim_accumulator = 0.0;  // drop the debt, stay live
        }
    }
}

void ViewerApp::draw_scenario() {
    sp_draw_scene(*impl_);
}

void ViewerApp::draw_scenario_panel() {
    auto& sp = impl_->scenario_player;
    ImGui::Begin("Scenario Player", nullptr, ImGuiWindowFlags_AlwaysAutoResize);
    ImGui::Text("Scenario: %s", sp.scenario.name.c_str());
    ImGui::Separator();
    if (ImGui::Button(sp.paused ? "Resume (Space)" : "Pause (Space)")) {
        sp.paused = !sp.paused;
    }
    ImGui::SameLine();
    if (ImGui::Button("Reset View (R)")) sp_reset_camera(*impl_);
    ImGui::SameLine();
    if (ImGui::Button("Focus Aircraft (F)")) sp_fit_to_aircraft(*impl_);
    ImGui::Checkbox("Follow aircraft (C)", &sp.follow_aircraft);
    ImGui::Separator();
    // Watched aircraft -- which jet the HUD / follow camera / F-focus track.
    if (sp.scenario.aircraft.size() > 1) {
        const std::size_t wi = sp.watched_index <
            sp.scenario.aircraft.size() ? sp.watched_index : 0;
        if (ImGui::BeginCombo("Watched",
                sp.scenario.aircraft[wi].callsign.c_str())) {
            for (std::size_t i = 0; i < sp.scenario.aircraft.size(); ++i) {
                const bool selected = (i == sp.watched_index);
                if (ImGui::Selectable(
                        sp.scenario.aircraft[i].callsign.c_str(),
                        selected)) {
                    sp.watched_index = i;
                }
                if (selected) ImGui::SetItemDefaultFocus();
            }
            ImGui::EndCombo();
        }
    }
    ImGui::Separator();
    float speed = static_cast<float>(sp.time_scale);
    if (ImGui::SliderFloat("Sim speed", &speed, 0.1f, 10.0f, "%.1fx",
                           ImGuiSliderFlags_Logarithmic)) {
        sp.time_scale = speed;
        if (sp.sim) sp.sim->set_trace_time_scale(speed);
    }
    ImGui::Separator();
    ImGui::Checkbox("Show airport", &sp.show_airport);
    ImGui::Checkbox("Show aircraft", &sp.show_aircraft);
    if (sp.terrain_loaded) {
        ImGui::Checkbox("Show terrain", &sp.show_terrain);
    }
    ImGui::Checkbox("Show taxi route", &sp.show_taxi_route);
    ImGui::Checkbox("Show flight plan", &sp.show_flightplan);
    ImGui::Checkbox("Show approach", &sp.show_approach);
    ImGui::Checkbox("Show taxi-in route", &sp.show_taxi_in);
    ImGui::Checkbox("Show radio log", &sp.show_radio);
    if (sp.scenario.combat.enabled) {
        ImGui::Checkbox("Show combat view", &sp.show_combat);
    }
    ImGui::Checkbox("Show compass", &sp.show_compass);
    ImGui::Checkbox("Show grid", &sp.show_grid);
    ImGui::Checkbox("Show axes", &sp.show_axes);
    ImGui::Checkbox("Show HUD", &sp.show_hud);
    if (!impl_->status_msg.empty()) {
        ImGui::Separator();
        ImGui::TextWrapped("%s", impl_->status_msg.c_str());
    }
    ImGui::End();
}

// ── run_harness: the --harness headless BVR-intercept QC path ──────────────
// Ported verbatim from the former PlayerApp::run_harness. Headless: no GL
// context, no render loop, no ImGui. Builds its OWN fresh Simulation per
// pass (the determinism proof demands it), writes the three artifacts, and
// returns the harness's exit code.
int ViewerApp::run_harness(const std::filesystem::path& summary_out,
                            std::int64_t horizon_sec,
                            double sample_sec,
                            int runs) {
    auto& sp = impl_->scenario_player;
    if (!sp.sim_initialized) {
        throw std::runtime_error(
            "ViewerApp::run_harness: no scenario loaded -- call "
            "load_scenario() first");
    }
    if (sp.scenario_json_path.empty()) {
        std::fprintf(stderr,
            "ViewerApp::run_harness: scenario JSON path not recorded "
            "(load_scenario was not called)\n");
        return 1;
    }

    f4::simulation::InterceptHarnessOptions opts;
    opts.scenario_json = sp.scenario_json_path;
    opts.asset_dir = sp.scenario_json_path.parent_path();
    opts.horizon_sec = horizon_sec;
    opts.sample_sec = sample_sec;
    opts.runs = runs;

    std::string err;
    auto harness = f4::simulation::BvrInterceptHarness::create(opts, &err);
    if (!harness) {
        std::fprintf(stderr,
            "ViewerApp::run_harness: harness create failed (exit 1): %s\n",
            err.c_str());
        return 1;
    }

    const auto& report = harness->execute(/*on_sample=*/nullptr);

    const auto out_dir = summary_out.parent_path();
    if (!out_dir.empty()) {
        std::error_code ec;
        std::filesystem::create_directories(out_dir, ec);
    }
    const auto result_path = summary_out;
    const auto summary_path = out_dir / "bvr_intercept_summary.json";
    const auto diary_path = out_dir / "bvr_intercept_diary.json";

    // 1. The recorder JSON (the byte-stable certificate).
    {
        std::ofstream out(result_path);
        if (!out) {
            std::fprintf(stderr,
                "ViewerApp::run_harness: cannot write %s (exit 1)\n",
                result_path.string().c_str());
            return 1;
        }
        out << report.recorder_json;
    }

    // 2. The summary JSON -- deterministic content only.
    {
        f4::json::Writer w;
        w.put("{\n  \"format\": \"f4-bvr-intercept-summary\",\n  ");
        w.put("\"version\": 1");
        w.put(",\n  \"scenario_json\": ");
        write_json_string_(w, sp.scenario_json_path.string());
        w.put(",\n  \"bvr\": {\n    ");
        w.number_key("horizon_sec", horizon_sec);
        w.put(",    ");
        w.number_key("sample_sec", sample_sec);
        w.put(",    ");
        w.number_key("runs", runs);
        w.put(",    ");
        w.number_key("samples", report.samples);
        w.put(",    ");
        w.put("\"combat_enabled\": ");
        w.put(report.combat_enabled ? "true" : "false");
        w.put(",    ");
        w.number_key("aircraft_count", report.aircraft_count);
        w.put(",    ");
        w.number_key("blue_aircraft", report.blue_aircraft);
        w.put(",    ");
        w.number_key("red_aircraft", report.red_aircraft);
        w.put(",    ");
        w.number_key("tracks_acquired", report.tracks_acquired);
        w.put(",    ");
        w.number_key("tracks_dropped", report.tracks_dropped);
        w.put(",    ");
        w.number_key("rwr_locks", report.rwr_locks);
        w.put(",    ");
        w.number_key("rwr_launches", report.rwr_launches);
        w.put(",    ");
        w.number_key("missiles_launched", report.missiles_launched);
        w.put(",    ");
        w.number_key("missiles_detonated", report.missiles_detonated);
        w.put(",    ");
        w.number_key("damage_events", report.damage_events);
        w.put(",    ");
        w.number_key("kills", report.kills);
        w.put(",\n    \"recorder_md5_run0\": ");
        write_json_string_(w, report.verdict.recorder_md5_run0);
        w.put(",\n    \"recorder_md5_run1\": ");
        write_json_string_(w, report.verdict.recorder_md5_run1);
        w.put(",\n    \"deterministic\": ");
        w.put(report.verdict.deterministic ? "true" : "false");
        w.put(",\n    \"engagement_completed\": ");
        w.put(report.verdict.engagement_completed ? "true" : "false");
        w.put(",\n    \"roster_bounded\": ");
        w.put(report.verdict.roster_bounded ? "true" : "false");
        w.put(",\n    \"fight_alive\": ");
        w.put(report.verdict.fight_alive ? "true" : "false");
        w.put(",\n    \"engagement_failure\": ");
        write_json_string_(w, report.verdict.engagement_failure);
        w.put(",\n    \"roster_leak\": ");
        write_json_string_(w, report.verdict.roster_leak);
        w.put(",\n    \"fight_stall\": ");
        write_json_string_(w, report.verdict.fight_stall);
        w.put(",\n    ");
        w.number_key("first_detect_s", report.verdict.first_detect_s);
        w.put(",    ");
        w.number_key("first_launch_s", report.verdict.first_launch_s);
        w.put(",    ");
        w.number_key("first_kill_s", report.verdict.first_kill_s);
        w.put(",    ");
        w.number_key("shots_fired", report.verdict.shots_fired);
        w.put(",    ");
        w.number_key("shots_hit", report.verdict.shots_hit);
        w.put(",    ");
        w.number_key("shots_missed", report.verdict.shots_missed);
        w.put(",\n    \"aborted\": ");
        w.put(report.aborted ? "true" : "false");
        w.put(",\n    \"abort_reason\": ");
        write_json_string_(w, report.abort_reason);
        w.put(",\n    \"result_json\": ");
        write_json_string_(w, result_path.string());
        w.put("\n  }");
        w.put("\n}\n");

        std::ofstream out(summary_path);
        if (!out) {
            std::fprintf(stderr,
                "ViewerApp::run_harness: cannot write %s (exit 1)\n",
                summary_path.string().c_str());
            return 1;
        }
        out << w.str();
    }

    // 3. The diary JSON -- per-sample telemetry (NOT byte-stable).
    {
        f4::json::Writer w;
        w.put("{\n  \"format\": \"f4-bvr-intercept-diary\",\n  ");
        w.put("\"version\": 1,\n  ");
        w.put("\"note\": \"performance telemetry (wall-clock, ticks/sec, "
              "RSS) varies by host; the byte-stable artifacts are "
              "bvr_intercept_result.json and bvr_intercept_summary.json\",\n  ");
        w.number_key("samples", static_cast<std::int64_t>(report.diary.size()));
        w.put(",\n  \"rows\": [");
        bool first = true;
        for (const auto& s : report.diary) {
            w.put(first ? "\n    {" : ",\n    {");
            first = false;
            w.number_key("sample", s.sample);
            w.put(", ");
            w.number_key("sim_time_s", s.sim_time_s);
            w.put(", ");
            w.number_key("initial_entities", s.initial_entities);
            w.put(", ");
            w.number_key("spawned_entities", s.spawned_entities);
            w.put(", ");
            w.number_key("retired_entities", s.retired_entities);
            w.put(", ");
            w.number_key("live_entities", s.live_entities);
            w.put(", ");
            w.number_key("live_missiles", s.live_missiles);
            w.put(", ");
            w.number_key("tracks_acquired", s.tracks_acquired);
            w.put(", ");
            w.number_key("tracks_dropped", s.tracks_dropped);
            w.put(", ");
            w.number_key("rwr_locks", s.rwr_locks);
            w.put(", ");
            w.number_key("rwr_launches", s.rwr_launches);
            w.put(", ");
            w.number_key("missiles_launched", s.missiles_launched);
            w.put(", ");
            w.number_key("missiles_detonated", s.missiles_detonated);
            w.put(", ");
            w.number_key("damage_events", s.damage_events);
            w.put(", ");
            w.number_key("kills", s.kills);
            w.put(", ");
            w.number_key("sample_launches", s.sample_launches);
            w.put(", ");
            w.number_key("sample_detonations", s.sample_detonations);
            w.put(", ");
            w.number_key("sample_kills", s.sample_kills);
            char buf[128];
            std::snprintf(buf, sizeof(buf),
                ", \"wall_sec\": %.3f, \"ticks_per_sec\": %.1f, "
                "\"rss_kb\": %ld",
                s.wall_sec, s.ticks_per_sec, s.rss_kb);
            w.put(buf);
            w.put("\n    }");
        }
        w.put(report.diary.empty() ? "]" : "\n  ]");
        w.put("\n}\n");

        std::ofstream out(diary_path);
        if (!out) {
            std::fprintf(stderr,
                "ViewerApp::run_harness: cannot write %s (exit 1)\n",
                diary_path.string().c_str());
            return 1;
        }
        out << w.str();
    }

    std::printf("bvr: deterministic=%s engagement=%s roster=%s alive=%s "
                "md5=%s\n",
                report.verdict.deterministic ? "yes" : "NO",
                report.verdict.engagement_completed ? "ok" : "FAIL",
                report.verdict.roster_bounded ? "ok" : "LEAK",
                report.verdict.fight_alive ? "ok" : "STALLED",
                report.verdict.recorder_md5_run0.c_str());
    std::printf("bvr: detect@%.1fs launch@%.1fs kill@%.1fs "
                "shots=%d hit=%d miss=%d\n",
                report.verdict.first_detect_s,
                report.verdict.first_launch_s,
                report.verdict.first_kill_s,
                report.verdict.shots_fired,
                report.verdict.shots_hit,
                report.verdict.shots_missed);
    std::printf("wrote: %s\n", result_path.string().c_str());
    std::printf("wrote: %s\n", summary_path.string().c_str());
    std::printf("wrote: %s\n", diary_path.string().c_str());

    if (report.aborted) {
        if (report.abort_reason.find("combat.enabled") != std::string::npos) {
            std::fprintf(stderr,
                "ViewerApp::run_harness: harness refused scenario "
                "(exit 2): %s\n",
                report.abort_reason.c_str());
            return 2;
        }
        std::fprintf(stderr,
            "ViewerApp::run_harness: harness aborted (exit 1): %s\n",
            report.abort_reason.c_str());
        return 1;
    }
    if (runs >= 2 && !report.verdict.deterministic) {
        std::fprintf(stderr,
            "ViewerApp::run_harness: NON-DETERMINISTIC (exit 9) -- "
            "run 0 md5 %s != run 1 md5 %s.\n",
            report.verdict.recorder_md5_run0.c_str(),
            report.verdict.recorder_md5_run1.c_str());
        return 9;
    }
    if (!report.verdict.roster_bounded) {
        std::fprintf(stderr,
            "ViewerApp::run_harness: ROSTER LEAK (exit 6): %s.\n",
            report.verdict.roster_leak.c_str());
        return 6;
    }
    if (!report.verdict.fight_alive) {
        std::fprintf(stderr,
            "ViewerApp::run_harness: FIGHT STALLED (exit 3): %s.\n",
            report.verdict.fight_stall.c_str());
        return 3;
    }
    if (report.verdict.first_launch_s < 0.0) {
        std::fprintf(stderr,
            "ViewerApp::run_harness: NO LAUNCH (exit 4).\n");
        return 4;
    }
    if (!report.verdict.engagement_completed) {
        std::fprintf(stderr,
            "ViewerApp::run_harness: ENGAGEMENT NOT COMPLETED (exit 5): "
            "%s.\n",
            report.verdict.engagement_failure.c_str());
        return 5;
    }
    return 0;
}

} // namespace f4::viewer
