// test_svg_map.cpp — the SVG/HTML renderer (Falcon 4 aesthetic + terrain).

#include <gtest/gtest.h>
#include <f4/vis/svg_map.hpp>
#include <f4/terrain/terrain_data.hpp>

#include <algorithm>
#include <string>
#include <vector>

using namespace f4::vis;

TEST(SvgMap, DefaultTeamColorsHasEightEntries) {
    auto colors = default_team_colors();
    EXPECT_EQ(colors.size(), 8u);
    EXPECT_EQ(colors[1].name, "Enemy");
    EXPECT_EQ(colors[2].name, "Friendly");
    EXPECT_EQ(colors[5].name, "DPRK");
    EXPECT_EQ(colors[6].name, "PRC");
}

TEST(SvgMap, EnemyAndAllyColorsMatchFalconPalette) {
    auto colors = default_team_colors();
    EXPECT_EQ(colors[1].fill, "#E60000");   // enemy red
    EXPECT_EQ(colors[2].fill, "#00C5CD");   // friendly cyan
    EXPECT_EQ(colors[5].fill, "#E60000");   // DPRK = red
    EXPECT_EQ(colors[6].fill, "#E60000");   // PRC = red
    EXPECT_EQ(colors[3].fill, "#00C5CD");   // ROK = cyan
}

TEST(SvgMap, RenderSvgProducesValidXmlWithFalconLayers) {
    std::vector<ObjectivePoint> objs = {
        {100, 200, 2, 2128, 0, 10},
        {300, 400, 1, 0, 0, 20},
    };
    std::vector<UnitPoint> units = {{150, 250, 2, 0, 200, 300}};
    auto colors = default_team_colors();
    std::string svg = render_svg(objs, units, colors);
    EXPECT_NE(svg.find("<?xml"), std::string::npos);
    EXPECT_NE(svg.find("<svg"), std::string::npos);
    EXPECT_NE(svg.find("layer-water"), std::string::npos);
    EXPECT_NE(svg.find("layer-landmask"), std::string::npos);
    EXPECT_NE(svg.find("layer-objectives"), std::string::npos);
    EXPECT_NE(svg.find(FalconPalette::WATER), std::string::npos);
}

TEST(SvgMap, RenderHtmlWrapsSvgWithToolbar) {
    std::vector<ObjectivePoint> objs = {{10, 20, 1, 0, 0, 5}};
    std::vector<UnitPoint> units;
    auto colors = default_team_colors();
    std::string html = render_html(objs, units, colors);
    EXPECT_NE(html.find("<!DOCTYPE html>"), std::string::npos);
    EXPECT_NE(html.find("<svg"), std::string::npos);
    EXPECT_NE(html.find("toggle('layer-water'"), std::string::npos);
    EXPECT_NE(html.find("Objectives (1)"), std::string::npos);
}

TEST(SvgMap, EmptyInputsProduceValidSvg) {
    auto colors = default_team_colors();
    std::string svg = render_svg({}, {}, colors);
    EXPECT_NE(svg.find("<svg"), std::string::npos);
    EXPECT_NE(svg.find("layer-water"), std::string::npos);
    EXPECT_NE(svg.find("layer-landmask"), std::string::npos);
}

TEST(SvgMap, GridOffByDefault) {
    MapRendererConfig cfg;
    EXPECT_FALSE(cfg.show_grid);
}

TEST(SvgMap, UnitDestinationLineRenderedWhenPresent) {
    std::vector<ObjectivePoint> objs;
    std::vector<UnitPoint> units = {
        {100, 100, 1, 0, 200, 300},
    };
    auto colors = default_team_colors();
    std::string svg = render_svg(objs, units, colors);
    EXPECT_NE(svg.find("stroke-dasharray"), std::string::npos);
}

TEST(SvgMap, AirbaseIconUsesRunwayShape) {
    std::vector<ObjectivePoint> objs = {
        {500, 500, 2, 2150, 0, 25},   // likely airbase
    };
    auto colors = default_team_colors();
    std::string svg = render_svg(objs, {}, colors);
    EXPECT_NE(svg.find("transform=\"translate("), std::string::npos);
}

TEST(SvgMap, TerrainRenderProducesTerrainLayer) {
    // With a TerrainData, render_svg_with_terrain emits a layer-terrain
    // with one rect per elevation cell.
    f4::terrain::TerrainData td;
    td.header.width = 4;
    td.header.height = 4;
    td.elevation.resize(16, 0);     // all water
    td.elevation[0] = 1000;          // one land cell
    td.overlay.resize(16, 0);
    std::vector<ObjectivePoint> objs;
    std::vector<UnitPoint> units;
    auto colors = default_team_colors();
    std::string svg = render_svg_with_terrain(objs, units, colors, td);
    EXPECT_NE(svg.find("layer-terrain"), std::string::npos);
    // 4x4 = 16 terrain tiles.
    int terrain_rects = 0;
    std::string::size_type pos = svg.find("layer-terrain");
    if (pos != std::string::npos) {
        std::string::size_type end = svg.find("</g>", pos);
        terrain_rects = static_cast<int>(std::count(svg.begin() + pos,
                                                     svg.begin() + end, '<'));
    }
    EXPECT_GE(terrain_rects, 16);
}
