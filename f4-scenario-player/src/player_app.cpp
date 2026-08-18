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

    // Observe the ATC traffic for the radio transcript overlay.
    impl_->radio_log.attach(*impl_->sim);

    // Build the airport geometry from the SIMULATION's scenario — after
    // initialize() this is the DERIVED copy (real airfield from
    // airbase_source: true runway/taxi/parking layout, resolved parking).
    impl_->airport = build_airport_geometry(impl_->sim->scenario());
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
    if (scale > 0.0) impl_->time_scale = scale;
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
        // inspection .. 16x fast-forward through the enroute legs).
        float speed = static_cast<float>(impl_->time_scale);
        if (ImGui::SliderFloat("Sim speed", &speed, 0.1f, 16.0f, "%.1fx",
                               ImGuiSliderFlags_Logarithmic)) {
            impl_->time_scale = speed;
        }
        ImGui::Separator();
        ImGui::Checkbox("Show airport", &impl_->show_airport);
        ImGui::Checkbox("Show aircraft", &impl_->show_aircraft);
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
    // LitShader destructor handles UnloadShader automatically.
    CloseWindow();

    // Write the flight recording if the scenario enabled it.
    if (impl_->sim) {
        impl_->sim->write_recording();
    }
}

} // namespace f4::scenario_player
