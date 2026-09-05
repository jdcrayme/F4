// f4-renderer/tests/test_symbol_library.cpp
//
// Unit tests for the runtime symbol system (symbol_library.hpp): the
// SymbolLibrary data model, earcut fill caches, the fallback square,
// and the lazy SVG-backed SymbolDirectory. No GPU context required
// (draw helpers are exercised at runtime by the world-viewer canvas).

#include <f4/renderer/symbol_library.hpp>
#include <f4/renderer/svg_import.hpp>

#include <imgui.h>  // ImVec2 (draw_imgui signature needs the full type)

#include <gtest/gtest.h>

#include <cmath>
#include <filesystem>
#include <fstream>

namespace {

using f4::renderer::SymbolColorRole;
using f4::renderer::SymbolDefinition;
using f4::renderer::SymbolDirectory;
using f4::renderer::SymbolLibrary;
using f4::renderer::make_fallback_square;
using f4::renderer::refresh_fill_caches;

double triangle_area_sum(
    const std::vector<std::array<f4::renderer::SymbolPoint, 3>>& tris) {
    double total = 0.0;
    for (const auto& t : tris) {
        total += std::fabs(
            static_cast<double>(t[1].x - t[0].x) * (t[2].y - t[0].y) -
            static_cast<double>(t[2].x - t[0].x) * (t[1].y - t[0].y)) * 0.5;
    }
    return total;
}

} // namespace

// ── SymbolLibrary — dictionary operations ─────────────────────────────────

TEST(SymbolLibrary, FindAddReplaceErase) {
    SymbolLibrary lib;
    EXPECT_TRUE(lib.empty());

    SymbolDefinition s;
    s.key = "a";
    lib.add_or_replace(s);
    EXPECT_EQ(1u, lib.size());
    ASSERT_NE(nullptr, lib.find("a"));
    EXPECT_EQ("a", lib.find("a")->key);
    EXPECT_EQ(nullptr, lib.find("missing"));

    s.display_name = "updated";
    lib.add_or_replace(s);  // replace in place
    EXPECT_EQ(1u, lib.size());
    EXPECT_EQ("updated", lib.find("a")->display_name);

    EXPECT_TRUE(lib.erase("a"));
    EXPECT_FALSE(lib.erase("a"));
    EXPECT_TRUE(lib.empty());
}

// ── refresh_fill_caches — the derived earcut fills ────────────────────────

TEST(RefreshFillCaches, ConvexPolygonKeepsFanFastPath) {
    SymbolDefinition def;
    f4::renderer::SymbolPolygon pg;
    pg.points = {{-0.5f, -0.5f}, {0.5f, -0.5f}, {0.5f, 0.5f}, {-0.5f, 0.5f}};
    def.polygons.push_back(pg);
    refresh_fill_caches(def);
    EXPECT_TRUE(def.polygons[0].triangles.empty());
}

TEST(RefreshFillCaches, ConcavePolygonTriangulates) {
    SymbolDefinition def;
    f4::renderer::SymbolPolygon pg;  // arrowhead — concave
    pg.points = {{0.0f, -0.8f}, {0.5f, 0.6f}, {0.0f, 0.2f}, {-0.5f, 0.6f}};
    def.polygons.push_back(pg);
    refresh_fill_caches(def);
    EXPECT_FALSE(def.polygons[0].triangles.empty());
    EXPECT_NEAR(triangle_area_sum(def.polygons[0].triangles), 0.52, 0.03);
}

TEST(RefreshFillCaches, HoledPolygonTriangulatesAroundTheHole) {
    SymbolDefinition def;
    f4::renderer::SymbolPolygon pg;
    pg.points = {{-0.8f, -0.8f}, {0.8f, -0.8f}, {0.8f, 0.8f}, {-0.8f, 0.8f}};
    pg.holes.push_back({{-0.2f, -0.2f}, {0.2f, -0.2f}, {0.2f, 0.2f}, {-0.2f, 0.2f}});
    def.polygons.push_back(pg);
    refresh_fill_caches(def);
    EXPECT_FALSE(def.polygons[0].triangles.empty());
    // Filled area = 2.56 - 0.16 = 2.40.
    EXPECT_NEAR(triangle_area_sum(def.polygons[0].triangles), 2.40, 0.1);
}

TEST(RefreshFillCaches, UnfilledPolygonsAreIgnored) {
    SymbolDefinition def;
    f4::renderer::SymbolPolygon pg;  // concave but unfilled
    pg.filled = false;
    pg.points = {{0.0f, -0.8f}, {0.5f, 0.6f}, {0.0f, 0.2f}, {-0.5f, 0.6f}};
    def.polygons.push_back(pg);
    refresh_fill_caches(def);
    EXPECT_TRUE(def.polygons[0].triangles.empty());
}

