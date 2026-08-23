// f4-world-viewer/src/viewer_app.cpp
//
// ViewerApp lifecycle (ctor/dtor/run) + the two tiny state-mutators
// (set_initial_camera, schedule_screenshot). This is the slim remnant
// of the original 1920-LoC god-file after the architecture-review split
// (item #5) into:
//   viewer_state.hpp    — Impl struct + color helpers
//   symbols.cpp         — procedural symbol drawing (replaces icons.cpp)
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
#include <f4/viewer/replay_mode.hpp>
#include <rlImGui.h>
#include <raylib.h>
#include <imgui.h>

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
                // Surface the restored install path in the status bar so
                // the user can see it's loaded (matches the --install flow
                // which sets status_msg in set_install_path()).
                impl_->status_msg = "Install: " + impl_->settings.install_path.string();
            }
        } catch (const std::exception&) {
            // Settings file may point at a path that no longer exists.
            // Leave install as std::nullopt; the user will be prompted
            // to set a new path when they try to open a campaign.
        }
    }
}

ViewerApp::~ViewerApp() {
    // POLISH-2.1: free the cached terrain RenderTexture. Only safe to
    // call UnloadRenderTexture if a GL context still exists — and at
    // dtor time, run() has already returned (which called CloseWindow).
    // Raylib's UnloadRenderTexture is a no-op (or silently safe) when
    // the texture id is 0, but calling it after CloseWindow would crash
    // on some drivers. We guard on terrain_cache.id != 0 AND check
    // IsWindowReady() — the latter returns true only between InitWindow
    // and CloseWindow.
    if (impl_->terrain_cache.id != 0 && IsWindowReady()) {
        UnloadRenderTexture(impl_->terrain_cache);
        impl_->terrain_cache = {0};
    }
    // Free the Ground Layout 3D panel's RenderTexture (same GL-context
    // constraint as above). Allocated lazily by draw_ground_layout_3d().
    if (impl_->ground_layout_3d_target.id != 0 && IsWindowReady()) {
        UnloadRenderTexture(impl_->ground_layout_3d_target);
        impl_->ground_layout_3d_target = {0};
        impl_->ground_layout_3d_target_valid = false;
    }
    // Free the KoreaObj mesh + texture caches used by the 3D panel.
    // Same GL-context constraint — only safe if IsWindowReady() is true
    // (i.e. run() has not yet returned). When run() does return, it
    // calls render_res_3d.unload_all() BEFORE CloseWindow() in its
    // shutdown path; this dtor call is a safety net for the case where
    // the viewer is destroyed without run() ever being called (CLI-only
    // usage). All caches now live on the shared RenderResources instance.
    if (IsWindowReady()) {
        impl_->render_res_3d.unload_all();
    }
}

