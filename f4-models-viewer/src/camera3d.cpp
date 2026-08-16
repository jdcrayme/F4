// f4-models-viewer/src/camera3d.cpp
//
// Orbit camera for 3D model viewing. Delegates to
// f4::renderer::OrbitCamera for the actual math.

#include "viewer_state.hpp"

#include <imgui.h>
#include <raylib.h>

namespace f4::models_viewer {

// ── update_camera_from_orbit ───────────────────────────────────────────────
void ViewerApp::Impl::update_camera_from_orbit() {
    orbit_cam.update_from_orbit();
}

// ── handle_camera_input ────────────────────────────────────────────────────
// Process mouse/keyboard for orbit, pan, zoom, fit, reset.
// Delegates to OrbitCamera::handle_input() for orbit/pan/zoom,
// but keeps keyboard shortcuts (F=fit, R=reset, Space=pause) local
// since those are viewer-specific.
void ViewerApp::Impl::handle_camera_input() {
    const ImGuiIO& io = ImGui::GetIO();

    // ── Keyboard ──────────────────────────────────────────────────────
    if (!io.WantCaptureKeyboard) {
        // F = fit to model
        if (IsKeyPressed(KEY_F)) {
            fit_to_model();
        }
        // R = reset camera
        if (IsKeyPressed(KEY_R)) {
            reset_camera();
        }
        // Space = pause/resume animation
        if (IsKeyPressed(KEY_SPACE)) {
            animation_paused = !animation_paused;
            status_msg = animation_paused ? "Animation paused" : "Animation running";
        }
    }

    // ── Mouse: delegate to OrbitCamera ────────────────────────────────
    if (io.WantCaptureMouse) return;

    orbit_cam.handle_input();
}

// ── fit_to_model ──────────────────────────────────────────────────────────
// Center on the selected model's bounding box and set distance to
// radius * 2.5 so the model fills a reasonable portion of the view.
void ViewerApp::Impl::fit_to_model() {
    if (!doc_loaded || selected_parent < 0) return;

    const auto* rec = db.model(selected_parent);
    if (!rec) return;

    // Convert bounding box center from LH Y-up to RH Y-up
    const Vector3 center = to_raylib(rec->bbox.center_x(),
                                     rec->bbox.center_y(),
                                     rec->bbox.center_z());
    orbit_cam.fit_to_bbox(center, rec->radius);
}

// ── reset_camera ──────────────────────────────────────────────────────────
void ViewerApp::Impl::reset_camera() {
    orbit_cam.reset();
}

} // namespace f4::models_viewer
