// f4-renderer/include/f4/renderer/orbit_camera.hpp
//
// Orbit camera: spherical coordinates (yaw, pitch, distance) around a
// target point, converted to a Raylib Camera3D each frame.
//
// Consolidated from 4 duplicated implementations across:
//   - f4-models-viewer/src/camera3d.cpp
//   - f4-scenario-player/src/renderer.cpp
//   - f4-world-viewer/src/ground_layout_3d.cpp
//   - f4-world-viewer/src/class_table_browser.cpp

#pragma once

#include <raylib.h>
// Undef raylib macros that pollute the namespace BEFORE including
// f4::math constants (raylib's #define PI would corrupt the
// inline constexpr double PI declaration otherwise).
#undef PI
#undef DEG2RAD
#undef RAD2DEG

#include <f4/math/constants.hpp>

namespace f4::renderer {

/// Configuration for an OrbitCamera. Different apps need different
/// defaults (e.g. model viewer uses MIN_DISTANCE=0.5, scenario player
/// uses 1.0, ground layout uses 50.0).
struct OrbitCameraConfig {
    float min_distance   = 0.5f;      ///< Minimum zoom distance
    float max_distance   = 100000.f;  ///< Maximum zoom distance
    float initial_yaw    = 45.0f;     ///< Initial horizontal angle (degrees)
    float initial_pitch  = 30.0f;     ///< Initial vertical angle (degrees)
    float initial_distance = 100.0f;  ///< Initial zoom distance
    float orbit_sensitivity = 0.3f;   ///< Degrees per pixel of mouse drag
    float pan_speed      = 0.003f;    ///< Pan scaling factor
    float zoom_speed     = 0.1f;      ///< Zoom scaling factor (per scroll notch)
};

/// Orbit camera that converts spherical coordinates to a Raylib Camera3D.
///
/// Usage:
///   OrbitCamera cam(config);
///   cam.update_from_orbit();        // call once to set initial position
///   cam.handle_input();             // call each frame for orbit/pan/zoom
///   BeginMode3D(cam.camera());      // use in rendering
class OrbitCamera {
public:
    explicit OrbitCamera(OrbitCameraConfig config = {});

    /// Recompute camera.position from (yaw, pitch, distance, target).
    void update_from_orbit();

    /// Process mouse/keyboard input for orbit, pan, zoom.
    /// Guards with ImGui::GetIO().WantCaptureMouse to avoid stealing
    /// input when the user is interacting with an ImGui panel.
    /// Returns true if any input was consumed (camera changed).
    bool handle_input();

    /// Reset to initial configuration values.
    void reset();

    /// Fit the camera to a bounding sphere.
    /// @param center  Center of the bounding sphere (in Raylib coords)
    /// @param radius  Radius of the bounding sphere
    /// @param margin  Multiplier for distance (default 2.5 = model fills ~40% of view)
    void fit_to_bbox(Vector3 center, float radius, float margin = 2.5f);

    /// Get the Raylib Camera3D for use with BeginMode3D.
    const Camera3D& camera() const noexcept { return camera_; }

    // ── Accessors ────────────────────────────────────────────────────────
    float yaw() const noexcept { return cam_yaw_; }
    float pitch() const noexcept { return cam_pitch_; }
    float distance() const noexcept { return cam_distance_; }
    Vector3 target() const noexcept { return cam_target_; }

    void set_yaw(float yaw) { cam_yaw_ = yaw; }
    void set_pitch(float pitch) { cam_pitch_ = pitch; }
    void set_distance(float dist) { cam_distance_ = dist; }
    void set_target(Vector3 target) { cam_target_ = target; }

private:
    OrbitCameraConfig config_;

    Camera3D camera_ = {};
    float cam_yaw_ = 45.0f;          // degrees, horizontal orbit
    float cam_pitch_ = 30.0f;        // degrees, vertical orbit (clamped)
    float cam_distance_ = 100.0f;    // distance from target
    Vector3 cam_target_ = {0, 0, 0}; // point the camera orbits around

    bool orbit_dragging_ = false;
    bool pan_dragging_ = false;
    Vector2 drag_start_ = {0, 0};
    float drag_yaw0_ = 0;
    float drag_pitch0_ = 0;
    Vector3 drag_target0_ = {};

    static constexpr float kDeg2Rad = static_cast<float>(f4::math::DEG_TO_RAD);
    static constexpr float MIN_PITCH = -89.0f;
    static constexpr float MAX_PITCH =  89.0f;
};

} // namespace f4::renderer