void ViewerApp::run() {
    SetConfigFlags(FLAG_WINDOW_RESIZABLE | FLAG_VSYNC_HINT);
    InitWindow(impl_->window_w, impl_->window_h, "F4 World Viewer");
    SetTargetFPS(60);
    rlImGuiSetup(true);

    // Default to a fit-to-world view — UNLESS the caller already set an
    // initial camera via set_initial_camera() (e.g. via --zoom/--center
    // CLI flags). In that case, respect the user's choice.
    if (!impl_->initial_camera_set) {
        impl_->fit_to_world();
    }

    // If a replay was loaded before run() (via --replay CLI flag or
    // load_replay() called programmatically), fit the replay camera
    // to the trail now that the window dimensions are known.
    if (impl_->replay_needs_fit && impl_->replay.active()) {
        fit_replay_camera(impl_->replay,
                          static_cast<float>(impl_->window_w),
                          static_cast<float>(impl_->window_h),
                          impl_->replay_cam_x,
                          impl_->replay_cam_y,
                          impl_->replay_cam_zoom);
        impl_->replay_needs_fit = false;
    }

    while (!WindowShouldClose() && !impl_->should_exit) {
        // Handle window resize
        const int new_w = GetScreenWidth();
        const int new_h = GetScreenHeight();
        if (new_w != impl_->window_w || new_h != impl_->window_h) {
            impl_->window_w = new_w;
            impl_->window_h = new_h;
        }

        // Dispatch input handling. Replay mode has its own input path
        // (arrow keys for stepping, etc.); the normal canvas path
        // handles pan/zoom/select.
        if (impl_->replay.active()) {
            handle_replay_input();
        } else {
            handle_input();
        }

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

        // Phase 2: keyboard shortcuts.
        //   F = fit to world (or fit to trail in replay mode)
        //   Esc = clear selection (normal mode only — replay ignores)
        //   / = focus search box (handled in Layers panel via ImGui)
        if (IsKeyPressed(KEY_F) && !ImGui::GetIO().WantCaptureKeyboard) {
            if (!impl_->replay.active()) {
                impl_->fit_to_world();
            }
            // In replay mode, F is handled by handle_replay_input()
            // (fit_replay_to_trail) — don't double-dispatch.
        }
        if (IsKeyPressed(KEY_ESCAPE) && !ImGui::GetIO().WantCaptureKeyboard) {
            if (!impl_->replay.active()) {
                impl_->sel_kind = Impl::SelectionKind::None;
                impl_->sel_entity = f4::entities::EntityId{};
            }
        }

        BeginDrawing();
        ClearBackground(Color{20, 22, 28, 255});
        if (impl_->replay.active()) {
            draw_replay_canvas();
            // The replay panel uses ImGui, so it must be wrapped in
            // rlImGuiBegin/End — same as the normal draw_imgui() path.
            // Without this, ImGui::Begin() asserts (g.WithinFrameScope).
            rlImGuiBegin();
            draw_replay_panel();
            rlImGuiEnd();
        } else {
            draw_canvas();
            draw_imgui();
        }
        EndDrawing();
    }

    rlImGuiShutdown();
    // POLISH-2.1: free the cached terrain RenderTexture BEFORE
    // CloseWindow — once the GL context is gone, UnloadRenderTexture
    // can't free GPU memory and may crash on some drivers. The dtor
    // also checks IsWindowReady() as a safety net for the case where
    // run() never got called (CLI-only usage).
    if (impl_->terrain_cache.id != 0) {
        UnloadRenderTexture(impl_->terrain_cache);
        impl_->terrain_cache = {0};
        impl_->terrain_cache_valid = false;
    }
    // Free the Ground Layout 3D panel's RenderTexture before the GL
    // context goes away. Same rationale as above.
    if (impl_->ground_layout_3d_target.id != 0) {
        UnloadRenderTexture(impl_->ground_layout_3d_target);
        impl_->ground_layout_3d_target = {0};
        impl_->ground_layout_3d_target_valid = false;
    }
    // Free the KoreaObj mesh + texture caches used by the 3D panel.
    // MUST be called before CloseWindow — UnloadMesh / UnloadTexture /
    // UnloadShader all need the GL context. After this call, the cache
    // is empty; subsequent selections won't re-render models until the
    // user re-runs the viewer (which is fine — we're shutting down).
    // All caches now live on the shared RenderResources instance.
    impl_->render_res_3d.unload_all();
    // Free the 3D terrain mesh (Path B1) — needs the GL context.
    if (impl_->terrain_mesh_3d_built) {
        f4::renderer::unload_terrain_mesh(impl_->terrain_mesh_3d);
        impl_->terrain_mesh_3d_built = false;
    }
    // Free the Class Table Browser's preview GPU resources (RenderTexture,
    // cached meshes, textures, lit shader, default material). The browser
    // owns its own cache (separate from impl_->mesh_cache_3d) so we must
    // clean it up here too. We call close() (which is public and calls
    // cleanup_preview() internally) rather than cleanup_preview() directly
    // (which is private). Safe to call when the browser was never opened
    // (cleanup_preview is a no-op in that case).
    impl_->class_table_browser.close();
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

// ---------------------------------------------------------------------------
// Replay mode (Path B2)
// ---------------------------------------------------------------------------

bool ViewerApp::load_replay(const std::filesystem::path& trace_json,
                             std::string* err_out) {
    std::string err;
    const bool ok = f4::viewer::load_replay(impl_->replay, trace_json, &err);
    if (!ok) {
        if (err_out) *err_out = err;
        impl_->last_error = err;
        return false;
    }
    // Fit the replay camera to the trail so the whole flight is visible.
    // Done lazily on the first draw_replay_canvas() call (camera fit
    // needs window dimensions, which aren't set until run()).
    impl_->replay_cam_x = 0.0f;
    impl_->replay_cam_y = 0.0f;
    impl_->replay_cam_zoom = 0.5f;
    impl_->replay_needs_fit = true;
    impl_->status_msg = "Replay loaded: " + trace_json.string() +
                         " (" + std::to_string(impl_->replay.recording->size()) +
                         " snapshots)";
    return true;
}

bool ViewerApp::replay_active() const noexcept {
    return impl_->replay.active();
}

} // namespace f4::viewer
