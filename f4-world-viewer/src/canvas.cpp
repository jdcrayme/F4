// f4-world-viewer/src/canvas.cpp
//
// ViewerApp::handle_input (mouse pan/zoom/select) and ViewerApp::draw_canvas
// (the Raylib 2D top-down render: terrain tiles, grid, routes, objectives,
// units). These two functions are paired — handle_input mutates the
// Impl camera + selection state that draw_canvas reads.
//
// Split out of the original 1920-LoC viewer_app.cpp god-file (item #5
// of the architecture review). No behavior change.
//
// Layout (per viewer_app.hpp header comment):
//   ┌──────────────────────────────────────────────────────────────────┐
//   │ Menu bar: File | View | Help                                    │
//   ├──────────────────────────────────────────────────┬────────────────┤
//   │ Raylib canvas (2D top-down)                      │ ImGui panels   │
//   │   • Color-coded terrain tiles                    │                │
//   │   • Objective icons by type + team               │                │
//   │   • Unit squares by team                         │                │
//   │   Pan: drag  Zoom: wheel  Click: select          │                │
//   └──────────────────────────────────────────────────┴────────────────┘

#include "viewer_state.hpp"

#include <f4/terrain/terrain_data.hpp>
#include <f4/world/world_state.hpp>

#include <imgui.h>

#include <algorithm>
#include <cmath>

