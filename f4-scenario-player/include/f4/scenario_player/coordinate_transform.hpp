// f4-scenario-player/include/f4/scenario_player/coordinate_transform.hpp
//
// Coordinate conversion functions used by the renderer. Extracted to a
// public header so unit tests can verify the math without depending on
// Raylib (which is private to the library).
//
// The simulation uses ENU feet (East-North-Up, z-up) for TransformComponent.
// Raylib uses RH Y-up (X right, Y up, Z toward viewer).
// FreeFalcon's BSP model data uses LH Y-up (X right, Y up, Z forward).
//
// These conversions are pure arithmetic — no Raylib types, no GL context.

#pragma once

namespace f4::scenario_player {

/// A simple 3-float vector. Defined here (not using Raylib's Vector3) so
/// the conversion functions can be tested without linking Raylib.
struct Float3 {
    float x, y, z;
};

/// Convert an ENU position (feet) to Raylib RH Y-up coordinates.
///
/// ENU: x=east, y=north, z=up
/// Raylib RH Y-up: x=right, y=up, z=toward viewer (-north)
///
/// So a point 100 ft east, 200 ft north, 50 ft up maps to Raylib (100, 50, -200).
inline Float3 enu_to_raylib(double east_ft, double north_ft, double up_ft) noexcept {
    return Float3{
        static_cast<float>(east_ft),
        static_cast<float>(up_ft),
        static_cast<float>(-north_ft)
    };
}

/// Convert a model-space vertex (LH Y-up, FreeFalcon BSP convention) to
/// Raylib RH Y-up coordinates.
///
/// LH Y-up: x=right, y=up, z=forward (into screen)
/// Raylib RH Y-up: x=right, y=up, z=toward viewer (out of screen)
///
/// Conversion: (x, y, z) → (x, y, -z). The X and Y axes stay the same;
/// negating Z flips the handedness from LH to RH.
inline Float3 model_vertex_to_raylib(float x, float y, float z) noexcept {
    return Float3{x, -z, y};
}

} // namespace f4::scenario_player
