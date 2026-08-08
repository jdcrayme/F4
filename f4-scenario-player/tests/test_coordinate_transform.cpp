// test_coordinate_transform.cpp — unit tests for the ENU → Raylib
// coordinate conversions used by the renderer.
//
// The renderer's biggest source of bugs is coordinate-frame confusion:
//   - Simulation uses ENU (East-North-Up, z-up, feet)
//   - Raylib uses RH Y-up (X right, Y up, Z toward viewer)
//   - FreeFalcon's BSP model data uses LH Y-up (X right, Y up, Z forward)
//   - The aircraft's body frame is X-forward, Y-right, Z-down (NED body)
//
// These tests verify the conversions are correct so the aircraft ends
// up on the runway (not underground, not facing backward, not at the
// wrong location). The tests are pure arithmetic — no GL context needed,
// no Raylib linkage required.

#include <gtest/gtest.h>

#include "f4/scenario_player/coordinate_transform.hpp"

#include <cmath>

using namespace f4::scenario_player;

TEST(CoordinateTransform, EnuToRaylibMapsAxesCorrectly) {
    // ENU: x=east, y=north, z=up
    // Raylib RH Y-up: x=right, y=up, z=toward viewer (-north)
    //
    // So a point 100 ft east, 200 ft north, 50 ft up should map to
    // Raylib (100, 50, -200).
    const auto v = enu_to_raylib(100.0, 200.0, 50.0);
    EXPECT_NEAR(v.x, 100.0f, 0.001f);  // east → x
    EXPECT_NEAR(v.y, 50.0f, 0.001f);   // up → y
    EXPECT_NEAR(v.z, -200.0f, 0.001f); // north → -z (toward viewer)
}

TEST(CoordinateTransform, EnuToRaylibZeroIsOrigin) {
    const auto v = enu_to_raylib(0.0, 0.0, 0.0);
    EXPECT_NEAR(v.x, 0.0f, 0.001f);
    EXPECT_NEAR(v.y, 0.0f, 0.001f);
    EXPECT_NEAR(v.z, 0.0f, 0.001f);
}

TEST(CoordinateTransform, EnuToRaylibUpIsPositiveY) {
    // Aircraft on the ground at z=50 (50 ft MSL) should appear at y=50
    // in Raylib (above the Y=0 ground plane).
    const auto v = enu_to_raylib(0.0, 0.0, 50.0);
    EXPECT_GT(v.y, 0.0f);
}

TEST(CoordinateTransform, EnuToRaylibNorthIsNegativeZ) {
    // A point north of origin should be at -Z in Raylib (in front of
    // the camera if the camera looks toward -Z, which is Raylib's
    // default forward direction).
    const auto v = enu_to_raylib(0.0, 100.0, 0.0);
    EXPECT_LT(v.z, 0.0f);
}

TEST(CoordinateTransform, EnuToRaylibEastIsPositiveX) {
    // A point east of origin should be at +X in Raylib (to the right
    // when looking toward -Z).
    const auto v = enu_to_raylib(100.0, 0.0, 0.0);
    EXPECT_GT(v.x, 0.0f);
}

TEST(CoordinateTransform, ModelVertexToRaylibSwapsYAndZ) {
    // The existing f4-models-viewer (which renders the F-16 correctly)
    // uses the conversion (x, y, z) → (x, -z, y). This is a Y/Z swap
    // with Z negated — appropriate for the FreeFalcon BSP model data
    // convention, which empirical testing shows is Z-up despite the
    // f4-models-viewer comment claiming Y-up. The DOF rotation matrices
    // only make sense under Z-up.
    //
    // A vertex at (10, 20, 30) in model space maps to (10, -30, 20)
    // in Raylib space.
    const auto v = model_vertex_to_raylib(10.0f, 20.0f, 30.0f);
    EXPECT_NEAR(v.x, 10.0f, 0.001f);   // x stays
    EXPECT_NEAR(v.y, -30.0f, 0.001f);  // model z (negated) becomes raylib y
    EXPECT_NEAR(v.z, 20.0f, 0.001f);   // model y becomes raylib z
}

TEST(CoordinateTransform, ModelVertexToRaylibPreservesXAxis) {
    // Model X (right) should stay X in Raylib (right).
    const auto v = model_vertex_to_raylib(100.0f, 0.0f, 0.0f);
    EXPECT_NEAR(v.x, 100.0f, 0.001f);
}

TEST(CoordinateTransform, ModelVertexToRaylibMapsModelZToRaylibY) {
    // Per the (x, -z, y) conversion, model +Z maps to Raylib -Y.
    // This is consistent with the model being Z-up: model up (+Z) becomes
    // Raylib down (-Y)? That doesn't sound right, but empirically the
    // f4-models-viewer renders the F-16 correctly with this conversion,
    // and the F-16's nose (which should point forward = model +Z) ends up
    // at Raylib -Y (which is down, not forward).
    //
    // The likely explanation: the model's "up" in its native frame is
    // actually -Z (not +Z), so -z → +y in Raylib (up). The negation
    // handles this implicitly. We don't need to understand why — we just
    // need the conversion to match what f4-models-viewer does.
    const auto v = model_vertex_to_raylib(0.0f, 0.0f, 50.0f);
    EXPECT_NEAR(v.y, -50.0f, 0.001f);
}

TEST(CoordinateTransform, ModelVertexToRaylibMapsModelYToRaylibZ) {
    // Per the (x, -z, y) conversion, model +Y maps to Raylib +Z.
    // This is the axis that points "forward" in Raylib RH Y-up (toward
    // the viewer is +Z, away from viewer is -Z; the F-16's nose should
    // be at -Z so it points away from us when we look at it from behind).
    //
    // For the F-16 model: vertices stored at +Y in model space (e.g. the
    // nose) end up at +Z in Raylib (toward the viewer). This means the
    // F-16 faces the camera by default — the renderer must apply a 180°
    // yaw rotation as part of the entity's transform to make it face away.
    //
    // We just verify the math here; the renderer handles the orientation.
    const auto v = model_vertex_to_raylib(0.0f, 50.0f, 0.0f);
    EXPECT_NEAR(v.z, 50.0f, 0.001f);
}
