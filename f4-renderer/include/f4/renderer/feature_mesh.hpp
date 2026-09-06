// f4-renderer/include/f4/renderer/feature_mesh.hpp
//
// Feature mesh rendering — resolves a class table entity_type → vis_type →
// glTF model (Data/Models/koreaobj/NNNNN.gltf) → Raylib Mesh, then
// DrawMesh-es it at a given ENU position with a given facing. Caches the
// meshes per vis_type (RuntimeModelCache) and the textures per tex_id
// (TextureCache) so multiple features of the same type reuse one GPU
// upload.
//
// This is the single source of truth for the feature → mesh pipeline.
// The 2D world-viewer canvas and the 3D Ground Layout panel both call
// draw_feature_mesh() to render real feature models at
// FeatureEntryState offsets — replacing the previous duplicate
// implementations in canvas.cpp (placeholder) and ground_layout_3d.cpp
// (inline).
//
// Tranche 0d (RENDERER_GLTF_REWIRE_PLAN.md): the model source is the
// glTF export (f4import models) + PNG textures (f4import textures) —
// f4-renderer no longer links f4-models / f4-world-convert. The class
// table is f4-world-types (the runtime-safe JSON loader).
//
// IMPORTANT: draw_feature_mesh() must be called inside a
// BeginMode3D/EndMode3D block. The caller is responsible for setting up
// the camera — typically:
//   - For the 3D panel: an orbit Camera3D positioned by OrbitCamera.
//   - For the 2D canvas: a top-down orthographic Camera3D that matches
//     the 2D world_to_screen transform (see canvas.cpp's feature pass
//     for the exact camera setup).
//
// Coordinate conventions:
//   - Position is ENU feet (East-North-Up), the simulation's native
//     frame. We convert to Raylib RH Y-up internally via enu_to_raylib().
//   - Facing is degrees of rotation around the vertical (ENU +Z, Raylib +Y)
//     axis. Sign matches the existing footprint code (positive = clockwise
//     when viewed from above).
//   - The class_table_index is an entity_type (descriptionIndex + 100,
//     where descriptionIndex is what FeatureEntryState.index stores).
//     We add 100 internally? NO — we expect the caller to pass entity_type
//     directly. This is the same convention as ClassTable::vis_type_for().

#pragma once

#include <f4/renderer/draw_3d.hpp>      // DrawStats
#include <f4/renderer/lit_shader.hpp>
#include <f4/renderer/mesh_builder.hpp>  // MeshEntry
#include <f4/renderer/texture_cache.hpp>
#include <f4/renderer/runtime_model_cache.hpp>

#include <raylib.h>
// Undef raylib macros that pollute the namespace
#undef PI
#undef DEG2RAD
#undef RAD2DEG

#include <cstdint>
#include <vector>

namespace f4::world_types {
struct ClassTable;
}  // namespace f4::world_types

namespace f4::renderer {

/// Bundle of resources needed to draw feature meshes. Built once and
/// passed (by reference) to draw_feature_mesh() for each feature.
///
/// Lifetime: the caller owns all pointed-to objects (RuntimeModelCache —
/// typically RenderResources::model_cache — ClassTable, TextureCache,
/// LitShader, default_material).
struct FeatureMeshResources {
    /// glTF runtime model cache (vis_type → loaded meshes). Required.
    /// Typically &RenderResources::model_cache. Configure the Data/
    /// root via RenderResources::set_model_data_dir().
    RuntimeModelCache* model_cache = nullptr;

    /// Runtime class table (entity_type → vis_type[7]), from
    /// f4-world-types (JSON). Required by draw_feature_mesh(); NOT
    /// required by draw_vis_type_mesh() (callers that already hold the
    /// vis type legitimately pass null).
    f4::world_types::ClassTable* class_table = nullptr;

    /// GPU texture cache (lazy, keyed by tex_id). Required.
    TextureCache* texture_cache = nullptr;

    /// Lit shader (compiled lazily via ensure()). Required.
    LitShader* lit_shader = nullptr;

