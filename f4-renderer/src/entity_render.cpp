// f4-renderer/src/entity_render.cpp
//
// Implementation of entity_render.hpp.
//
// RenderEntity() inspects an entity's components and dispatches to the
// appropriate render functions. This consolidates the per-component
// rendering logic that was previously duplicated across viewer apps
// (canvas.cpp, renderer.cpp, ground_layout_3d.cpp).
//
// Current dispatch:
//   FeatureSetComponent + TransformComponent → draw_feature_mesh() per feature
//   ObjectiveTypeComponent                   → (future: objective 3D model)
//   UnitCoreComponent                        → (future: unit 3D model)
//
// entity_icon_info() and RenderEntityIcon() handle the 2D symbol case:
//   ObjectiveTypeComponent → symbol_for_objective_type()
//   UnitCoreComponent      → symbol_for_unit()

#include <f4/renderer/entity_render.hpp>

#include <f4/renderer/feature_mesh.hpp>     // build_feature_mesh, draw_feature_mesh

namespace f4::renderer {

// ── RenderEntity ──────────────────────────────────────────────────────────

DrawStats RenderEntity(EntityRenderResources& res,
                       f4::entities::EntityHandle& entity)
{
    DrawStats total{};

    if (!entity.valid()) return total;

    // ── FeatureSetComponent + TransformComponent ──────────────────────
    //
    // Render KoreaObj 3D models at each feature's offset position.
    // This is the pattern from canvas.cpp's feature 3D mesh overlay
    // (lines 587-719), now encapsulated as a reusable function so any
    // viewer can render an objective's features with a single call.
    //
    // The feature loop:
    //   1. Get the objective's world position from TransformComponent
    //   2. For each FeatureEntryState in FeatureSetComponent.features:
    //      a. Skip empty placeholders (index=0, offset=0)
    //      b. Skip destroyed features (if skip_destroyed_features)
    //      c. Compute entity_type = VU_LAST_ENTITY_TYPE + feature.index
    //      d. Compute feature world position = objective + feature offset
    //      e. Call draw_feature_mesh() with the entity's resources
    //
    // The mesh cache + texture cache + lit shader are shared with the
    // 3D Ground Layout panel and any other caller — a feature rendered
    // once in any view is cached for all others.
    if (res.show_features) {
        auto* tf = entity.get<f4::entities::TransformComponent>();
        auto* fs = entity.get<f4::entities::FeatureSetComponent>();

        if (tf && fs && !fs->features.empty()) {
            // Objective world position in ENU feet. Feature offsets are
            // RELATIVE to this — we add them per-feature below.
            const float obj_east_ft  = static_cast<float>(tf->position.x);
            const float obj_north_ft = static_cast<float>(tf->position.y);

            for (const auto& feature : fs->features) {
                // Skip empty placeholder features (the bridge emits
                // these when the FED entry is unused). Matches the
                // filter in canvas.cpp lines 693-697.
                if (feature.index == 0 &&
                    feature.offset_x == 0.0f &&
                    feature.offset_y == 0.0f) {
                    continue;
                }

                // Skip fully-destroyed features if configured.
                // damage_state 0=intact, 1=damaged, 2=heavily damaged,
                // 3=destroyed. Rendering destroyed buildings as rubble
                // is a future enhancement; for now we just skip them.
                if (res.skip_destroyed_features &&
                    feature.damage_state >= 3) {
                    continue;
                }

                // FeatureEntryState.index is a 0-based descriptionIndex
                // into the class table. ClassTable::vis_type_for() takes
                // entity_type = descriptionIndex + VU_LAST_ENTITY_TYPE (=100).
                // See canvas.cpp line 688 and feature_mesh.cpp header docs.
                const uint16_t entity_type = static_cast<uint16_t>(
                    res.vu_last_entity_type +
                    static_cast<uint16_t>(feature.index));

                // Feature world position = objective center + feature offset.
                // FeatureEntryState.offset_{x,y,z} are in feet relative
                // to the objective center (see types.hpp docs).
                const float feat_east_ft  = obj_east_ft  + feature.offset_x;
                const float feat_north_ft = obj_north_ft + feature.offset_y;
                const float feat_up_ft    = feature.offset_z;

                // Delegate to the existing feature mesh pipeline.
                const auto stats = draw_feature_mesh(
                    res, entity_type,
                    feat_east_ft, feat_north_ft, feat_up_ft,
                    static_cast<float>(feature.facing));

                total.draw_calls   += stats.draw_calls;
                total.meshes_drawn += stats.meshes_drawn;
                total.vertices_drawn += stats.vertices_drawn;
            }
        }
    }

    // ── Future dispatch points ────────────────────────────────────────
    //
    // VisualModelComponent (from f4-simulation):
    //   Currently handled by scenario-player's draw_visual_entities().
    //   To add here, f4-renderer would need to depend on f4-simulation,
    //   which is architecturally wrong (renderer is lower than sim).
    //   Instead, a future render-pass registration system could allow
    //   f4-simulation to register a VisualModelComponent renderer without
    //   creating a dependency cycle.
    //
    // GroundLayoutComponent:
    //   Currently handled by ground_layout_3d.cpp. Could be added here
    //   once the ground layout rendering is consolidated into f4-renderer.
    //
    // RadarComponent:
    //   Radar arc rendering (DrawCircleSector) is straightforward but
    //   currently inlined in canvas.cpp. Could be extracted here.

    return total;
}

// ── entity_icon_info ─────────────────────────────────────────────────────

EntityIconInfo entity_icon_info(f4::entities::EntityHandle& entity)
{
    EntityIconInfo info{};

    if (!entity.valid()) return info;

    // ── ObjectiveTypeComponent → objective symbol ─────────────────────
    //
    // The objective type is needed to look up the correct SymbolKind.
    // Two sources:
    //   1. PropertyBag.ints["objective_type"] — the raw type value (1..39)
    //      set by the world bridge during loading. This is the same
    //      lookup as ViewerApp::Impl::obj_type_from_pb().
    //   2. ObjectiveTypeComponent.type — the entity_type (100+), from
    //      which we derive obj_type = type - 100.
    //
    // We prefer the PropertyBag value (it's the exact original value
    // from the campaign file), but fall back to the component field
    // if PropertyBag is absent or the key is missing.
    auto* ot = entity.get<f4::entities::ObjectiveTypeComponent>();
    if (ot) {
        uint8_t obj_type = 0;

        // Try PropertyBag first (same as obj_type_from_pb).
        auto* pb = entity.get<f4::entities::PropertyBag>();
        if (pb) {
            auto it = pb->ints.find("objective_type");
            if (it != pb->ints.end()) {
                obj_type = static_cast<uint8_t>(it->second);
            }
        }

        // Fallback: derive from ObjectiveTypeComponent.type.
        // type is entity_type (100+), so obj_type = type - 100.
        // This works for types 101..139 → obj_types 1..39.
        if (obj_type == 0 && ot->type >= 100) {
            obj_type = static_cast<uint8_t>(ot->type - 100);
        }

        // Also try class_table_index if type didn't work.
        // class_table_index may also be an entity_type (100+).
        if (obj_type == 0 && ot->class_table_index >= 100) {
            obj_type = static_cast<uint8_t>(ot->class_table_index - 100);
        }

        // Last resort: if type is already in 1..39 range, use directly.
        if (obj_type == 0 && ot->type > 0 && ot->type < 100) {
            obj_type = static_cast<uint8_t>(ot->type);
        }

        if (obj_type > 0) {
            info.kind  = symbol_for_objective_type(obj_type);
            info.valid = true;
            return info;
        }
    }

    // ── UnitCoreComponent → unit symbol ───────────────────────────────
    //
    // SymbolKind is determined by unit_class (the frame shape) and
    // unit_subtype (the inner glyph — armor, fighter, bomber, etc.).
    auto* uc = entity.get<f4::entities::UnitCoreComponent>();
    if (uc) {
        info.kind  = symbol_for_unit(uc->unit_class, uc->unit_subtype);
        info.valid = true;
        return info;
    }

    // No renderable icon components found.
    return info;
}

// ── RenderEntityIcon ─────────────────────────────────────────────────────

void RenderEntityIcon(f4::entities::EntityHandle& entity,
                      float center_x, float center_y,
                      float size_px,
                      RlColor fill_color, RlColor outline_color,
                      bool filled)
{
    // Determine the icon, then draw it. If the entity has no icon
    // components (e.g. a Campaign or Team entity), this is a no-op.
    const auto info = entity_icon_info(entity);
    if (!info.valid) return;

    draw_symbol(info.kind, center_x, center_y, size_px,
                fill_color, outline_color, filled);
}

} // namespace f4::renderer
