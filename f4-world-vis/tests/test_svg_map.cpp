// test_svg_map.cpp — the SVG/HTML renderer.

#include <gtest/gtest.h>
#include <f4/vis/svg_map.hpp>

#include <string>
#include <vector>

using namespace f4::vis;

TEST(SvgMap, DefaultTeamColorsHasEightEntries) {
    auto colors = default_team_colors();
    EXPECT_EQ(colors.size(), 8u);
    EXPECT_EQ(colors[0].name, "Neutral");
    EXPECT_EQ(colors[1].name, "Enemy");
    EXPECT_EQ(colors[2].name, "Ally");
}

TEST(SvgMap, RenderSvgProducesValidXml) {
    std::vector<ObjectivePoint> objs = {
        {100, 200, 2, 0, 0, 10},
        {300, 400, 1, 0, 0, 20},
    };
    std::vector<UnitPoint> units = {
        {150, 250, 2, 0, 200, 300},
    };
    auto colors = default_team_colors();
    std::string svg = render_svg(objs, units, colors);
    EXPECT_NE(svg.find("<?xml"), std::string::npos);
    EXPECT_NE(svg.find("<svg"), std::string::npos);
    EXPECT_NE(svg.find("layer-objectives"), std::string::npos);
    EXPECT_NE(svg.find("layer-units"), std::string::npos);
    EXPECT_NE(svg.find("layer-legend"), std::string::npos);
    // The objective positions must appear in the output (as cx/cy attrs).
    EXPECT_NE(svg.find("cx=\"200\""), std::string::npos);  // 100 * 2.0 px/cell
}

TEST(SvgMap, RenderHtmlWrapsSvgWithToolbar) {
    std::vector<ObjectivePoint> objs = {{10, 20, 1, 0, 0, 5}};
    std::vector<UnitPoint> units;
    auto colors = default_team_colors();
    std::string html = render_html(objs, units, colors);
    EXPECT_NE(html.find("<!DOCTYPE html>"), std::string::npos);
    EXPECT_NE(html.find("<svg"), std::string::npos);
    EXPECT_NE(html.find("toggle('layer-objectives'"), std::string::npos);
    EXPECT_NE(html.find("Objectives (1)"), std::string::npos);
}

TEST(SvgMap, EmptyInputsProduceValidSvg) {
    auto colors = default_team_colors();
    std::string svg = render_svg({}, {}, colors);
    EXPECT_NE(svg.find("<svg"), std::string::npos);
    // Should still have the terrain placeholder and grid layers.
    EXPECT_NE(svg.find("layer-terrain"), std::string::npos);
    EXPECT_NE(svg.find("layer-grid"), std::string::npos);
}

TEST(SvgMap, UnitDestinationLineRenderedWhenPresent) {
    std::vector<ObjectivePoint> objs;
    std::vector<UnitPoint> units = {
        {100, 100, 1, 0, 200, 300},  // has destination
        {400, 400, 1, 0, 0, 0},       // no destination (0,0)
    };
    auto colors = default_team_colors();
    std::string svg = render_svg(objs, units, colors);
    // Dashed line for the unit with a destination.
    EXPECT_NE(svg.find("stroke-dasharray=\"2,2\""), std::string::npos);
}

TEST(SvgMap, GridSizeAndScaleRespected) {
    MapRendererConfig cfg;
    cfg.grid_size = 512;
    cfg.pixels_per_cell = 1.0;
    std::string svg = render_svg({}, {}, default_team_colors(), cfg);
    // SVG dimensions = grid_size * pixels_per_cell = 512.
    EXPECT_NE(svg.find("width=\"512\""), std::string::npos);
    EXPECT_NE(svg.find("height=\"512\""), std::string::npos);
}
