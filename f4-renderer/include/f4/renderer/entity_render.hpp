// f4-renderer/include/f4/renderer/entity_render.hpp
//
// Entity-level rendering — inspects an entity's components and dispatches
// to the appropriate render functions (3D meshes, 2D map symbols).
//
// Design
// ------
// Instead of each viewer app duplicating the "check component X, then
// draw Y" pattern, these functions encapsulate the component→render
// dispatch so any viewer can render an entity with a single call.
//
// RenderEntity() handles 3D visual representation:
//   - FeatureSetComponent + TransformComponent
//     → renders KoreaObj 3D models at each FeatureEntryState offset
//       (the pattern from canvas.cpp's feature 3D mesh overlay, now
//       reusable by any viewer)
//   - GroundLayoutComponent + TransformComponent
//     → renders airfield geometry (runways, taxiways, parking, etc.)
//   - UnitCoreComponent + TransformComponent
//     → renders a KoreaObj 3D model for the unit at its campaign
//       position, oriented by GroundTacticalComponent::heading. This
//       is the campaign-view (low-fidelity) path: one model per unit,
//       not per individual vehicle. See "Mode A" in the unit-rendering
//       design notes. Deaggregated vehicle rendering (Mode B) is a
//       future f4-simulation responsibility and will ride the existing
//       VisualModelComponent / draw_entity_meshes() path.
//   - Future: VisualModelComponent (draw_entity_meshes already covers
//     this for callers that pre-resolve ModelRecord* into EntityMeshDraw).
//
// RenderEntityIcon() handles 2D map symbol representation:
//   - ObjectiveTypeComponent → objective symbol (shape encodes type)
//   - UnitCoreComponent → unit symbol (frame + glyph)
//
// entity_icon_info() is a pure-query variant that returns the SymbolKind
// without drawing — useful for legends, tooltips, and hover states.
//
// Coordinate conventions
// ---------------------
// RenderEntity() uses ENU feet (the simulation's native frame) from
// TransformComponent and converts to Raylib RH Y-up internally via
// draw_feature_mesh(). Must be called inside BeginMode3D/EndMode3D.
//
// RenderEntityIcon() takes explicit screen-space coordinates (the caller
// is responsible for world→screen projection, which varies between the
// 2D canvas and any other projection). Must be called inside
// BeginDrawing/EndDrawing.

#pragma once

#include <f4/renderer/feature_mesh.hpp>   // FeatureMeshResources, DrawStats
#include <f4/renderer/ground_layout_models.hpp>  // AirfieldGeometry3D, AirfieldDrawToggles
#include <f4/renderer/symbols.hpp>         // SymbolKind, RlColor, draw_symbol

#include <f4/entities/entity.hpp>          // EntityHandle, components

#include <cstdint>
#include <unordered_map>

namespace f4::renderer {

class RenderResources;

// ---------------------------------------------------------------------------
// EntityRenderResources
// ---------------------------------------------------------------------------
// Extends FeatureMeshResources (model_db, class_table, texture_cache,
// lit_shader, mesh_cache, default_material, lighting) with entity-specific
// render state and toggles.
//
// Inheritance: EntityRenderResources IS-A FeatureMeshResources, so it
// can be passed directly to draw_feature_mesh() and any other function
// that takes a FeatureMeshResources&. No virtual methods — no slicing risk.
// ---------------------------------------------------------------------------

/// Bundle of resources needed to render entities. Built once and passed
/// (by reference) to RenderEntity() for each entity.
///
/// Lifetime: the caller owns all pointed-to objects (ModelDatabase,
/// ClassTable, TextureCache, LitShader, default_material, EntityWorld).
/// The mesh_cache is also caller-owned so the same cache can be reused
/// across multiple RenderEntity() calls.
struct EntityRenderResources : FeatureMeshResources {
    /// Whether to render features (3D KoreaObj models) on objectives.
    /// When false, the FeatureSetComponent path is skipped entirely.
    bool show_features = true;

