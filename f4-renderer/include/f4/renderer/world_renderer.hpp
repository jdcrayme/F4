// f4-renderer/include/f4/renderer/world_renderer.hpp
//
// render_world() — the single entry point for rendering a chunk of the
// world from a specific camera.
//
// A viewer populates a SceneDescription from its state each frame and
// calls render_world(). Everything else in f4-renderer (draw_ground,
// RenderEntity, draw_entity_meshes, draw_airfield_geometry, feature
// meshes, texture/mesh caches) is composed underneath this call.
//
// Current state of the world rendering (no terrain yet):
//   - flat ground plane + grid anchored anywhere in the world
//   - airfield geometry (standalone or via GroundLayoutComponent dispatch)
//   - KoreaObj feature models on objectives (FeatureSetComponent dispatch)
//   - VisualModelComponent entity meshes (aircraft, vehicles) via the
//     entity_meshes list (extracted by the app — see EntityMeshDraw)
//   - optional range culling around the camera
//   - optional in-scene overlay callback and 2D label pass
//
// Terrain rendering (theater heightmap tiles) is the eventual extension;
// it slots in behind this same signature via GroundConfig.
//
// Must be called between BeginDrawing/EndDrawing. C++20.

#pragma once

#include <f4/renderer/render_resources.hpp>
#include <f4/renderer/scene_draw.hpp>
#include <f4/renderer/world_camera.hpp>    // camera construction helpers
#include <f4/renderer/entity_render.hpp>
#include <f4/renderer/feature_mesh.hpp>
#include <f4/renderer/draw_3d.hpp>           // DrawStats
#include <f4/renderer/ground_layout_models.hpp>

#include <f4/entities/entity.hpp>

#include <raylib.h>
// Undef raylib macros that pollute the namespace
#undef PI
#undef DEG2RAD
#undef RAD2DEG

#include <functional>
#include <vector>

namespace f4::models { class ModelDatabase; }
namespace f4::world_convert { class ClassTable; }

namespace f4::renderer {

/// Aggregated statistics for one rendered frame. Used by viewers for
/// diagnostics (e.g. the world-viewer's 3D panel counters, HUDs).
struct FrameStats {
    DrawStats draw;             ///< meshes/quads/lines drawn + vertices

    int entities_total = 0;     ///< component-dispatch entities considered
    int entities_drawn = 0;     ///< that produced at least one draw call
    int entity_meshes_drawn = 0;///< VisualModelComponent entities drawn
};

/// Complete description of one frame: what camera, what chunk of world,
/// which entities, which overlays. Populate from app state each frame.
struct SceneDescription {
    // ── Camera ─────────────────────────────────────────────────────────
    /// Raylib camera in RH Y-up feet. Build it from world coordinates
    /// with the helpers in world_camera.hpp (free/orbit) or set up an
    /// orthographic top-down camera directly.
    Camera3D camera = {};
    float near_plane = 1.0f;
    float far_plane = 250000.0f;

    // ── Sky / ground ───────────────────────────────────────────────────
    Color sky_color = {135, 175, 220, 255};
    GroundConfig ground;

    // ── Offscreen target ───────────────────────────────────────────────
    /// When non-null, render_world() wraps the frame in
    /// BeginTextureMode/EndTextureMode so the result lands in this render
    /// texture (e.g. the world-viewer's 3D layout tab blits it into
    /// ImGui). Null renders to the window backbuffer.
    RenderTexture2D* target = nullptr;

    // ── Component-dispatch entities ────────────────────────────────────
    //
    // Entities rendered by walking `entities` and dispatching on their
    // f4-entities components (GroundLayoutComponent → airfield geometry,
    // FeatureSetComponent → KoreaObj feature models). Requires `world`.
    //
    // Apps select what belongs in the chunk: e.g. the world-viewer's 3D
    // world mode fills this with EntityWorld::within_radius() around the
    // camera; the 3D layout tab fills it with just the selected objective.
    f4::entities::EntityWorld* world = nullptr;
    std::vector<f4::entities::EntityId> entities;

    /// KoreaObj model database + Falcon4.ct class table for the feature
    /// model path. May be null (feature dispatch degrades to no-ops).
    f4::models::ModelDatabase* model_db = nullptr;
    f4::world_convert::ClassTable* class_table = nullptr;

    /// Layer toggles applied to GroundLayoutComponent dispatch and the
    /// standalone airfield below.
    AirfieldDrawToggles airfield_toggles;

    /// Render 3D KoreaObj feature models for FeatureSetComponent entities
    /// (requires model_db + class_table). When false or when models
    /// aren't available, enable airfield_toggles.features to draw flat
    /// footprints instead.
    bool draw_feature_models = true;

    // ── VisualModelComponent entity meshes ─────────────────────────────
    /// Entities whose meshes are drawn via draw_entity_meshes() — the app
    /// extracts these from VisualModelComponent (which lives in
    /// f4-simulation; f4-renderer can't depend on it). See EntityMeshDraw.
    std::vector<EntityMeshDraw> entity_meshes;

    // ── Standalone airfield ────────────────────────────────────────────
    /// Optional pre-built airfield geometry rendered without going
    /// through the entity list (e.g. the scenario-player's real campaign
    /// layout, the world-viewer 3D tab's selected-objective geometry).
    /// origin_enu places the objective-local geometry in the world.
    const AirfieldGeometry3D* airfield = nullptr;
    float airfield_origin_enu[3] = {0.0f, 0.0f, 0.0f};

    // ── Range culling ──────────────────────────────────────────────────
    /// Entities + entity meshes farther than this from the camera
    /// position (ENU feet) are skipped. 0 = unlimited.
    float cull_radius_ft = 0.0f;

    // ── Overlays ───────────────────────────────────────────────────────
    /// Invoked inside the 3D mode after entities are drawn (same camera,
    /// far plane already extended) — for app-specific in-scene overlays
    /// (scenario routes, compass rose, selection highlights).
    std::function<void(const Camera3D&)> overlay_3d;

    /// Draw airfield marker labels (parking spots, runway ends, helipads)
    /// in the 2D pass after EndMode3D. Uses the toggles' parking/labels.
    bool airfield_labels = false;
};

/// Render one frame of the world. Composes everything below it in
/// f4-renderer: ClearBackground, (optional) BeginTextureMode,
/// BeginMode3D + far-plane extension, draw_ground, entity dispatch,
/// entity meshes, standalone airfield, overlay_3d, EndMode3D, label pass.
///
/// Call between BeginDrawing/EndDrawing. After it returns the viewer can
/// draw app-specific 2D overlays (HUDs, icons, ImGui is drawn later by
/// the app as usual).
FrameStats render_world(RenderResources& res, const SceneDescription& scene);

} // namespace f4::renderer
