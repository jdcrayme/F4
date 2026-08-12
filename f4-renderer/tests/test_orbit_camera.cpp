// f4-renderer/tests/test_orbit_camera.cpp
//
// Unit tests for OrbitCamera. Requires a Raylib GPU context (InitWindow)
// because OrbitCamera uses Camera3D, Vector3, and mouse input functions.

#include <f4/renderer/orbit_camera.hpp>

#include <gtest/gtest.h>
#include <raylib.h>
#include <cmath>

using namespace f4::renderer;

class OrbitCameraTest : public ::testing::Test {
protected:
    void SetUp() override {
        // Only init once for all tests in this suite
        if (!initialized_) {
            SetConfigFlags(FLAG_WINDOW_HIDDEN);
            InitWindow(1, 1, "test");
            initialized_ = true;
        }
    }

    void TearDown() override {
        // Don't close window — keep it for subsequent tests
    }

    static void TearDownTestSuite() {
        if (initialized_) {
            CloseWindow();
            initialized_ = false;
        }
    }

    static bool initialized_;
};

bool OrbitCameraTest::initialized_ = false;

// ── Construction ──────────────────────────────────────────────────────────────

TEST_F(OrbitCameraTest, DefaultConstruction) {
    OrbitCamera cam;
    // Default config: yaw=45, pitch=30, distance=100
    EXPECT_FLOAT_EQ(cam.yaw(), 45.0f);
    EXPECT_FLOAT_EQ(cam.pitch(), 30.0f);
    EXPECT_FLOAT_EQ(cam.distance(), 100.0f);
}

TEST_F(OrbitCameraTest, CustomConfig) {
    OrbitCameraConfig cfg;
    cfg.initial_yaw = 90.0f;
    cfg.initial_pitch = 45.0f;
    cfg.initial_distance = 50.0f;
    cfg.min_distance = 1.0f;
    cfg.max_distance = 500.0f;

    OrbitCamera cam(cfg);
    EXPECT_FLOAT_EQ(cam.yaw(), 90.0f);
    EXPECT_FLOAT_EQ(cam.pitch(), 45.0f);
    EXPECT_FLOAT_EQ(cam.distance(), 50.0f);
}

// ── update_from_orbit ─────────────────────────────────────────────────────────

TEST_F(OrbitCameraTest, UpdateFromOrbit_SetsPosition) {
    OrbitCamera cam;
    cam.update_from_orbit();
    const auto& c = cam.camera();
    // Camera position should be computed from yaw/pitch/distance
    // Default: yaw=45, pitch=30, distance=100
    // The camera position should be non-zero (we're orbiting around origin)
    EXPECT_NE(c.position.x, 0.0f);
    EXPECT_NE(c.position.y, 0.0f);
    EXPECT_NE(c.position.z, 0.0f);
}

TEST_F(OrbitCameraTest, UpdateFromOrbit_TargetAtOrigin) {
    OrbitCamera cam;
    cam.update_from_orbit();
    const auto& c = cam.camera();
    EXPECT_FLOAT_EQ(c.target.x, 0.0f);
    EXPECT_FLOAT_EQ(c.target.y, 0.0f);
    EXPECT_FLOAT_EQ(c.target.z, 0.0f);
}

TEST_F(OrbitCameraTest, UpdateFromOrbit_UpIsY) {
    OrbitCamera cam;
    cam.update_from_orbit();
    const auto& c = cam.camera();
    EXPECT_FLOAT_EQ(c.up.x, 0.0f);
    EXPECT_FLOAT_EQ(c.up.y, 1.0f);
    EXPECT_FLOAT_EQ(c.up.z, 0.0f);
}

TEST_F(OrbitCameraTest, UpdateFromOrbit_PerspectiveProjection) {
    OrbitCamera cam;
    cam.update_from_orbit();
    EXPECT_EQ(cam.camera().projection, CAMERA_PERSPECTIVE);
}

// ── Distance from target ──────────────────────────────────────────────────────

TEST_F(OrbitCameraTest, PositionDistanceMatchesConfig) {
    OrbitCameraConfig cfg;
    cfg.initial_yaw = 0.0f;
    cfg.initial_pitch = 0.0f;
    cfg.initial_distance = 200.0f;
    OrbitCamera cam(cfg);
    cam.update_from_orbit();

    const auto pos = cam.camera().position;
    const auto tgt = cam.camera().target;
    const float dx = pos.x - tgt.x;
    const float dy = pos.y - tgt.y;
    const float dz = pos.z - tgt.z;
    const float dist = std::sqrt(dx*dx + dy*dy + dz*dz);
    EXPECT_NEAR(dist, 200.0f, 0.01f);
}