namespace f4::viewer {

// ---------------------------------------------------------------------------
// Input
// ---------------------------------------------------------------------------
void ViewerApp::handle_input() {
    // Pan: middle-mouse or left-mouse-drag on empty canvas
    const Vector2 mouse = GetMousePosition();
    if (IsMouseButtonPressed(MOUSE_BUTTON_MIDDLE) ||
        (IsMouseButtonPressed(MOUSE_BUTTON_LEFT) && !ImGui::GetIO().WantCaptureMouse)) {
        impl_->dragging = true;
        impl_->drag_start = mouse;
        impl_->drag_cam_x0 = impl_->cam_x;
        impl_->drag_cam_y0 = impl_->cam_y;
    }
    if (impl_->dragging &&
        (IsMouseButtonReleased(MOUSE_BUTTON_MIDDLE) || IsMouseButtonReleased(MOUSE_BUTTON_LEFT))) {
        impl_->dragging = false;
    }
    if (impl_->dragging) {
        const float dx = (mouse.x - impl_->drag_start.x) / impl_->cam_zoom;
        const float dy = (mouse.y - impl_->drag_start.y) / impl_->cam_zoom;
        impl_->cam_x = impl_->drag_cam_x0 - dx;
        impl_->cam_y = impl_->drag_cam_y0 + dy;   // y flipped
    }

    // Zoom: mouse wheel
    const float wheel = GetMouseWheelMove();
    if (wheel != 0.0f && !ImGui::GetIO().WantCaptureMouse) {
        // Zoom toward the cursor.
        float gx_before, gy_before;
        impl_->screen_to_world(mouse.x, mouse.y, &gx_before, &gy_before);
        impl_->cam_zoom *= (wheel > 0) ? 1.15f : (1.0f / 1.15f);
        impl_->cam_zoom = std::clamp(impl_->cam_zoom, 0.05f, 50.0f);
        float gx_after, gy_after;
        impl_->screen_to_world(mouse.x, mouse.y, &gx_after, &gy_after);
        impl_->cam_x += gx_before - gx_after;
        impl_->cam_y += gy_before - gy_after;
    }

    // Click: select nearest objective/unit within a tolerance radius
    if (IsMouseButtonReleased(MOUSE_BUTTON_LEFT) && !ImGui::GetIO().WantCaptureMouse) {
        float gx, gy;
        impl_->screen_to_world(mouse.x, mouse.y, &gx, &gy);
        const float tol = 8.0f / impl_->cam_zoom;  // 8-pixel pick radius

        // Try objectives first (drawn on top of terrain).
        if (impl_->show_objectives && impl_->world_loaded) {
            int best = -1;
            float best_d2 = tol * tol;
            for (int i = 0; i < static_cast<int>(impl_->world.objectives.size()); ++i) {
                const auto& o = impl_->world.objectives[i];
                const float dx = o.x - gx, dy = o.y - gy;
                const float d2 = dx * dx + dy * dy;
                if (d2 < best_d2) { best_d2 = d2; best = i; }
            }
            if (best >= 0) {
                impl_->sel_kind = Impl::SelectionKind::Objective;
                impl_->sel_index = best;
                return;
            }
        }
        // Then units.
        if (impl_->show_units && impl_->world_loaded) {
            int best = -1;
            float best_d2 = tol * tol;
            for (int i = 0; i < static_cast<int>(impl_->world.units.size()); ++i) {
                const auto& u = impl_->world.units[i];
                const float dx = u.x - gx, dy = u.y - gy;
                const float d2 = dx * dx + dy * dy;
                if (d2 < best_d2) { best_d2 = d2; best = i; }
            }
            if (best >= 0) {
                impl_->sel_kind = Impl::SelectionKind::Unit;
                impl_->sel_index = best;
                return;
            }
        }
        // Nothing hit — clear selection.
        impl_->sel_kind = Impl::SelectionKind::None;
        impl_->sel_index = -1;
    }
}

// ---------------------------------------------------------------------------
// Canvas (Raylib 2D drawing)
// ---------------------------------------------------------------------------
void ViewerApp::draw_canvas() {
    // --- Terrain tiles ---
    if (impl_->show_terrain && impl_->world.terrain_loaded) {
        const auto& td = impl_->world.terrain;
        const uint32_t gw = td.header.width;
        const uint32_t gh = td.header.height;
        // Theater grid is 1024x1024; terrain is 128x128 by default — each
        // terrain cell maps to (1024/width) theater grid units.
        const float scale = 1024.0f / static_cast<float>(gw);

        for (uint32_t y = 0; y < gh; ++y) {
            for (uint32_t x = 0; x < gw; ++x) {
                const auto t = td.tile_type_at(x, y);
                const auto c = f4::terrain::TerrainData::color_for_tile_type(t);
                const RlColor rc = to_rl(c);
                // Terrain cell (x, y) in sim coords maps to world grid
                // (x*scale, y*scale) ... ((x+1)*scale, (y+1)*scale).
                const Vector2 p0 = impl_->world_to_screen(x * scale, y * scale);
                const Vector2 p1 = impl_->world_to_screen((x + 1) * scale, (y + 1) * scale);
                const Rectangle rect = {
                    p0.x, p1.y,
                    p1.x - p0.x, p0.y - p1.y
                };
                DrawRectangleRec(rect, Color{rc.r, rc.g, rc.b, rc.a});
            }
        }
    } else {
        // No terrain — show a hint.
        DrawText("No terrain loaded. Use File > Import Terrain Binary...",
                 impl_->window_w / 2 - 200, impl_->window_h / 2, 18,
                 Color{180, 180, 180, 255});
    }

    // --- Grid (optional) ---
    if (impl_->show_grid) {
        const Color gc = {80, 80, 100, 128};
        const float step = 64.0f;   // grid lines every 64 units
        for (float g = 0; g <= 1024; g += step) {
            const Vector2 a = impl_->world_to_screen(g, 0);
            const Vector2 b = impl_->world_to_screen(g, 1024);
            DrawLineV(a, b, gc);
            const Vector2 c = impl_->world_to_screen(0, g);
            const Vector2 d = impl_->world_to_screen(1024, g);
            DrawLineV(c, d, gc);
        }
    }

    // --- Routes (road/rail network from objective link_data) ---
    // Draw thin lines between connected objectives. Roads are tan/brown,
    // rail links are dark gray. This is the ground movement network that
    // ground units (battalions/brigades) use to navigate.
    if (impl_->show_routes && impl_->world_loaded) {
        const Color road_color = {180, 160, 120, 140};
        const Color rail_color = {100, 100, 110, 160};
        const Color sel_color  = {255, 255, 100, 220};
        for (int i = 0; i < static_cast<int>(impl_->world.objectives.size()); ++i) {
            const auto& o = impl_->world.objectives[i];
            const Vector2 p = impl_->world_to_screen(o.x, o.y);
            for (const auto& link : o.links) {
                // Resolve neighbor VU_ID → objective index → position
                auto it = impl_->obj_id_to_index.find(link.neighbor_num);
                if (it == impl_->obj_id_to_index.end()) continue;
                const auto& n = impl_->world.objectives[it->second];
                const Vector2 q = impl_->world_to_screen(n.x, n.y);
                // Draw each link once (only when i < neighbor_index to avoid
                // drawing every road twice).
                if (i >= it->second) continue;
                const Color c = link.is_rail ? rail_color : road_color;
                DrawLineEx(p, q, 1.0f, c);
                // Highlight links from the selected objective.
                if (impl_->sel_kind == Impl::SelectionKind::Objective &&
                    impl_->sel_index == i) {
                    DrawLineEx(p, q, 2.0f, sel_color);
                }
            }
        }
    }

    // --- Objectives ---
    // Render with type-specific icons (airbase, bridge, city, port, radar,
    // powerplant, railroad, factory). Unknown types fall back to a small
    // circle (sized independently of priority). Icons are tinted by owner
    // color so team affiliation is visible. Priority is encoded as a thin
    // gold ring around high-priority objectives, NOT as icon size — this
    // keeps the map legible even when priority values are 1-100.
    if (impl_->show_objectives && impl_->world_loaded) {
        // Icon diameter in pixels. Scales mildly with zoom so icons stay
        // readable when zoomed in but never dominate the screen. The cap
        // keeps the fit-to-world view legible when thousands of objectives
        // share the screen.
        const float base_size = std::clamp(8.0f + impl_->cam_zoom * 0.6f,
                                           10.0f, 24.0f);
        for (int i = 0; i < static_cast<int>(impl_->world.objectives.size()); ++i) {
            const auto& o = impl_->world.objectives[i];
            const Vector2 p = impl_->world_to_screen(o.x, o.y);
            const RlColor c = color_for_owner(o.owner);
            // Use objective_type (from class table) if available; otherwise
            // fall back to the raw type (entity_type) which won't match icons.
            const int icon_idx = Impl::icon_for_objective_type(o.objective_type);
            impl_->draw_icon(icon_idx, p.x, p.y, base_size, c);
            // Priority halo: gold ring for high-priority objectives (>=40).
            // Two tiers for visual hierarchy without making icons huge.
            if (o.priority >= 40) {
                const float ring_r = base_size * 0.5f + 3.0f;
                const Color ring = (o.priority >= 70)
                    ? Color{255, 215, 0,   255}
                    : Color{255, 215, 0,   150};
                DrawCircleLines(static_cast<int>(p.x), static_cast<int>(p.y),
                                static_cast<int>(ring_r), ring);
            }
            // Outline selected.
            if (impl_->sel_kind == Impl::SelectionKind::Objective &&
                impl_->sel_index == i) {
                DrawCircleLines(static_cast<int>(p.x), static_cast<int>(p.y),
                                static_cast<int>(base_size * 0.6f + 4),
                                Color{255, 255, 0, 255});
            }
        }
    }

    // --- Units ---
    // Render with class-specific icons (square, diamond, circle, triangle)
    // tinted by owner color. Falls back to drawn shapes if icons aren't loaded.
    //   Battalion  → square icon
    //   Brigade    → diamond icon
    //   Squadron   → circle icon
    //   TaskForce  → triangle icon
    //   Flight     → hollow circle (drawn — no icon)
    //   Package    → plus/cross (drawn — no icon)
    if (impl_->show_units && impl_->world_loaded) {
        // Unit icon diameter — fixed for consistency with objective icons.
        const float s = std::clamp(6.0f + impl_->cam_zoom * 0.5f, 8.0f, 20.0f);
        for (int i = 0; i < static_cast<int>(impl_->world.units.size()); ++i) {
            const auto& u = impl_->world.units[i];
            const Vector2 p = impl_->world_to_screen(u.x, u.y);
            const RlColor c = color_for_owner(u.owner);
            const Color fill = {c.r, c.g, c.b, 220};
            // Use subtype-specific icon when available; falls back to generic
            // shape icon (square/diamond/circle/triangle) if not.
            const int icon_idx = impl_->icon_for_unit(u.unit_class, u.unit_subtype);

            if (icon_idx >= 0) {
                // Use the sprite icon.
                impl_->draw_icon(icon_idx, p.x, p.y, s, c);
            } else {
                // No icon for this class — draw a fallback shape.
                switch (u.unit_class) {
                    case f4::world::UnitClass::Flight:
                        DrawCircleLines(static_cast<int>(p.x), static_cast<int>(p.y),
                                        static_cast<int>(s * 0.5f), fill);
                        break;
                    case f4::world::UnitClass::Package:
                        DrawLineEx({p.x - s * 0.6f, p.y}, {p.x + s * 0.6f, p.y},
                                   s * 0.3f, fill);
                        DrawLineEx({p.x, p.y - s * 0.6f}, {p.x, p.y + s * 0.6f},
                                   s * 0.3f, fill);
                        break;
                    default:
                        DrawCircleV(p, s * 0.4f, fill);
                        break;
                }
            }

            // Destination line (only if moved)
            if (u.dest_x != u.x || u.dest_y != u.y) {
                const Vector2 d = impl_->world_to_screen(u.dest_x, u.dest_y);
                DrawLineEx(p, d, 1.0f, Color{c.r, c.g, c.b, 160});
            }

            // Waypoint polyline: draws the unit's flight/ground plan as a
            // thin dashed line through each waypoint. Only meaningful when
            // wp_count > 0 (rare in fresh campaign saves, common once
            // missions are assigned).
            if (u.wp_count > 0 && !u.waypoints.empty()) {
                Vector2 prev = p;
                for (const auto& w : u.waypoints) {
                    const Vector2 q = impl_->world_to_screen(
                        static_cast<float>(w.x), static_cast<float>(w.y));
                    DrawLineEx(prev, q, 1.0f, Color{c.r, c.g, c.b, 200});
                    // Small marker at each waypoint
                    DrawCircleV(q, 2.0f, Color{c.r, c.g, c.b, 220});
                    prev = q;
                }
            }

            // Squadron → home airbase link line. The airbase_id is the
            // VU_ID.num of the objective this squadron is based at. Drawn
            // as a thin curved-feeling straight line in the squadron's
            // team color so the user can see at a glance where each
            // squadron calls home.
            if (u.unit_class == f4::world::UnitClass::Squadron && u.airbase_id != 0) {
                auto it = impl_->obj_id_to_index.find(u.airbase_id);
                if (it != impl_->obj_id_to_index.end() && it->second < static_cast<int>(impl_->world.objectives.size())) {
                    const auto& ab = impl_->world.objectives[it->second];
                    const Vector2 a = impl_->world_to_screen(ab.x, ab.y);
                    DrawLineEx(p, a, 0.5f, Color{c.r, c.g, c.b, 90});
                }
            }

            // Selection outline (yellow, drawn around any shape)
            if (impl_->sel_kind == Impl::SelectionKind::Unit &&
                impl_->sel_index == i) {
                DrawCircleLines(static_cast<int>(p.x), static_cast<int>(p.y),
                                static_cast<int>(s * 0.6f + 4),
                                Color{255, 255, 0, 255});
            }
        }
    }
}

} // namespace f4::viewer