    /// Default Material for untextured meshes (tex_id < 0). Must be
    /// created by the caller with a 1x1 opaque-white fallback texture
    /// bound to MATERIAL_MAP_DIFFUSE and (optionally) the lit shader
    /// assigned. See viewer's ensure_default_material_3d() for the recipe.
    /// Required.
    ::Material* default_material = nullptr;

    /// Lighting state — applied to the lit shader at the start of each
    /// draw_feature_mesh() call via set_lighting(). The caller may also
    /// set these once before the loop and pass the same values; we set
    /// them per-call so callers don't have to remember to.
    Vector3 light_direction   = { 0.65f, -1.0f, 0.35f };
    Color   light_color       = { 255, 250, 235, 255 };
    float   light_intensity    = 1.0f;
    Color   ambient_color      = {  80,  80,  90, 255 };
};

/// Lazily build (load + upload) the glTF model for one vis_type,
/// cached by RuntimeModelCache. No-op if already cached (or if the
/// previous build attempt failed — the cache marks it built). Used
/// internally by draw_vis_type_mesh, but exposed so callers can
/// pre-warm the cache outside the render loop (e.g. when the selection
/// changes, before the first frame draws).
///
/// Requires the GL context. Safe to call when model_cache/texture_cache
/// are null (no-op).
void build_feature_mesh(FeatureMeshResources& res, int vis_type);

/// V-3DLIVE: draw one model by VIS TYPE directly — everything
/// draw_feature_mesh does AFTER its class-table lookup, for callers
/// that already hold the resolved vis type. The session entities carry
/// theirs in VisualModelComponent::vis_type (resolved at spawn against
/// the SESSION's class table; the viewer resolves the mesh through ITS
/// OWN model cache — the session's is deliberately empty). Shares the
/// RuntimeModelCache and draw path with draw_feature_mesh, so a static
/// feature and a session entity of the same vis type reuse one GPU
/// upload.
///
/// Must be called inside a BeginMode3D/EndMode3D block (same contract
/// as draw_feature_mesh). Behavior: vis_type <= 0 → zeroed DrawStats;
/// otherwise lazily build + DrawMesh each mesh entry at the ENU
/// position rotated by facing_deg around the vertical axis.
DrawStats draw_vis_type_mesh(
    FeatureMeshResources& res,
    int vis_type,
    float enu_x, float enu_y, float enu_z,
    float facing_deg);

/// Resolve class_table_index (entity_type) → vis_type[0] → cached mesh,
/// then DrawMesh each mesh entry at the given ENU position rotated by
/// the given facing around the vertical axis.
///
/// Must be called inside a BeginMode3D/EndMode3D block (caller sets up
/// the camera — typically a top-down orthographic Camera3D for the 2D
/// canvas, or an orbit Camera3D for the 3D ground-layout panel).
///
/// Behavior:
///   - Looks up vis_type[0] for the given entity_type. If 0 (no model),
///     returns zeroed DrawStats.
///   - Lazily builds the model (via build_feature_mesh) if not cached.
///   - Ensures the lit shader is compiled (idempotent) and sets lighting
///     uniforms once per call.
///   - Disables backface culling (FreeFalcon-derived models have
///     inconsistent winding order — same convention as draw_meshes()
///     and the existing ground_layout_3d panel).
///   - For each mesh entry: picks the material (default_material for
///     untextured, or tex_cache.lookup(tex_id)->material for textured),
///     then DrawMesh(mesh, material, model_matrix).
///
/// @param res                Feature mesh resources
/// @param class_table_index  Feature's class table entity_type
///                           (= FeatureEntryState.index + 100).
/// @param enu_x, enu_y, enu_z  Feature world position in ENU feet.
/// @param facing_deg         Feature facing in degrees (CCW around +Z up).
/// @return  DrawStats describing what was drawn. draw_calls == 0 means
///          the feature was skipped (no vis_type, no mesh, empty
///          geometry, or model_cache/class_table pointers were null).
DrawStats draw_feature_mesh(
    FeatureMeshResources& res,
    uint16_t class_table_index,
    float enu_x, float enu_y, float enu_z,
    float facing_deg);

} // namespace f4::renderer