    /// VU_LAST_ENTITY_TYPE — the constant added to FeatureEntryState.index
    /// to compute the class_table_index passed to ClassTable::vis_type_for().
    /// Default 100 per FreeFalcon convention (see canvas.cpp line 688).
    uint16_t vu_last_entity_type = 100;

    /// Whether to skip fully-destroyed features (damage_state == 3).
    /// When true, features at maximum damage are not rendered (they've
    /// been destroyed and should not appear in the scene).
    bool skip_destroyed_features = true;

    /// Whether to render GroundLayoutComponent airfield geometry.
    /// Disable when the caller draws the same objective's geometry via a
    /// standalone SceneDescription::airfield (double draw) or wants only
    /// feature models.
    bool show_ground_layout = true;

    /// Whether to render UnitCoreComponent 3D models for campaign units.
    /// When true, each unit entity with a TransformComponent and a
    /// class_table_index >= 100 is rendered via draw_feature_mesh() at
    /// its campaign position, oriented by GroundTacticalComponent::heading
    /// (when present). This is the Mode A (single-icon-per-unit) path —
    /// see file header for the Mode A vs Mode B distinction. Disable
    /// when the caller already draws units via VisualModelComponent /
    /// draw_entity_meshes() (Mode B / aircraft path), or when only 2D
    /// unit symbols are wanted.
    bool show_units = true;

    /// Per-layer toggles for the GroundLayoutComponent dispatch.
    AirfieldDrawToggles airfield_toggles;

    /// Airfield geometry cache (EntityId.value → pre-built
    /// AirfieldGeometry3D), so RenderEntity() doesn't rebuild geometry
    /// every frame. Typically points at RenderResources::airfield_cache.
    std::unordered_map<uint64_t, AirfieldGeometry3D>* airfield_cache =
        nullptr;
};

/// Build an EntityRenderResources bundle from a RenderResources instance
/// (shader/texture/mesh caches, default material, lighting) plus the
/// feature-path database + class table. This replaces the per-viewer
/// pointer wiring that each app used to duplicate. If the class table or
/// model db is null (or the default material can't be created), the
/// affected paths degrade to no-ops rather than crashing.
EntityRenderResources make_entity_render_resources(
    RenderResources& res,
    f4::models::ModelDatabase* db,
    f4::world_convert::ClassTable* ct);

// ---------------------------------------------------------------------------
// RenderEntity
// ---------------------------------------------------------------------------
// Render the 3D visual representation of an entity by inspecting its
// components and dispatching to appropriate render functions.
//
// Must be called inside a BeginMode3D/EndMode3D block. The caller is
// responsible for setting up the camera — typically:
//   - For the 2D canvas: a top-down orthographic Camera3D matching the
//     world_to_screen transform (see canvas.cpp lines 637-657).
//   - For the 3D panel: an orbit Camera3D positioned by OrbitCamera.
//
// Component dispatch order:
//   1. GroundLayoutComponent + TransformComponent:
//      Renders the airfield geometry (runway surfaces, threshold bars,
//      centerline dashes, taxiways, parking/helipad/runway-end markers)
//      via draw_airfield_geometry(), built once and cached in
//      res.airfield_cache (when non-null) keyed by EntityId.
//   2. FeatureSetComponent + TransformComponent:
//      Renders KoreaObj 3D models at each FeatureEntryState offset,
//      using the existing draw_feature_mesh() pipeline. This handles
//      the buildings, structures, and other features that sit on
//      objectives (airbases, cities, etc.).
//   3. UnitCoreComponent + TransformComponent:
//      Renders one KoreaObj 3D model for the unit at its campaign
//      position (TransformComponent.position, ENU feet), oriented by
//      GroundTacticalComponent::heading when present. The model is
//      resolved through the same ClassTable::vis_type_for() →
//      ModelDatabase pipeline as features — the class_table_index is
//      the unit's entity_type from the campaign (150+ for unit classes).
//
//      This is the Mode A (campaign-view) unit render: one model per
//      unit, not per vehicle. It mirrors the inline code that lived in
//      f4-world-viewer/src/canvas.cpp (now lifted here so any
//      render_world() caller gets units for free).
//
//      Mode B (deaggregated vehicles, one mesh per tank/aircraft in
//      the unit's roster) is a future f4-simulation responsibility —
//      it will populate VisualModelComponent per spawned vehicle and
//      ride the existing draw_entity_meshes() path, not this branch.
//
// Future extensions (not yet implemented):
//   - VisualModelComponent: direct dispatch (currently the app extracts
//     these into EntityMeshDraw / draw_entity_meshes()).
//   - RadarComponent: radar detection arcs
//
// @param res     Entity render resources (model DB, caches, toggles)
// @param entity  Entity handle to render (must be valid)
// @return        DrawStats describing what was drawn
// ---------------------------------------------------------------------------

DrawStats RenderEntity(EntityRenderResources& res,
                       f4::entities::EntityHandle& entity);

// ---------------------------------------------------------------------------
// EntityIconInfo
// ---------------------------------------------------------------------------
// Result of inspecting an entity to determine its 2D map icon.
// ---------------------------------------------------------------------------

/// Information about an entity's 2D map icon, determined by inspecting
/// its components without performing any drawing.
struct EntityIconInfo {
    /// Which symbol to draw. SymbolCount means no icon (entity has no
    /// renderable icon components — e.g. a Campaign or Team entity).
    SymbolKind kind = SymbolKind::SymbolCount;

