// f4-renderer/tests/test_texture_cache.cpp
//
// Unit tests for TextureCache. Requires a Raylib GPU context (InitWindow)
// because TextureCache calls LoadTextureFromImage.

#include <f4/renderer/texture_cache.hpp>

#include <gtest/gtest.h>
#include <raylib.h>

using namespace f4::renderer;

class TextureCacheTest : public ::testing::Test {
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

bool TextureCacheTest::initialized_ = false;

// ── Construction ──────────────────────────────────────────────────────────────

TEST_F(TextureCacheTest, DefaultConstruction_Empty) {
    TextureCache cache;
    EXPECT_FALSE(cache.contains(0));
    EXPECT_FALSE(cache.contains(1));
    EXPECT_EQ(cache.lookup(0), nullptr);
}

TEST_F(TextureCacheTest, HasAlpha_NotCached_ReturnsFalse) {
    TextureCache cache;
    EXPECT_FALSE(cache.has_alpha(0));
    EXPECT_FALSE(cache.has_alpha(99));
}

// ── Lookup ────────────────────────────────────────────────────────────────────

TEST_F(TextureCacheTest, Lookup_Uncached_ReturnsNull) {
    TextureCache cache;
    EXPECT_EQ(cache.lookup(42), nullptr);
}

// ── Destructor ────────────────────────────────────────────────────────────────

TEST_F(TextureCacheTest, Destructor_DoesNotCrash) {
    {
        TextureCache cache;
        // Let it go out of scope — should not crash even if empty
    }
    SUCCEED();
}

// ── unload_all ────────────────────────────────────────────────────────────────

TEST_F(TextureCacheTest, UnloadAll_EmptyCache) {
    TextureCache cache;
    cache.unload_all(); // should not crash
    SUCCEED();
}
