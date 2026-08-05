// f4-models-viewer/src/camera3d.cpp
//
// Orbit camera for 3D model viewing. Converts spherical coordinates
// (yaw, pitch, distance) into a Raylib Camera3D each frame.

#include "viewer_state.hpp"
#include "camera3d.hpp"

#include <imgui.h>
#include <raylib.h>

#include <cmath>

namespace f4::models_viewer {

// ── Constants ──────────────────────────────────────────────────────────────
static constexpr float MY_DEG2RAD = 3.14159265358979323846f / 180.0f;
static constexpr float MIN_PITCH = -89.0f;   // degrees
static constexpr float MAX_PITCH =  89.0f;
static constexpr float MIN_DISTANCE = 0.5f;
static constexpr float PAN_SPEED = 0.003f;   // screen-pixels → world units

// ── update_camera_from_orbit ───────────────────────────────────────────────
// Recomputes camera.position from (yaw, pitch, distance, target).
void ViewerApp::Impl::update_camera_from_orbit() {
    const float yaw_rad = cam_yaw * MY_DEG2RAD;
    const float pitch_rad = cam_pitch * MY_DEG2RAD;

    const float cx = cam_distance * std::cos(pitch_rad) * std::sin(yaw_rad);
    const float cy = cam_distance * std::sin(pitch_rad);
    const float cz = cam_distance * std::cos(pitch_rad) * std::cos(yaw_rad);

    camera.position = {
        cam_target.x + cx,
        cam_target.y + cy,
        cam_target.z + cz
    };
    camera.target = cam_target;
    camera.up = {0, 1, 0};
    camera.fovy = 45.0f;
    camera.projection = CAMERA_PERSPECTIVE;
}

// ── handle_camera_input ────────────────────────────────────────────────────
// Process mouse/keyboard for orbit, pan, zoom, fit, reset.
// Guards with ImGui::GetIO().WantCaptureMouse to avoid stealing
// input when the user is interacting with an ImGui panel.
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
    }

    // ── Mouse ─────────────────────────────────────────────────────────
    if (io.WantCaptureMouse) return;

    const Vector2 mouse = GetMousePosition();

    // Left-drag: orbit
    if (IsMouseButtonDown(MOUSE_BUTTON_LEFT)) {
        if (!orbit_dragging) {
            orbit_dragging = true;
            drag_start = mouse;
            drag_yaw0 = cam_yaw;
            drag_pitch0 = cam_pitch;
        } else {
            const float dx = mouse.x - drag_start.x;
            const float dy = mouse.y - drag_start.y;
            cam_yaw = drag_yaw0 - dx * 0.3f;
            cam_pitch = drag_pitch0 + dy * 0.3f;
            if (cam_pitch < MIN_PITCH) cam_pitch = MIN_PITCH;
            if (cam_pitch > MAX_PITCH) cam_pitch = MAX_PITCH;
            update_camera_from_orbit();
        }
    } else {
        orbit_dragging = false;
    }

    // Right-drag: pan (translate target in screen-plane)
    if (IsMouseButtonDown(MOUSE_BUTTON_RIGHT)) {
        if (!pan_dragging) {
            pan_dragging = true;
            drag_start = mouse;
            drag_target0 = cam_target;
        } else {
            const float dx = mouse.x - drag_start.x;
            const float dy = mouse.y - drag_start.y;

            // Compute right and up vectors in world space from the camera
            const Vector3 delta = {camera.target.x - camera.position.x,
                                     camera.target.y - camera.position.y,
                                     camera.target.z - camera.position.z};
            const float len = std::sqrt(delta.x*delta.x + delta.y*delta.y + delta.z*delta.z);
            const Vector3 fwd = {delta.x/len, delta.y/len, delta.z/len};
            const Vector3 world_up = {0, 1, 0};
            // right = fwd x world_up
            const Vector3 r = {fwd.y*world_up.z - fwd.z*world_up.y,
                               fwd.z*world_up.x - fwd.x*world_up.z,
                               fwd.x*world_up.y - fwd.y*world_up.x};
            const float rlen = std::sqrt(r.x*r.x + r.y*r.y + r.z*r.z);
            const Vector3 right = {r.x/rlen, r.y/rlen, r.z/rlen};
            // up = right x fwd
            const Vector3 u = {right.y*fwd.z - right.z*fwd.y,
                               right.z*fwd.x - right.x*fwd.z,
                               right.x*fwd.y - right.y*fwd.x};
            const float ulen = std::sqrt(u.x*u.x + u.y*u.y + u.z*u.z);
            const Vector3 up = {u.x/ulen, u.y/ulen, u.z/ulen};

            const float pan_scale = cam_distance * PAN_SPEED;
            cam_target.x = drag_target0.x - right.x * dx * pan_scale + up.x * dy * pan_scale;
            cam_target.y = drag_target0.y - right.y * dx * pan_scale + up.y * dy * pan_scale;
            cam_target.z = drag_target0.z - right.z * dx * pan_scale + up.z * dy * pan_scale;
            update_camera_from_orbit();
        }
    } else {
        pan_dragging = false;
    }

    // Scroll: dolly (zoom)
    const float wheel = GetMouseWheelMove();
    if (wheel != 0.0f) {
        cam_distance *= (1.0f - wheel * 0.1f);
        if (cam_distance < MIN_DISTANCE) cam_distance = MIN_DISTANCE;
        update_camera_from_orbit();
    }
}

// ── fit_to_model ──────────────────────────────────────────────────────────
// Center on the selected model's bounding box and set distance to
// radius * 2.5 so the model fills a reasonable portion of the view.
void ViewerApp::Impl::fit_to_model() {
    if (!doc_loaded || selected_parent < 0) return;

    const auto* rec = db.model(selected_parent);
    if (!rec) return;

    // Convert bounding box center from LH Z-up to RH Y-up
    cam_target = to_raylib(rec->bbox.center_x(),
                           rec->bbox.center_y(),
                           rec->bbox.center_z());
    cam_distance = rec->radius * 2.5f;
    if (cam_distance < 1.0f) cam_distance = 100.0f;  // degenerate model

    update_camera_from_orbit();
}

// ── reset_camera ──────────────────────────────────────────────────────────
void ViewerApp::Impl::reset_camera() {
    cam_yaw = 45.0f;
    cam_pitch = 30.0f;
    cam_target = {0, 0, 0};
    cam_distance = 100.0f;
    update_camera_from_orbit();
}

} // namespace f4::models_viewer
