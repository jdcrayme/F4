// f4-scenario-player/src/viewer_state.hpp
//
// PRIVATE HEADER — internal to the f4-scenario-player library. Not
// installed, not visible to consumers. Holds all render-loop state in
// one pimpl struct, mirroring the pattern from f4-models-viewer.

#pragma once

#include "f4/scenario_player/player_app.hpp"
#include "f4/scenario_player/airport_geometry.hpp"
#include <f4/renderer/coord_transform.hpp>       // Float3, enu_to_raylib, model_vertex_to_raylib
#include <f4/renderer/orbit_camera.hpp>          // OrbitCamera
#include <f4/renderer/lit_shader.hpp>            // LitShader
#include <f4/renderer/mesh_builder.hpp>          // MeshEntry
#include <f4/renderer/texture_cache.hpp>         // TextureCache

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

inline Vector3 model_vertex_to_raylib_v3(float x, float y, float z) noexcept {
    const auto v = f4::renderer::model_vertex_to_raylib(x, y, z);
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

    // ── Visual-entity mesh cache (Phase 2A) ───────────────────────────
    // One entry per unique model_record (keyed by parent_index). Multiple
    // entities sharing the same vis_type reuse one mesh cache entry —
    // e.g. three hangars of the same type upload their geometry only once.
    //
    // Built lazily from draw_visual_entities() the first time an entity
    // with a previously-unseen parent_index is encountered. Requires the
    // GL context (UploadMesh), so the first build happens inside run().
    // Uses f4::renderer::MeshEntry from f4-renderer.
    struct MeshCacheEntry {
        std::vector<f4::renderer::MeshEntry> meshes;
        bool built = false;
    };
    std::unordered_map<int, MeshCacheEntry> mesh_cache;  // key = parent_index

    /// Legacy alias — the aircraft's mesh list, used by code paths that
    /// haven't been refactored to walk the world yet. Points at the
    /// mesh_cache entry for the aircraft's vis_type_index (built lazily).
    bool meshes_built = false;

    // ── Texture cache (delegated to f4::renderer::TextureCache) ──────
    f4::renderer::TextureCache texture_cache;

    // ── Lit shader (delegated to f4::renderer::LitShader) ────────────
    f4::renderer::LitShader lit_shader;

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
    void handle_camera_input();
    void fit_to_aircraft();
    void reset_camera();

    void build_aircraft_meshes();       // legacy: ensures aircraft's mesh is cached
    void build_mesh_for_model(int parent_index);  // Phase 2A: lazy mesh build
    void upload_textures();
    void unload_meshes();

    void draw_scene();
    void draw_airport();
    void draw_aircraft();               // legacy: draws only the primary aircraft
    void draw_visual_entities();        // Phase 2A: walks all VMC-bearing entities
    void draw_hud();
};

} // namespace f4::scenario_player
