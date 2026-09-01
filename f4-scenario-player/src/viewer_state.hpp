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
#include <f4/renderer/terrain_mesh.hpp>          // TerrainMesh (untextured fallback)
#include <f4/renderer/world_view.hpp>            // WorldView (textured theater path)
#include <f4/terrain/terrain_data.hpp>           // TerrainData
#include <f4/terrain/terrain_adapter.hpp>        // TerrainDataAdapter (sim ground clamp)

#include <f4/simulation/simulation.hpp>
#include <f4/simulation/scenario.hpp>
#include <f4/simulation/combat_transcript.hpp>
#include <f4/simulation/visual_model_component.hpp>
#include <f4/models/model_database.hpp>
#include <f4/models/geometry.hpp>
#include <f4/models/texture.hpp>
#include <f4/entities/entity.hpp>
#include <f4/weapons/missile_battery.hpp>   // MissileComponent (combat view)
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
    // Fixed-timestep accumulator (seconds of SIM time owed). The render
    // loop fills it with wall-clock dt * time_scale and drains it in
    // whole scenario.sim_dt ticks, so every tick the flight model sees
    // is exactly sim_dt wide — regardless of the speed slider or the
    // frame rate. See player_app.cpp's tick loop for the rationale.
    double sim_accumulator = 0.0;

    // ── Airfield geometry (shared f4-renderer builder + scenario overlays) ──
    AirfieldOverlays airfield;
    bool airport_built = false;

    // ── Terrain (Path B1) ─────────────────────────────────────────────
    // Loaded from scenario.terrain_json_path in load_scenario(). The
    // TerrainDataAdapter wraps it for the sim (TerrainSource interface);
    // the TerrainMesh is built for the renderer (heightmap mesh around
    // the airfield center). Both are empty/invalid when no terrain JSON
    // is configured — the sim falls back to FlatTerrainSource and the
    // renderer falls back to the flat green ground plane.
    f4::terrain::TerrainData terrain;
    bool terrain_loaded = false;
    f4::terrain::TerrainDataAdapter terrain_adapter{terrain};
    f4::renderer::TerrainMesh terrain_mesh;
    bool terrain_mesh_built = false;
    bool show_terrain = true;

    // ── Textured theater — the ONE shared load-a-world path ──────────
    // f4::renderer::WorldView owns the theater post levels + tile
    // databases (CPU) and the terrain shader + tile arrays + chunk set
    // (GPU), replacing the per-app terrain lifecycle this file used to
    // hand-roll. Loaded from scenario.theater_dir's raw binaries; when
    // the theater lacks tile data (JSON-only terrain) the viewer keeps
    // the untextured TerrainMesh fallback above.
    f4::renderer::WorldView world;
    bool theater_tiles_loaded = false;        // world.theater_loaded()

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
    bool show_fcs_hud = false;  // FCS internals column (F3 toggle)
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
    bool show_combat = true;       // missiles + shot lines + COMBAT panel
    bool follow_aircraft = false;  // camera tracks the aircraft (C)
    double camera_distance_override = -1.0;  // CLI --camera-distance (ft)

    // ── Combat view (bvr_intercept scenarios) ──────────────────────────
    // Which aircraft the HUD + follow camera + F-focus track (Tab cycles).
    // The scenario may spawn TWO+ fighters (bvr_intercept: EAGLE1 blue +
    // BANDIT1 red); everything that used aircraft_entity() (the first)
    // now goes through watched_entity().
    std::size_t watched_index = 0;
    // The brevity narration (M4 observability) — engine-agnostic ring
    // buffer maintained by f4-simulation; the player only draws it.
    f4::simulation::CombatTranscript combat_log;
    // Anchors the COMBAT panel under the ATC panel (set by draw_radio).
    int last_radio_h = 0;
    // Per-missile contrail: entity id -> recent ENU positions (one per
    // rendered frame, newest last). Entries vanish with the missile.
    struct MissileTrail {
        std::vector<Vector3> points;
    };
    std::unordered_map<std::uint64_t, MissileTrail> missile_trails;

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
    void draw_fcs_hud();                // FCS-internals column (F3 toggle)
    void draw_radio();                  // ATC transcript panel (top-right)
    void draw_combat();                 // COMBAT transcript panel (under ATC)
    void draw_missiles();               // 3D missile bodies + trails + lines
    void update_missile_trails();       // sample live missile positions

    // The entity the HUD/camera watch (Tab cycles). Falls back to the
    // first aircraft when the index outruns the spawn list.
    [[nodiscard]] f4::entities::EntityId watched_entity() const noexcept;
    void cycle_watched();
};

} // namespace f4::scenario_player
