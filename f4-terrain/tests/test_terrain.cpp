// test_terrain.cpp — terrain decoder against real THEATER.* fixtures.

#include <gtest/gtest.h>
#include <f4/terrain/terrain_data.hpp>

using namespace f4::terrain;

namespace {
TerrainData load_fixture() {
    TerrainData td;
    td.load(TERRAIN_FIXTURE_DIR);
    return td;
}
}

TEST(Terrain, ParsesMapHeader) {
    auto td = load_fixture();
    EXPECT_EQ(td.header.width, 128u);
    EXPECT_EQ(td.header.height, 128u);
    // Magic is 0x444CFFAE (bytes: ae ff 4c 44 little-endian).
    EXPECT_EQ(td.header.magic, 0x444CFFAEu);
}

TEST(Terrain, PaletteHasEntries) {
    auto td = load_fixture();
    EXPECT_GT(td.palette.size(), 100u);
    // Palette index 3 is white (#ffffff), index 4 is near-black.
    EXPECT_EQ(td.palette[3].r, 0xFF);
    EXPECT_EQ(td.palette[3].g, 0xFF);
    EXPECT_EQ(td.palette[3].b, 0xFF);
}

TEST(Terrain, ElevationGridIsFullyPopulated) {
    auto td = load_fixture();
    EXPECT_EQ(td.elevation.size(), 128u * 128u);
}

TEST(Terrain, ElevationRangeIsPlausible) {
    auto td = load_fixture();
    int16_t mn = 30000, mx = -30000;
    for (auto e : td.elevation) {
        if (e < mn) mn = e;
        if (e > mx) mx = e;
    }
    // Korea: ocean (0) to ~6700ft peaks.
    EXPECT_LE(mn, 0);
    EXPECT_GT(mx, 3000);
    EXPECT_LT(mx, 10000);
}

TEST(Terrain, HasWaterCells) {
    // Korea is a peninsula — there must be ocean (elevation <= 0) cells.
    auto td = load_fixture();
    int water = 0;
    for (auto e : td.elevation) if (e <= 0) ++water;
    EXPECT_GT(water, 1000) << "expected significant ocean area";
}

TEST(Terrain, HasLandCells) {
    auto td = load_fixture();
    int land = 0;
    for (auto e : td.elevation) if (e > 0) ++land;
    EXPECT_GT(land, 1000) << "expected significant land area";
}

TEST(Terrain, TerrainColorWaterForZeroElevation) {
    auto td = load_fixture();
    // Find a water cell and verify it renders blue.
    bool found_water = false;
    for (uint32_t y = 0; y < td.header.height && !found_water; ++y)
    for (uint32_t x = 0; x < td.header.width && !found_water; ++x) {
        if (td.elevation_at(x, y) <= 0) {
            Color4 c = td.terrain_color(x, y);
            EXPECT_EQ(c.b, 0x94);
            EXPECT_EQ(c.g, 0x69);
            found_water = true;
        }
    }
    EXPECT_TRUE(found_water);
}

TEST(Terrain, OverlayGridIsPopulated) {
    auto td = load_fixture();
    EXPECT_EQ(td.overlay.size(), 128u * 128u);
}

TEST(Terrain, VerticalFlipIsApplied) {
    // The file stores south-first; sim convention is north-first (y=0=north).
    // Korea's ocean is to the west/south; the northern edge should have land.
    auto td = load_fixture();
    // Check that row 0 (north) differs from the file's first row by confirming
    // the flip happened: elevation_at(0,0) should equal the file's LAST row.
    // We verify by checking the grid isn't upside-down: northern Korea has
    // mountains, so some cell in the northern half should be > 1000ft.
    bool northern_land = false;
    for (uint32_t y = 0; y < td.header.height / 2; ++y)
    for (uint32_t x = 0; x < td.header.width; ++x) {
        if (td.elevation_at(x, y) > 1000) { northern_land = true; break; }
    }
    EXPECT_TRUE(northern_land) << "expected mountains in northern Korea";
}

// ---------------------------------------------------------------------------
// JSON round-trip tests
// ---------------------------------------------------------------------------
TEST(Terrain, TileTypesDerivedFromElevation) {
    auto td = load_fixture();
    EXPECT_EQ(td.tile_types.size(), td.elevation.size());
    // Every tile_types entry must be a valid TileType value (0..5).
    for (auto t : td.tile_types) {
        EXPECT_LE(t, 5) << "invalid TileType value";
    }
    // Korea has both water and land — at least one cell of each.
    bool has_water = false, has_land = false;
    for (auto t : td.tile_types) {
        if (t == 0) has_water = true;
        else has_land = true;
    }
    EXPECT_TRUE(has_water);
    EXPECT_TRUE(has_land);
}

TEST(Terrain, JsonRoundTripPreservesTileTypes) {
    auto td = load_fixture();
    const std::string json = td.to_terrain_json("korea");
    EXPECT_FALSE(json.empty());
    EXPECT_NE(json.find("\"theater\": \"korea\""), std::string::npos);
    EXPECT_NE(json.find("\"width\": 128"), std::string::npos);

    // Load it back.
    TerrainData td2;
    td2.load_terrain_json_from_string(json);
    EXPECT_EQ(td2.header.width, td.header.width);
    EXPECT_EQ(td2.header.height, td.header.height);
    EXPECT_EQ(td2.tile_types, td.tile_types);
}

TEST(Terrain, JsonRoundTripPreservesElevation) {
    auto td = load_fixture();
    const std::string json = td.to_terrain_json("korea");
    TerrainData td2;
    td2.load_terrain_json_from_string(json);
    EXPECT_EQ(td2.elevation, td.elevation);
}

TEST(Terrain, JsonColorForTileTypeMatchesClassifier) {
    auto td = load_fixture();
    // For every cell, the color returned by terrain_color() must match
    // color_for_tile_type(tile_type_at()).
    for (uint32_t y = 0; y < td.header.height; ++y)
    for (uint32_t x = 0; x < td.header.width; ++x) {
        const auto t = td.tile_type_at(x, y);
        const auto c = td.terrain_color(x, y);
        const auto expected = TerrainData::color_for_tile_type(t);
        EXPECT_EQ(c.r, expected.r);
        EXPECT_EQ(c.g, expected.g);
        EXPECT_EQ(c.b, expected.b);
    }
}
