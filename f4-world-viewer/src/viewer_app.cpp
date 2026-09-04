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

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif

namespace f4::viewer {

std::filesystem::path resolve_symbols_dir() {
    namespace fs = std::filesystem;
    if (const char* env = std::getenv("F4_SYMBOLS_DIR")) {
        return fs::path(env);
    }
    std::vector<fs::path> candidates;
#if defined(_WIN32)
    char buf[1024] = {};
    const DWORD n = GetModuleFileNameA(nullptr, buf, sizeof(buf));
    const fs::path exe_dir = n > 0 && n < sizeof(buf)
        ? fs::path(std::string(buf, n)).parent_path()
        : fs::current_path();
#else
    const fs::path exe_dir = fs::current_path();
#endif
    const fs::path cwd = fs::current_path();
    for (const fs::path& base : {exe_dir, cwd}) {
        candidates.push_back(base / "symbols");
        candidates.push_back(base / ".." / "symbols");
        candidates.push_back(base / ".." / ".." / "symbols");
    }
    std::error_code ec;
    for (const auto& c : candidates) {
        if (fs::exists(c, ec)) return c;
    }
    return candidates.front();  // nonexistent → every key falls back
}

namespace {

// raylib's TakeScreenshot saves to basePath + BASENAME only (rcore.c:
// "Provided fileName should not contain paths") — the --screenshot
// CLI flag's directory part was silently dropped (a /tmp/out.png
// request landed in the CWD). Take the shot, then put the file where
// the caller actually asked (copy+remove, not rename: /tmp and the
// working directory are routinely different filesystems).
bool take_screenshot_to(const std::string& requested) {
    TakeScreenshot(requested.c_str());
    namespace fs = std::filesystem;
    std::error_code ec;
    const fs::path want = fs::absolute(fs::path{requested}, ec);
    if (ec || want.empty()) return false;
    if (fs::exists(want, ec)) return true;  // raylib wrote it in place
    const fs::path landed = fs::current_path(ec) / want.filename();
    if (ec) return false;
    if (!fs::exists(landed, ec)) return false;
    std::error_code ec2;
    fs::copy_file(landed, want, fs::copy_options::overwrite_existing, ec2);
    if (ec2) return false;
    fs::remove(landed, ec2);
    return true;
}

} // namespace

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
                // If a world was restored from the command line before
                // this ran, its theater tiles couldn't load without the
                // install — pick them up now.
                impl_->try_load_theater_tiles();
            }
        } catch (const std::exception&) {
            // Settings file may point at a path that no longer exists.
            // Leave install as std::nullopt; the user will be prompted
            // to set a new path when they try to open a campaign.
        }
    }
}

