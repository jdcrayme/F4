// f4-renderer/src/world_camera.cpp
//
// World-space camera helpers implementation (see world_camera.hpp).

#include <f4/renderer/world_camera.hpp>

#include <f4/renderer/coord_transform.hpp>

#include <imgui.h>
#include <raylib.h>

#include <cmath>

namespace f4::renderer {

// ── Pure helpers ──────────────────────────────────────────────────────────

f4::math::Vec3f heading_forward_enu(float heading_deg, float pitch_deg) {
    constexpr float kDeg2Rad = static_cast<float>(f4::math::DEG_TO_RAD);
    const float h = heading_deg * kDeg2Rad;
    const float p = pitch_deg * kDeg2Rad;
    return f4::math::Vec3f{
        std::sin(h) * std::cos(p),   // east
        std::cos(h) * std::cos(p),   // north
        std::sin(p)                  // up
    };
}

Camera3D make_perspective_camera(
    float enu_e, float enu_n, float enu_u,
    float heading_deg, float pitch_deg, float fovy_deg)
{
    const auto pos = enu_to_raylib(enu_e, enu_n, enu_u);
    const auto fwd = heading_forward_enu(heading_deg, pitch_deg);
    const auto fwd_r = enu_to_raylib(fwd.x, fwd.y, fwd.z);

    Camera3D cam = {};
    cam.position   = {pos.x, pos.y, pos.z};
    cam.target     = {pos.x + fwd_r.x, pos.y + fwd_r.y, pos.z + fwd_r.z};
    cam.up         = {0.0f, 1.0f, 0.0f};
    cam.fovy       = fovy_deg;
    cam.projection = CAMERA_PERSPECTIVE;
    return cam;
}

Camera3D make_topdown_ortho_camera(
    float enu_e, float enu_n,
    float visible_height_ft, float alt_ft)
{
    // Matches the camera built inline in world-viewer canvas.cpp's
    // feature-mesh pass: up = (0,0,-1) so screen-up = ENU +Y (north),
    // and ortho fovy is the vertical world extent in feet.
    Camera3D cam = {};
    cam.position   = {enu_e, alt_ft, -enu_n};
    cam.target     = {enu_e, 0.0f, -enu_n};
    cam.up         = {0.0f, 0.0f, -1.0f};
    cam.fovy       = visible_height_ft;
    cam.projection = CAMERA_ORTHOGRAPHIC;
    return cam;
}

// ── FreeCamera ────────────────────────────────────────────────────────────

FreeCamera::FreeCamera(FreeCameraConfig config)
    : config_(config)
    , heading_(config.initial_heading_deg)
    , pitch_(config.initial_pitch_deg)
    , speed_(config.initial_speed_ft_s)
{
    update_from_pose();
}

void FreeCamera::update_from_pose() {
    camera_ = make_perspective_camera(
        enu_e_, enu_n_, enu_u_, heading_, pitch_, config_.fov_deg);
}

void FreeCamera::set_position_enu(float e, float n, float u) {
    enu_e_ = e;
    enu_n_ = n;
    enu_u_ = u;
    update_from_pose();
}

void FreeCamera::set_heading_pitch(float heading_deg, float pitch_deg) {
    heading_ = heading_deg;
    pitch_ = pitch_deg;
    update_from_pose();
}

void FreeCamera::set_speed(float ft_s) {
    speed_ = ft_s;
    if (speed_ < config_.min_speed_ft_s) speed_ = config_.min_speed_ft_s;
    if (speed_ > config_.max_speed_ft_s) speed_ = config_.max_speed_ft_s;
}

void FreeCamera::reset_orientation() {
    heading_ = config_.initial_heading_deg;
    pitch_ = config_.initial_pitch_deg;
    speed_ = config_.initial_speed_ft_s;
    update_from_pose();
}

bool FreeCamera::handle_input() {
    const ImGuiIO& io = ImGui::GetIO();
    bool changed = false;

    // ── Look: left-drag adjusts heading/pitch ─────────────────────────
    if (!io.WantCaptureMouse) {
        if (IsMouseButtonDown(MOUSE_BUTTON_LEFT)) {
            const Vector2 mouse = GetMousePosition();
            if (!dragging_) {
                dragging_ = true;
                drag_start_ = mouse;
                drag_heading0_ = heading_;
                drag_pitch0_ = pitch_;
            } else {
                const float dx = mouse.x - drag_start_.x;
                const float dy = mouse.y - drag_start_.y;
                // Dragging right turns the view clockwise (heading east),
                // dragging down looks down — the usual FPS feel.
                heading_ = drag_heading0_ + dx * config_.look_sensitivity;
                pitch_ = drag_pitch0_ - dy * config_.look_sensitivity;
                if (pitch_ < kMinPitch) pitch_ = kMinPitch;
                if (pitch_ > kMaxPitch) pitch_ = kMaxPitch;
                update_from_pose();
                changed = true;
            }
        } else {
            dragging_ = false;
        }

        // Wheel: speed (there's no "distance" to zoom on a free camera).
        const float wheel = GetMouseWheelMove();
        if (wheel != 0.0f) {
            set_speed(speed_ * std::pow(config_.speed_mult_per_notch, wheel));
            changed = true;
        }
    }

    // ── Move: WASD on the horizontal heading frame, Q/E vertical ──────
    if (!io.WantCaptureKeyboard) {
        float fwd = 0.0f, side = 0.0f, vert = 0.0f;
        if (IsKeyDown(KEY_W)) fwd += 1.0f;
        if (IsKeyDown(KEY_S)) fwd -= 1.0f;
        if (IsKeyDown(KEY_D)) side += 1.0f;
        if (IsKeyDown(KEY_A)) side -= 1.0f;
        if (IsKeyDown(KEY_E)) vert += 1.0f;
        if (IsKeyDown(KEY_Q)) vert -= 1.0f;

        if (fwd != 0.0f || side != 0.0f || vert != 0.0f) {
            constexpr float kDeg2Rad = static_cast<float>(f4::math::DEG_TO_RAD);
            const float h = heading_ * kDeg2Rad;
            // Horizontal frame: forward along the heading (not pitch —
            // flying diagonally into the ground while looking down is
            // rarely what you want), right = forward rotated 90° CW.
            const float fwd_e = std::sin(h), fwd_n = std::cos(h);
            const float right_e = std::cos(h), right_n = -std::sin(h);

            const float dt = GetFrameTime();
            float v = speed_;
            if (IsKeyDown(KEY_LEFT_SHIFT)) v *= 3.0f;

            enu_e_ += (fwd * fwd_e + side * right_e) * v * dt;
            enu_n_ += (fwd * fwd_n + side * right_n) * v * dt;
            enu_u_ += vert * v * dt;
            if (enu_u_ < 0.0f) enu_u_ = 0.0f;   // don't dig into the ground
            update_from_pose();
            changed = true;
        }
    }

    return changed;
}

} // namespace f4::renderer
