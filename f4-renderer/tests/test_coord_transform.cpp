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
    // Conversion: (x, y, z) -> (y, -z, -x) — the mapping the 3D model
    // viewer, the world viewer's Ground Layout 3D, the class table
    // browser, and mesh_builder's default all render FreeFalcon BSP
    // models upright with (visually verified across those tranches).
    // NOTE: this test's PREVIOUS expectations ("X unchanged, Y becomes
    // Z, Z becomes -Y", i.e. (x, -z, y)) matched no implementation
    // that ever shipped — neither the original (y, z, -x) nor the
    // current (y, -z, -x) — so it had been red since the renderer
    // targets were first enabled in CI; pinned to the shipped
    // conversion here.
    // X axis -> -Z (LH x-right becomes RH -z, away from the viewer)
    const auto x = model_vertex_to_raylib(1, 0, 0);
    EXPECT_FLOAT_EQ(x.x, 0);
    EXPECT_FLOAT_EQ(x.y, 0);
    EXPECT_FLOAT_EQ(x.z, -1);

    // Y axis -> X (LH y-up becomes RH x-right)
    const auto y = model_vertex_to_raylib(0, 1, 0);
    EXPECT_FLOAT_EQ(y.x, 1);
    EXPECT_FLOAT_EQ(y.y, 0);
    EXPECT_FLOAT_EQ(y.z, 0);

    // Z axis -> -Y (LH z-forward becomes RH -y, down)
    const auto z = model_vertex_to_raylib(0, 0, 1);
    EXPECT_FLOAT_EQ(z.x, 0);
    EXPECT_FLOAT_EQ(z.y, -1);
    EXPECT_FLOAT_EQ(z.z, 0);
}

TEST(CoordTransform, ModelVertexToRaylib_General) {
    // (3, 5, 7) -> (5, -7, -3)  [(x, y, z) -> (y, -z, -x)]
    const auto v = model_vertex_to_raylib(3, 5, 7);
    EXPECT_FLOAT_EQ(v.x, 5);
    EXPECT_FLOAT_EQ(v.y, -7);
    EXPECT_FLOAT_EQ(v.z, -3);
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
