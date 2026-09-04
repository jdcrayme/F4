// f4-renderer/tests/test_svg_import.cpp
//
// SVG (authoring format) <-> SymbolDefinition tests:
//   - subset parsing: shapes, styles, transforms, path commands
//   - geometry: viewBox normalization, curve flattening, evenodd holes
//   - earcut fill caches (concave + holed polygons triangulate)
//   - loud failure on out-of-subset elements/attributes
//   - export/import round-trips, including the full f4_symbols.json corpus
//
// Pure data tests — no GPU context (draw helpers are exercised by the
// Symbol Creator tool).

#include <f4/renderer/svg_import.hpp>

#include <gtest/gtest.h>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <vector>

namespace {

using f4::renderer::SymbolColorRole;
using f4::renderer::SymbolDefinition;
using f4::renderer::SymbolPoint;
using f4::renderer::SymbolPolygon;
using f4::renderer::SymbolPolyline;
using f4::renderer::import_symbol_from_svg_string;
using f4::renderer::refresh_fill_caches;
using f4::renderer::symbol_to_svg;

[[nodiscard]] std::string svg_doc(const std::string& body) {
    return "<svg xmlns=\"http://www.w3.org/2000/svg\" viewBox=\"-1 -1 2 2\">" +
           body + "</svg>";
}

[[nodiscard]] bool points_near(const std::vector<f4::renderer::SymbolPoint>& a,
                               const std::vector<f4::renderer::SymbolPoint>& b,
                               float tol) {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (std::fabs(a[i].x - b[i].x) > tol || std::fabs(a[i].y - b[i].y) > tol) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] double triangle_area_sum(
    const std::vector<std::array<f4::renderer::SymbolPoint, 3>>& tris) {
    double total = 0.0;
    for (const auto& t : tris) {
        total += std::fabs(
            static_cast<double>(t[1].x - t[0].x) * (t[2].y - t[0].y) -
            static_cast<double>(t[2].x - t[0].x) * (t[1].y - t[0].y)) * 0.5;
    }
    return total;
}

// ---------------------------------------------------------------------------

TEST(SvgImport, RectNormalizesThroughViewBox) {
    const SymbolDefinition def = import_symbol_from_svg_string(
        "<svg xmlns=\"http://www.w3.org/2000/svg\" viewBox=\"0 0 100 100\">"
        "<rect x=\"25\" y=\"25\" width=\"50\" height=\"50\" fill=\"currentColor\"/>"
        "</svg>",
        "rect_test");

    ASSERT_EQ(def.polygons.size(), 1u);
    const auto& pg = def.polygons[0];
    ASSERT_EQ(pg.points.size(), 4u);
    EXPECT_TRUE(pg.filled);
    EXPECT_EQ(pg.color_role, SymbolColorRole::Fill);
    EXPECT_TRUE(points_near(pg.points,
                            {{-0.5f, -0.5f}, {0.5f, -0.5f}, {0.5f, 0.5f}, {-0.5f, 0.5f}},
                            1e-4f));
    // Convex + hole-free: fan fast path, no triangles needed.
    EXPECT_TRUE(pg.triangles.empty());
}

TEST(SvgImport, CircleFlattensToThirtyTwoGon) {
    const SymbolDefinition def = import_symbol_from_svg_string(
        svg_doc("<circle cx=\"0\" cy=\"0\" r=\"0.9\"/>"), "circle_test");

    ASSERT_EQ(def.polygons.size(), 1u);
    const auto& pg = def.polygons[0];
    EXPECT_EQ(pg.points.size(), 32u);
    // Absent fill attribute means Fill role (documented deviation from
    // SVG's black default).
    EXPECT_EQ(pg.color_role, SymbolColorRole::Fill);
    for (const auto& p : pg.points) {
        EXPECT_NEAR(std::hypot(p.x, p.y), 0.9, 1e-3);
    }
}

TEST(SvgImport, StrokedPathBecomesPolylineWithConvertedWidth) {
    const SymbolDefinition def = import_symbol_from_svg_string(
        svg_doc("<path d=\"M -0.5 -0.5 L 0.5 0.5\" fill=\"none\" "
                "stroke=\"#000000\" stroke-width=\"0.1\"/>"),
        "stroke_test");

    ASSERT_EQ(def.polylines.size(), 1u);
    const auto& pl = def.polylines[0];
    EXPECT_FALSE(pl.closed);
    EXPECT_EQ(pl.color_role, SymbolColorRole::Outline);
    // 0.1 viewBox units * scale 1.0 * (64/2) reference px = 3.2 px.
    EXPECT_NEAR(pl.width, 3.2f, 1e-3);
    ASSERT_EQ(pl.points.size(), 2u);
}

TEST(SvgImport, TransformComposesAcrossGroups) {
    const SymbolDefinition def = import_symbol_from_svg_string(
        svg_doc("<g transform=\"translate(0.5,0)\">"
                "<g transform=\"scale(0.5)\">"
                "<rect x=\"0\" y=\"0\" width=\"1\" height=\"1\"/>"
                "</g></g>"),
        "xform_test");

    ASSERT_EQ(def.polygons.size(), 1u);
    EXPECT_TRUE(points_near(def.polygons[0].points,
                            {{0.5f, 0.0f}, {1.0f, 0.0f}, {1.0f, 0.5f}, {0.5f, 0.5f}},
                            1e-4f));
}

TEST(SvgImport, CurvesFlattenToSegmentCounts) {
    // Cubic: start point + 16 flattened samples.
    {
        const SymbolDefinition def = import_symbol_from_svg_string(
            svg_doc("<path d=\"M -0.8 0 C -0.8 0.4 -0.4 0.8 0 0.8\" "
                    "fill=\"none\" stroke=\"black\"/>"),
            "cubic_test");
        ASSERT_EQ(def.polylines.size(), 1u);
        EXPECT_EQ(def.polylines[0].points.size(), 17u);
        EXPECT_NEAR(def.polylines[0].points.back().x, 0.0, 1e-3);
        EXPECT_NEAR(def.polylines[0].points.back().y, 0.8, 1e-3);
    }
    // Arc: start + 16 samples, landing on the endpoint, on-radius.
    {
        const SymbolDefinition def = import_symbol_from_svg_string(
            svg_doc("<path d=\"M -0.5 0 A 0.5 0.5 0 0 1 0.5 0\" "
                    "fill=\"none\" stroke=\"black\"/>"),
            "arc_test");
        ASSERT_EQ(def.polylines.size(), 1u);
        const auto& pts = def.polylines[0].points;
        EXPECT_EQ(pts.size(), 17u);
        EXPECT_NEAR(pts.back().x, 0.5, 1e-3);
        EXPECT_NEAR(pts.back().y, 0.0, 1e-3);
        EXPECT_NEAR(std::hypot(pts[8].x, pts[8].y), 0.5, 0.02);
    }
    // Relative commands + implicit repetition: m, l, lowercase z.
    {
        const SymbolDefinition def = import_symbol_from_svg_string(
            svg_doc("<path d=\"m -0.5 -0.5 l 1 0 0 1 -1 0 z\" "
                    "fill=\"currentColor\"/>"),
            "rel_test");
        ASSERT_EQ(def.polygons.size(), 1u);
        EXPECT_TRUE(points_near(def.polygons[0].points,
                                {{-0.5f, -0.5f}, {0.5f, -0.5f}, {0.5f, 0.5f}, {-0.5f, 0.5f}},
                                1e-4f));
    }
}

TEST(SvgImport, EvenoddDonutTriangulates) {
    const SymbolDefinition def = import_symbol_from_svg_string(
        svg_doc("<path fill-rule=\"evenodd\" fill=\"currentColor\" d=\""
                "M -0.9 -0.9 L 0.9 -0.9 L 0.9 0.9 L -0.9 0.9 Z "
                "M -0.45 -0.45 L 0.45 -0.45 L 0.45 0.45 L -0.45 0.45 Z\"/>"),
        "donut_test");

    ASSERT_EQ(def.polygons.size(), 1u);
    const auto& pg = def.polygons[0];
    ASSERT_EQ(pg.points.size(), 4u);
    ASSERT_EQ(pg.holes.size(), 1u);
    ASSERT_EQ(pg.holes[0].size(), 4u);
    ASSERT_FALSE(pg.triangles.empty());
    // Filled area = outer (1.8^2) minus hole (0.9^2) = 2.43.
    EXPECT_NEAR(triangle_area_sum(pg.triangles), 2.43, 0.15);
}

TEST(SvgImport, DisjointLoopsBecomeSeparatePolygons) {
    const SymbolDefinition def = import_symbol_from_svg_string(
        svg_doc("<path fill=\"currentColor\" d=\""
                "M -0.9 -0.4 L -0.1 -0.4 L -0.1 0.4 L -0.9 0.4 Z "
                "M 0.1 -0.4 L 0.9 -0.4 L 0.9 0.4 L 0.1 0.4 Z\"/>"),
        "two_test");

    ASSERT_EQ(def.polygons.size(), 2u);
    EXPECT_TRUE(def.polygons[0].holes.empty());
    EXPECT_TRUE(def.polygons[1].holes.empty());
}

TEST(SvgImport, DataColorRoleOverridesPaintMapping) {
    const SymbolDefinition def = import_symbol_from_svg_string(
        svg_doc("<rect x=\"-0.5\" y=\"-0.5\" width=\"1\" height=\"1\" "
                "fill=\"currentColor\" data-color-role=\"fill_blend\"/>"),
        "role_test");
    ASSERT_EQ(def.polygons.size(), 1u);
    EXPECT_EQ(def.polygons[0].color_role, SymbolColorRole::FillBlend);
}

TEST(SvgImport, UnsupportedElementFailsByName) {
    try {
        (void)import_symbol_from_svg_string(
            svg_doc("<text x=\"0\" y=\"0\">hello</text>"), "bad");
        FAIL() << "expected throw";
    } catch (const std::runtime_error& e) {
        EXPECT_NE(std::string(e.what()).find("<text>"), std::string::npos);
    }
}

TEST(SvgImport, DangerousAttributesFailLoudly) {
    // filter is always outside the subset.
    try {
        (void)import_symbol_from_svg_string(
            svg_doc("<rect x=\"0\" y=\"0\" width=\"1\" height=\"1\" "
                    "filter=\"url(#blur)\"/>"),
            "bad");
        FAIL() << "expected throw";
    } catch (const std::runtime_error& e) {
        EXPECT_NE(std::string(e.what()).find("filter"), std::string::npos);
    }
    // fill-opacity="0.5" changes rendering -> fail; "1" is identity -> ok.
    try {
        (void)import_symbol_from_svg_string(
            svg_doc("<rect x=\"0\" y=\"0\" width=\"1\" height=\"1\" "
                    "fill-opacity=\"0.5\"/>"),
            "bad");
        FAIL() << "expected throw";
    } catch (const std::runtime_error& e) {
        EXPECT_NE(std::string(e.what()).find("fill-opacity"), std::string::npos);
    }
    EXPECT_NO_THROW({
        (void)import_symbol_from_svg_string(
            svg_doc("<rect x=\"0\" y=\"0\" width=\"1\" height=\"1\" "
                    "fill-opacity=\"1\" stroke-opacity=\"1\" display=\"inline\"/>"),
            "ok");
    });
}

TEST(SvgImport, MissingViewBoxFails) {
    try {
        (void)import_symbol_from_svg_string(
            "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"100\" height=\"100\">"
            "<rect x=\"0\" y=\"0\" width=\"10\" height=\"10\"/></svg>",
            "bad");
        FAIL() << "expected throw";
    } catch (const std::runtime_error& e) {
        EXPECT_NE(std::string(e.what()).find("viewBox"), std::string::npos);
    }
}

TEST(SvgImport, TitleAndDescBecomeMetadata) {
    const SymbolDefinition def = import_symbol_from_svg_string(
        svg_doc("<title>Bridge</title><desc>Road bridge symbol</desc>"
                "<rect x=\"-0.5\" y=\"-0.5\" width=\"1\" height=\"1\"/>"),
        "meta_test");
    EXPECT_EQ(def.display_name, "Bridge");
    EXPECT_EQ(def.description, "Road bridge symbol");
}

// ---------------------------------------------------------------------------
// Round-trips
// ---------------------------------------------------------------------------

TEST(SvgRoundTrip, SyntheticSymbolPreservesGeometry) {
    SymbolDefinition d;
    d.key = "rt";
    d.display_name = "Round Trip";
    d.description = "all primitive kinds";

    // Filled octagon-ish shape with a hole, FillBlend role.
    SymbolPolygon filled;
    filled.color_role = SymbolColorRole::FillBlend;
    filled.points = {{-0.8f, -0.6f}, {0.8f, -0.6f}, {0.8f, 0.6f}, {-0.8f, 0.6f}};
    filled.holes.push_back({{-0.3f, -0.2f}, {0.3f, -0.2f}, {0.3f, 0.2f}, {-0.3f, 0.2f}});
    d.polygons.push_back(filled);

    // Unfilled triangle -> round-trips as a closed 1px Outline polyline
    // (the model's fixed-width outline representation).
    SymbolPolygon unfilled;
    unfilled.filled = false;
    unfilled.points = {{0.0f, -0.7f}, {0.6f, 0.4f}, {-0.6f, 0.4f}};
    d.polygons.push_back(unfilled);

    SymbolPolyline open_line;
    open_line.points = {{-0.9f, 0.9f}, {0.0f, 0.95f}};
    open_line.width = 2.0f;
    d.polylines.push_back(open_line);  // default role: Outline

    SymbolPolyline closed_line;
    closed_line.points = {{-0.2f, 0.0f}, {0.2f, 0.0f}, {0.0f, 0.3f}};
    closed_line.width = 1.5f;
    closed_line.closed = true;
    closed_line.color_role = SymbolColorRole::Fill;
    d.polylines.push_back(closed_line);

    refresh_fill_caches(d);

    const SymbolDefinition back =
        import_symbol_from_svg_string(symbol_to_svg(d), "rt");

    EXPECT_EQ(back.display_name, "Round Trip");
    EXPECT_EQ(back.description, "all primitive kinds");

    // Filled polygons map 1:1 with roles + holes.
    ASSERT_EQ(back.polygons.size(), 1u);
    EXPECT_TRUE(points_near(back.polygons[0].points, filled.points, 1e-3));
    EXPECT_EQ(back.polygons[0].color_role, SymbolColorRole::FillBlend);
    ASSERT_EQ(back.polygons[0].holes.size(), 1u);
    EXPECT_TRUE(points_near(back.polygons[0].holes[0], filled.holes[0], 1e-3));
    EXPECT_FALSE(back.polygons[0].triangles.empty());  // holed -> earcut path

    // Polylines: mapped unfilled polygon first (exporter order), then the
    // original polylines in order.
    ASSERT_EQ(back.polylines.size(), 3u);
    EXPECT_TRUE(back.polylines[0].closed);
    EXPECT_NEAR(back.polylines[0].width, 1.0f, 1e-2);
    EXPECT_EQ(back.polylines[0].color_role, SymbolColorRole::Outline);
    EXPECT_TRUE(points_near(back.polylines[0].points, unfilled.points, 1e-3));

    EXPECT_TRUE(points_near(back.polylines[1].points, open_line.points, 1e-3));
    EXPECT_NEAR(back.polylines[1].width, 2.0f, 1e-2);
    EXPECT_FALSE(back.polylines[1].closed);

    EXPECT_TRUE(points_near(back.polylines[2].points, closed_line.points, 1e-3));
    EXPECT_NEAR(back.polylines[2].width, 1.5f, 1e-2);
    EXPECT_TRUE(back.polylines[2].closed);
    EXPECT_EQ(back.polylines[2].color_role, SymbolColorRole::Fill);
}

TEST(SvgRoundTrip, FullCorpusOfFSymbols) {
    const std::filesystem::path corpus_path = F4_SYMBOLS_JSON_PATH;
    if (!std::filesystem::exists(corpus_path)) {
        GTEST_SKIP() << "f4_symbols.json not found at " << corpus_path;
    }
    const f4::renderer::SymbolLibrary lib =
        f4::renderer::load_symbol_library(corpus_path);
    ASSERT_GE(lib.symbols().size(), 70u);

    int exported = 0;
    for (const auto& s : lib.symbols()) {
        if (s.polygons.empty() && s.polylines.empty()) continue;
        ++exported;

        const SymbolDefinition back =
            import_symbol_from_svg_string(symbol_to_svg(s), s.key);

        if (std::getenv("F4_SVG_DEBUG") && std::getenv("F4_SVG_DEBUG")[0] == '1') {
            fprintf(stderr, "=== %s ===\n%s", s.key.c_str(), symbol_to_svg(s).c_str());
            for (std::size_t i = 0; i < s.polygons.size(); ++i) {
                fprintf(stderr, "orig poly[%zu] filled=%d role=%d npts=%zu holes=%zu first=(%.3f,%.3f)\n",
                        i, (int)s.polygons[i].filled, (int)s.polygons[i].color_role,
                        s.polygons[i].points.size(), s.polygons[i].holes.size(),
                        s.polygons[i].points.empty() ? 0.f : s.polygons[i].points[0].x,
                        s.polygons[i].points.empty() ? 0.f : s.polygons[i].points[0].y);
            }
            for (std::size_t i = 0; i < s.polylines.size(); ++i) {
                fprintf(stderr, "orig line[%zu] closed=%d role=%d w=%.3f npts=%zu first=(%.3f,%.3f)\n",
                        i, (int)s.polylines[i].closed, (int)s.polylines[i].color_role,
                        s.polylines[i].width, s.polylines[i].points.size(),
                        s.polylines[i].points.empty() ? 0.f : s.polylines[i].points[0].x,
                        s.polylines[i].points.empty() ? 0.f : s.polylines[i].points[0].y);
            }
            for (std::size_t i = 0; i < back.polygons.size(); ++i) {
                fprintf(stderr, "back poly[%zu] filled=%d role=%d npts=%zu holes=%zu first=(%.3f,%.3f)\n",
                        i, (int)back.polygons[i].filled, (int)back.polygons[i].color_role,
                        back.polygons[i].points.size(), back.polygons[i].holes.size(),
                        back.polygons[i].points.empty() ? 0.f : back.polygons[i].points[0].x,
                        back.polygons[i].points.empty() ? 0.f : back.polygons[i].points[0].y);
            }
            for (std::size_t i = 0; i < back.polylines.size(); ++i) {
                fprintf(stderr, "back line[%zu] closed=%d role=%d w=%.3f npts=%zu first=(%.3f,%.3f)\n",
                        i, (int)back.polylines[i].closed, (int)back.polylines[i].color_role,
                        back.polylines[i].width, back.polylines[i].points.size(),
                        back.polylines[i].points.empty() ? 0.f : back.polylines[i].points[0].x,
                        back.polylines[i].points.empty() ? 0.f : back.polylines[i].points[0].y);
            }
        }

        // Filled polygons survive 1:1 (points, roles, holes).
        std::size_t filled_count = 0;
        for (const auto& pg : s.polygons) {
            if (pg.filled) ++filled_count;
        }
        ASSERT_EQ(back.polygons.size(), filled_count) << "symbol " << s.key;
        std::size_t fi = 0;
        for (const auto& pg : s.polygons) {
            if (!pg.filled) continue;
            const auto& got = back.polygons[fi++];
            EXPECT_TRUE(points_near(got.points, pg.points, 1e-3)) << s.key;
            EXPECT_EQ(got.color_role, pg.color_role) << s.key;
            ASSERT_EQ(got.holes.size(), pg.holes.size()) << s.key;
            for (std::size_t h = 0; h < pg.holes.size(); ++h) {
                EXPECT_TRUE(points_near(got.holes[h], pg.holes[h], 1e-3)) << s.key;
            }
        }

        // Unfilled polygons become closed 1px Outline polylines; original
        // polylines follow in order.
        std::size_t unfilled_count = 0;
        for (const auto& pg : s.polygons) {
            if (!pg.filled) ++unfilled_count;
        }
        ASSERT_EQ(back.polylines.size(), unfilled_count + s.polylines.size())
            << "symbol " << s.key;
        std::size_t pi = 0;
        for (const auto& pg : s.polygons) {
            if (pg.filled) continue;  // only the unfilled ones map to polylines
            const auto& got = back.polylines[pi++];
            EXPECT_TRUE(got.closed) << s.key;
            EXPECT_NEAR(got.width, 1.0f, 1e-2) << s.key;
            EXPECT_EQ(got.color_role, SymbolColorRole::Outline) << s.key;
            EXPECT_TRUE(points_near(got.points, pg.points, 1e-3)) << s.key;
        }
        for (const auto& pl : s.polylines) {
            const auto& got = back.polylines[pi++];
            EXPECT_TRUE(points_near(got.points, pl.points, 1e-3)) << s.key;
            EXPECT_NEAR(got.width, pl.width, 1e-2) << s.key;
            EXPECT_EQ(got.closed, pl.closed) << s.key;
            EXPECT_EQ(got.color_role, pl.color_role) << s.key;
        }
    }
    EXPECT_GT(exported, 60);  // corpus is ~75 symbols; all should carry geometry
}

TEST(SvgRoundTrip, JsonIOCarriesColorRolesAndHoles) {
    // The JSON side of the v2 fields (the corpus's schema).
    const std::string json = R"({
      "version": 2,
      "symbols": [ {
        "key": "holed", "display_name": "Holed", "category": "t", "description": "",
        "polylines": [],
        "polygons": [ {
          "filled": true, "color_role": "fill_blend",
          "points": [ {"x": -0.8, "y": -0.8}, {"x": 0.8, "y": -0.8},
                      {"x": 0.8, "y": 0.8}, {"x": -0.8, "y": 0.8} ],
          "holes": [ [ {"x": -0.2, "y": -0.2}, {"x": 0.2, "y": -0.2},
                       {"x": 0.2, "y": 0.2}, {"x": -0.2, "y": 0.2} ] ]
        } ]
      } ]
    })";
    const f4::renderer::SymbolLibrary lib =
        f4::renderer::load_symbol_library_from_string(json);
    const SymbolDefinition* def = lib.find("holed");
    ASSERT_NE(def, nullptr);
    EXPECT_EQ(def->polygons[0].color_role, SymbolColorRole::FillBlend);
    ASSERT_EQ(def->polygons[0].holes.size(), 1u);
    EXPECT_FALSE(def->polygons[0].triangles.empty());  // loader refreshed caches

    // And it serializes back out.
    const std::string out = f4::renderer::symbol_library_to_json(lib);
    EXPECT_NE(out.find("\"color_role\": \"fill_blend\""), std::string::npos);
    EXPECT_NE(out.find("\"holes\""), std::string::npos);
}

} // namespace
