// f4-renderer/src/orbit_camera.cpp
//
// Orbit camera implementation. Converts spherical coordinates
// (yaw, pitch, distance) into a Raylib Camera3D each frame.

#include <f4/renderer/orbit_camera.hpp>

#include <imgui.h>
#include <raylib.h>

#include <cmath>

namespace f4::renderer {

// ── Construction ──────────────────────────────────────────────────────────────

OrbitCamera::OrbitCamera(OrbitCameraConfig config)
    : config_(config)
    , cam_yaw_(config.initial_yaw)
    , cam_pitch_(config.initial_pitch)
    , cam_distance_(config.initial_distance)
{
    camera_.up = {0, 1, 0};
    camera_.fovy = 45.0f;
    camera_.projection = CAMERA_PERSPECTIVE;
}

// ── update_from_orbit ─────────────────────────────────────────────────────────

void OrbitCamera::update_from_orbit() {
    const float yaw_rad = cam_yaw_ * kDeg2Rad;
    const float pitch_rad = cam_pitch_ * kDeg2Rad;

    const float cx = cam_distance_ * std::cos(pitch_rad) * std::sin(yaw_rad);
    const float cy = cam_distance_ * std::sin(pitch_rad);
    const float cz = cam_distance_ * std::cos(pitch_rad) * std::cos(yaw_rad);

    camera_.position = {
        cam_target_.x + cx,
        cam_target_.y + cy,
        cam_target_.z + cz
    };
    camera_.target = cam_target_;
    camera_.up = {0, 1, 0};
    camera_.fovy = 45.0f;
    camera_.projection = CAMERA_PERSPECTIVE;
}

// ── handle_input ──────────────────────────────────────────────────────────────

bool OrbitCamera::handle_input() {
    const ImGuiIO& io = ImGui::GetIO();
    bool changed = false;

    // Mouse input
    if (io.WantCaptureMouse) return false;

    const Vector2 mouse = GetMousePosition();

    // Left-drag: orbit
    if (IsMouseButtonDown(MOUSE_BUTTON_LEFT)) {
        if (!orbit_dragging_) {
            orbit_dragging_ = true;
            drag_start_ = mouse;
            drag_yaw0_ = cam_yaw_;
            drag_pitch0_ = cam_pitch_;
        } else {
            const float dx = mouse.x - drag_start_.x;
            const float dy = mouse.y - drag_start_.y;
            cam_yaw_ = drag_yaw0_ - dx * config_.orbit_sensitivity;
            cam_pitch_ = drag_pitch0_ + dy * config_.orbit_sensitivity;
            if (cam_pitch_ < MIN_PITCH) cam_pitch_ = MIN_PITCH;
            if (cam_pitch_ > MAX_PITCH) cam_pitch_ = MAX_PITCH;
            update_from_orbit();
            changed = true;
        }
    } else {
        orbit_dragging_ = false;
    }

    // Right-drag: pan (translate target in screen-plane)
    if (IsMouseButtonDown(MOUSE_BUTTON_RIGHT)) {
        if (!pan_dragging_) {
            pan_dragging_ = true;
            drag_start_ = mouse;
            drag_target0_ = cam_target_;
        } else {
            const float dx = mouse.x - drag_start_.x;
            const float dy = mouse.y - drag_start_.y;

            // Compute right and up vectors in world space from the camera
            const Vector3 delta = {camera_.target.x - camera_.position.x,
                                    camera_.target.y - camera_.position.y,
                                    camera_.target.z - camera_.position.z};
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

            const float pan_scale = cam_distance_ * config_.pan_speed;
            cam_target_.x = drag_target0_.x - right.x * dx * pan_scale + up.x * dy * pan_scale;
            cam_target_.y = drag_target0_.y - right.y * dx * pan_scale + up.y * dy * pan_scale;
            cam_target_.z = drag_target0_.z - right.z * dx * pan_scale + up.z * dy * pan_scale;
            update_from_orbit();
            changed = true;
        }
    } else {
        pan_dragging_ = false;
    }

    // Scroll: dolly (zoom)
    const float wheel = GetMouseWheelMove();
    if (wheel != 0.0f) {
        cam_distance_ *= (1.0f - wheel * config_.zoom_speed);
        if (cam_distance_ < config_.min_distance) cam_distance_ = config_.min_distance;
        if (cam_distance_ > config_.max_distance) cam_distance_ = config_.max_distance;
        update_from_orbit();
        changed = true;
    }

    return changed;
}

// ── reset ─────────────────────────────────────────────────────────────────────

void OrbitCamera::reset() {
    cam_yaw_ = config_.initial_yaw;
    cam_pitch_ = config_.initial_pitch;
    cam_distance_ = config_.initial_distance;
    cam_target_ = {0, 0, 0};
    update_from_orbit();
}

// ── fit_to_bbox ───────────────────────────────────────────────────────────────

void OrbitCamera::fit_to_bbox(Vector3 center, float radius, float margin) {
    cam_target_ = center;
    cam_distance_ = radius * margin;
    if (cam_distance_ < config_.min_distance) cam_distance_ = config_.min_distance;
    update_from_orbit();
}

} // namespace f4::renderer
