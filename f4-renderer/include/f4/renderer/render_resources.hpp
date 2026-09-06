// f4-renderer/include/f4/renderer/render_resources.hpp
//
// RenderResources — owns the shared GPU resources that every viewer needs:
//   - LitShader (lazy-compiled GLSL 330 Lambertian + chroma-key discard)
//   - TextureCache (tex_id → GPU Texture2D + Material, loaded from PNG)
//   - RuntimeModelCache (vis_type → glTF-loaded Raylib meshes)
//   - Default Material (1×1 white fallback texture + lit shader)
//   - Lighting state (direction, color, intensity, ambient)
//   - AirfieldGeometry3D cache (EntityId → pre-built geometry)
//
// One instance per viewer application. The world-viewer shares one between
// its 2D canvas, the 3D Ground Layout tab, and the 3D world mode; the
// scenario-player owns one. A mesh or texture uploaded through any view is
// cached for all the others.
//
// Tranche 0d (RENDERER_GLTF_REWIRE_PLAN.md): the model path is
// RuntimeModelCache — glTF + PNG from Data/Models/koreaobj/ (produced by
// `f4import models` / `f4import textures`). The legacy KoreaObj
// ModelDatabase path is gone; f4-renderer no longer links f4-models or
// f4-world-convert.
//
// Consolidates the per-app GPU caches that used to live in:
//   - f4-world-viewer ViewerApp::Impl (mesh_cache_3d, texture_cache_3d,
//     lit_shader_3d, default_mat_3d, fallback_white_tex_3d, lighting fields)
//   - f4-scenario-player PlayerApp::Impl (mesh_cache, texture_cache,
//     lit_shader, lighting fields)
// and their per-app build/upload/unload methods
// (build_mesh_for_model / build_mesh_3d, upload_textures,
// ensure_default_material_3d, unload_meshes / unload_meshes_3d).
//
// Dependencies: f4-gltf, f4-world-types, f4-entities, raylib.
// C++20.

#pragma once

#include <f4/renderer/lit_shader.hpp>
#include <f4/renderer/mesh_builder.hpp>             // MeshEntry
#include <f4/renderer/texture_cache.hpp>
#include <f4/renderer/runtime_model_cache.hpp>      // RuntimeModelCache
#include <f4/renderer/ground_layout_models.hpp>     // AirfieldGeometry3D
#include <f4/renderer/feature_mesh.hpp>             // FeatureMeshResources

#include <f4/entities/entity.hpp>                   // EntityId

#include <raylib.h>
// Undef raylib macros that pollute the namespace
#undef PI
#undef DEG2RAD
#undef RAD2DEG

#include <filesystem>
#include <unordered_map>

namespace f4::renderer {

/// Owns the shared GPU resources that every viewer needs.
///
/// Usage:
///   RenderResources res;                       // one per app
///   res.set_model_data_dir(data_dir);          // Data/ root (glTF models)
///   res.ensure_default_material();             // after GL context exists
///   res.build_mesh_for_model(vis_type);        // lazy, idempotent
///   // ... per frame:
///   auto eres = make_entity_render_resources(res, &ct);
///   RenderEntity(eres, handle);                // or render_world()
///
/// Cleanup: unload_all() — also invoked by the destructor, but must run
/// BEFORE the GL context goes away, so apps that own the window should
/// call unload_all() before CloseWindow().
class RenderResources {
public:
    RenderResources() = default;
    ~RenderResources();

    // Non-copyable (owns GPU handles)
    RenderResources(const RenderResources&) = delete;
    RenderResources& operator=(const RenderResources&) = delete;

    // ── GPU resources ──────────────────────────────────────────────────

    /// Lazy-compiled Lambertian lit shader (falls back to unlit when the
    /// GL context can't compile GLSL 330).
    LitShader lit_shader;

    /// Lazy texture cache, keyed by KoreaObj tex_id (loaded from PNG).
    TextureCache texture_cache;

    /// glTF model cache, keyed by vis_type (the FALCON4.CT visType[0]
    /// value — the same key the legacy KoreaObj parent_index path used).
    /// Shared by the feature-mesh path and the VisualModelComponent
    /// path — both resolve to the same vis_type.
    RuntimeModelCache model_cache;

    /// Airfield geometry cache: EntityId.value → pre-built
    /// AirfieldGeometry3D from GroundLayoutComponent. Rebuilt only when
    /// an entity's layouts change (they're static campaign data, so in
    /// practice this is build-once). Cleared by unload_all().
    std::unordered_map<uint64_t, AirfieldGeometry3D> airfield_cache;

    // ── Default material (lazy, idempotent) ─────────────────────────────

    /// Ensure the default material exists: a 1×1 opaque-white fallback
    /// texture bound to MATERIAL_MAP_DIFFUSE with the lit shader assigned
    /// (if it compiled). Required so UTEXTURED meshes (tex_id < 0) sample
    /// (1,1,1,1) instead of undefined data — the lit shader's
    /// `if (tex.a < 0.5) discard;` would otherwise hide them.
    ///
    /// Requires the GL context. Returns true once valid (stays true until
    /// unload_all()).
    bool ensure_default_material();

    /// Whether ensure_default_material() has succeeded.
    bool default_material_valid() const noexcept { return default_mat_valid_; }

    /// The cached default material. Only meaningful after
    /// ensure_default_material() returns true.
    Material& default_material() noexcept { return default_mat_; }

    // ── Model data ──────────────────────────────────────────────────────

    /// Point the glTF model cache at a Data/ root (models at
    /// <data_dir>/Models/koreaobj/). Changing the dir clears the cache.
    void set_model_data_dir(const std::filesystem::path& data_dir);

    // ── Mesh building ──────────────────────────────────────────────

    /// Build (or skip if already cached) the Raylib meshes for one
    /// model, then upload any new PNG textures it references. Sets the
    /// cache entry's built = true even on failure so we don't retry
    /// every frame.
    ///
    /// Requires the GL context (mesh upload + texture upload). Called
    /// lazily from draw loops the first time a previously-unseen
    /// vis_type appears, or eagerly at startup for the primary aircraft.
    void build_mesh_for_model(int vis_type);

    // ── Lighting ───────────────────────────────────────────────────────

    Vector3 light_direction = {0.65f, -1.0f, 0.35f};  // surface → sun
    Color   light_color     = {255, 250, 235, 255};
    float   light_intensity = 1.0f;
    Color   ambient_color   = { 80,  80,  90, 255};

    // ── Cleanup ────────────────────────────────────────────────────────

    /// Unload all GPU resources: textures, glTF model meshes, default
    /// material, fallback texture; clear the airfield cache. Must be
    /// called before the GL context goes away. Safe to call when
    /// nothing is cached.
    void unload_all();

private:
    Material  default_mat_ = {};
    Texture2D fallback_white_tex_ = {};
    bool default_mat_valid_ = false;
    bool fallback_white_tex_valid_ = false;
};

} // namespace f4::renderer
