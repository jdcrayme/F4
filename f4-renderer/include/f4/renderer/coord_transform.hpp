// f4-renderer/include/f4/renderer/coord_transform.hpp
//
// Coordinate conversion functions for the F4 rendering pipeline.
// Pure arithmetic — no Raylib types, no GL context — so unit tests
// can verify the math without linking Raylib.
//
// The simulation uses ENU feet (East-North-Up, z-up) for TransformComponent.
// Raylib uses RH Y-up (X right, Y up, Z toward viewer).
// FreeFalcon's BSP model data uses LH Y-up (X right, Y up, Z forward).

#pragma once

namespace f4::renderer {

/// A simple 3-float vector. Defined here (not using Raylib's Vector3) so
/// the conversion functions can be tested without linking Raylib.
struct Float3 {
    float x, y, z;
};

/// Convert a model-space vertex (LH Y-up, FreeFalcon BSP convention) to
/// RH Y-up coordinates.
///
/// LH Y-up: x=right, y=up, z=forward (into screen)
/// RH Y-up: x=right, y=up, z=toward viewer (out of screen)
///
/// Conversion: (x, y, z) -> (x, -z, y). The X and Y axes stay the same;
/// negating Z flips the handedness from LH to RH, and Y swaps with Z to
/// convert from Y-up to the target Y-up orientation.
inline Float3 model_vertex_to_raylib(float x, float y, float z) noexcept {
    return Float3{y, z, -x};
}

/// Convert an ENU position (feet) to RH Y-up coordinates.
///
/// ENU: x=east, y=north, z=up
/// RH Y-up: x=right, y=up, z=toward viewer (-north)
///
/// So a point 100 ft east, 200 ft north, 50 ft up maps to (100, 50, -200).
inline Float3 enu_to_raylib(double east_ft, double north_ft, double up_ft) noexcept {
    return Float3{
        static_cast<float>(east_ft),
        static_cast<float>(up_ft),
        static_cast<float>(-north_ft)
    };
}

} // namespace f4::renderer
