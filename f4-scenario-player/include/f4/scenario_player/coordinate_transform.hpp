// f4-scenario-player/include/f4/scenario_player/coordinate_transform.hpp
//
// Coordinate conversion functions used by the renderer.
//
// DEPRECATED: These are now thin wrappers that delegate to
// f4::renderer::coord_transform.hpp from the f4-renderer library.
// Kept for backward compatibility with existing code and tests.
//
// The simulation uses ENU feet (East-North-Up, z-up) for TransformComponent.
// Raylib uses RH Y-up (X right, Y up, Z toward viewer).
// FreeFalcon's BSP model data uses LH Y-up (X right, Y up, Z forward).
//
// These conversions are pure arithmetic — no Raylib types, no GL context.

#pragma once

#include <f4/renderer/coord_transform.hpp>

namespace f4::scenario_player {

/// Float3 — backward-compatible alias for f4::renderer::Float3.
using Float3 = f4::renderer::Float3;

/// Convert an ENU position (feet) to Raylib RH Y-up coordinates.
/// Delegates to f4::renderer::enu_to_raylib.
inline Float3 enu_to_raylib(double east_ft, double north_ft, double up_ft) noexcept {
    return f4::renderer::enu_to_raylib(east_ft, north_ft, up_ft);
}

/// Convert a model-space vertex (LH Y-up, FreeFalcon BSP convention) to
/// Raylib RH Y-up coordinates.
/// Delegates to f4::renderer::model_vertex_to_raylib.
inline Float3 model_vertex_to_raylib(float x, float y, float z) noexcept {
    return f4::renderer::model_vertex_to_raylib(x, y, z);
}

} // namespace f4::scenario_player
