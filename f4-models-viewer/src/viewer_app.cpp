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
#include <cmath>
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
    if (IsWindowReady()) {
        if (!impl_->raylib_meshes.empty()) {
            impl_->unload_meshes();
        }
        if (impl_->colorbank_texture.id != 0) {
            impl_->unload_colorbank_texture();
        }
        if (impl_->lit_shader.id != 0) {
            UnloadShader(impl_->lit_shader);
            impl_->lit_shader = {};
        }
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

    float last_frame_time = static_cast<float>(GetTime());

    while (!WindowShouldClose() && !impl_->should_exit) {
        // Window resize
        const int new_w = GetScreenWidth();
        const int new_h = GetScreenHeight();
        if (new_w != impl_->window_w || new_h != impl_->window_h) {
            impl_->window_w = new_w;
            impl_->window_h = new_h;
        }

        // Frame time (clamped to 1/15s to avoid huge jumps on stalls)
        const float now = static_cast<float>(GetTime());
        float dt = now - last_frame_time;
        last_frame_time = now;
        if (dt > 1.0f / 15.0f) dt = 1.0f / 15.0f;
        if (dt < 0.0f) dt = 0.0f;

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

        // Animation tick: advances enabled DOF tracks and marks the
        // mesh cache dirty so the geometry is rebuilt with new DOF values.
        if (dt > 0.0f) {
            impl_->tick_animation(dt);
        }

        // Draw
        BeginDrawing();
        // Use a brighter, neutral background so vertex-colored meshes show up
        // clearly. The previous dark-grey background (30,30,30) was visually
        // indistinguishable from many of the garbage colors produced by the
        // old "treat ColorBank index as packed ABGR" bug. A medium grey gives
        // good contrast against both light and dark vertex colors.
        ClearBackground({100, 100, 110, 255});
        impl_->draw_canvas();
        impl_->draw_imgui();
        EndDrawing();
    }

    rlImGuiShutdown();
    impl_->unload_meshes();
    if (impl_->colorbank_texture.id != 0) {
        impl_->unload_colorbank_texture();
    }
    if (impl_->lit_shader.id != 0) {
        UnloadShader(impl_->lit_shader);
        impl_->lit_shader = {};
        impl_->lit_shader_loaded = false;
    }
    CloseWindow();
}

// ── tick_animation ─────────────────────────────────────────────────────────
// Advance enabled animation tracks. Each track writes its computed value
// into the corresponding DofState, then marks meshes_dirty so the next
// draw_canvas() rebuilds the geometry with the new DOF value.
//
// Two modes per track:
//   wrap_2pi == true : value = phase (mod 2π)   — typical rotor spin
//   wrap_2pi == false: ping-pong between min and max over a half-cycle
void ViewerApp::Impl::tick_animation(float dt) {
    if (animation_paused) return;
    bool any_enabled = false;
    for (const auto& t : animations) {
        if (t.enabled && t.dof_number >= 0) { any_enabled = true; break; }
    }
    if (!any_enabled) return;

    bool changed = false;
    for (auto& t : animations) {
        if (!t.enabled || t.dof_number < 0) continue;

        // Find the matching DofState
        f4::models::DofState* ds = nullptr;
        for (auto& d : model_state.dofs) {
            if (d.dof_number == t.dof_number) { ds = &d; break; }
        }
        if (!ds) continue;

        const float range = t.wrap_2pi ? 6.28318530718f : (ds->max - ds->min);
        if (range <= 0.0f) continue;

        // Advance phase by 2π × speed × dt (one full revolution per second
        // at speed=1 in wrap mode, or one full min→max→min cycle in
        // ping-pong mode).
        t.phase += 6.28318530718f * t.speed * dt;

        if (t.wrap_2pi) {
            // Wrap into [0, 2π)
            while (t.phase >= 6.28318530718f) t.phase -= 6.28318530718f;
            while (t.phase < 0.0f) t.phase += 6.28318530718f;
            ds->value = t.phase;
        } else {
            // Ping-pong: triangle wave between min and max
            const float period = 6.28318530718f;
            float p = t.phase;
            while (p >= period) p -= period;
            // Map [0, 2π) → [0, 2] → fold to [0, 1]
            float frac = p / period;            // 0..1
            frac = frac * 2.0f;                 // 0..2
            if (frac > 1.0f) frac = 2.0f - frac; // 1..0
            ds->value = ds->min + frac * range;
        }
        changed = true;
    }
    if (changed) meshes_dirty = true;
}

// ── reset_animations ────────────────────────────────────────────────────────
void ViewerApp::Impl::reset_animations() {
    for (auto& t : animations) {
        t.phase = 0.0f;
    }
    for (auto& d : model_state.dofs) {
        d.value = 0.0f;
    }
    meshes_dirty = true;
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
            // Build a matching animation track vector. By convention the
            // first DOF is usually the rotor for aircraft, so default its
            // speed to 8 Hz (typical helicopter rotor RPM range) — the user
            // can disable or adjust it in the Animation panel.
            impl_->animations.clear();
            impl_->animations.reserve(rec->effective_dofs());
            for (int d = 0; d < rec->effective_dofs(); ++d) {
                Impl::AnimationTrack t;
                t.dof_number = d;
                t.enabled = (d == 0) && (rec->effective_dofs() >= 1);
                t.speed = (d == 0) ? 8.0f : 1.0f;
                t.phase = 0.0f;
                t.wrap_2pi = true;
                impl_->animations.push_back(t);
            }
        }
        impl_->fit_to_model();
    } else {
        impl_->animations.clear();
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
