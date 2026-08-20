// f4-renderer/include/f4/renderer/world_camera.hpp
//
// World-space camera helpers: construct a Raylib Camera3D from a position
// in the world (ENU feet), plus a free-fly camera controller.
//
// The three world-rendering views use different controllers but produce
// the same Camera3D fed to render_world():
//   - world-viewer Ground Layout 3D tab → OrbitCamera (around the objective)
//   - scenario-player                  → OrbitCamera (around the aircraft)
//   - world-viewer 3D world mode       → FreeCamera (placed anywhere)
//
// Pure functions (no controller state) are separated so the math is
// unit-testable without a GL context or ImGui.
//
// C++20.

#pragma once

#include <raylib.h>
// Undef raylib macros that pollute the namespace BEFORE including
// f4::math constants (raylib's #define PI would corrupt the
// inline constexpr double PI declaration otherwise).
#undef PI
#undef DEG2RAD
#undef RAD2DEG

#include <f4/math/constants.hpp>
#include <f4/math/vec3.hpp>

namespace f4::renderer {

// ---------------------------------------------------------------------------
// Pure camera-construction helpers
// ---------------------------------------------------------------------------

/// Build a perspective camera from a world position + heading.
///
/// @param enu_e, enu_n, enu_u   Camera position, ENU feet.
/// @param heading_deg  Compass heading (0 = North, 90 = East, clockwise
///                     when viewed from above).
/// @param pitch_deg    Positive looks up, negative looks down.
/// @param fovy_deg     Vertical field of view (degrees).
Camera3D make_perspective_camera(
    float enu_e, float enu_n, float enu_u,
    float heading_deg, float pitch_deg, float fovy_deg = 45.0f);

/// Build a top-down orthographic camera over a world position whose
/// projection matches the 2D canvas transform: an ENU point (e, n, alt)
/// projects to the same screen pixel as the 2D world_to_screen() with
/// the camera centered at (center_e, center_n) and `visible_height_ft`
/// of world height across the viewport.
///
/// @param enu_e, enu_n  Look-at center on the ground, ENU feet.
/// @param visible_height_ft  Vertical world extent visible (orthographic
///                          fovy in Raylib).
/// @param alt_ft       Camera altitude; must clear the tallest geometry
///                     (KoreaObj features are < 200 ft; 5000 is safe) and
///                     stay inside the far plane.
Camera3D make_topdown_ortho_camera(
    float enu_e, float enu_n,
    float visible_height_ft, float alt_ft = 5000.0f);

/// Unit forward vector (ENU) for a compass heading + pitch.
/// heading 0 = +Y (North), 90 = +X (East); pitch positive = up.
/// Pure, unit-testable.
f4::math::Vec3f heading_forward_enu(float heading_deg, float pitch_deg);

// ---------------------------------------------------------------------------
// FreeCamera
// ---------------------------------------------------------------------------

/// Configuration for a FreeCamera.
struct FreeCameraConfig {
    float initial_heading_deg = 0.0f;    ///< 0 = North, 90 = East
    float initial_pitch_deg   = -20.0f;  ///< slightly downward
    float initial_speed_ft_s  = 500.0f;  ///< base movement speed

    float min_speed_ft_s = 10.0f;
    float max_speed_ft_s = 200000.0f;
    float speed_mult_per_notch = 1.5f;   ///< wheel zoom factor on speed
    float look_sensitivity = 0.25f;      ///< degrees per pixel of drag
    float fov_deg = 45.0f;
};

/// First-person free-fly camera specified by a position in the world.
///
/// Usage:
///   FreeCamera cam;                         // at ENU (0,0,0)
///   cam.set_position_enu(e, n, u);
///   cam.update_from_pose();
///   // per frame:
///   cam.handle_input();                     // WASD/QE + drag + wheel
///   scene.camera = cam.camera();
///   render_world(res, scene);
///
/// Controls (skipped when ImGui wants the mouse/keyboard):
///   left-drag   look (heading/pitch)
///   W/S         move forward/back along the heading
///   A/D         strafe left/right
///   Q/E         down/up
///   wheel       adjust movement speed
///   Left Shift  3× speed while held
class FreeCamera {
public:
    explicit FreeCamera(FreeCameraConfig config = {});

    /// Recompute camera_.position/target from (position_, heading_, pitch_).
    void update_from_pose();

    /// Process input. Returns true if anything changed.
    bool handle_input();

    /// Reset heading/pitch/speed to the initial configuration (position
    /// is kept — the world is big; losing your place is worse than
    /// losing your orientation).
    void reset_orientation();

    const Camera3D& camera() const noexcept { return camera_; }

    // ── Accessors ────────────────────────────────────────────────────
    float enu_e() const noexcept { return enu_e_; }
    float enu_n() const noexcept { return enu_n_; }
    float enu_u() const noexcept { return enu_u_; }
    float heading_deg() const noexcept { return heading_; }
    float pitch_deg() const noexcept { return pitch_; }
    float speed_ft_s() const noexcept { return speed_; }

    void set_position_enu(float e, float n, float u);
    void set_heading_pitch(float heading_deg, float pitch_deg);
    void set_speed(float ft_s);

private:
    FreeCameraConfig config_;

    Camera3D camera_ = {};
    float enu_e_ = 0.0f, enu_n_ = 0.0f, enu_u_ = 0.0f;
    float heading_ = 0.0f;   // compass degrees
    float pitch_ = -20.0f;   // degrees
    float speed_ = 500.0f;   // ft/s

    bool dragging_ = false;
    Vector2 drag_start_ = {};
    float drag_heading0_ = 0.0f;
    float drag_pitch0_ = 0.0f;

    static constexpr float kMinPitch = -89.0f;
    static constexpr float kMaxPitch = 89.0f;
};

} // namespace f4::renderer
