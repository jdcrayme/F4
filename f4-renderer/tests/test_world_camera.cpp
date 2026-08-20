// f4-renderer/tests/test_world_camera.cpp
//
// Pure-math tests for world_camera.hpp: heading vectors, perspective
// camera construction, top-down ortho camera construction, and
// FreeCamera pose math (no input handling — that needs a window).
//
// The math here must stay consistent with coord_transform.hpp:
// ENU (e, n, u) → Raylib (e, u, -n).

#include <f4/renderer/world_camera.hpp>
#include <f4/renderer/coord_transform.hpp>

#include <gtest/gtest.h>
#include <raylib.h>

#include <cmath>

using namespace f4::renderer;

static void expect_float_eq(float a, float b, float eps = 1e-4f) {
    EXPECT_NEAR(a, b, eps);
}

TEST(WorldCameraTest, HeadingForward_CardinalDirections) {
    // North: heading 0 → +Y (ENU north), level.
    auto north = heading_forward_enu(0.0f, 0.0f);
    expect_float_eq(north.x, 0.0f);
    expect_float_eq(north.y, 1.0f);
    expect_float_eq(north.z, 0.0f);

    // East: heading 90 → +X.
    auto east = heading_forward_enu(90.0f, 0.0f);
    expect_float_eq(east.x, 1.0f);
    expect_float_eq(east.y, 0.0f);
    expect_float_eq(east.z, 0.0f);

    // South: heading 180 → -Y.
    auto south = heading_forward_enu(180.0f, 0.0f);
    expect_float_eq(south.x, 0.0f);
    expect_float_eq(south.y, -1.0f);

    // West: heading 270 → -X.
    auto west = heading_forward_enu(270.0f, 0.0f);
    expect_float_eq(west.x, -1.0f);
    expect_float_eq(west.y, 0.0f);
}

TEST(WorldCameraTest, HeadingForward_PitchIsUnitLength) {
    const auto fwd = heading_forward_enu(37.0f, -25.0f);
    const float len = std::sqrt(fwd.x * fwd.x + fwd.y * fwd.y + fwd.z * fwd.z);
    expect_float_eq(len, 1.0f);
    // Pitch -25° → negative up component.
    EXPECT_LT(fwd.z, 0.0f);
}

TEST(WorldCameraTest, MakePerspectiveCamera_PositionAndTarget) {
    const auto cam = make_perspective_camera(
        /*enu_e=*/1000.0f, /*enu_n=*/2000.0f, /*enu_u=*/300.0f,
        /*heading=*/0.0f, /*pitch=*/0.0f, /*fovy=*/60.0f);

    // Position: ENU → Raylib (e, u, -n).
    expect_float_eq(cam.position.x, 1000.0f);
    expect_float_eq(cam.position.y, 300.0f);
    expect_float_eq(cam.position.z, -2000.0f);

    // Looking north (heading 0) = ENU +Y = Raylib -Z.
    expect_float_eq(cam.target.x - cam.position.x, 0.0f);
    expect_float_eq(cam.target.y - cam.position.y, 0.0f);
    expect_float_eq(cam.target.z - cam.position.z, -1.0f);

    EXPECT_EQ(cam.projection, CAMERA_PERSPECTIVE);
    expect_float_eq(cam.fovy, 60.0f);
}

TEST(WorldCameraTest, MakePerspectiveCamera_LookEast) {
    const auto cam = make_perspective_camera(0, 0, 0, 90.0f, 0.0f);
    expect_float_eq(cam.target.x - cam.position.x, 1.0f);   // Raylib +X = East
    expect_float_eq(cam.target.z - cam.position.z, 0.0f);
}

TEST(WorldCameraTest, MakeTopdownOrtho_MatchesCanvasConventions) {
    const auto cam = make_topdown_ortho_camera(
        /*enu_e=*/500.0f, /*enu_n=*/-700.0f,
        /*visible_height_ft=*/20480.0f, /*alt=*/5000.0f);

    expect_float_eq(cam.position.x, 500.0f);
    expect_float_eq(cam.position.y, 5000.0f);
    expect_float_eq(cam.position.z, 700.0f);      // -(-700)
    expect_float_eq(cam.target.y, 0.0f);
    // up = (0,0,-1) so screen-up is ENU +Y (north).
    expect_float_eq(cam.up.x, 0.0f);
    expect_float_eq(cam.up.y, 0.0f);
    expect_float_eq(cam.up.z, -1.0f);
    expect_float_eq(cam.fovy, 20480.0f);
    EXPECT_EQ(cam.projection, CAMERA_ORTHOGRAPHIC);
}

TEST(FreeCameraTest, DefaultPoseProducesValidCamera) {
    FreeCamera cam;
    cam.update_from_pose();
    const Camera3D& c = cam.camera();
    // Position starts at world origin; the target sits ~1 unit ahead.
    const float dx = c.target.x - c.position.x;
    const float dy = c.target.y - c.position.y;
    const float dz = c.target.z - c.position.z;
    const float dist = std::sqrt(dx * dx + dy * dy + dz * dz);
    EXPECT_NEAR(dist, 1.0f, 1e-4f);
    EXPECT_EQ(c.projection, CAMERA_PERSPECTIVE);
}

TEST(FreeCameraTest, SetPositionUpdatesCamera) {
    FreeCamera cam;
    cam.set_position_enu(100.0f, 200.0f, 50.0f);
    expect_float_eq(cam.enu_e(), 100.0f);
    expect_float_eq(cam.enu_n(), 200.0f);
    expect_float_eq(cam.enu_u(), 50.0f);
    expect_float_eq(cam.camera().position.x, 100.0f);
    expect_float_eq(cam.camera().position.y, 50.0f);
    expect_float_eq(cam.camera().position.z, -200.0f);
}

TEST(FreeCameraTest, SetHeadingPitchClampsNothingInNormalRange) {
    FreeCamera cam;
    cam.set_heading_pitch(135.0f, -45.0f);
    expect_float_eq(cam.heading_deg(), 135.0f);
    expect_float_eq(cam.pitch_deg(), -45.0f);
    // Looking southeast & down: forward ENU x>0, y<0, z<0.
    const auto fwd = heading_forward_enu(cam.heading_deg(), cam.pitch_deg());
    EXPECT_GT(fwd.x, 0.0f);
    EXPECT_LT(fwd.y, 0.0f);
    EXPECT_LT(fwd.z, 0.0f);
}

TEST(FreeCameraTest, SpeedClampedToConfigBounds) {
    FreeCamera cam;
    cam.set_speed(1.0f);       // below min → clamped up
    EXPECT_GT(cam.speed_ft_s(), 1.0f);
    cam.set_speed(1.0e9f);     // above max → clamped down
    EXPECT_LT(cam.speed_ft_s(), 1.0e9f);
}

TEST(FreeCameraTest, ResetOrientationKeepsPosition) {
    FreeCamera cam;
    cam.set_position_enu(1234.0f, 5678.0f, 90.0f);
    cam.set_heading_pitch(200.0f, 30.0f);
    cam.reset_orientation();
    expect_float_eq(cam.enu_e(), 1234.0f);
    expect_float_eq(cam.enu_n(), 5678.0f);
    expect_float_eq(cam.enu_u(), 90.0f);
}