// ── make_fallback_square ──────────────────────────────────────────────────

TEST(FallbackSquare, IsAnUnfilledDataDefinedSquare) {
    SymbolDefinition fb = make_fallback_square();
    ASSERT_EQ(1u, fb.polygons.size());
    EXPECT_FALSE(fb.polygons[0].filled);
    EXPECT_EQ(4u, fb.polygons[0].points.size());
    refresh_fill_caches(fb);
    EXPECT_TRUE(fb.polygons[0].triangles.empty());  // outline only
}

// ── SymbolDirectory — lazy loading + fallback ─────────────────────────────

class SymbolDirectoryTest : public ::testing::Test {
protected:
    void SetUp() override {
        dir_ = std::filesystem::temp_directory_path() /
               ("f4_symbols_test_" +
                std::to_string(::testing::UnitTest::GetInstance()->random_seed()));
        std::error_code ec;
        std::filesystem::remove_all(dir_, ec);
        std::filesystem::create_directories(dir_, ec);
    }
    void TearDown() override {
        std::error_code ec;
        std::filesystem::remove_all(dir_, ec);
    }
    std::filesystem::path dir_;
};

TEST_F(SymbolDirectoryTest, LoadsSvgOnFirstRequest) {
    SymbolDefinition d;
    d.key = "probe";
    d.display_name = "Probe";
    f4::renderer::SymbolPolygon pg;
    pg.points = {{-0.5f, -0.5f}, {0.5f, -0.5f}, {0.5f, 0.5f}, {-0.5f, 0.5f}};
    d.polygons.push_back(pg);
    f4::renderer::save_symbol_as_svg(d, dir_ / "probe.svg");

    SymbolDirectory symbols(dir_);
    EXPECT_TRUE(symbols.library().empty());
    EXPECT_TRUE(symbols.failed_keys().empty());

    // Null ImGui draw list is a safe no-op — but it still triggers the load.
    symbols.draw_imgui("probe", nullptr, ImVec2{}, 32.0f, 0, 0);
    ASSERT_NE(nullptr, symbols.library().find("probe"));
    EXPECT_EQ("Probe", symbols.library().find("probe")->display_name);
    EXPECT_TRUE(symbols.failed_keys().empty());
}

TEST_F(SymbolDirectoryTest, MissingKeyFallsBackToSquareAndProbesOnce) {
    SymbolDirectory symbols(dir_);
    symbols.draw_imgui("no_such_symbol", nullptr, ImVec2{}, 32.0f, 0, 0);

    const SymbolDefinition* fb = symbols.library().find("no_such_symbol");
    ASSERT_NE(nullptr, fb);
    ASSERT_EQ(1u, fb->polygons.size());
    EXPECT_FALSE(fb->polygons[0].filled);          // the square outline
    ASSERT_EQ(1u, symbols.failed_keys().size());
    EXPECT_EQ("no_such_symbol", symbols.failed_keys()[0]);

    // Second request is served from the cache — no duplicate failure.
    symbols.draw_imgui("no_such_symbol", nullptr, ImVec2{}, 32.0f, 0, 0);
    EXPECT_EQ(1u, symbols.failed_keys().size());
}

TEST_F(SymbolDirectoryTest, UnparseableSvgFallsBackToSquare) {
    // A file that exists but violates the subset must not throw through
    // the draw path — it degrades to the square and is reported.
    {
        std::ofstream out(dir_ / "broken.svg", std::ios::binary);
        out << "<svg xmlns=\"http://www.w3.org/2000/svg\" viewBox=\"-1 -1 2 2\">"
               "<text>x</text></svg>";
    }
    SymbolDirectory symbols(dir_);
    symbols.draw_imgui("broken", nullptr, ImVec2{}, 32.0f, 0, 0);
    ASSERT_EQ(1u, symbols.failed_keys().size());
    EXPECT_EQ("broken", symbols.failed_keys()[0]);
    ASSERT_NE(nullptr, symbols.library().find("broken"));
    EXPECT_FALSE(symbols.library().find("broken")->polygons.empty());
}

TEST_F(SymbolDirectoryTest, NonexistentDirectoryAlwaysFallsBack) {
    SymbolDirectory symbols(dir_ / "does_not_exist");
    symbols.draw_imgui("anything", nullptr, ImVec2{}, 32.0f, 0, 0);
    EXPECT_EQ(1u, symbols.failed_keys().size());
    ASSERT_NE(nullptr, symbols.library().find("anything"));
}
