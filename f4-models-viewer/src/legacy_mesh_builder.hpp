// f4-models-viewer/src/legacy_mesh_builder.hpp
//
// Legacy KoreaObj geometry → Raylib mesh pipeline for the dev-tool
// model viewer.
//
// Tranche 0d moved the RUNTIME model path to glTF (f4-renderer's
// RuntimeModelCache), and f4-renderer no longer links f4-models — so
// the ModelGeometry → ::Mesh conversion this viewer needs lives here,
// in the one tool that still reads the KoreaObj binary by design
// (F4_SIDE=importer). The math mirrors what f4-renderer's mesh_builder
// used to provide for the binary path.

#pragma once

#include <f4/models/geometry.hpp>
#include <f4/models/model_database.hpp>
#include <f4/renderer/draw_3d.hpp>       // DrawStats
#include <f4/renderer/texture_cache.hpp> // TextureCache (insert API)

#include <raylib.h>
// Undef raylib macros that pollute the namespace
#undef PI
#undef DEG2RAD
#undef RAD2DEG

#include <vector>

namespace f4::renderer {
class LitShader;
}

namespace f4::models_viewer {

/// Convert extracted ModelGeometry into Raylib meshes + mesh entries
/// (one entry per source mesh, paired with its tex_id). Uploads each
/// mesh to the GPU — requires the GL context.
std::vector<f4::renderer::MeshEntry> build_legacy_meshes(
    const f4::models::ModelGeometry& geom,
    const f4::models::ColorBank& color_bank);

/// Decode + upload the given tex_ids from the ModelDatabase's KoreaObj
/// TEX bank into the shared TextureCache (via its insert() API — the
/// decoding lives here, the cache stays decoder-agnostic). No-op for
/// ids already cached.
void upload_legacy_textures(f4::renderer::TextureCache& cache,
                            f4::models::ModelDatabase& db,
                            const std::vector<int>& tex_ids);

} // namespace f4::models_viewer
