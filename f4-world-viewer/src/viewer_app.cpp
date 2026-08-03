// f4-world-viewer/src/viewer_app.cpp
//
// ViewerApp lifecycle (ctor/dtor/run) + the two tiny state-mutators
// (set_initial_camera, schedule_screenshot). This is the slim remnant
// of the original 1920-LoC god-file after the architecture-review split
// (item #5) into:
//   viewer_state.hpp    — Impl struct + color helpers
//   icons.cpp           — icon-table + draw_icon + icon_for_*
//   camera.cpp          — world<->screen transforms + fit_to_world
//   file_ops.cpp        — load_*_json / import_*
//   install_flow.cpp    — set_install_path* / open_campaign_dialog /
//                         load_campaign_from_install / install_diagnostics_text
//                         / open_install_diagnostics / open_hex_inspector_with_file
//   diagnostics.cpp     — build_install_diagnostics / build_campaign_load_error
//                         free functions
//   canvas.cpp          — handle_input / draw_canvas
//   imgui_panels.cpp    — draw_imgui / open_file_dialog
//
// Every other .cpp in this directory includes viewer_state.hpp for the
// Impl struct definition. This file does too — for the ctor's
// restore-from-settings logic and the run() render loop.

#include "viewer_state.hpp"

#include <f4/install/installation.hpp>
#include <f4/viewer/settings.hpp>
#include <rlImGui.h>
#include <raylib.h>

#include <memory>
#include <string>

namespace f4::viewer {

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------
ViewerApp::ViewerApp()  : impl_(std::make_unique<Impl>()) {
    // Restore the last install path from persisted settings. If the user
    // has already pointed at a Falcon install, we don't make them do it
    // again on every launch — detect() runs in ~50ms, fast enough that
    // there's no perceptible startup delay.
    impl_->settings = load_settings();
    if (!impl_->settings.install_path.empty()) {
        try {
            auto inst = f4::install::Installation::detect(impl_->settings.install_path);
            if (inst.valid()) {
                impl_->install = std::move(inst);
            }
        } catch (const std::exception&) {
            // Settings file may point at a path that no longer exists.
            // Leave install as std::nullopt; the user will be prompted
            // to set a new path when they try to open a campaign.
        }
    }
}

ViewerApp::~ViewerApp() = default;

void ViewerApp::run() {
    SetConfigFlags(FLAG_WINDOW_RESIZABLE | FLAG_VSYNC_HINT);
    InitWindow(impl_->window_w, impl_->window_h, "F4 World Viewer");
    SetTargetFPS(60);
    rlImGuiSetup(true);

    // Load icon textures (falls back to drawn shapes if not found).
    impl_->load_icons();

    // Default to a fit-to-world view — UNLESS the caller already set an
    // initial camera via set_initial_camera() (e.g. via --zoom/--center
    // CLI flags). In that case, respect the user's choice.
    if (!impl_->initial_camera_set) {
        impl_->fit_to_world();
    }

    while (!WindowShouldClose()) {
        // Handle window resize
        const int new_w = GetScreenWidth();
        const int new_h = GetScreenHeight();
        if (new_w != impl_->window_w || new_h != impl_->window_h) {
            impl_->window_w = new_w;
            impl_->window_h = new_h;
        }

        handle_input();

        // F2 = screenshot (useful for headless smoke tests)
        if (IsKeyPressed(KEY_F2)) {
            const std::string path = "f4_viewer_screenshot.png";
            TakeScreenshot(path.c_str());
            impl_->status_msg = "Saved screenshot: " + path;
        }

        // Scheduled screenshot (used by schedule_screenshot — for headless tests)
        if (impl_->screenshot_pending && GetTime() >= impl_->screenshot_at) {
            TakeScreenshot(impl_->screenshot_path.c_str());
            impl_->status_msg = "Saved screenshot: " + impl_->screenshot_path;
            impl_->screenshot_pending = false;
        }

        BeginDrawing();
        ClearBackground(Color{20, 22, 28, 255});
        draw_canvas();
        draw_imgui();
        EndDrawing();
    }

    rlImGuiShutdown();
    CloseWindow();
}

void ViewerApp::schedule_screenshot(float delay_sec, const std::string& path) {
    impl_->screenshot_pending = true;
    impl_->screenshot_at = GetTime() + delay_sec;
    impl_->screenshot_path = path;
}

void ViewerApp::set_initial_camera(float center_x, float center_y, float zoom) {
    impl_->cam_x = center_x;
    impl_->cam_y = center_y;
    impl_->cam_zoom = zoom;
    impl_->initial_camera_set = true;
}

} // namespace f4::viewer
