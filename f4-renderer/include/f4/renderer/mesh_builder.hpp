// f4-renderer/include/f4/renderer/mesh_builder.hpp
//
// glTF primitive → Raylib ::Mesh conversion.
// Extracts per-primitive vertex data from a f4::gltf::GltfDocument (the
// runtime's model format — Tranche 0d, RENDERER_GLTF_REWIRE_PLAN.md) and
// uploads it as GPU-ready Raylib meshes, applying the glTF→Raylib
// coordinate transform and binding KoreaObj texture bank ids through the
// glTF material chain (material → baseColorTexture → image URI).
//
// Replaces the previous f4::models::ModelGeometry path (the legacy
// binary pipeline) — Tranche 0d cut f4-models from the runtime link
// closure, so the renderer now consumes the glTF+PNG exports produced by
// `f4import models` / `f4import textures` (Tasks 53 / 0c).
//
// The extraction half (extract_gltf_lod_geometry) is pure CPU data —
// no Raylib types, no GL context — so unit tests verify the geometry
// pipeline headlessly. Only build_gltf_mesh() touches the GPU.
//
// Consolidated from 4 duplicated implementations (2026 cleanup pass).

#pragma once

#include <f4/renderer/coord_transform.hpp>

#include <raylib.h>
// Undef raylib macros that pollute the namespace
#undef PI
#undef DEG2RAD
#undef RAD2DEG

#include <cstdint>
#include <string>
#include <vector>

namespace f4::gltf {
struct GltfDocument;
}  // namespace f4::gltf

namespace f4::renderer {

/// A mesh entry that pairs a Raylib Mesh with its texture bank index.
/// Used by the texture upload and draw pipeline.
struct MeshEntry {
    ::Mesh mesh = {};
    int tex_id = -1;              ///< texture bank index for this mesh (-1 = no texture)
    std::string texture_uri;      ///< PNG URI the tex_id came from (relative to the model dir)
};

/// Extracted per-primitive vertex data — pure CPU data, no GPU handles,
/// no Raylib dependency. Produced by extract_gltf_lod_geometry();
/// consumed by build_gltf_mesh() (GPU upload) and by headless unit
/// tests.
struct GltfMeshData {
    std::vector<float> positions;        // xyz triplets (Raylib RH Y-up, feet)
    std::vector<float> normals;          // xyz triplets (Raylib RH Y-up)
    std::vector<float> texcoords;        // uv pairs (empty when no TEXCOORD_0)
    std::vector<unsigned char> colors;   // rgba quads (empty when no COLOR_0)
    std::vector<unsigned short> indices; // triangle vertex indices
    int tex_id = -1;                     ///< KoreaObj texture bank id (-1 = none)
    std::string texture_uri;             ///< PNG URI relative to the model dir
};

/// Extract one LOD level's geometry from a glTF model document.
///
/// The f4import models emitter writes one glTF mesh per LOD level,
/// named "LOD_0", "LOD_1", ...; each mesh holds one primitive per
/// source BSP mesh. Returns one GltfMeshData per primitive of the
/// requested LOD (empty vector when the LOD doesn't exist).
///
/// Texture binding: each primitive's material chain
/// (material → baseColorTexture → image) resolves to a PNG URI of the
/// form "textures/NNNNN.png"; NNNNN is the KoreaObj texture bank id,
/// stored in GltfMeshData::tex_id (the TextureCache key — the same key
/// the old ModelDatabase::fetch_texture path used).
///
/// Pure function: no Raylib, no GL, no I/O (the document's external
/// .bin buffers must already be loaded — GltfDocument::load() does
/// that).
std::vector<GltfMeshData> extract_gltf_lod_geometry(
    const f4::gltf::GltfDocument& doc, int lod_level);

/// Upload one extracted mesh to the GPU as a Raylib ::Mesh.
/// Requires the GL context. Returns an empty ::Mesh when the data has
/// no vertices.
::Mesh build_gltf_mesh(const GltfMeshData& data);

/// Unload (free GPU memory for) a vector of Raylib meshes.
void unload_meshes(std::vector<::Mesh>& meshes);

} // namespace f4::renderer
