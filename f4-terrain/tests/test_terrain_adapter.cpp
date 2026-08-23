// f4-terrain/tests/test_terrain_adapter.cpp
//
// Unit tests for TerrainDataAdapter — the TerrainSource implementation
// that bridges f4::terrain::TerrainData to the sim's elevation query
// interface. Tests the bilinear interpolation + coordinate mapping
// (ENU feet → terrain grid cell).

#include <gtest/gtest.h>

#include <f4/terrain/terrain_data.hpp>
#include <f4/terrain/terrain_adapter.hpp>
#include <f4/terrain/terrain_source.hpp>

#include <cmath>
#include <filesystem>

using namespace f4::terrain;

namespace {

// Build a synthetic 4x4 terrain with known elevations for testing.
// The grid spans the full theater (1024*1024 ft per side = 1,048,576 ft).
// With 4 cells, ft_per_cell = 1048576 / 4 = 262144 ft.
TerrainData make_test_terrain(uint32_t w, uint32_t h) {
    TerrainData td;
    td.header.width = w;
    td.header.height = h;
    td.elevation.resize(w * h);
    td.tile_types.resize(w * h, static_cast<uint8_t>(TileType::Lowland));
    // Fill with a known pattern: elevation = y * 100 + x * 10
    // (stored in sim convention: y=0 is north)
    for (uint32_t y = 0; y < h; ++y) {
        for (uint32_t x = 0; x < w; ++x) {
            td.elevation[y * w + x] = static_cast<int16_t>(y * 100 + x * 10);
        }
    }
    return td;
}

} // anonymous namespace

// ============================================================================
// FlatTerrainSource
// ============================================================================

TEST(FlatTerrainSource, ReturnsConstantElevation) {
    FlatTerrainSource flat(500.0);
    EXPECT_DOUBLE_EQ(flat.elevation_at_ft(0.0, 0.0), 500.0);
    EXPECT_DOUBLE_EQ(flat.elevation_at_ft(1000.0, -2000.0), 500.0);
    EXPECT_DOUBLE_EQ(flat.elevation_at_ft(1e9, -1e9), 500.0);
}

TEST(FlatTerrainSource, ZeroElevation) {
    FlatTerrainSource flat(0.0);
    EXPECT_DOUBLE_EQ(flat.elevation_at_ft(0.0, 0.0), 0.0);
}

// ============================================================================
// NullTerrainSource
// ============================================================================

TEST(NullTerrainSource, ReturnsZero) {
    NullTerrainSource null;
    EXPECT_DOUBLE_EQ(null.elevation_at_ft(0.0, 0.0), 0.0);
    EXPECT_DOUBLE_EQ(null.elevation_at_ft(123.0, 456.0), 0.0);
}

// ============================================================================
// TerrainDataAdapter — coordinate mapping + bilinear interpolation
// ============================================================================

TEST(TerrainDataAdapter, EmptyElevationReturnsZero) {
    TerrainData td;  // no elevation array
    td.header.width = 128;
    td.header.height = 128;
    TerrainDataAdapter adapter(td);
    EXPECT_DOUBLE_EQ(adapter.elevation_at_ft(0.0, 0.0), 0.0);
    EXPECT_DOUBLE_EQ(adapter.elevation_at_ft(50000.0, 50000.0), 0.0);
}

TEST(TerrainDataAdapter, ExactCellCenter) {
    // 4x4 grid, ft_per_cell = 262144. ENU (0,0) = cell (0,0).
    // In sim convention (y=0 north), cell (0,0) has elevation 0*100+0*10 = 0.
    // But ENU (0,0) = south corner = file row 0 = sim y = height-1 = 3.
    // So ENU (0,0) maps to sim cell (0, 3) → elevation 3*100+0*10 = 300.
    auto td = make_test_terrain(4, 4);
    TerrainDataAdapter adapter(td);
    // At ENU (0, 0): south-west corner. sim y = 3 (north is up).
    // elevation_at(0, 3) = 3*100 + 0*10 = 300.
    EXPECT_NEAR(adapter.elevation_at_ft(0.0, 0.0), 300.0, 0.1);
}

