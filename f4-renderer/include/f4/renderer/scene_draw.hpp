// f4-renderer/include/f4/renderer/scene_draw.hpp
//
// Scene composition: ground rendering, VisualModelComponent entity meshes,
// and airfield geometry — the mid layer between the primitive functions
// (draw_3d, layout_draw, feature_mesh) and render_world() (world_renderer).
//
// Each function here replaces code that was duplicated across the viewer
// apps:
//   - draw_ground()          ← scenario-player draw_scene()'s plane/grid/
//                              axes block + world-viewer draw_ground_grid()
//   - draw_entity_meshes()   ← scenario-player draw_visual_entities()
//   - draw_airfield_geometry() ← world-viewer ground_layout_3d.cpp's
//                              per-layer loops + scenario-player
//                              draw_airport()'s real-layout block
//
// Must be called inside BeginMode3D/EndMode3D unless noted otherwise.
// C++20.

#pragma once

#include <f4/renderer/render_resources.hpp>
#include <f4/renderer/coord_transform.hpp>
#include <f4/renderer/draw_3d.hpp>          // DrawStats, draw_grid_at
#include <f4/renderer/layout_draw.hpp>      // draw_layout_quad/line/marker
#include <f4/renderer/ground_layout_models.hpp>  // AirfieldGeometry3D, AirfieldDrawToggles

#include <raylib.h>
// Undef raylib macros that pollute the namespace
#undef PI
#undef DEG2RAD
#undef RAD2DEG

#include <vector>

namespace f4::models {
class ModelDatabase;
} // namespace f4::models

namespace f4::renderer {

// ---------------------------------------------------------------------------
// Ground
// ---------------------------------------------------------------------------

/// Configuration for ground-level rendering, populated by each viewer from
/// its UI state. When real terrain arrives, this struct gains terrain
/// fields (heightmap db, LOD bias, tile colors) that the flat
/// implementation ignores — the signature of draw_ground() stays fixed.
struct GroundConfig {
    bool plane = true;     ///< flat colored ground plane
    bool grid  = true;     ///< grid lines on the ground plane
    bool axes  = false;    ///< RGB coordinate axes at the origin

    float plane_size  = 20000.0f;  ///< plane half-width (feet)
    float grid_extent = 10000.0f;  ///< grid half-extent (feet)
    float grid_step   =  1000.0f;  ///< grid line spacing (feet)
    float axis_length =    50.0f;  ///< axis line length (feet)

    /// Z offsets (feet) relative to the origin elevation. The plane sits
    /// below the grid and both sit below layout surfaces so nothing
    /// z-fights with runway/taxiway quads drawn at the origin elevation.
    float plane_z_offset = -2.0f;
    float grid_z_offset  = -1.0f;

    Color plane_color = { 50,  70,  35, 255};  // ground (green grass)
    Color grid_color  = { 60,  60,  60, 255};

    /// ENU anchor (feet). Scenes are not centered on the world origin —
    /// a grid-referenced airbase lives at its objective center, hundreds
    /// of thousands of feet from (0,0,0). The plane, grid, and axes all
    /// anchor here.
    float origin_enu_x = 0.0f;
    float origin_enu_y = 0.0f;
    float origin_enu_z = 0.0f;
};

/// Draw the ground: flat plane + grid + axes anchored at
/// config.origin_enu_*. Replaces the per-app ground blocks.
///
/// Must be called inside BeginMode3D/EndMode3D.
void draw_ground(const GroundConfig& config);

// ---------------------------------------------------------------------------
// Entity meshes (VisualModelComponent path)
// ---------------------------------------------------------------------------

/// One entity's render data for the mesh draw pass. VisualModelComponent
/// lives in f4-simulation, which f4-renderer must not depend on — so the
/// app extracts position + orientation + KoreaObj model index from the
/// component and passes plain values here.
struct EntityMeshDraw {
    float enu_x = 0.0f;      ///< ENU position (feet)
    float enu_y = 0.0f;
    float enu_z = 0.0f;
    float qw = 1.0f;         ///< ENU quaternion (Hamilton, body-to-world)
    float qx = 0.0f;
    float qy = 0.0f;
    float qz = 0.0f;
    int parent_index = -1;   ///< KoreaObj model cache key
};

/// Draw entities as KoreaObj meshes at their ENU positions/orientations.
/// Meshes are built lazily via RenderResources::build_mesh_for_model() on
/// first encounter; the lit shader, backface-cull disable, and alpha blend
/// are managed internally.
///
/// When cull_radius_ft > 0, entities farther than that from
/// (cull_enu_x, cull_enu_y, cull_enu_z) are skipped (typically the camera
/// position — see world_renderer).
///
/// Must be called inside BeginMode3D/EndMode3D.
DrawStats draw_entity_meshes(
    RenderResources& res,
    f4::models::ModelDatabase& db,
    const std::vector<EntityMeshDraw>& entities,
    float cull_enu_x = 0.0f, float cull_enu_y = 0.0f, float cull_enu_z = 0.0f,
    float cull_radius_ft = 0.0f);

// ---------------------------------------------------------------------------
// Airfield geometry
// ---------------------------------------------------------------------------

/// Draw all airfield layers of an AirfieldGeometry3D respecting per-layer
/// toggles, offset from objective-local to world ENU by (origin_x/y/z).
///
/// Used by render_world() for the scene's standalone airfield and by
/// RenderEntity()'s GroundLayoutComponent dispatch; also callable directly
/// for overlays.
///
/// Must be called inside BeginMode3D/EndMode3D.
DrawStats draw_airfield_geometry(
    const AirfieldGeometry3D& geom,
    const AirfieldDrawToggles& toggles,
    float origin_x = 0.0f,
    float origin_y = 0.0f,
    float origin_z = 0.0f);

} // namespace f4::renderer
