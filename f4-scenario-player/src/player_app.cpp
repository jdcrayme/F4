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
#include <f4/assets/asset_root.hpp>   // Data/ discovery for the glTF models

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

    // Tranche 0d: the model source is the glTF export tree
    // (Data/Models/koreaobj — RuntimeModelCache), NOT the KoreaObj
    // binary. Locate Data/ from the scenario's data_dir (Task 58) or by
    // AssetRoot discovery; the render path builds meshes lazily per
    // vis_type once the GL context exists.
    {
        std::filesystem::path data_dir = impl_->scenario.data_dir;
        if (data_dir.empty()) {
            if (auto root = f4::assets::AssetRoot::discover()) {
                data_dir = root->data_dir();
            }
        }
        if (!data_dir.empty() &&
            std::filesystem::exists(data_dir / "Models" / "koreaobj")) {
            impl_->render_res.set_model_data_dir(data_dir);
        } else {
            std::fprintf(stderr,
                "PlayerApp: no glTF models under Data/Models/koreaobj — "
                "aircraft will render without meshes. Run `f4import models "
                "--install <root> --data Data --all` to export them.\n");
        }
    }

    // Adopt the DERIVED scenario (initialize() resolves airbase_source:
    // real runway/taxi/parking layout, runway-frame waypoints rotated to
    // world, parking:"auto" assigned a real spot). The player's pre-init
    // copy still has the unresolved authoring values — e.g. parking:"auto"
    // reads as the origin, which would point the camera ~700k ft away
    // from a grid-referenced airbase.
    impl_->scenario = impl_->sim->scenario();

    // Observe the ATC traffic for the radio transcript overlay.
    impl_->radio_log.attach(*impl_->sim);

    // Observe the combat traffic for the COMBAT brevity panel (M4
    // observability). Attach runs for every scenario — the callsign map
    // is useful even without combat, and the bus stays silent when
    // combat is disabled.
    impl_->combat_log.attach(*impl_->sim);

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

    // Textured theater data (Phase 2): the shared WorldView loads the
    // raw post levels + tile databases from scenario.theater_dir.
    // Non-fatal on any failure — the viewer falls back to the
    // terrain-JSON mesh (vertex colors).
    if (!impl_->scenario.theater_dir.empty()) {
        try {
            impl_->theater_tiles_loaded =
                impl_->world.load_theater(impl_->scenario.theater_dir);
            if (!impl_->theater_tiles_loaded) {
                impl_->status_msg =
                    "Theater tiles incomplete — using untextured terrain";
            }
        } catch (const std::exception& e) {
            impl_->theater_tiles_loaded = false;
            impl_->status_msg = std::string("Theater tile load failed: ") + e.what();
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
    // Clamp to [0.1, 10.0]. The slider scales the WALL-CLOCK time fed
    // into the fixed-timestep accumulator — never the per-tick dt — so
    // FCS filter stability no longer depends on it. (The old 4x cap
    // existed because the slider used to inflate dt directly, pushing
    // the FM's minor step past the discrete filters' stability margin;
    // see FLIGHT_CONTROL_STABILITY_PLAN.md §4.2 RC-2.) The practical
    // limit now is CPU: at 10x/60 FPS the loop runs 10 sim ticks per
    // frame, each with the FM's tuned 1/360 s minor step, bounded by
    // kMaxSimStepsPerFrame in run().
    if (scale > 0.0) {
        impl_->time_scale = std::clamp(scale, 0.1, 10.0);
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

    // Terrain (Path B1 + Phase 2 textured path). Deferred from
    // load_scenario() because UploadMesh requires the GL context. The
    // shared WorldView builds the textured chunk set (post levels +
    // tile art) when the theater data is usable; the MEA heightmap mesh
    // remains the fallback for JSON-only terrain.
    if (impl_->terrain_loaded && !impl_->terrain_mesh_built) {
        float center_e = 0.0f, center_n = 0.0f;
        if (impl_->scenario.has_airbase_source) {
            center_e = static_cast<float>(impl_->scenario.layout_center.x);
            center_n = static_cast<float>(impl_->scenario.layout_center.y);
        } else if (!impl_->scenario.aircraft.empty()) {
            center_e = static_cast<float>(impl_->scenario.aircraft.front().parking_spot.x);
            center_n = static_cast<float>(impl_->scenario.aircraft.front().parking_spot.y);
        }

        bool built_textured = false;
        if (impl_->theater_tiles_loaded && impl_->world.ensure_gpu()) {
            built_textured = impl_->world.set_view(
                impl_->terrain, center_e, center_n,
                /*extent_ft=*/250000.0f,      // far ring reaches the horizon
                /*near_extent_ft=*/60000.0f,  // near tiles around the airbase
                /*z_offset_ft=*/-5.0f);       // sink below airfield geometry
            if (const auto* cs = impl_->world.chunk_set()) {
                std::fprintf(stderr,
                    "terrain: textured chunks n=%d near_quads=%d far_quads=%d "
                    "untextured=%d tile_layers=%d\n",
                    cs->chunks_total, cs->near_quads, cs->far_quads,
                    cs->quads_untextured,
                    impl_->world.tile_cache().total_layers());
            }
        }
        impl_->terrain_mesh_built = true;   // either path — suppress re-entry

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
            impl_->terrain_mesh = f4::renderer::build_terrain_mesh(impl_->terrain, tc);
        }
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

        // Fixed-timestep tick loop ("Fix Your Timestep" accumulator).
        //
        // The accumulator collects WALL-CLOCK seconds scaled by the
        // speed slider and is drained in whole scenario.sim_dt ticks —
        // dt is ALWAYS scenario.sim_dt, so the flight model's minor
        // step stays at its tuned 1/360 s (6 sub-steps of the 1/60 s
        // major) and the FCS's discrete filters run at their designed
        // operating point at every speed. The old code ticked ONCE per
        // frame with sim_dt*time_scale, which (a) moved every discrete
        // filter off its tuned discretization as the slider moved (the
        // reason for the old 4x cap — FLIGHT_CONTROL_STABILITY_PLAN.md
        // §4.2 RC-2), (b) made real-time pacing depend on the frame
        // rate ("1.0x = real time" silently required 60 FPS), and (c)
        // changed the trajectory itself with the slider, so recorded
        // traces were never comparable across speeds.
        //
        // Now the tick sequence — hence the trajectory, the flight
        // recording, and the FCS CSV trace — is identical at any slider
        // setting and matches the headless harnesses (sim.tick(1/60)
        // loops), and 1.0x is true real time at any frame rate.
        if (!impl_->paused && dt > 0.0) {
            impl_->sim_accumulator += dt * impl_->time_scale;

            // Spiral-of-death guard: after a stall (window drag,
            // breakpoint) the clamped 1/15 s frame dt at 10x asks for
            // up to 40 ticks in one frame; cap the burst and drop the
            // remainder rather than freezing the render loop. 30 steps
            // covers 10x at 30 FPS (20 ticks) with headroom.
            constexpr int kMaxSimStepsPerFrame = 30;
            int steps = 0;
            while (impl_->sim_accumulator >= impl_->scenario.sim_dt &&
                   steps < kMaxSimStepsPerFrame) {
                impl_->sim->tick(impl_->scenario.sim_dt);
                impl_->sim_accumulator -= impl_->scenario.sim_dt;
                ++steps;
            }
            if (steps == kMaxSimStepsPerFrame) {
                impl_->sim_accumulator = 0.0;  // drop the debt, stay live
            }
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
        // Watched aircraft — which jet the HUD / follow camera / F-focus
        // track. bvr_intercept flies two fighters; Tab cycles.
        if (impl_->scenario.aircraft.size() > 1) {
            const std::size_t wi = impl_->watched_index <
                impl_->scenario.aircraft.size() ? impl_->watched_index : 0;
            if (ImGui::BeginCombo("Watched",
                    impl_->scenario.aircraft[wi].callsign.c_str())) {
                for (std::size_t i = 0;
                     i < impl_->scenario.aircraft.size(); ++i) {
                    const bool selected = (i == impl_->watched_index);
                    if (ImGui::Selectable(
                            impl_->scenario.aircraft[i].callsign.c_str(),
                            selected)) {
                        impl_->watched_index = i;
                    }
                    if (selected) ImGui::SetItemDefaultFocus();
                }
                ImGui::EndCombo();
            }
        }
        ImGui::Separator();
        // Sim speed: scales the WALL-CLOCK time fed into the
        // fixed-timestep accumulator (0.1x taxi inspection .. 10x
        // fast-forward). The tick dt is always scenario.sim_dt, so the
        // FM's minor step stays at its tuned 1/360 s and the FCS PI +
        // lead-lag filters run at their designed operating point at
        // every speed — no stability cap needed. (The old 4x cap
        // existed because the slider used to inflate dt directly,
        // pushing the minor step past the filters' stability margin;
        // see FLIGHT_CONTROL_STABILITY_PLAN.md §4.2 RC-2.) The limit
        // now is CPU: 10x = 10 ticks x 6 minor steps per frame at
        // 60 FPS, bounded by kMaxSimStepsPerFrame above.
        float speed = static_cast<float>(impl_->time_scale);
        if (ImGui::SliderFloat("Sim speed", &speed, 0.1f, 10.0f, "%.1fx",
                               ImGuiSliderFlags_Logarithmic)) {
            impl_->time_scale = speed;
            // Trace metadata only (see Simulation::set_trace_time_scale).
            if (impl_->sim) impl_->sim->set_trace_time_scale(speed);
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
        if (impl_->scenario.combat.enabled) {
            ImGui::Checkbox("Show combat view", &impl_->show_combat);
        }
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
    // Free the textured-theater GPU resources (chunk meshes, tile
    // arrays, terrain shader) — one call for the whole shared path.
    impl_->world.unload();
    // LitShader destructor handles UnloadShader automatically.
    CloseWindow();

    // Write the flight recording if the scenario enabled it.
    if (impl_->sim) {
        impl_->sim->write_recording();
    }
}

} // namespace f4::scenario_player
