// f4-scenario-player/src/player_app.cpp
//
// PlayerApp lifecycle (ctor/dtor/load_scenario/run) — the entry point
// of the host. Mirrors f4-models-viewer's ViewerApp::run() pattern.
//
// CRITICAL: f4-flight-model headers must be included BEFORE raylib.h
// because Raylib's PI macro breaks `using f4::math::PI;` in
// f4/flight/constants.hpp. See renderer.cpp for the same note.

#include "viewer_state.hpp"

#include <f4/simulation/simulation.hpp>
#include <f4/simulation/visual_model_component.hpp>
#include <f4/entities/entity.hpp>
#include <f4/flight/flight_model_component.hpp>

// Now safe to include Raylib (PI macro won't break the flight headers).
#include <rlImGui.h>
#include <raylib.h>
#include <imgui.h>

#include <algorithm>
#include <chrono>
#include <stdexcept>
#include <thread>

namespace f4::scenario_player {

// ── ctor ───────────────────────────────────────────────────────────────────
PlayerApp::PlayerApp() : impl_(std::make_unique<Impl>()) {
    impl_->orbit_cam.update_from_orbit();
}

// ── dtor ───────────────────────────────────────────────────────────────────
PlayerApp::~PlayerApp() {
    if (IsWindowReady()) {
        if (impl_->meshes_built) {
            impl_->unload_meshes();
        }
        // LitShader destructor handles UnloadShader automatically.
    }
}

// ── set_window_size ────────────────────────────────────────────────────────
void PlayerApp::set_window_size(int width, int height) noexcept {
    impl_->window_w = width;
    impl_->window_h = height;
}

// ── load_scenario ──────────────────────────────────────────────────────────
void PlayerApp::load_scenario(const std::filesystem::path& json_path) {
    // Load and validate the scenario JSON (resolves asset paths).
    impl_->scenario = f4::simulation::load_scenario(json_path);

    // Build the simulation. The asset dir is the scenario file's parent
    // directory — the scenario JSON's asset paths are already resolved
    // against it by load_scenario(), but we keep a copy for any future
    // runtime asset loading (e.g. additional aircraft configs).
    const auto asset_dir = json_path.parent_path();

    impl_->sim = std::make_unique<f4::simulation::Simulation>(impl_->scenario, asset_dir);
    impl_->sim->initialize();
    impl_->sim_initialized = true;

    // Adopt the DERIVED scenario (initialize() resolves airbase_source:
    // real runway/taxi/parking layout, runway-frame waypoints rotated to
    // world, parking:"auto" assigned a real spot). The player's pre-init
    // copy still has the unresolved authoring values — e.g. parking:"auto"
    // reads as the origin, which would point the camera ~700k ft away
    // from a grid-referenced airbase.
    impl_->scenario = impl_->sim->scenario();

    // Observe the ATC traffic for the radio transcript overlay.
    impl_->radio_log.attach(*impl_->sim);

    // Load terrain (Path B1). The scenario's terrain_json_path points at
    // a korea.terrain.json (produced by f4-terrain-convert). When present,
    // we load it, register a TerrainDataAdapter with the sim (so the FM's
    // ground clamp follows real Korea elevation), and flag for mesh
    // building (deferred to run() when the GL context exists). When
    // absent, the sim falls back to FlatTerrainSource (pre-terrain behavior).
    if (!impl_->scenario.terrain_json_path.empty()) {
        try {
            impl_->terrain.load_terrain_json(impl_->scenario.terrain_json_path);
            impl_->terrain_loaded = true;
            impl_->sim->set_terrain_source(&impl_->terrain_adapter);
            impl_->status_msg = "Terrain loaded: " +
                std::to_string(impl_->terrain.header.width) + "x" +
                std::to_string(impl_->terrain.header.height) + " grid";
        } catch (const std::exception& e) {
            // Non-fatal — the sim works without terrain (flat ground).
            impl_->status_msg = std::string("Terrain load failed: ") + e.what();
        }
    }

    // Build the airfield geometry from the SIMULATION's scenario — after
    // initialize() this is the DERIVED copy (real airfield from
    // airbase_source: true runway/taxi/parking layout, resolved parking).
    impl_->airfield = build_airfield_overlays(impl_->sim->scenario());
    impl_->airport_built = true;

    // Set the initial camera target to the parking spot so the user
    // opens the window already looking at the F-16.
    impl_->reset_camera();

    // Status message for the HUD.
    impl_->status_msg = "Scenario '" + impl_->scenario.name + "' loaded.";
}

// ── schedule_screenshot ────────────────────────────────────────────────────
void PlayerApp::schedule_screenshot(float delay_sec,
                                    const std::filesystem::path& path) {
    impl_->screenshot_pending = true;
    // screenshot_at is set in run() once the GL context is alive.
    impl_->screenshot_at = static_cast<double>(delay_sec);
    impl_->screenshot_path = path;
}

void PlayerApp::set_paused(bool paused) noexcept {
    impl_->paused = paused;
}

void PlayerApp::set_time_scale(double scale) noexcept {
    // Clamp to [0.1, 4.0]: see the comment at the slider below for the
    // FCS-stability rationale. Values above 4x drive the FM's minor-frame
    // step past the discrete-filter stability margin.
    if (scale > 0.0) {
        impl_->time_scale = std::clamp(scale, 0.1, 4.0);
    }
}

void PlayerApp::set_follow_camera(bool follow) noexcept {
    impl_->follow_aircraft = follow;
}

void PlayerApp::set_camera_distance(double dist_ft) noexcept {
    impl_->camera_distance_override = dist_ft;
}

// ── run ────────────────────────────────────────────────────────────────────
void PlayerApp::run() {
    if (!impl_->sim_initialized) {
        throw std::runtime_error(
            "PlayerApp::run: no scenario loaded — call load_scenario() first");
    }

    SetConfigFlags(FLAG_WINDOW_RESIZABLE | FLAG_VSYNC_HINT);
    InitWindow(impl_->window_w, impl_->window_h, "F4 Scenario Player");
    SetTargetFPS(60);
    rlImGuiSetup(true);

    // Build the aircraft meshes now that we have a GL context (UploadMesh
    // requires it). If we built them in load_scenario, they'd fail.
    impl_->build_aircraft_meshes();

    // Build the terrain mesh (Path B1). Deferred from load_scenario()
    // because UploadMesh requires the GL context. The mesh is centered on
    // the airfield (layout_center for real airbases, parking spot for
    // hand-authored scenarios) and spans 2*extent_ft in each direction.
    if (impl_->terrain_loaded && !impl_->terrain_mesh_built) {
        f4::renderer::TerrainMeshConfig tc;
        if (impl_->scenario.has_airbase_source) {
            tc.center_east_ft = static_cast<float>(impl_->scenario.layout_center.x);
            tc.center_north_ft = static_cast<float>(impl_->scenario.layout_center.y);
        } else if (!impl_->scenario.aircraft.empty()) {
            tc.center_east_ft = static_cast<float>(impl_->scenario.aircraft.front().parking_spot.x);
            tc.center_north_ft = static_cast<float>(impl_->scenario.aircraft.front().parking_spot.y);
        }
        tc.extent_ft = 100000.0f;  // ~19 nm half-extent (38 nm square)
        tc.resolution = 128;       // 16641 vertices, 16384 triangles
        tc.vertical_scale = 1.0f;
        tc.z_offset_ft = -5.0f;    // sink below airfield geometry to avoid z-fight
        tc.color_by_tile_type = true;
        impl_->terrain_mesh = f4::renderer::build_terrain_mesh(impl_->terrain, tc);
        impl_->terrain_mesh_built = true;
    }

    // Reset the camera to look at the parking spot.
    if (!impl_->initial_camera_set) {
        impl_->reset_camera();
    }
    if (impl_->camera_distance_override > 0.0) {
        impl_->orbit_cam.set_distance(
            static_cast<float>(impl_->camera_distance_override));
        impl_->orbit_cam.update_from_orbit();
    }

    // Re-base the screenshot time to window time.
    if (impl_->screenshot_pending) {
        impl_->screenshot_at = GetTime() + impl_->screenshot_at;
    }

    double last_frame_time = GetTime();
    bool exit_after_screenshot = false;

    while (!WindowShouldClose() && !impl_->should_exit) {
        // Window resize
        const int new_w = GetScreenWidth();
        const int new_h = GetScreenHeight();
        if (new_w != impl_->window_w || new_h != impl_->window_h) {
            impl_->window_w = new_w;
            impl_->window_h = new_h;
        }

        // Frame time (clamped)
        const double now = GetTime();
        double dt = now - last_frame_time;
        last_frame_time = now;
        if (dt > 1.0 / 15.0) dt = 1.0 / 15.0;
        if (dt < 0.0) dt = 0.0;

        // Input
        impl_->handle_camera_input();

        // F2 = manual screenshot
        if (IsKeyPressed(KEY_F2)) {
            const std::string path = "f4_scenario_player_screenshot.png";
            TakeScreenshot(path.c_str());
            impl_->status_msg = "Saved: " + path;
        }

        // F3 = toggle FCS-internals HUD column (Phase 0a observability).
        // Shows pstick/rstick/ypedal/throttle/speedBrake + FCS intermediates
        // (aoacmd, pscmd, pstab, pitchIntegral, nzcgs, body rates p/q/r).
        // Critical for diagnosing roll flutter and altitude phugoid live,
        // without having to instrument and rerun.
        if (IsKeyPressed(KEY_F3)) {
            impl_->show_fcs_hud = !impl_->show_fcs_hud;
        }

        // Scheduled screenshot
        if (impl_->screenshot_pending && GetTime() >= impl_->screenshot_at) {
            TakeScreenshot(impl_->screenshot_path.string().c_str());
            impl_->status_msg = "Saved: " + impl_->screenshot_path.string();
            impl_->screenshot_pending = false;
            exit_after_screenshot = true;
        }

        // Tick the simulation if not paused.
        if (!impl_->paused && dt > 0.0) {
            impl_->sim->tick(impl_->scenario.sim_dt * impl_->time_scale);
        }

        // Draw
        BeginDrawing();
        impl_->draw_scene();

        // ImGui demo bar (minimal — just a status line + pause button)
        rlImGuiBegin();
        ImGui::Begin("Scenario Player", nullptr,
                     ImGuiWindowFlags_AlwaysAutoResize);
        ImGui::Text("Scenario: %s", impl_->scenario.name.c_str());
        ImGui::Separator();
        if (ImGui::Button(impl_->paused ? "Resume (Space)" : "Pause (Space)")) {
            impl_->paused = !impl_->paused;
        }
        ImGui::SameLine();
        if (ImGui::Button("Reset View (R)")) impl_->reset_camera();
        ImGui::SameLine();
        if (ImGui::Button("Focus Aircraft (F)")) impl_->fit_to_aircraft();
        ImGui::Checkbox("Follow aircraft (C)", &impl_->follow_aircraft);
        ImGui::Separator();
        // Sim speed: the tick scales by this multiplier (0.1x taxi
        // inspection .. 4x fast-forward through the enroute legs).
        //
        // Capped at 4x: the FCS PI + lead-lag filters were tuned for a
        // 1/360 s minor step (6 sub-steps of 1/60 s major). At 16x the
        // minor step is effectively 1/22.5 s, past the stability margin
        // of the FCS's discrete filters — the closed loop develops a
        // high-frequency oscillation that the integrator cannot damp.
        // See FLIGHT_CONTROL_STABILITY_PLAN.md §4.2 RC-2.
        float speed = static_cast<float>(impl_->time_scale);
        if (ImGui::SliderFloat("Sim speed", &speed, 0.1f, 4.0f, "%.1fx",
                               ImGuiSliderFlags_Logarithmic)) {
            impl_->time_scale = speed;
        }
        ImGui::Separator();
        ImGui::Checkbox("Show airport", &impl_->show_airport);
        ImGui::Checkbox("Show aircraft", &impl_->show_aircraft);
        if (impl_->terrain_loaded) {
            ImGui::Checkbox("Show terrain", &impl_->show_terrain);
        }
        ImGui::Checkbox("Show taxi route", &impl_->show_taxi_route);
        ImGui::Checkbox("Show flight plan", &impl_->show_flightplan);
        ImGui::Checkbox("Show approach", &impl_->show_approach);
        ImGui::Checkbox("Show taxi-in route", &impl_->show_taxi_in);
        ImGui::Checkbox("Show radio log", &impl_->show_radio);
        ImGui::Checkbox("Show compass", &impl_->show_compass);
        ImGui::Checkbox("Show grid", &impl_->show_grid);
        ImGui::Checkbox("Show axes", &impl_->show_axes);
        ImGui::Checkbox("Show HUD", &impl_->show_hud);
        if (!impl_->status_msg.empty()) {
            ImGui::Separator();
            ImGui::TextWrapped("%s", impl_->status_msg.c_str());
        }
        ImGui::End();
        rlImGuiEnd();

        EndDrawing();

        if (exit_after_screenshot) {
            // Give the screenshot one more frame to flush, then exit.
            break;
        }
    }

    rlImGuiShutdown();
    impl_->unload_meshes();
    // Free the terrain mesh GPU resources (Path B1).
    if (impl_->terrain_mesh_built) {
        f4::renderer::unload_terrain_mesh(impl_->terrain_mesh);
        impl_->terrain_mesh_built = false;
    }
    // LitShader destructor handles UnloadShader automatically.
    CloseWindow();

    // Write the flight recording if the scenario enabled it.
    if (impl_->sim) {
        impl_->sim->write_recording();
    }
}

} // namespace f4::scenario_player
