// f4-renderer/tests/test_draw_3d.cpp
//
// Unit tests for draw_3d helpers. Requires a Raylib GPU context (InitWindow)
// because draw_grid/draw_axes use Raylib drawing functions.

#include <f4/renderer/draw_3d.hpp>

#include <gtest/gtest.h>
#include <raylib.h>

class Draw3DTest : public ::testing::Test {
protected:
    void SetUp() override {
        if (!initialized_) {
            SetConfigFlags(FLAG_WINDOW_HIDDEN);
            InitWindow(256, 256, "test");
            initialized_ = true;
        }
    }

    static void TearDownTestSuite() {
        if (initialized_) {
            CloseWindow();
            initialized_ = false;
        }
    }

    static bool initialized_;
};

bool Draw3DTest::initialized_ = false;

// ── draw_grid ─────────────────────────────────────────────────────────────────

TEST_F(Draw3DTest, DrawGrid_DoesNotCrash) {
    BeginDrawing();
    ClearBackground(BLACK);
    Camera3D cam = {};
    cam.position = {10, 10, 10};
    cam.target = {0, 0, 0};
    cam.up = {0, 1, 0};
    cam.fovy = 45.0f;
    cam.projection = CAMERA_PERSPECTIVE;
    BeginMode3D(cam);
    f4::renderer::draw_grid(100.0f, 10.0f);
    EndMode3D();
    EndDrawing();
    SUCCEED();
}

TEST_F(Draw3DTest, DrawGrid_SmallExtent) {
    BeginDrawing();
    ClearBackground(BLACK);
    Camera3D cam = {};
    cam.position = {5, 5, 5};
    cam.target = {0, 0, 0};
    cam.up = {0, 1, 0};
    cam.fovy = 45.0f;
    cam.projection = CAMERA_PERSPECTIVE;
    BeginMode3D(cam);
    f4::renderer::draw_grid(1.0f, 0.5f);
    EndMode3D();
    EndDrawing();
    SUCCEED();
}

// ── draw_axes ─────────────────────────────────────────────────────────────────

TEST_F(Draw3DTest, DrawAxes_DoesNotCrash) {
    BeginDrawing();
    ClearBackground(BLACK);
    Camera3D cam = {};
    cam.position = {10, 10, 10};
    cam.target = {0, 0, 0};
    cam.up = {0, 1, 0};
    cam.fovy = 45.0f;
    cam.projection = CAMERA_PERSPECTIVE;
    BeginMode3D(cam);
    f4::renderer::draw_axes(5.0f);
    EndMode3D();
    EndDrawing();
    SUCCEED();
}

// ── DrawStats ─────────────────────────────────────────────────────────────────

TEST_F(Draw3DTest, DrawStats_DefaultZeroed) {
    f4::renderer::DrawStats stats;
    EXPECT_EQ(stats.draw_calls, 0);
    EXPECT_EQ(stats.meshes_drawn, 0);
    EXPECT_EQ(stats.vertices_drawn, 0u);
}
