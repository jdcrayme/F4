// f4-models-viewer/src/scene.hpp
//
// ModelGeometry → Raylib ::Mesh conversion.
// Converts the engine-agnostic f4::models geometry into GPU-ready
// Raylib meshes, applying the LH Z-up → RH Y-up coordinate transform.

#pragma once

#include <raylib.h>

#include <vector>

namespace f4::models { struct ModelGeometry; }

namespace f4::models_viewer {

/// Build Raylib meshes from extracted model geometry.
/// Each f4::models::Mesh becomes one Raylib ::Mesh (uploaded to GPU).
/// Applies LH Z-up → RH Y-up coordinate conversion.
std::vector<::Mesh> build_raylib_meshes(const f4::models::ModelGeometry& geom);

/// Unload (free GPU memory for) a vector of Raylib meshes.
void unload_meshes(std::vector<::Mesh>& meshes);

} // namespace f4::models_viewer
