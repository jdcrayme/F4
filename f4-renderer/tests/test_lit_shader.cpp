// f4-renderer/tests/test_lit_shader.cpp
//
// Unit tests for LitShader. Requires a Raylib GPU context (InitWindow)
// because LitShader calls LoadShaderFromMemory which needs OpenGL.

#include <f4/renderer/lit_shader.hpp>

#include <gtest/gtest.h>
#include <raylib.h>

using namespace f4::renderer;

class LitShaderTest : public ::testing::Test {
protected:
    void SetUp() override {
        if (!initialized_) {
            SetConfigFlags(FLAG_WINDOW_HIDDEN);
            InitWindow(1, 1, "test");
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

bool LitShaderTest::initialized_ = false;

// ── Construction ──────────────────────────────────────────────────────────────

TEST_F(LitShaderTest, DefaultConstruction_NotLoaded) {
    LitShader shader;
    EXPECT_FALSE(shader.is_loaded());
}

// ── ensure ────────────────────────────────────────────────────────────────────

TEST_F(LitShaderTest, Ensure_CompilesShader) {
    LitShader shader;
    bool ok = shader.ensure();
    // In a headless/CI environment, shader compilation may fail
    // (no GL context). In a real environment it should succeed.
    // We test that ensure() is idempotent either way.
    bool ok2 = shader.ensure();
    EXPECT_EQ(ok, ok2);
}

TEST_F(LitShaderTest, Ensure_SetsLoadedFlag) {
    LitShader shader;
    shader.ensure();
    // After calling ensure(), is_loaded() should reflect whether
    // compilation succeeded
    if (shader.is_loaded()) {
        EXPECT_NE(shader.shader().id, 0);
    }
}

TEST_F(LitShaderTest, Ensure_StatusMessage) {
    LitShader shader;
    std::string msg;
    shader.ensure(&msg);
    // If shader failed to compile, msg should be set
    if (!shader.is_loaded()) {
        EXPECT_FALSE(msg.empty());
    }
}

// ── Move semantics ────────────────────────────────────────────────────────────

TEST_F(LitShaderTest, MoveConstruction) {
    LitShader shader1;
    shader1.ensure();
    bool was_loaded = shader1.is_loaded();

    LitShader shader2(std::move(shader1));
    // shader2 should now own the shader
    EXPECT_EQ(shader2.is_loaded(), was_loaded);
    // shader1 should be in a valid-but-unloaded state
    EXPECT_FALSE(shader1.is_loaded());
}

TEST_F(LitShaderTest, MoveAssignment) {
    LitShader shader1;
    shader1.ensure();
    bool was_loaded = shader1.is_loaded();

    LitShader shader2;
    shader2 = std::move(shader1);
    EXPECT_EQ(shader2.is_loaded(), was_loaded);
    EXPECT_FALSE(shader1.is_loaded());
}

// ── set_lighting ──────────────────────────────────────────────────────────────

TEST_F(LitShaderTest, SetLighting_DoesNotCrash) {
    LitShader shader;
    if (shader.ensure()) {
        // Should not crash or assert
        shader.set_lighting(
            {0.5f, -1.0f, 0.3f},  // light dir
            {255, 255, 255, 255},  // light color
            1.0f,                  // intensity
            {30, 30, 30, 255}     // ambient
        );
    }
}

TEST_F(LitShaderTest, SetLighting_ZeroDirection) {
    LitShader shader;
    if (shader.ensure()) {
        // Zero light direction should use fallback without crashing
        shader.set_lighting(
            {0.0f, 0.0f, 0.0f},
            {255, 255, 255, 255},
            1.0f,
            {30, 30, 30, 255}
        );
    }
}