TEST(TerrainDataAdapter, KnownElevations) {
    // 4x4 grid. ft_per_cell = 1048576/4 = 262144.
    // elevation[y][x] = y*100 + x*10 (sim convention, y=0 north).
    // ENU north increases northward = toward sim y=0.
    // So ENU north_ft = 0 → sim y = 3 (south row).
    // ENU north_ft = 262144 → sim y = 2.
    // ENU north_ft = 3*262144 = 786432 → sim y = 0 (north row).
    auto td = make_test_terrain(4, 4);
    TerrainDataAdapter adapter(td);

    // At ENU (0, 786432): north row, sim y=0, x=0 → elevation 0.
    EXPECT_NEAR(adapter.elevation_at_ft(0.0, 786432.0), 0.0, 0.1);

    // At ENU (262144, 786432): north row, sim y=0, x=1 → elevation 10.
    EXPECT_NEAR(adapter.elevation_at_ft(262144.0, 786432.0), 10.0, 0.1);

    // At ENU (0, 0): south row, sim y=3, x=0 → elevation 300.
    EXPECT_NEAR(adapter.elevation_at_ft(0.0, 0.0), 300.0, 0.1);

    // At ENU (786432, 0): south row, sim y=3, x=3 → elevation 330.
    EXPECT_NEAR(adapter.elevation_at_ft(786432.0, 0.0), 330.0, 0.1);
}

TEST(TerrainDataAdapter, BilinearInterpolation) {
    // 4x4 grid. At the center of 4 cells, the elevation should be the
    // average of the 4 surrounding cell centers.
    // Cells at sim (1,1), (2,1), (1,2), (2,2):
    //   (1,1) = 1*100 + 1*10 = 110
    //   (2,1) = 1*100 + 2*10 = 120
    //   (1,2) = 2*100 + 1*10 = 210
    //   (2,2) = 2*100 + 2*10 = 220
    // Average = (110+120+210+220)/4 = 165.
    //
    // The center between cells (1,1) and (2,2) in ENU:
    //   cell (1,1) ENU: east = 1*262144, north = (4-1-1)*262144 = 2*262144
    //   cell (2,2) ENU: east = 2*262144, north = (4-1-2)*262144 = 1*262144
    //   center: east = 1.5*262144, north = 1.5*262144
    auto td = make_test_terrain(4, 4);
    TerrainDataAdapter adapter(td);

    const double center_east = 1.5 * 262144.0;
    const double center_north = 1.5 * 262144.0;
    EXPECT_NEAR(adapter.elevation_at_ft(center_east, center_north), 165.0, 0.5);
}

TEST(TerrainDataAdapter, ClampsOutsideGrid) {
    // Positions outside the theater should clamp to the edge, not wrap.
    auto td = make_test_terrain(4, 4);
    TerrainDataAdapter adapter(td);

    // Far south-west: should clamp to cell (0,3) = 300.
    EXPECT_NEAR(adapter.elevation_at_ft(-10000.0, -10000.0), 300.0, 0.1);

    // Far north-east: should clamp to cell (3,0) = 30.
    EXPECT_NEAR(adapter.elevation_at_ft(1e7, 1e7), 30.0, 0.1);
}

TEST(TerrainDataAdapter, RealKoreaFixture) {
    // Load the real Korea terrain fixture and verify we can query it
    // without crashing. The fixture has 128x128 cells with elevations
    // up to 6691 ft.
    const char* fixture = FIXTURE_DIR "korea.terrain.json";
    if (!std::filesystem::exists(fixture)) GTEST_SKIP();

    TerrainData td;
    ASSERT_NO_THROW(td.load_terrain_json(fixture));
    ASSERT_EQ(td.header.width, 128u);
    ASSERT_EQ(td.header.height, 128u);
    ASSERT_FALSE(td.elevation.empty());

    TerrainDataAdapter adapter(td);

    // Query a few positions across the theater. We don't assert exact
    // values (the fixture's elevation pattern is arbitrary) — we just
    // verify the queries return finite values in a reasonable range.
    for (int i = 0; i < 10; ++i) {
        const double east = i * 100000.0;
        const double north = i * 100000.0;
        const double elev = adapter.elevation_at_ft(east, north);
        EXPECT_TRUE(std::isfinite(elev));
        EXPECT_GE(elev, 0.0);
        EXPECT_LE(elev, 7000.0);  // max is 6691
    }
}

// ============================================================================
// TerrainSource interface — polymorphism
// ============================================================================

TEST(TerrainSource, PolymorphicDispatch) {
    FlatTerrainSource flat(100.0);
    NullTerrainSource null;

    f4::terrain::TerrainSource* src = &flat;
    EXPECT_DOUBLE_EQ(src->elevation_at_ft(0.0, 0.0), 100.0);

    src = &null;
    EXPECT_DOUBLE_EQ(src->elevation_at_ft(0.0, 0.0), 0.0);
}