// ── Reset ─────────────────────────────────────────────────────────────────────

TEST_F(OrbitCameraTest, Reset_RestoresInitialValues) {
    OrbitCameraConfig cfg;
    cfg.initial_yaw = 60.0f;
    cfg.initial_pitch = 20.0f;
    cfg.initial_distance = 150.0f;
    OrbitCamera cam(cfg);
    cam.update_from_orbit();

    // Modify
    cam.set_yaw(120.0f);
    cam.set_pitch(80.0f);
    cam.set_distance(10.0f);

    // Reset
    cam.reset();
    EXPECT_FLOAT_EQ(cam.yaw(), 60.0f);
    EXPECT_FLOAT_EQ(cam.pitch(), 20.0f);
    EXPECT_FLOAT_EQ(cam.distance(), 150.0f);
}

// ── Accessors ─────────────────────────────────────────────────────────────────

TEST_F(OrbitCameraTest, SetYaw) {
    OrbitCamera cam;
    cam.set_yaw(180.0f);
    EXPECT_FLOAT_EQ(cam.yaw(), 180.0f);
}

TEST_F(OrbitCameraTest, SetPitch) {
    OrbitCamera cam;
    cam.set_pitch(-45.0f);
    EXPECT_FLOAT_EQ(cam.pitch(), -45.0f);
}

TEST_F(OrbitCameraTest, SetDistance) {
    OrbitCamera cam;
    cam.set_distance(500.0f);
    EXPECT_FLOAT_EQ(cam.distance(), 500.0f);
}

TEST_F(OrbitCameraTest, SetTarget) {
    OrbitCamera cam;
    Vector3 new_target = {10, 20, 30};
    cam.set_target(new_target);
    EXPECT_FLOAT_EQ(cam.target().x, 10.0f);
    EXPECT_FLOAT_EQ(cam.target().y, 20.0f);
    EXPECT_FLOAT_EQ(cam.target().z, 30.0f);
}

// ── fit_to_bbox ───────────────────────────────────────────────────────────────

TEST_F(OrbitCameraTest, FitToBbox_SetsTargetAndDistance) {
    OrbitCamera cam;
    cam.fit_to_bbox({50, 0, 50}, 100.0f, 2.5f);
    EXPECT_FLOAT_EQ(cam.target().x, 50.0f);
    EXPECT_FLOAT_EQ(cam.target().y, 0.0f);
    EXPECT_FLOAT_EQ(cam.target().z, 50.0f);
    EXPECT_FLOAT_EQ(cam.distance(), 250.0f); // 100 * 2.5
}

TEST_F(OrbitCameraTest, FitToBbox_RespectsMinDistance) {
    OrbitCameraConfig cfg;
    cfg.min_distance = 50.0f;
    OrbitCamera cam(cfg);
    cam.fit_to_bbox({0, 0, 0}, 1.0f, 2.0f);
    // 1.0 * 2.0 = 2.0, but min_distance is 50
    EXPECT_FLOAT_EQ(cam.distance(), 50.0f);
}

// ── Known camera positions ────────────────────────────────────────────────────

TEST_F(OrbitCameraTest, PitchZero_LooksHorizontal) {
    OrbitCameraConfig cfg;
    cfg.initial_yaw = 0.0f;
    cfg.initial_pitch = 0.0f;
    cfg.initial_distance = 100.0f;
    OrbitCamera cam(cfg);
    cam.update_from_orbit();

    // At yaw=0, pitch=0: camera looks along +Z axis
    // Position should be (0, 0, 100) looking at origin
    const auto pos = cam.camera().position;
    EXPECT_NEAR(pos.x, 0.0f, 0.01f);
    EXPECT_NEAR(pos.y, 0.0f, 0.01f);
    EXPECT_NEAR(pos.z, 100.0f, 0.01f);
}

TEST_F(OrbitCameraTest, Pitch90_LooksDown) {
    OrbitCameraConfig cfg;
    cfg.initial_yaw = 0.0f;
    cfg.initial_pitch = 89.0f; // near-90 to avoid gimbal lock
    cfg.initial_distance = 100.0f;
    OrbitCamera cam(cfg);
    cam.update_from_orbit();

    const auto pos = cam.camera().position;
    // Camera should be nearly directly above the target.
    // At pitch=89°, cos(89°)≈0.0175, so horizontal offset is ~1.75 units.
    // The Y position should be very large (near 100).
    EXPECT_GT(pos.y, 90.0f);
    // The X and Z offsets should be small (cos(89°) * 100 ≈ 1.75)
    EXPECT_NEAR(pos.x, 0.0f, 5.0f);
    EXPECT_NEAR(pos.z, 0.0f, 5.0f);
}
