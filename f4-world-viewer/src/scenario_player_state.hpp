// f4-world-viewer/src/scenario_player_state.hpp
//
// PRIVATE HEADER — internal to the f4-world-viewer library.
//
// ScenarioPlayerState — the self-contained state for the world viewer's
// "scenario" mode (the consolidated f4-scenario-player). Lives as a field
// on ViewerApp::Impl; activated by ViewerApp::load_scenario(). When
// active(), run()'s main loop dispatches to handle_scenario_input() +
// draw_scenario() + draw_scenario_panel() instead of the campaign canvas
// or the replay view — the same branch pattern ReplayState uses.
//
// OWNED state (scenario-specific): the Simulation, the Scenario, its own
// terrain (TerrainData + adapter + mesh), its own textured-theater
// WorldView, an aircraft-scale OrbitCamera, the AirfieldOverlays, the
// ATC RadioLog, the CombatTranscript, missile contrail samples, the
// fixed-timestep accumulator, the record override, and the show flags.
//
// BORROWED state (shared with the rest of the viewer): the RenderResources
// (impl.render_res_3d — the glTF model cache, lit shader, texture cache),
// the window (impl.window_w/h), the exit flag (impl.should_exit), the
// screenshot fields (impl.screenshot_*), and the status line
// (impl.status_msg). Sharing RenderResources — the heaviest GPU resource
// — is the single biggest win of the consolidation: the scenario mode no
// longer maintains its own parallel model/shader cache.
//
// CRITICAL: f4-flight-model headers must be included BEFORE raylib.h
// because Raylib's PI macro breaks `using f4::math::PI;` in
// f4/flight/constants.hpp. This header includes the flight + simulation
// headers first, then raylib.h — so it is safe to include from
// viewer_state.hpp before its own raylib.h.

#pragma once

#include <f4/viewer/airfield_overlays.hpp>

#include <f4/simulation/simulation.hpp>
#include <f4/simulation/scenario.hpp>
#include <f4/simulation/combat_transcript.hpp>
#include <f4/simulation/visual_model_component.hpp>
#include <f4/renderer/orbit_camera.hpp>
#include <f4/renderer/terrain_mesh.hpp>
#include <f4/renderer/world_view.hpp>
#include <f4/renderer/coord_transform.hpp>
#include <f4/terrain/terrain_data.hpp>
#include <f4/terrain/terrain_adapter.hpp>
#include <f4/flight/flight_model_component.hpp>   // MUST precede raylib.h (PI macro)
#include <f4/entities/entity.hpp>

// Now safe to include Raylib (the PI macro is defined after the flight
// headers' `using f4::math::PI;` has already resolved).
#include <raylib.h>

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace f4::viewer {

// ── Coordinate conversion (Raylib-typed wrapper) ───────────────────────────
// Wraps the engine-agnostic f4::renderer::enu_to_raylib (returns Float3) to
// return Raylib's Vector3. The math lives in f4-renderer so it's unit-tested
// without a Raylib dependency.
inline Vector3 scenario_enu_to_raylib_v3(double east_ft, double north_ft,
                                          double up_ft) noexcept {
    const auto v = f4::renderer::enu_to_raylib(east_ft, north_ft, up_ft);
    return Vector3{v.x, v.y, v.z};
}

/// The scenario player's state. One instance lives on ViewerApp::Impl
/// (impl->scenario_player). Activated by load_scenario(); deactivated by
/// closing the scenario (a future File > Close Scenario, or loading a
/// world/replay which exits scenario mode).
struct ScenarioPlayerState {
    // ── Simulation ────────────────────────────────────────────────────
    std::unique_ptr<f4::simulation::Simulation> sim;
    f4::simulation::Scenario scenario;
    bool sim_initialized = false;
    bool paused = true;          // start paused so the aircraft sits at parking
    double time_scale = 1.0;
    // SHOWCASE-1: the --record CLI override (set_recording). Applied in
    // load_scenario() AFTER the JSON parse but BEFORE the Simulation build
    // — the CLI wins over the template's own record fields.
    bool record_override = false;
    std::filesystem::path record_override_path;
    int record_override_every = 0;
    // Fixed-timestep accumulator (seconds of SIM time owed). The render
    // loop fills it with wall-clock dt * time_scale and drains it in whole
    // scenario.sim_dt ticks, so every tick the flight model sees is exactly
    // sim_dt wide — regardless of the speed slider or the frame rate.
    double sim_accumulator = 0.0;
    // The scenario JSON path — remembered for run_harness (which re-loads
    // the scenario fresh to build its own Simulation).
    std::filesystem::path scenario_json_path;

    // ── Airfield geometry (shared f4-renderer builder + scenario overlays)
    AirfieldOverlays airfield;
    bool airport_built = false;

    // ── Terrain (Path B1) ─────────────────────────────────────────────
    // The scenario's OWN terrain (loaded from scenario.terrain_json_path),
    // separate from the viewer's campaign terrain. The TerrainDataAdapter
    // wraps it for the sim (TerrainSource); the TerrainMesh is built for
    // the renderer around the airfield center.
    f4::terrain::TerrainData terrain;
    bool terrain_loaded = false;
    f4::terrain::TerrainDataAdapter terrain_adapter{terrain};
    f4::renderer::TerrainMesh terrain_mesh;
    bool terrain_mesh_built = false;
    bool show_terrain = true;

    // ── Textured theater — the scenario's OWN WorldView ──────────────
    // Separate from impl.world (the campaign theater) so entering scenario
    // mode never disturbs a loaded campaign. Loaded from
    // scenario.theater_dir; falls back to the untextured TerrainMesh when
    // the theater lacks tile data.
    f4::renderer::WorldView world;
    bool theater_tiles_loaded = false;        // world.theater_loaded()
    bool world_gpu_initialized = false;       // world.ensure_gpu() ran (GL context)

    // ── Orbit camera (aircraft-scale, delegated to OrbitCamera) ───────
    // Configured for close aircraft inspection (1 ft .. 100 000 ft, 250 ft
    // initial — vs the 3D panel's 50/50 000/4 000). The scenario mode owns
    // its own camera so it doesn't fight the 3D Ground Layout panel's.
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
    double camera_distance_override = -1.0;  // CLI --camera-distance (ft)

    /// True once the primary aircraft's mesh has been ensured in the shared
    /// cache (build_aircraft_meshes ran after GL context creation).
    bool meshes_built = false;

    // ── HUD / show flags ──────────────────────────────────────────────
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
    bool show_combat = true;       // missiles + gun tracers + COMBAT panel
    bool follow_aircraft = false;  // camera tracks the aircraft (C)

    // ── Combat view (bvr_intercept scenarios) ──────────────────────────
    std::size_t watched_index = 0;   // Tab cycles; HUD/follow/F-focus track this
    f4::simulation::CombatTranscript combat_log;
    int last_radio_h = 0;            // anchors the COMBAT panel under the ATC panel
    // Per-missile contrail: entity id -> recent ENU positions (newest last).
    struct MissileTrail {
        std::vector<Vector3> points;
    };
    std::unordered_map<std::uint64_t, MissileTrail> missile_trails;

    // ── ATC radio transcript (observes the bus) ───────────────────────
    RadioLog radio_log;

    // ── Frame timing (for the in-frame fixed-timestep tick loop) ──────
    double last_frame_time = 0.0;
    bool first_frame = true;

    /// True when a scenario is loaded and run() should dispatch to the
    /// scenario render path instead of the campaign canvas or replay view.
    [[nodiscard]] bool active() const noexcept { return sim_initialized; }
};

} // namespace f4::viewer
