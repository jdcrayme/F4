// f4-renderer/tests/test_coord_transform.cpp
//
// Unit tests for coordinate transforms. No Raylib dependency.

#include <f4/renderer/coord_transform.hpp>

#include <gtest/gtest.h>
#include <cmath>

using namespace f4::renderer;

TEST(CoordTransform, ModelVertexToRaylib_Identity) {
    // Origin stays at origin
    const auto v = model_vertex_to_raylib(0, 0, 0);
    EXPECT_FLOAT_EQ(v.x, 0);
    EXPECT_FLOAT_EQ(v.y, 0);
    EXPECT_FLOAT_EQ(v.z, 0);
}

TEST(CoordTransform, ModelVertexToRaylib_Axes) {
    // X axis is unchanged
    const auto x = model_vertex_to_raylib(1, 0, 0);
    EXPECT_FLOAT_EQ(x.x, 1);
    EXPECT_FLOAT_EQ(x.y, 0);
    EXPECT_FLOAT_EQ(x.z, 0);

    // Y axis becomes Z (LH Y-up -> RH Y-up: Y swaps with Z)
    const auto y = model_vertex_to_raylib(0, 1, 0);
    EXPECT_FLOAT_EQ(y.x, 0);
    EXPECT_FLOAT_EQ(y.y, 0);
    EXPECT_FLOAT_EQ(y.z, 1);

    // Z axis becomes -Y (negating Z flips handedness)
    const auto z = model_vertex_to_raylib(0, 0, 1);
    EXPECT_FLOAT_EQ(z.x, 0);
    EXPECT_FLOAT_EQ(z.y, -1);
    EXPECT_FLOAT_EQ(z.z, 0);
}

TEST(CoordTransform, ModelVertexToRaylib_General) {
    // (3, 5, 7) -> (3, -7, 5)
    const auto v = model_vertex_to_raylib(3, 5, 7);
    EXPECT_FLOAT_EQ(v.x, 3);
    EXPECT_FLOAT_EQ(v.y, -7);
    EXPECT_FLOAT_EQ(v.z, 5);
}

TEST(CoordTransform, EnuToRaylib_Axes) {
    // East -> +X
    const auto e = enu_to_raylib(1, 0, 0);
    EXPECT_FLOAT_EQ(e.x, 1);
    EXPECT_FLOAT_EQ(e.y, 0);
    EXPECT_FLOAT_EQ(e.z, 0);

    // North -> -Z
    const auto n = enu_to_raylib(0, 1, 0);
    EXPECT_FLOAT_EQ(n.x, 0);
    EXPECT_FLOAT_EQ(n.y, 0);
    EXPECT_FLOAT_EQ(n.z, -1);

    // Up -> +Y
    const auto u = enu_to_raylib(0, 0, 1);
    EXPECT_FLOAT_EQ(u.x, 0);
    EXPECT_FLOAT_EQ(u.y, 1);
    EXPECT_FLOAT_EQ(u.z, 0);
}

TEST(CoordTransform, EnuToRaylib_General) {
    // 100 ft east, 200 ft north, 50 ft up -> (100, 50, -200)
    const auto v = enu_to_raylib(100, 200, 50);
    EXPECT_FLOAT_EQ(v.x, 100);
    EXPECT_FLOAT_EQ(v.y, 50);
    EXPECT_FLOAT_EQ(v.z, -200);
}