    /// Whether the entity has an icon at all.
    /// When false, kind is SymbolCount and should not be drawn.
    bool valid = false;
};

// ---------------------------------------------------------------------------
// entity_icon_info
// ---------------------------------------------------------------------------
// Determine the 2D map icon for an entity by inspecting its components.
// Pure query — no drawing, no GL context required.
//
// Resolution order:
//   1. ObjectiveTypeComponent + PropertyBag:
//      Uses PropertyBag.ints["objective_type"] if available (the same
//      lookup as ViewerApp::Impl::obj_type_from_pb()). Falls back to
//      deriving from ObjectiveTypeComponent.type (type - 100) if the
//      PropertyBag key is absent.
//   2. UnitCoreComponent:
//      Uses unit_class + unit_subtype to select a unit symbol.
//   3. If neither component is present, returns {SymbolCount, false}.
//
// @param entity  Entity handle to query
// @return        EntityIconInfo with the resolved SymbolKind
// ---------------------------------------------------------------------------

EntityIconInfo entity_icon_info(f4::entities::EntityHandle& entity);

// ---------------------------------------------------------------------------
// RenderEntityIcon
// ---------------------------------------------------------------------------
// Render the 2D map icon/symbol for an entity.
//
// Must be called inside BeginDrawing/EndDrawing (uses raylib 2D
// primitives, not 3D mode). The caller provides screen-space coordinates
// because world→screen projection varies between the 2D canvas, minimap,
// and any other projection.
//
// Component dispatch:
//   - ObjectiveTypeComponent → draws an objective symbol (shape encodes
//     the objective type — airbase, bridge, city, etc.)
//   - UnitCoreComponent → draws a unit symbol (frame + glyph, where
//     the frame encodes UnitClass and the glyph encodes unit_subtype)
//
// This is a convenience wrapper: it calls entity_icon_info() and then
// draw_symbol(). If the entity has no icon components, this is a no-op.
//
// @param entity        Entity handle to render (must be valid)
// @param center_x      Screen-space X center of the symbol
// @param center_y      Screen-space Y center of the symbol
// @param size_px       Symbol extent in pixels (width = height = size_px)
// @param fill_color    Fill color (typically team color)
// @param outline_color Outline color
// @param filled        If false, draws outline only (for hover/selection)
// ---------------------------------------------------------------------------

void RenderEntityIcon(f4::entities::EntityHandle& entity,
                      float center_x, float center_y,
                      float size_px,
                      RlColor fill_color, RlColor outline_color,
                      bool filled = true);

} // namespace f4::renderer
