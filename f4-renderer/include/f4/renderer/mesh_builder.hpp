// f4-renderer/include/f4/renderer/mesh_builder.hpp
//
// ModelGeometry -> Raylib ::Mesh conversion.
// Converts the engine-agnostic f4::models geometry into GPU-ready
// Raylib meshes, applying coordinate transforms and resolving
// ColorBank vertex color indices.
//
// Consolidated from 4 duplicated implementations.

#pragma once

#include <f4/renderer/coord_transform.hpp>

#include <raylib.h>
// Undef raylib macros that pollute the namespace
#undef PI
#undef DEG2RAD
#undef RAD2DEG

#include <cstdint>
#include <vector>

namespace f4::models {
struct ModelGeometry;
struct ColorBank;
}  // namespace f4::models

namespace f4::renderer {

/// A mesh entry that pairs a Raylib Mesh with its texture bank index.
/// Used by the texture upload and draw pipeline.
struct MeshEntry {
    ::Mesh mesh = {};
    int tex_id = -1;              ///< texture bank index for this mesh (-1 = no texture)
};

/// Build Raylib meshes from extracted model geometry.
///
/// Each f4::models::Mesh becomes one Raylib ::Mesh (uploaded to GPU).
/// Resolves vertex color indices through the ColorBank (Prim.rgba is an
/// int index into the ColorBank, NOT packed ABGR — see f4-models
/// ColorBank docs).
///
/// @param geom        Extracted model geometry
/// @param color_bank  Color bank for vertex color resolution
/// @param transform   Coordinate transform function (model_vertex_to_raylib
///                    or enu_to_raylib). Defaults to model_vertex_to_raylib.
std::vector<::Mesh> build_raylib_meshes(
    const f4::models::ModelGeometry& geom,
    const f4::models::ColorBank& color_bank,
    Float3 (*transform)(float, float, float) = model_vertex_to_raylib);

/// Build MeshEntry vector from geometry + already-built Raylib meshes.
/// Pairs each mesh with its tex_id from the geometry for per-mesh
/// material lookup.
std::vector<MeshEntry> build_mesh_entries(
    const f4::models::ModelGeometry& geom,
    const std::vector<::Mesh>& raylib_meshes);

/// Unload (free GPU memory for) a vector of Raylib meshes.
void unload_meshes(std::vector<::Mesh>& meshes);

/// Resolve a ColorBank index to a Raylib Color.
///
/// f4::models::Vertex::color stores `Prim.rgba`, which in FreeFalcon is
/// an `int rgba` INDEX into the ColorBank. Falls back to a neutral grey
/// for textured meshes (whose color comes from the texture, not the index)
/// and uncolored meshes.
Color resolve_vertex_color(uint32_t color_index,
                           const f4::models::ColorBank& color_bank,
                           bool mesh_is_textured) noexcept;

} // namespace f4::renderer
