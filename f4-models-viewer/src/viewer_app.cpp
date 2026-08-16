// f4-models-viewer/src/viewer_app.cpp
//
// ViewerApp lifecycle (ctor/dtor/run) + the small public-API mutators
// (set_install_path, load_model, select_parent, select_lod,
// set_initial_camera, schedule_screenshot). Delegates rendering to
// the other .cpp files.

#include "viewer_state.hpp"

#include <f4/install/installation.hpp>
#include <f4/math/constants.hpp>
#include <f4/math/vec3.hpp>
#include <rlImGui.h>
#include <raylib.h>

#include <algorithm>
#include <cmath>
#include <filesystem>

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
        // LitShader is RAII; its destructor handles GPU cleanup.
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

        // F2 = screenshot (default filename)
        if (IsKeyPressed(KEY_F2)) {
            impl_->take_screenshot("f4_model_viewer_screenshot.png");
        }

        // Scheduled screenshot (set via --screenshot CLI flag or
        // Save Screenshot... menu item). Fires once, then optionally
        // exits the run loop if screenshot_exit_after is set (used by
        // the --screenshot smoke-test path, which used to rely on a
        // detached std::thread calling std::exit(0) — fragile and
        // skipped ViewerApp dtor).
        if (impl_->screenshot_pending && GetTime() >= impl_->screenshot_at) {
            impl_->take_screenshot(impl_->screenshot_path);
            impl_->screenshot_pending = false;
            if (impl_->screenshot_exit_after) {
                impl_->should_exit = true;
            }
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
    // LitShader destructor handles GPU cleanup automatically.
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

// ── take_screenshot ────────────────────────────────────────────────────────
// Single screenshot implementation used by both the F2 hotkey and the
// scheduled-screenshot path. Writes the PNG via Raylib's TakeScreenshot
// and surfaces the result in status_msg.
void ViewerApp::Impl::take_screenshot(const std::filesystem::path& path) {
    TakeScreenshot(path.string().c_str());
    status_msg = "Saved: " + path.string();
}

// ── select_parent_internal ─────────────────────────────────────────────────
// Shared implementation for the three sites that select a parent model:
//   - ViewerApp::select_parent()      (public API, called from CLI/tests)
//   - Impl::load_model_files()         (auto-select first model after load)
//   - draw_model_browser() Selectable  (user click in the model list)
//
// Initializes model_state from the model record's effective_dofs() and
// effective_switches() with conservative defaults (DOF range [0, 2π],
// switches default to "Show All" with n_children=2). The BSP-tree sync
// in scene.cpp::sync_model_state_with_bsp_tree() refines these on the
// next rebuild — removing unused DOFs/switches, fixing n_children, and
// clamping active_child.
//
// Also rebuilds the animations vector with one track per DOF. By
// convention DOF 0 is auto-enabled at 8 Hz (typical helicopter rotor);
// the user can disable/adjust in the Animation panel.
void ViewerApp::Impl::select_parent_internal(int index) {
    selected_parent = index;
    selected_lod = 0;
    meshes_dirty = true;

    if (!doc_loaded || index < 0) {
        animations.clear();
        return;
    }

    const auto* rec = db.model(index);
    if (!rec) {
        animations.clear();
        return;
    }

    model_state = {};
    for (int d = 0; d < rec->effective_dofs(); ++d) {
        f4::models::DofState ds;
        ds.dof_number = d;
        ds.value = 0;
        ds.min = 0;
        ds.max = 6.28318530718f;  // 2π
        model_state.dofs.push_back(ds);
    }
    for (int s = 0; s < rec->effective_switches(); ++s) {
        f4::models::SwitchState ss;
        ss.switch_number = s;
        // Default to "Show All" — a model viewer should display all
        // geometry by default. sync_model_state_with_bsp_tree() in
        // scene.cpp will refine n_children once the BSP tree is parsed.
        ss.active_child = -1;
        ss.n_children = 2;
        model_state.switches.push_back(ss);
    }

    animations.clear();
    animations.reserve(rec->effective_dofs());
    for (int d = 0; d < rec->effective_dofs(); ++d) {
        AnimationTrack t;
        t.dof_number = d;
        t.enabled = (d == 0);
        t.speed = (d == 0) ? 8.0f : 1.0f;
        t.phase = 0.0f;
        t.wrap_2pi = true;
        animations.push_back(t);
    }

    fit_to_model();
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
    impl_->select_parent_internal(index);
}

// ── select_lod ─────────────────────────────────────────────────────────────
void ViewerApp::select_lod(int lod_index) {
    impl_->selected_lod = lod_index;
    impl_->meshes_dirty = true;
}

// ── set_initial_camera ─────────────────────────────────────────────────────
void ViewerApp::set_initial_camera(const float eye[3], const float target[3]) {
    const Vector3 tgt = {target[0], target[1], target[2]};
    const Vector3 eye_v = {eye[0], eye[1], eye[2]};
    impl_->orbit_cam.set_target(tgt);

    // Compute orbit parameters (yaw, pitch, distance) from eye position.
    // OrbitCamera convention:
    //   position = target + distance * (cos(pitch)*sin(yaw),
    //                                    sin(pitch),
    //                                    cos(pitch)*cos(yaw))
    const Vector3 diff = {eye_v.x - tgt.x, eye_v.y - tgt.y, eye_v.z - tgt.z};
    const float dist = f4::math::Vec3f{diff.x, diff.y, diff.z}.length();
    impl_->orbit_cam.set_distance(dist > 0.001f ? dist : 100.0f);

    if (dist > 0.001f) {
        static constexpr float kRad2Deg = static_cast<float>(f4::math::RAD_TO_DEG);
        const float pitch_rad = std::asin(std::max(-1.0f, std::min(1.0f, diff.y / dist)));
        const float yaw_rad = std::atan2(diff.x, diff.z);
        impl_->orbit_cam.set_yaw(yaw_rad * kRad2Deg);
        impl_->orbit_cam.set_pitch(pitch_rad * kRad2Deg);
    }

    impl_->orbit_cam.update_from_orbit();
    impl_->initial_camera_set = true;
}

// ── schedule_screenshot ────────────────────────────────────────────────────
void ViewerApp::schedule_screenshot(float delay_sec,
                                     const std::filesystem::path& path,
                                     bool exit_after) {
    impl_->screenshot_pending = true;
    impl_->screenshot_at = GetTime() + delay_sec;
    impl_->screenshot_path = path;
    impl_->screenshot_exit_after = exit_after;
}

} // namespace f4::models_viewer