ViewerApp::~ViewerApp() {
    // V-THREAD: last-resort stop for the campaign runner (run()'s exit
    // path stops it first; this covers destruction without run()).
    // Must run BEFORE Impl dies — the worker borrows the session. The
    // member order (runner declared after session) is the second line
    // of defense; this is the first.
    if (impl_->session_runner) {
        impl_->session_runner->stop();
    }
    // V-CAMP: last-resort join for a session-start worker that is still
    // running (run()'s exit path joins first; this covers destruction
    // without run() — CLI-only usage). A joinable std::thread dtor would
    // terminate() the process; the future's shared state is freed by
    // get()/dtor either way.
    if (impl_->session_start_thread.joinable()) {
        impl_->session_start_thread.join();
        impl_->session_starting = false;
    }
    // POLISH-2.1: free the cached terrain RenderTexture. Only safe to
    // call UnloadRenderTexture if a GL context still exists — and at
    // dtor time, run() has already returned (which called CloseWindow).
    // Raylib's UnloadRenderTexture is a no-op (or silently safe) when
    // the texture id is 0, but calling it after CloseWindow would crash
    // on some drivers. We guard on terrain_cache.id != 0 AND check
    // IsWindowReady() — the latter returns true only between InitWindow
    // and CloseWindow.
    if (impl_->terrain_cache.id != 0 && IsWindowReady()) {
        UnloadTexture(impl_->terrain_cache);
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

    while (!WindowShouldClose() && !impl_->should_exit &&
           !impl_->exit_requested.load()) {
        // Handle window resize
        const int new_w = GetScreenWidth();
        const int new_h = GetScreenHeight();
        if (new_w != impl_->window_w || new_h != impl_->window_h) {
            impl_->window_w = new_w;
            impl_->window_h = new_h;
        }

        // F2 = screenshot (useful for headless smoke tests)
        if (IsKeyPressed(KEY_F2)) {
            const std::string path = "f4_viewer_screenshot.png";
            const bool ok = take_screenshot_to(path);
            impl_->status_msg = ok ? "Saved screenshot: " + path
                                   : "Screenshot failed to save: " + path;
        }

        // Scheduled screenshot (used by schedule_screenshot — for headless
        // tests). HELD while a session start is in flight so a
        // --session smoke screenshot always captures the adopted state,
        // however long the (async) create took.
        if (impl_->screenshot_pending && !impl_->session_starting &&
            GetTime() >= impl_->screenshot_at) {
            // take_screenshot_to: raylib drops the directory part — the
            // helper restores it, so --screenshot /tmp/x.png REALLY
            // writes /tmp/x.png (smokes assert on the exact path).
            const bool ok =
                take_screenshot_to(impl_->screenshot_path);
            impl_->status_msg = ok
                ? "Saved screenshot: " + impl_->screenshot_path
                : "Screenshot failed to save: " + impl_->screenshot_path;
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

        // V-CAMP: adopt a finished async session start BEFORE anything
        // reads impl_->session this frame (the Space toggle below, the
        // canvas live layer, the Campaign window). Returns quickly
        // while the worker is still building. V-THREAD: this also
        // launches the campaign runner thread when a session lands —
        // before any frame scope below could lock it.
        adopt_session_start();

        // V-THREAD: the frame READ scope. The campaign runner's worker
        // advances the session on its own thread in short mutex-guarded
        // batches; this scope takes the SAME lock for the whole
        // input-read + draw phase, so every existing session read —
        // hit tests, canvas layers, the Campaign window, the inspector,
        // the threat overlay, session_handle() derefs — sees a frozen,
        // consistent session for the frame without touching any of
        // those call sites. The old inline advance() lived here; it
        // could legally run 240 ticks (seconds) inside the ImGui frame
        // and froze the UI ("not responding"). Gone: the worker owns
        // advance().
        //
        // V-THREAD-2 (the "campaign time doesn't advance" fix): the lock
        // is the runner's FairMutex (FIFO ticket order) — under the old
        // plain std::mutex this whole-frame scope STARVED the worker
        // completely (the scope held the lock ~99.9% of wall time:
        // draw + EndDrawing's pace wait, released for ~tens of µs; the
        // UI's uncontended fast-path re-lock beat the woken worker every
        // time — 0.0 sim-seconds over 3 wall-seconds, measured). Ticket
        // order guarantees the worker's queued lock is served before
        // this frame's re-lock, every frame. AND EndDrawing() now runs
        // OUTSIDE the scope below: raylib's 60 FPS pace wait doesn't
        // need the session lock — releasing it for the wait gives the
        // worker the whole pace window (multiple batches per frame at
        // high speed presets) instead of one.
        {
            std::unique_lock<f4::simulation::FairMutex> session_frame_lock;
            if (impl_->session_runner) {
                session_frame_lock =
                    std::unique_lock<f4::simulation::FairMutex>(
                        impl_->session_runner->mutex());
            }

            // Dispatch input handling INSIDE the frame scope — the
            // canvas click path hit-tests the session's live aircraft
            // (session_handle derefs), which must not race the worker.
            // Replay mode has its own input path (arrow keys for
            // stepping, etc.); it never touches the session.
            if (impl_->replay.active()) {
                handle_replay_input();
            } else {
                handle_input();
            }

            // V-CAMP: Space toggles the live campaign session's clock
            // (the Campaign window's own button mirrors it). The pause
            // contract lives in set_session_paused() — we hold the
            // frame lock here, which is exactly what it expects.
            if (IsKeyPressed(KEY_SPACE) &&
                !ImGui::GetIO().WantCaptureKeyboard &&
                impl_->session) {
                set_session_paused(!impl_->session->paused());
            }

            // V-CAMP speed presets: 1-4 pick a preset, +/- step through
            // them (mirrors the replay view's speed keys). The runner's
            // speed is an atomic — lock-free under the frame scope.
            if (impl_->session_runner &&
                !ImGui::GetIO().WantCaptureKeyboard) {
                auto apply_preset = [&](int idx) {
                    impl_->campaign_speed_index = std::clamp(
                        idx, 0, kSessionSpeedCount - 1);
                    impl_->session_runner->set_speed(
                        kSessionSpeedTable[impl_->campaign_speed_index]);
                };
                if (IsKeyPressed(KEY_ONE)) apply_preset(0);
                else if (IsKeyPressed(KEY_TWO)) apply_preset(1);
                else if (IsKeyPressed(KEY_THREE)) apply_preset(2);
                else if (IsKeyPressed(KEY_FOUR)) apply_preset(3);
                else if (IsKeyPressed(KEY_KP_ADD) ||
                         IsKeyPressed(KEY_EQUAL)) {
                    apply_preset(impl_->campaign_speed_index + 1);
                } else if (IsKeyPressed(KEY_KP_SUBTRACT) ||
                           IsKeyPressed(KEY_MINUS)) {
                    apply_preset(impl_->campaign_speed_index - 1);
                }
            }

            // V-THREAD: mirror the worker's one-frame flags (the worker,
            // not this frame, advances now — its atomic is the source).
            if (impl_->session_runner) {
                impl_->campaign_time_dilated =
                    impl_->session_runner->time_dilated();
            }

            // V-3DLIVE: the camera bubble — when the user is zoomed in
            // enough to see models, the deaggregation bubble follows
            // the CAMERA (the map viewer's "player"), scaled with the
            // visible extent: zoom into a battalion and its vehicles /
            // personnel appear, even while the session is paused (the
            // session refreshes the bubble immediately). Re-pointed
            // only when the camera actually moved (grid threshold =
            // 1/16 of the visible extent; zoom threshold = 15%) — a
            // still camera never churns deagg state.
            if (impl_->session && impl_->campaign_view_bubble &&
                !impl_->replay.active()) {
                const bool zoomed_in = impl_->cam_zoom > 4.0f;
                if (zoomed_in) {
                    const float vis_w_grid =
                        static_cast<float>(impl_->window_w) /
                        impl_->cam_zoom;
                    const float vis_h_grid =
                        static_cast<float>(impl_->window_h) /
                        impl_->cam_zoom;
                    const float moved =
                        std::max(std::abs(impl_->cam_x -
                                          impl_->last_bubble_gx),
                                 std::abs(impl_->cam_y -
                                          impl_->last_bubble_gy));
                    const bool camera_moved =
                        moved > vis_w_grid / 16.0f ||
                        std::abs(impl_->cam_zoom -
                                 impl_->last_bubble_zoom) >
                            impl_->last_bubble_zoom * 0.15f;
                    if (camera_moved ||
                        impl_->last_bubble_zoom < 0.0f) {
                        // Radius: a quarter of the visible extent, in
                        // FEET (grid = 1024 ft), clamped [2.5, 25] grid
                        // units — deaggregate what's around the view
                        // center without spawning a battalion-per-grid
                        // tile across the whole screen.
                        const double radius_ft =
                            std::clamp(0.25 * std::max(vis_w_grid, vis_h_grid),
                                       2.5, 25.0) * 1024.0;
                        impl_->session->set_view_bubble(
                            radius_ft,
                            f4::geo::WorldPosition(
                                impl_->cam_x * 1024.0,
                                impl_->cam_y * 1024.0, 0.0));
                        impl_->last_bubble_gx = impl_->cam_x;
                        impl_->last_bubble_gy = impl_->cam_y;
                        impl_->last_bubble_zoom = impl_->cam_zoom;
                    }
                } else if (impl_->last_bubble_zoom >= 0.0f) {
                    // Zoomed back out: ownship bubble (FreeFalcon's
                    // default), applied immediately. The still-camera
                    // guard (last_bubble_zoom < 0) keeps this a one-shot
                    // per zoom-out, not a per-frame churn.
                    impl_->session->clear_view_bubble();
                    impl_->last_bubble_zoom = -1.0f;
                    impl_->last_bubble_gx = -1.0e9f;
                    impl_->last_bubble_gy = -1.0e9f;
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
            // V-THREAD-2: scope ends BEFORE EndDrawing() — every raylib
            // Draw* has already copied its vertices/parameters into
            // raylib's own batch buffers (and ImGui's render pass is
            // done), so the session lock is free to release here.
            // EndDrawing's buffer swap + the 60 FPS pace wait run
            // UNLOCKED — that's ~two thirds of the frame the worker
            // can now use for advance batches.
        }  // frame read scope — the worker resumes advancing here

        EndDrawing();

        // V-THREAD: the Stop button's deferred stop — processed OUTSIDE
        // the frame lock (the join needs the worker able to finish its
        // current batch).
        process_session_stop();
    }

    rlImGuiShutdown();

    // V-THREAD: stop the campaign runner FIRST — its worker borrows the
    // session; the session must not die (below, when Impl is destroyed)
    // while the worker is mid-advance. stop() signals + joins; called
    // outside any frame lock (the loop above is done), so no deadlock.
    if (impl_->session_runner) {
        impl_->session_runner->stop();
        impl_->session_runner.reset();
    }
    // V-SMOKE: the session's final state, one line — headless
    // --session --play runs assert the campaign clock ADVANCED here
    // (the starved-worker regression printed sim 0.0s while the UI
    // looked perfectly healthy).
    if (impl_->session) {
        const auto line = session_exit_summary();
        if (!line.empty()) std::fprintf(stderr, "%s\n", line.c_str());
    }

    // A session start still running when the window closes: join it
    // BEFORE Impl dies (the worker writes only into the future's
    // shared state, but the thread object itself must be joined and
    // the std::thread dtor would terminate() on a joinable thread).
    if (impl_->session_starting) {
        if (impl_->session_start_thread.joinable()) {
            impl_->session_start_thread.join();
        }
        if (impl_->session_start_future.valid()) {
            impl_->session_start_future.get();  // discard
        }
        impl_->session_starting = false;
    }
    // POLISH-2.1: free the cached terrain map texture BEFORE
    // CloseWindow — once the GL context is gone, UnloadTexture
    // can't free GPU memory and may crash on some drivers. The dtor
    // also checks IsWindowReady() as a safety net for the case where
    // run() never got called (CLI-only usage).
    if (impl_->terrain_cache.id != 0) {
        UnloadTexture(impl_->terrain_cache);
        impl_->terrain_cache = {};
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
    // Free the 3D terrain chunk set (Path B1 chunked) — same GL-context
    // requirement. Both paths are mutually exclusive at runtime (the
    // toggle use_terrain_chunks picks one), but both may have been
    // built if the user toggled during the session — free both to be
    // safe. unload_terrain_chunk_set is a no-op when valid is false.
    if (impl_->terrain_chunk_set_3d_built) {
        f4::renderer::unload_terrain_chunk_set(impl_->terrain_chunk_set_3d);
        impl_->terrain_chunk_set_3d_built = false;
    }
    // Free the textured-theater GPU resources (WorldView: chunk meshes,
    // tile arrays, terrain shader) — same GL-context requirement.
    impl_->world.unload();
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

bool ViewerApp::select_by_name(const std::string& substring) {
    auto entities =
        impl_->eworld.with_component<f4::entities::ObjectiveTypeComponent>();
    // Two passes: prefer an objective that has ground-layout content
    // (the 3D panel has something to show), then fall back to any match.
    // Objectives are identified by their NAME tag when present (campaign
    // worlds: "Kunsan AB", ...) and by the objective-type class name
    // otherwise (fixture worlds: "Highway", ...).
    for (int pass = 0; pass < 2; ++pass) {
        for (const auto eid : entities) {
            auto h = f4::entities::EntityHandle(eid, &impl_->eworld);
            if (!h.valid()) continue;
            const auto name_tag = h.get_tag(f4::entities::tags::NAME);
            const std::string* name =
                name_tag ? name_tag->as_string() : nullptr;
            bool matched = name && name->find(substring) != std::string::npos;
            if (!matched) {
                auto* ot = h.get<f4::entities::ObjectiveTypeComponent>();
                matched = ot && ot->class_name.find(substring) !=
                                 std::string::npos;
            }
            if (!matched) continue;
            if (pass == 0) {
                auto* gl = h.get<f4::entities::GroundLayoutComponent>();
                auto* fs = h.get<f4::entities::FeatureSetComponent>();
                const bool has_content =
                    (gl && !gl->layouts.empty()) ||
                    (fs && !fs->features.empty());
                if (!has_content) continue;
            }
            impl_->sel_kind = Impl::SelectionKind::Objective;
            impl_->sel_entity = eid;
            // NOTE: deliberately NOT calling fit_to_selection_layout() —
            // the map keeps showing the whole theater (F / double-click
            // zooms to a selection on demand).
            impl_->inspector_force_3d_tab = true;
            return true;
        }
    }
    return false;
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
