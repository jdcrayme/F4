// f4-scenario-player/src/viewer_state.hpp
//
// PRIVATE HEADER — internal to the f4-scenario-player library. Not
// installed, not visible to consumers. Holds all render-loop state in
// one pimpl struct, mirroring the pattern from f4-models-viewer.

#pragma once

#include "f4/scenario_player/player_app.hpp"
#include "f4/scenario_player/airfield_overlays.hpp"
#include "radio_log.hpp"
#include <f4/renderer/coord_transform.hpp>       // Float3, enu_to_raylib, model_vertex_to_raylib
#include <f4/renderer/orbit_camera.hpp>          // OrbitCamera
#include <f4/renderer/render_resources.hpp>      // RenderResources (owns all GPU caches)
#include <f4/renderer/world_renderer.hpp>        // SceneDescription, render_world()
#include <f4/renderer/world_camera.hpp>

#include <f4/simulation/simulation.hpp>
#include <f4/simulation/scenario.hpp>
#include <f4/simulation/visual_model_component.hpp>
#include <f4/models/model_database.hpp>
#include <f4/models/geometry.hpp>
#include <f4/models/texture.hpp>
#include <f4/entities/entity.hpp>
#include <f4/flight/flight_model_component.hpp>  // must come BEFORE raylib.h
                                                 // (Raylib's PI macro breaks
                                                 // `using f4::math::PI;` in
                                                 // f4/flight/constants.hpp)

// Now safe to include Raylib.
#include <raylib.h>

#include <filesystem>
#include <string>
#include <unordered_map>
#include <vector>

namespace f4::scenario_player {

// ── Coordinate conversion (Raylib-typed wrappers) ─────────────────────────
// These wrap the engine-agnostic functions from f4::renderer::coord_transform
// to return Raylib's Vector3 type. The math is in the f4-renderer header so
// unit tests can verify it without depending on Raylib.

inline Vector3 enu_to_raylib_v3(double east_ft, double north_ft, double up_ft) noexcept {
    const auto v = f4::renderer::enu_to_raylib(east_ft, north_ft, up_ft);
    return Vector3{v.x, v.y, v.z};
}


struct PlayerApp::Impl {
    // ── Window ────────────────────────────────────────────────────────
    int window_w = 1600;
    int window_h = 900;
    bool should_exit = false;

    // ── Simulation ────────────────────────────────────────────────────
    std::unique_ptr<f4::simulation::Simulation> sim;
    f4::simulation::Scenario scenario;
    bool sim_initialized = false;
    bool paused = true;  // start paused so the aircraft sits at parking
    double time_scale = 1.0;

    // ── Airfield geometry (shared f4-renderer builder + scenario overlays) ──
    AirfieldOverlays airfield;
    bool airport_built = false;

    // ── Orbit camera (delegated to f4::renderer::OrbitCamera) ────────
    f4::renderer::OrbitCamera orbit_cam{
        f4::renderer::OrbitCameraConfig{
            .min_distance     = 1.0f,
            .max_distance     = 100000.f,
            .initial_yaw      = 45.0f,
            .initial_pitch    = 25.0f,
            .initial_distance = 250.0f,  // feet (larger than model viewer — we have a runway)
            .orbit_sensitivity = 0.3f,
            .pan_speed        = 0.003f,
            .zoom_speed       = 0.1f,
        }
    };
    bool initial_camera_set = false;

    // ── Shared GPU resources (f4::renderer::RenderResources) ────────
    // Owns the lit shader, texture cache, KoreaObj mesh cache, default
    // material, lighting state, and airfield geometry cache. Replaces the
    // per-app mesh_cache / texture_cache / lit_shader / lighting fields
    // that used to live here (identical caches also lived in the
    // world-viewer — one implementation now serves both).
    f4::renderer::RenderResources render_res;

    /// True once the primary aircraft's mesh has been ensured in the
    /// cache (build_aircraft_meshes ran after GL context creation).
    bool meshes_built = false;

    // ── HUD ───────────────────────────────────────────────────────────
    bool show_hud = true;
    bool show_grid = true;
    bool show_axes = true;
    bool show_airport = true;
    bool show_aircraft = true;
    bool show_taxi_route = true;
    bool show_compass = true;
    bool show_flightplan = true;   // cyan waypoint route at altitude
    bool show_approach = true;     // orange extended centerline + glide slope
    bool show_taxi_in = true;      // purple runway-exit -> parking route
    bool show_radio = true;        // ATC transcript panel
    bool follow_aircraft = false;  // camera tracks the aircraft (C)
    double camera_distance_override = -1.0;  // CLI --camera-distance (ft)

    // ── ATC radio transcript (observes the bus) ───────────────────────
    RadioLog radio_log;

    // ── Status / screenshot ───────────────────────────────────────────
    std::string status_msg;
    bool screenshot_pending = false;
    double screenshot_at = 0.0;
    std::filesystem::path screenshot_path;

    // ── Functions (defined in other .cpp files) ───────────────────────
    void handle_camera_input();
    void fit_to_aircraft();
    void reset_camera();

    void build_aircraft_meshes();       // ensures aircraft's mesh is cached
    void unload_meshes();               // render_res.unload_all() wrapper

    void draw_scene();                  // render_world() + HUD + radio
    void draw_airport();                // scenario-specific 3D overlays
    void draw_hud();
    void draw_radio();                  // ATC transcript panel (top-right)
};

} // namespace f4::scenario_player
