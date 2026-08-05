// f4-models-viewer/src/viewer_app.cpp
//
// ViewerApp lifecycle (ctor/dtor/run) + the small public-API mutators
// (set_install_path, load_model, select_parent, select_lod,
// set_initial_camera, schedule_screenshot). Delegates rendering to
// the other .cpp files.

#include "viewer_state.hpp"
#include "camera3d.hpp"
#include "canvas3d.hpp"
#include "file_ops.hpp"
#include "imgui_panels.hpp"
#include "scene.hpp"

#include <f4/install/installation.hpp>
#include <rlImGui.h>
#include <raylib.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <thread>

namespace f4::models_viewer {

// ── ctor ───────────────────────────────────────────────────────────────────
ViewerApp::ViewerApp() : impl_(std::make_unique<Impl>()) {
    // Set default camera
    impl_->update_camera_from_orbit();
}

// ── dtor ───────────────────────────────────────────────────────────────────
ViewerApp::~ViewerApp() {
    // Free GPU meshes only if the GL context still exists
    if (!impl_->raylib_meshes.empty() && IsWindowReady()) {
        impl_->unload_meshes();
    }
}

// ── run ────────────────────────────────────────────────────────────────────
void ViewerApp::run() {
    SetConfigFlags(FLAG_WINDOW_RESIZABLE | FLAG_VSYNC_HINT);
    InitWindow(impl_->window_w, impl_->window_h, "F4 Model Viewer");
    SetTargetFPS(60);
    rlImGuiSetup(true);

    // If no doc loaded and no initial camera, reset to default orbit view
    if (!impl_->initial_camera_set) {
        impl_->reset_camera();
    }

    // If a screenshot was scheduled before run(), re-base it to window time
    if (impl_->screenshot_pending) {
        impl_->screenshot_at = GetTime() + 1.5;
    }

    while (!WindowShouldClose() && !impl_->should_exit) {
        // Window resize
        const int new_w = GetScreenWidth();
        const int new_h = GetScreenHeight();
        if (new_w != impl_->window_w || new_h != impl_->window_h) {
            impl_->window_w = new_w;
            impl_->window_h = new_h;
        }

        // Input
        impl_->handle_camera_input();

        // F2 = screenshot
        if (IsKeyPressed(KEY_F2)) {
            const std::string path = "f4_model_viewer_screenshot.png";
            TakeScreenshot(path.c_str());
            impl_->status_msg = "Saved: " + path;
        }

        // Scheduled screenshot
        if (impl_->screenshot_pending && GetTime() >= impl_->screenshot_at) {
            TakeScreenshot(impl_->screenshot_path.string().c_str());
            impl_->status_msg = "Saved: " + impl_->screenshot_path.string();
            impl_->screenshot_pending = false;
        }

        // Draw
        BeginDrawing();
        ClearBackground({30, 30, 30, 255});
        impl_->draw_canvas();
        impl_->draw_imgui();
        EndDrawing();
    }

    rlImGuiShutdown();
    impl_->unload_meshes();
    CloseWindow();
}

// ── set_install_path ───────────────────────────────────────────────────────
bool ViewerApp::set_install_path(const std::filesystem::path& path) {
    auto inst = f4::install::Installation::detect(path);
    if (inst.valid()) {
        impl_->install = std::move(inst);
        impl_->load_from_install();
        return true;
    }
    return false;
}

// ── installation ───────────────────────────────────────────────────────────
const std::optional<f4::install::Installation>&
ViewerApp::installation() const noexcept {
    return impl_->install;
}

// ── load_model ─────────────────────────────────────────────────────────────
void ViewerApp::load_model(const std::filesystem::path& hdr_path,
                            const std::filesystem::path& lod_path) {
    impl_->load_model_files(hdr_path, lod_path);
}

// ── select_parent ──────────────────────────────────────────────────────────
void ViewerApp::select_parent(int index) {
    impl_->selected_parent = index;
    impl_->selected_lod = 0;
    impl_->meshes_dirty = true;
    impl_->model_list_scroll_to = index;

    if (impl_->doc_loaded && index >= 0) {
        const auto* rec = impl_->db.model(index);
        if (rec) {
            impl_->model_state = {};
            for (int d = 0; d < rec->effective_dofs(); ++d) {
                f4::models::DofState ds;
                ds.dof_number = d;
                ds.value = 0;
                ds.min = 0;
                ds.max = 6.28318530718f;
                impl_->model_state.dofs.push_back(ds);
            }
            for (int s = 0; s < rec->effective_switches(); ++s) {
                f4::models::SwitchState ss;
                ss.switch_number = s;
                ss.active_child = 0;
                ss.n_children = 2;
                impl_->model_state.switches.push_back(ss);
            }
        }
        impl_->fit_to_model();
    }
}

// ── select_lod ─────────────────────────────────────────────────────────────
void ViewerApp::select_lod(int lod_index) {
    impl_->selected_lod = lod_index;
    impl_->meshes_dirty = true;
}

// ── set_initial_camera ─────────────────────────────────────────────────────
void ViewerApp::set_initial_camera(const float eye[3], const float target[3]) {
    impl_->camera.position = {eye[0], eye[1], eye[2]};
    impl_->camera.target = {target[0], target[1], target[2]};
    impl_->camera.up = {0, 1, 0};
    impl_->camera.fovy = 45.0f;
    impl_->camera.projection = CAMERA_PERSPECTIVE;
    impl_->cam_target = impl_->camera.target;
    impl_->initial_camera_set = true;
}

// ── schedule_screenshot ────────────────────────────────────────────────────
void ViewerApp::schedule_screenshot(float delay_sec,
                                     const std::filesystem::path& path) {
    impl_->screenshot_pending = true;
    impl_->screenshot_at = GetTime() + delay_sec;
    impl_->screenshot_path = path;
}

} // namespace f4::models_viewer
