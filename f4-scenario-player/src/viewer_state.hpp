// f4-scenario-player/src/viewer_state.hpp
//
// PRIVATE HEADER — internal to the f4-scenario-player library. Not
// installed, not visible to consumers. Holds all render-loop state in
// one pimpl struct, mirroring the pattern from f4-models-viewer.

#pragma once

#include "f4/scenario_player/player_app.hpp"
#include "f4/scenario_player/airport_geometry.hpp"
#include "f4/scenario_player/coordinate_transform.hpp"  // Float3, enu_to_raylib, model_vertex_to_raylib

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
// These wrap the engine-agnostic functions from coordinate_transform.hpp
// to return Raylib's Vector3 type. The math is in the public header so
// unit tests can verify it without depending on Raylib.

inline Vector3 enu_to_raylib_v3(double east_ft, double north_ft, double up_ft) noexcept {
    const auto v = enu_to_raylib(east_ft, north_ft, up_ft);
    return Vector3{v.x, v.y, v.z};
}

inline Vector3 model_vertex_to_raylib_v3(float x, float y, float z) noexcept {
    const auto v = model_vertex_to_raylib(x, y, z);
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

    // ── Airport geometry (derived from scenario) ──────────────────────
    AirportGeometry airport;
    bool airport_built = false;

    // ── Orbit camera (ENU feet) ───────────────────────────────────────
    Camera3D camera = {};
    float cam_yaw = 45.0f;          // degrees
    float cam_pitch = 25.0f;        // degrees
    float cam_distance = 250.0f;    // feet (larger than model viewer — we have a runway)
    Vector3 cam_target = {0, 0, 0}; // Raylib coords
    bool orbit_dragging = false;
    bool pan_dragging = false;
    Vector2 drag_start = {0, 0};
    float drag_yaw0 = 0;
    float drag_pitch0 = 0;
    Vector3 drag_target0 = {};
    bool initial_camera_set = false;

    // ── Aircraft mesh cache ───────────────────────────────────────────
    // One entry per ModelGeometry::mesh. Rebuilt only when the model
    // changes (which it doesn't — the F-16's model_record is fixed at
    // scenario load). The entity's transform is applied per-frame as
    // a DrawMesh model matrix.
    struct MeshEntry {
        ::Mesh mesh = {};
        int tex_id = -1;  // -1 = no texture, use vertex colors
    };
    std::vector<MeshEntry> aircraft_meshes;
    bool meshes_built = false;

    // ── Texture cache (lazy, like f4-models-viewer) ───────────────────
    struct TexCacheEntry {
        ::Texture2D texture = {};
        ::Material material = {};
        bool has_alpha = false;
        bool uploaded = false;
    };
    std::unordered_map<int, TexCacheEntry> texture_cache;

    // ── Lit shader (same source as f4-models-viewer) ──────────────────
    Shader lit_shader = {};
    bool lit_shader_loaded = false;
    int lit_shader_dir_loc = -1;
    int lit_shader_color_loc = -1;
    int lit_shader_ambient_loc = -1;

    // ── Lighting (single directional sun + ambient) ───────────────────
    Vector3 light_direction = { 0.65f, -1.0f, 0.35f };  // points from scene toward sun
    float  light_intensity = 1.0f;
    Color  ambient_color = { 80, 80, 90, 255 };
    Color  light_color   = { 255, 250, 235, 255 };

    // ── HUD ───────────────────────────────────────────────────────────
    bool show_hud = true;
    bool show_grid = true;
    bool show_axes = true;
    bool show_airport = true;
    bool show_aircraft = true;
    bool show_taxi_route = true;
    bool show_compass = true;

    // ── Status / screenshot ───────────────────────────────────────────
    std::string status_msg;
    bool screenshot_pending = false;
    double screenshot_at = 0.0;
    std::filesystem::path screenshot_path;

    // ── Functions (defined in other .cpp files) ───────────────────────
    void update_camera_from_orbit();
    void handle_camera_input();
    void fit_to_aircraft();
    void reset_camera();

    void build_aircraft_meshes();
    void upload_textures();
    void unload_meshes();
    void unload_textures();
    bool ensure_lit_shader();

    void draw_scene();
    void draw_airport();
    void draw_aircraft();
    void draw_hud();
};

} // namespace f4::scenario_player
