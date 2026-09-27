// f4-renderer/tests/test_symbol_draw.cpp
//
// PIXEL tests for the data-driven symbol draw path. These need a real GL
// context: each test draws into a render texture and reads the result
// back, asserting that FILLS actually rasterize. This is the net that the
// Symbol Creator's removal orphaned — the library raylib path had never
// been exercised on a canvas before the SYMBOL-SVG-2 wiring, and its
// first live run drew every icon as unfilled strokes.
//
// Probe discipline: the read point must sit INSIDE a filled region and
// away from the outline strokes (obj_city's building seams pass exactly
// through the symbol center — the first cut of this test probed (0, 0)
// and read the stroke, poisoning itself). Render texture + pixel readback
// is deterministic; the screen back-buffer read races the frame swap.
// The FBO's origin is bottom-left, so rows are mirrored on readback.

#include <f4/renderer/svg_import.hpp>
#include <f4/renderer/symbol_library.hpp>
#include <f4/renderer/symbols.hpp>

#include <gtest/gtest.h>
#include <raylib.h>

#include <cmath>
#include <cstdlib>
#include <filesystem>

#include "display_guard.hpp"

namespace {

constexpr int kW = 128;
constexpr int kH = 128;
constexpr int kCenter = 64;

bool near_rgb(Color a, Color b, int tol) {
    return std::abs(a.r - b.r) <= tol && std::abs(a.g - b.g) <= tol &&
           std::abs(a.b - b.b) <= tol;
}

// Draw `key` from `lib` centered over black at 64 px; return the number
// of FILL-dominant pixels (green channel well above the red outline's).
// Placement-independent: single-pixel probes kept landing on the glyph
// and seam strokes, which cross the symbol interior.
int count_fill_pixels(const f4::renderer::SymbolLibrary& lib,
                      const char* key) {
    const RenderTexture rt = LoadRenderTexture(kW, kH);
    BeginTextureMode(rt);
    ClearBackground(BLACK);
    f4::renderer::draw_library_symbol(lib, key, kCenter, kCenter, 64.0f,
                                      {0, 255, 0, 255}, {255, 0, 0, 255});
    EndTextureMode();
    const Image img = LoadImageFromTexture(rt.texture);
    int fills = 0;
    for (int y = 0; y < kH; ++y) {
        for (int x = 0; x < kW; ++x) {
            const Color c = GetImageColor(img, x, y);
            if (c.g > 150 && c.r < 120 && c.b < 80) ++fills;
        }
    }
    UnloadImage(img);
    UnloadRenderTexture(rt);
    return fills;
}

class SymbolDrawTest : public ::testing::Test {
protected:
    void SetUp() override {
        if (!initialized_) {
            if (!f4::testing::init_window_if_display(kW, kH, "test_symbol_draw",
                                                     FLAG_WINDOW_HIDDEN)) {
                GTEST_SKIP() << "no display available — GPU-context test skipped";
            }
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

bool SymbolDrawTest::initialized_ = false;

// The corpus library fills its shapes: obj_city's three buildings at
// 64 px cover ~1600 px; a fill-less render (outlines only) leaves
// almost none. The threshold is a fraction of the filled area, so the
// check is placement-independent and AA-tolerant.
TEST_F(SymbolDrawTest, LibraryObjectiveFills) {
    const f4::renderer::SymbolLibrary lib =
        f4::renderer::load_symbol_library(F4_SYMBOLS_JSON_PATH);
    const int fills = count_fill_pixels(lib, "obj_city");
    EXPECT_GT(fills, 400)
        << "library obj_city rendered " << fills
        << " fill pixels - the polygon fills did not rasterize";
}

// The VIEWER's exact load: corpus merged with the checked-in symbols/
// SVG overrides. If the overrides lose fills, this catches it (the
// merge test alone only checks keys).
TEST_F(SymbolDrawTest, ViewerMergedLibraryFills) {
    const auto corpus = std::filesystem::path(F4_SYMBOLS_JSON_PATH);
    auto lib = f4::renderer::load_symbol_library(corpus);
    const auto overrides = corpus.parent_path() / "symbols";
    ASSERT_GT(f4::renderer::merge_symbol_svg_directory(lib, overrides), 0u)
        << "the checked-in symbols/ overrides did not merge";
    const int fills = count_fill_pixels(lib, "obj_city");
    EXPECT_GT(fills, 400)
        << "merged obj_city rendered " << fills
        << " fill pixels - the polygon fills did not rasterize";
}

// A composed unit kind through the same path: the fighter's squadron
// frame (a filled circle, ~800 px at 64 px) plus glyph strokes.
TEST_F(SymbolDrawTest, LibraryUnitFills) {
    const f4::renderer::SymbolLibrary lib =
        f4::renderer::load_symbol_library(F4_SYMBOLS_JSON_PATH);
    const int fills = count_fill_pixels(lib, "unit_fighter");
    EXPECT_GT(fills, 400)
        << "library unit_fighter rendered " << fills
        << " fill pixels - the frame fill did not rasterize";
}

} // namespace
