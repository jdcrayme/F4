// f4-world-viewer/src/canvas.cpp
//
// ViewerApp::handle_input (mouse pan/zoom/select) and ViewerApp::draw_canvas
// (the Raylib 2D top-down render: terrain tiles, grid, routes, objectives,
// units). These two functions are paired — handle_input mutates the
// Impl camera + selection state that draw_canvas reads.
//
// Migrated from WorldState to EntityWorld (Step 4c).

#include "viewer_state.hpp"

#include <f4/terrain/terrain_data.hpp>
#include <f4/world_convert/objective_decoder.hpp>  // objective_type_name()

#include <imgui.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>

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
        impl_->cam_zoom = std::clamp(impl_->cam_zoom, 0.05f, 2000.0f);
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
            f4::entities::EntityId best_id;
            float best_d2 = tol * tol;
            for (const auto& eid : impl_->pop.objectives) {
                auto h = impl_->handle(eid);
                auto* tr = h.get<f4::entities::TransformComponent>();
                if (!tr) continue;
                const float ox = impl_->grid_x(tr), oy = impl_->grid_y(tr);
                const float dx = ox - gx, dy = oy - gy;
                const float d2 = dx * dx + dy * dy;
                if (d2 < best_d2) { best_d2 = d2; best_id = eid; }
            }
            if (best_id.valid()) {
                impl_->sel_kind = Impl::SelectionKind::Objective;
                impl_->sel_entity = best_id;
                return;
            }
        }
        // Then units.
        if (impl_->show_units && impl_->world_loaded) {
            f4::entities::EntityId best_id;
            float best_d2 = tol * tol;
            for (const auto& eid : impl_->pop.units) {
                auto h = impl_->handle(eid);
                auto* tr = h.get<f4::entities::TransformComponent>();
                if (!tr) continue;
                const float ux = impl_->grid_x(tr), uy = impl_->grid_y(tr);
                const float dx = ux - gx, dy = uy - gy;
                const float d2 = dx * dx + dy * dy;
                if (d2 < best_d2) { best_d2 = d2; best_id = eid; }
            }
            if (best_id.valid()) {
                impl_->sel_kind = Impl::SelectionKind::Unit;
                impl_->sel_entity = best_id;
                return;
            }
        }
        // Nothing hit — clear selection.
        impl_->sel_kind = Impl::SelectionKind::None;
        impl_->sel_entity = f4::entities::EntityId{};
    }
}

// ---------------------------------------------------------------------------
// Canvas (Raylib 2D drawing)
// ---------------------------------------------------------------------------

void ViewerApp::Impl::invalidate_terrain_cache() {
    terrain_cache_valid = false;
}

void ViewerApp::Impl::ensure_terrain_cache() {
    if (terrain_cache_valid) return;
    if (!terrain_loaded) return;
    const auto& td = terrain;
    const uint32_t gw = td.header.width;
    const uint32_t gh = td.header.height;
    if (gw == 0 || gh == 0) return;

    if (terrain_cache.id != 0) {
        UnloadRenderTexture(terrain_cache);
        terrain_cache = {0};
    }
    terrain_cache = LoadRenderTexture(static_cast<int>(gw),
                                       static_cast<int>(gh));
    if (terrain_cache.id == 0) {
        return;
    }
    SetTextureFilter(terrain_cache.texture, TEXTURE_FILTER_POINT);

    BeginTextureMode(terrain_cache);
    ClearBackground(Color{0, 0, 0, 255});
    for (uint32_t y = 0; y < gh; ++y) {
        for (uint32_t x = 0; x < gw; ++x) {
            const auto t = td.tile_type_at(x, gh-y-1);
            const auto c = f4::terrain::TerrainData::color_for_tile_type(t);
            DrawRectangle(static_cast<int>(x), static_cast<int>(y),
                          1, 1, Color{c.r, c.g, c.b, c.a});
        }
    }
    EndTextureMode();
    terrain_cache_valid = true;
}

void ViewerApp::draw_canvas() {
    // --- Terrain tiles ---
    if (impl_->show_terrain && impl_->terrain_loaded) {
        impl_->ensure_terrain_cache();
        if (impl_->terrain_cache_valid && impl_->terrain_cache.id != 0) {
            const Vector2 p0 = impl_->world_to_screen(0.0f, 0.0f);
            const Vector2 p1 = impl_->world_to_screen(1024.0f, 1024.0f);
            const Rectangle src = {0, 0,
                static_cast<float>(impl_->terrain_cache.texture.width),
                -static_cast<float>(impl_->terrain_cache.texture.height)};
            const Rectangle dst = {p0.x, p1.y,
                p1.x - p0.x, p0.y - p1.y};
            const Vector2 origin = {0, 0};
            DrawTexturePro(impl_->terrain_cache.texture, src, dst, origin,
                           0.0f, Color{255, 255, 255, 255});
        } else {
            const auto& td = impl_->terrain;
            const uint32_t gw = td.header.width;
            const uint32_t gh = td.header.height;
            const float scale = 1024.0f / static_cast<float>(gw);
            for (uint32_t y = 0; y < gh; ++y) {
                for (uint32_t x = 0; x < gw; ++x) {
                    const auto t = td.tile_type_at(x, y);
                    const auto c = f4::terrain::TerrainData::color_for_tile_type(t);
                    const RlColor rc = to_rl(c);
                    const Vector2 p0 = impl_->world_to_screen(x * scale, y * scale);
                    const Vector2 p1 = impl_->world_to_screen((x + 1) * scale, (y + 1) * scale);
                    const Rectangle rect = {p0.x, p1.y, p1.x - p0.x, p0.y - p1.y};
                    DrawRectangleRec(rect, Color{rc.r, rc.g, rc.b, rc.a});
                }
            }
        }
    } else if (!impl_->terrain_loaded) {
        DrawText("No terrain loaded. Use File > Import Terrain Binary...",
                 impl_->window_w / 2 - 200, impl_->window_h / 2, 18,
                 Color{180, 180, 180, 255});
    }

    // --- Grid (optional) ---
    if (impl_->show_grid) {
        const Color gc = {80, 80, 100, 128};
        const float step = 64.0f;
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
    if (impl_->show_routes && impl_->world_loaded) {
        const Color road_color = {180, 160, 120, 140};
        const Color rail_color = {100, 100, 110, 160};
        const Color sel_color  = {255, 255, 100, 220};
        for (std::size_t i = 0; i < impl_->pop.objectives.size(); ++i) {
            const auto eid = impl_->pop.objectives[i];
            auto h = impl_->handle(eid);
            auto* tr = h.get<f4::entities::TransformComponent>();
            auto* nl = h.get<f4::entities::NetworkLinksComponent>();
            if (!tr || !nl) continue;
            const float ox = impl_->grid_x(tr), oy = impl_->grid_y(tr);
            const Vector2 p = impl_->world_to_screen(ox, oy);
            for (const auto& link : nl->links) {
                auto it = impl_->pop.objective_id_map.find(link.neighbor_num);
                if (it == impl_->pop.objective_id_map.end()) continue;
                auto nh = impl_->handle(it->second);
                auto* n_tr = nh.get<f4::entities::TransformComponent>();
                if (!n_tr) continue;
                const Vector2 q = impl_->world_to_screen(impl_->grid_x(n_tr), impl_->grid_y(n_tr));
                // Draw each link once (only when i < neighbor_index).
                // Find the neighbor's index in pop.objectives.
                std::size_t n_idx = 0;
                for (; n_idx < impl_->pop.objectives.size(); ++n_idx) {
                    if (impl_->pop.objectives[n_idx] == it->second) break;
                }
                if (i >= n_idx) continue;
                const Color c = link.is_rail ? rail_color : road_color;
                DrawLineEx(p, q, 1.0f, c);
                if (impl_->sel_kind == Impl::SelectionKind::Objective &&
                    impl_->sel_entity == eid) {
                    DrawLineEx(p, q, 2.0f, sel_color);
                }
            }
        }
    }

    // --- Objectives ---
    if (impl_->show_objectives && impl_->world_loaded) {
        const float base_size = std::clamp(8.0f + impl_->cam_zoom * 0.6f,
                                           10.0f, 24.0f);
        const bool draw_labels = impl_->cam_zoom > 8.0f;
        const float cull_margin = base_size + (draw_labels ? 80.0f : 0.0f);
        const float sx_min = -cull_margin;
        const float sx_max = static_cast<float>(impl_->window_w) + cull_margin;
        const float sy_min = -cull_margin;
        const float sy_max = static_cast<float>(impl_->window_h) + cull_margin;

        for (const auto& eid : impl_->pop.objectives) {
            auto h = impl_->handle(eid);
            auto* tr = h.get<f4::entities::TransformComponent>();
            auto* own = h.get<f4::entities::OwnershipComponent>();
            auto* ot = h.get<f4::entities::ObjectiveTypeComponent>();
            auto* pri = h.get<f4::entities::ObjectivePriorityComponent>();
            auto* pb = h.get<f4::entities::PropertyBag>();
            if (!tr || !own || !ot) continue;

            const float ox = impl_->grid_x(tr), oy = impl_->grid_y(tr);
            const Vector2 p = impl_->world_to_screen(ox, oy);
            if (p.x < sx_min || p.x > sx_max || p.y < sy_min || p.y > sy_max) continue;

            // Search filter
            if (impl_->objective_search_lower[0] != '\0') {
                if (!ot->class_name.empty()) {
                    char haystack_buf[256];
                    const std::size_t hn = std::min<std::size_t>(ot->class_name.size(), 255);
                    for (std::size_t k = 0; k < hn; ++k) {
                        char c = ot->class_name[k];
                        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
                        haystack_buf[k] = c;
                    }
                    haystack_buf[hn] = '\0';
                    if (std::strstr(haystack_buf, impl_->objective_search_lower) == nullptr) {
                        continue;
                    }
                } else {
                    continue;
                }
            }
            // Team filter
            RlColor c = color_for_owner(own->team);
            if (impl_->team_filter != 0xFF && own->team != impl_->team_filter) {
                c.r = static_cast<unsigned char>(c.r * 0.3f);
                c.g = static_cast<unsigned char>(c.g * 0.3f);
                c.b = static_cast<unsigned char>(c.b * 0.3f);
                c.a = static_cast<unsigned char>(c.a * 0.3f);
            }

            const uint8_t obj_type = impl_->obj_type_from_pb(pb);
            const SymbolKind kind = symbol_for_objective_type(obj_type);
            const RlColor outline = {
                static_cast<unsigned char>(c.r * 0.4f),
                static_cast<unsigned char>(c.g * 0.4f),
                static_cast<unsigned char>(c.b * 0.4f),
                255};
            impl_->draw_symbol(kind, p.x, p.y, base_size, c, outline);
            if (pri && pri->priority >= 40) {
                const float ring_r = base_size * 0.5f + 3.0f;
                const Color ring = (pri->priority >= 70)
                    ? Color{255, 215, 0,   255}
                    : Color{255, 215, 0,   150};
                DrawCircleLines(static_cast<int>(p.x), static_cast<int>(p.y),
                                static_cast<int>(ring_r), ring);
            }
            if (draw_labels) {
                std::string label;
                if (!ot->class_name.empty()) {
                    label = ot->class_name;
                } else {
                    label = f4::world_convert::objective_type_name(
                        static_cast<int16_t>(obj_type));
                }
                if (!label.empty() && label != "Unknown") {
                    const float lx = p.x + base_size * 0.6f + 4;
                    const float ly = p.y - 6;
                    DrawText(label.c_str(),
                             static_cast<int>(lx) + 1,
                             static_cast<int>(ly) + 1, 10,
                             Color{0, 0, 0, 200});
                    DrawText(label.c_str(),
                             static_cast<int>(lx),
                             static_cast<int>(ly), 10,
                             Color{240, 240, 240, 230});
                }
            }
            if (impl_->sel_kind == Impl::SelectionKind::Objective &&
                impl_->sel_entity == eid) {
                DrawCircleLines(static_cast<int>(p.x), static_cast<int>(p.y),
                                static_cast<int>(base_size * 0.6f + 4),
                                Color{255, 255, 0, 255});
            }
        }
    }

    // --- Units ---
    if (impl_->show_units && impl_->world_loaded) {
        const float s = std::clamp(6.0f + impl_->cam_zoom * 0.5f, 8.0f, 20.0f);
        const float cull_margin = s + 4.0f;
        const float sx_min = -cull_margin;
        const float sx_max = static_cast<float>(impl_->window_w) + cull_margin;
        const float sy_min = -cull_margin;
        const float sy_max = static_cast<float>(impl_->window_h) + cull_margin;
        for (const auto& eid : impl_->pop.units) {
            auto h = impl_->handle(eid);
            auto* tr = h.get<f4::entities::TransformComponent>();
            auto* uc = h.get<f4::entities::UnitCoreComponent>();
            auto* pb = h.get<f4::entities::PropertyBag>();
            if (!tr || !uc) continue;

            const float ux = impl_->grid_x(tr), uy = impl_->grid_y(tr);
            const Vector2 p = impl_->world_to_screen(ux, uy);
            if (p.x < sx_min || p.x > sx_max || p.y < sy_min || p.y > sy_max) continue;

            // Owner from tag
            auto team_tag = h.get_tag(f4::entities::tags::TEAM);
            const uint8_t owner = (team_tag && team_tag->type == f4::entities::TagValue::Type::Int)
                ? static_cast<uint8_t>(team_tag->int_val) : 0;
            RlColor c = color_for_owner(owner);
            if (impl_->team_filter != 0xFF && owner != impl_->team_filter) {
                c.r = static_cast<unsigned char>(c.r * 0.3f);
                c.g = static_cast<unsigned char>(c.g * 0.3f);
                c.b = static_cast<unsigned char>(c.b * 0.3f);
                c.a = static_cast<unsigned char>(c.a * 0.3f);
            }
            const SymbolKind kind = symbol_for_unit(uc->unit_class, uc->unit_subtype);
            const RlColor outline = {
                static_cast<unsigned char>(c.r * 0.4f),
                static_cast<unsigned char>(c.g * 0.4f),
                static_cast<unsigned char>(c.b * 0.4f),
                255};
            impl_->draw_symbol(kind, p.x, p.y, s, c, outline);

            // Destination line — reads from MovementOrdersComponent (promoted
            // from PropertyBag in Phase 5 cleanup).
            if (impl_->show_unit_destinations) {
                auto* mo = h.get<f4::entities::MovementOrdersComponent>();
                if (mo) {
                    const int16_t dest_x = mo->dest_x;
                    const int16_t dest_y = mo->dest_y;
                    if (dest_x != static_cast<int16_t>(ux) || dest_y != static_cast<int16_t>(uy)) {
                        const Vector2 d = impl_->world_to_screen(dest_x, dest_y);
                        DrawLineEx(p, d, 1.0f, Color{c.r, c.g, c.b, 160});
                    }
                }
            }

            // Waypoint polyline
            if (impl_->show_waypoints) {
                auto* wp = h.get<f4::entities::WaypointPlanComponent>();
                if (wp && !wp->waypoints.empty()) {
                    Vector2 prev = p;
                    for (const auto& w : wp->waypoints) {
                        const Vector2 q = impl_->world_to_screen(
                            static_cast<float>(w.x), static_cast<float>(w.y));
                        DrawLineEx(prev, q, 1.0f, Color{c.r, c.g, c.b, 200});
                        DrawCircleV(q, 2.0f, Color{c.r, c.g, c.b, 220});
                        prev = q;
                    }
                }
            }

            // Squadron → home airbase link line
            if (impl_->show_squadron_links &&
                uc->unit_class == f4::entities::UnitClass::Squadron) {
                auto* sq = h.get<f4::entities::SquadronComponent>();
                // The raw airbase_id VU_ID was removed from SquadronComponent;
                // use the resolved airbase EntityId directly (no map lookup needed).
                if (sq && sq->airbase.valid()) {
                    auto ah = impl_->handle(sq->airbase);
                    auto* a_tr = ah.get<f4::entities::TransformComponent>();
                    if (a_tr) {
                        const Vector2 a = impl_->world_to_screen(impl_->grid_x(a_tr), impl_->grid_y(a_tr));
                        DrawLineEx(p, a, 0.5f, Color{c.r, c.g, c.b, 90});
                    }
                }
            }

            // Battalion → Brigade hierarchy lines
            if (impl_->show_hierarchy_lines &&
                uc->unit_class == f4::entities::UnitClass::Battalion) {
                auto* hier = h.get<f4::entities::HierarchyComponent>();
                // The raw parent_id VU_ID was removed; use the resolved parent EntityId.
                if (hier && hier->parent.valid()) {
                    auto ph = impl_->handle(hier->parent);
                    auto* p_tr = ph.get<f4::entities::TransformComponent>();
                    if (p_tr) {
                        const Vector2 pp = impl_->world_to_screen(impl_->grid_x(p_tr), impl_->grid_y(p_tr));
                        DrawLineEx(p, pp, 1.0f, Color{c.r, c.g, c.b, 140});
                    }
                }
            }

            // Brigade → child Battalion elements
            if (impl_->show_hierarchy_lines &&
                uc->unit_class == f4::entities::UnitClass::Brigade) {
                auto* hier = h.get<f4::entities::HierarchyComponent>();
                // The raw element_ids VU_ID vector was removed; iterate the
                // resolved children EntityIds directly.
                if (hier && !hier->children.empty()) {
                    for (const auto& child_eid : hier->children) {
                        if (!child_eid.valid()) continue;
                        auto ch = impl_->handle(child_eid);
                        auto* c_tr = ch.get<f4::entities::TransformComponent>();
                        if (c_tr) {
                            const Vector2 cp = impl_->world_to_screen(impl_->grid_x(c_tr), impl_->grid_y(c_tr));
                            DrawLineEx(p, cp, 1.0f, Color{c.r, c.g, c.b, 140});
                        }
                    }
                }
            }

            // Selection outline
            if (impl_->sel_kind == Impl::SelectionKind::Unit &&
                impl_->sel_entity == eid) {
                DrawCircleLines(static_cast<int>(p.x), static_cast<int>(p.y),
                                static_cast<int>(s * 0.6f + 4),
                                Color{255, 255, 0, 255});
            }
        }
    }

    // --- Radar detection arcs overlay ---
    if (impl_->world_loaded && impl_->show_objectives &&
        impl_->show_radar_arcs) {
        const float fallback_radius_grid = 32.0f;
        constexpr float KM_TO_GRID = 1.0f / 0.312f;
        for (const auto& eid : impl_->pop.objectives) {
            auto h = impl_->handle(eid);
            auto* tr = h.get<f4::entities::TransformComponent>();
            auto* own = h.get<f4::entities::OwnershipComponent>();
            auto* rad = h.get<f4::entities::RadarComponent>();
            if (!tr || !own || !rad) continue;

            const float ox = impl_->grid_x(tr), oy = impl_->grid_y(tr);
            const Vector2 origin = impl_->world_to_screen(ox, oy);
            const float radar_range_grid = (rad->range_km > 0.0f)
                ? rad->range_km * KM_TO_GRID
                : fallback_radius_grid;
            const float cull_radius_px = radar_range_grid * impl_->cam_zoom;
            if (origin.x < -cull_radius_px ||
                origin.x > impl_->window_w + cull_radius_px ||
                origin.y < -cull_radius_px ||
                origin.y > impl_->window_h + cull_radius_px) continue;

            const bool is_selected =
                (impl_->sel_kind == Impl::SelectionKind::Objective &&
                 impl_->sel_entity == eid);
            const RlColor owner = color_for_owner(own->team);
            for (int arc = 0; arc < 8; ++arc) {
                const float ratio = rad->detect_ratio[arc];
                if (ratio <= 0.0f) continue;
                const float radius_px = radar_range_grid * impl_->cam_zoom * ratio;
                if (radius_px < 2.0f) continue;
                const float center_angle = arc * 45.0f - 90.0f;
                const float start_angle = center_angle - 22.5f;
                const float end_angle = center_angle + 22.5f;
                const uint8_t base_alpha = is_selected ? 80 : 50;
                const uint8_t alpha = static_cast<uint8_t>(
                    base_alpha * std::min(1.0f, ratio));
                const Color fill = {owner.r, owner.g, owner.b, alpha};
                DrawCircleSector(origin, radius_px, start_angle, end_angle,
                                  8, fill);
                if (is_selected) {
                    DrawCircleSectorLines(origin, radius_px,
                                           start_angle, end_angle, 8,
                                           Color{owner.r, owner.g, owner.b, 200});
                }
            }
        }
    }

    // --- Ground Layout overlay (selected objective only, zoom-gated) ---
    if (impl_->world_loaded &&
        impl_->show_ground_layout_overlay &&
        impl_->sel_kind == Impl::SelectionKind::Objective &&
        impl_->sel_entity.valid() &&
        impl_->cam_zoom > 4.0f) {
        auto h = impl_->handle(impl_->sel_entity);
        auto* tr = h.get<f4::entities::TransformComponent>();
        auto* gl = h.get<f4::entities::GroundLayoutComponent>();
        if (tr && gl && !gl->layouts.empty()) {
            constexpr float FT_PER_GRID = 1024.0f;
            const float ox = impl_->grid_x(tr), oy = impl_->grid_y(tr);
            const Vector2 origin = impl_->world_to_screen(ox, oy);
            const float px_per_ft = impl_->cam_zoom / FT_PER_GRID;
            bool worth_drawing = false;
            for (const auto& layout : gl->layouts) {
                for (const auto& pt : layout.points) {
                    const float dx = pt.x * px_per_ft;
                    const float dy = pt.y * px_per_ft;
                    if (dx * dx + dy * dy > 4.0f) {
                        worth_drawing = true;
                        break;
                    }
                }
                if (worth_drawing) break;
            }
            if (worth_drawing) {
                for (const auto& layout : gl->layouts) {
                    Color stroke;
                    float line_w;
                    switch (layout.type) {
                        case 1:  stroke = Color{ 30,  30,  30, 230}; line_w = 2.0f; break;
                        case 8:  stroke = Color{120, 120, 120, 180}; line_w = 1.0f; break;
                        case 11: stroke = Color{ 60, 200,  80, 230}; line_w = 1.0f; break;
                        case 14: stroke = Color{ 80, 200, 220, 230}; line_w = 1.0f; break;
                        case 16: stroke = Color{ 60, 120, 220, 230}; line_w = 1.0f; break;
                        case 17: stroke = Color{140, 100,  60, 220}; line_w = 1.0f; break;
                        case 4:  stroke = Color{220,  60,  60, 230}; line_w = 1.0f; break;
                        case 5:  stroke = Color{220, 140,  40, 230}; line_w = 1.0f; break;
                        case 6:  stroke = Color{220, 200,  40, 230}; line_w = 1.0f; break;
                        case 10: stroke = Color{180,  60, 220, 230}; line_w = 1.0f; break;
                        default: stroke = Color{160, 160, 160, 180}; line_w = 1.0f; break;
                    }
                    const std::size_t n = layout.points.size();
                    if (n < 2) {
                        if (n == 1) {
                            const float px = origin.x + layout.points[0].x * px_per_ft;
                            const float py = origin.y - layout.points[0].y * px_per_ft;
                            DrawCircleV({px, py}, 3.0f, stroke);
                        }
                        continue;
                    }
                    for (std::size_t i = 0; i + 1 < n; ++i) {
                        const float x0 = origin.x + layout.points[i].x * px_per_ft;
                        const float y0 = origin.y - layout.points[i].y * px_per_ft;
                        const float x1 = origin.x + layout.points[i + 1].x * px_per_ft;
                        const float y1 = origin.y - layout.points[i + 1].y * px_per_ft;
                        DrawLineEx({x0, y0}, {x1, y1}, line_w, stroke);
                    }
                    for (const auto& pt : layout.points) {
                        const float px = origin.x + pt.x * px_per_ft;
                        const float py = origin.y - pt.y * px_per_ft;
                        DrawCircleV({px, py}, 2.0f, stroke);
                    }
                }
            }
        }
    }

    // --- HUD overlay ---
    {
        const int pad = 6;
        int y = 30 + pad;
        const int x = pad;
        const int line_h = 14;
        const int font_size = 10;
        const Color shadow = {0, 0, 0, 200};
        const Color text = {235, 235, 235, 230};
        const Color accent = {120, 200, 255, 230};
        const Color ok_color = {120, 220, 120, 230};
        const Color warn_color = {240, 220, 120, 230};
        const Color bad_color = {240, 120, 120, 230};

        auto draw_text = [&](const char* buf, Color c) {
            DrawText(buf, x + 1, y + 1, font_size, shadow);
            DrawText(buf, x, y, font_size, c);
            y += line_h;
        };

        const int fps = GetFPS();
        char buf[160];
        const Color fps_c = (fps >= 55) ? ok_color
                          : (fps >= 30) ? warn_color
                                        : bad_color;
        snprintf(buf, sizeof(buf), "FPS %d", fps);
        draw_text(buf, fps_c);

        if (impl_->world_loaded) {
            const Vector2 mouse = GetMousePosition();
            float gx = 0.0f, gy = 0.0f;
            impl_->screen_to_world(mouse.x, mouse.y, &gx, &gy);
            snprintf(buf, sizeof(buf), "Cursor  grid (%.1f, %.1f)", gx, gy);
            draw_text(buf, accent);

            snprintf(buf, sizeof(buf), "Objectives %zu   Units %zu   Teams %zu",
                     impl_->pop.objectives.size(),
                     impl_->pop.units.size(),
                     impl_->pop.teams.size());
            draw_text(buf, text);

            snprintf(buf, sizeof(buf), "Cam  (%.1f, %.1f)  zoom %.2fx",
                     impl_->cam_x, impl_->cam_y, impl_->cam_zoom);
            draw_text(buf, text);

            // Selection summary
            if (impl_->sel_kind == Impl::SelectionKind::Objective && impl_->sel_entity.valid()) {
                auto h = impl_->handle(impl_->sel_entity);
                auto* ot = h.get<f4::entities::ObjectiveTypeComponent>();
                auto* own = h.get<f4::entities::OwnershipComponent>();
                auto* pb = h.get<f4::entities::PropertyBag>();
                if (ot && own) {
                    std::string sel_name;
                    if (!ot->class_name.empty()) {
                        sel_name = ot->class_name;
                    } else {
                        sel_name = f4::world_convert::objective_type_name(
                            static_cast<int16_t>(impl_->obj_type_from_pb(pb)));
                    }
                    snprintf(buf, sizeof(buf), "Sel: [Obj] %s  owner=%u",
                             sel_name.c_str(), own->team);
                    draw_text(buf, accent);
                }
            } else if (impl_->sel_kind == Impl::SelectionKind::Unit && impl_->sel_entity.valid()) {
                auto h = impl_->handle(impl_->sel_entity);
                auto* uc = h.get<f4::entities::UnitCoreComponent>();
                if (uc) {
                    const char* name = uc->class_name.empty() ? "(no class)" : uc->class_name.c_str();
                    auto team_tag = h.get_tag(f4::entities::tags::TEAM);
                    const uint8_t owner = (team_tag && team_tag->type == f4::entities::TagValue::Type::Int)
                        ? static_cast<uint8_t>(team_tag->int_val) : 0;
                    snprintf(buf, sizeof(buf), "Sel: [Unit] %s  owner=%u", name, owner);
                    draw_text(buf, accent);
                }
            }
        } else {
            draw_text("No world loaded", warn_color);
        }

        // Hovered-entity hint
        if (impl_->world_loaded &&
            impl_->sel_kind == Impl::SelectionKind::None &&
            !ImGui::GetIO().WantCaptureMouse) {
            const Vector2 mouse = GetMousePosition();
            float gx = 0.0f, gy = 0.0f;
            impl_->screen_to_world(mouse.x, mouse.y, &gx, &gy);
            f4::entities::EntityId best_id;
            float best_dist_sq = 100.0f;  // 10px radius squared
            for (const auto& eid : impl_->pop.objectives) {
                auto h = impl_->handle(eid);
                auto* tr = h.get<f4::entities::TransformComponent>();
                if (!tr) continue;
                const float ox = impl_->grid_x(tr), oy = impl_->grid_y(tr);
                const Vector2 p = impl_->world_to_screen(ox, oy);
                const float dx = p.x - mouse.x;
                const float dy = p.y - mouse.y;
                const float d2 = dx * dx + dy * dy;
                if (d2 < best_dist_sq) {
                    best_dist_sq = d2;
                    best_id = eid;
                }
            }
            if (best_id.valid()) {
                auto h = impl_->handle(best_id);
                auto* ot = h.get<f4::entities::ObjectiveTypeComponent>();
                auto* pb = h.get<f4::entities::PropertyBag>();
                std::string hover_name;
                if (ot && !ot->class_name.empty()) {
                    hover_name = ot->class_name;
                } else {
                    hover_name = f4::world_convert::objective_type_name(
                        static_cast<int16_t>(impl_->obj_type_from_pb(pb)));
                }
                snprintf(buf, sizeof(buf), "Hover: [Obj] %s", hover_name.c_str());
                draw_text(buf, text);
            }
        }
    }

    // --- Minimap ---
    if (impl_->show_minimap && impl_->world_loaded) {
        const int mm_size = impl_->minimap_size;
        const int mm_pad = 8;
        const int mm_x = impl_->window_w - mm_size - mm_pad;
        const int mm_y = impl_->window_h - mm_size - mm_pad - 24;

        DrawRectangle(mm_x - 2, mm_y - 2, mm_size + 4, mm_size + 4,
                      Color{0, 0, 0, 180});
        DrawRectangleLines(mm_x - 2, mm_y - 2, mm_size + 4, mm_size + 4,
                           Color{180, 180, 180, 200});

        auto world_to_mini = [&](float gx, float gy) -> Vector2 {
            const float fx = gx / 1024.0f;
            const float fy = gy / 1024.0f;
            return {
                mm_x + fx * mm_size,
                mm_y + mm_size - fy * mm_size
            };
        };

        if (impl_->terrain_cache_valid && impl_->terrain_cache.id != 0) {
            const Rectangle src = {0, 0,
                static_cast<float>(impl_->terrain_cache.texture.width),
                -static_cast<float>(impl_->terrain_cache.texture.height)};
            const Rectangle dst = {static_cast<float>(mm_x),
                                    static_cast<float>(mm_y),
                                    static_cast<float>(mm_size),
                                    static_cast<float>(mm_size)};
            DrawTexturePro(impl_->terrain_cache.texture, src, dst,
                           {0, 0}, 0.0f, Color{255, 255, 255, 200});
        } else {
            DrawRectangle(mm_x, mm_y, mm_size, mm_size,
                          Color{40, 42, 48, 255});
        }

        if (impl_->show_objectives) {
            for (const auto& eid : impl_->pop.objectives) {
                auto h = impl_->handle(eid);
                auto* tr = h.get<f4::entities::TransformComponent>();
                auto* own = h.get<f4::entities::OwnershipComponent>();
                if (!tr || !own) continue;
                const Vector2 p = world_to_mini(impl_->grid_x(tr), impl_->grid_y(tr));
                const RlColor oc = color_for_owner(own->team);
                if (impl_->sel_kind == Impl::SelectionKind::Objective &&
                    impl_->sel_entity == eid) {
                    DrawCircleV(p, 3.0f, Color{255, 255, 100, 255});
                } else {
                    DrawPixel(static_cast<int>(p.x), static_cast<int>(p.y),
                              Color{oc.r, oc.g, oc.b, 255});
                }
            }
        }

        if (impl_->show_units) {
            for (const auto& eid : impl_->pop.units) {
                auto h = impl_->handle(eid);
                auto* tr = h.get<f4::entities::TransformComponent>();
                if (!tr) continue;
                auto team_tag = h.get_tag(f4::entities::tags::TEAM);
                const uint8_t owner = (team_tag && team_tag->type == f4::entities::TagValue::Type::Int)
                    ? static_cast<uint8_t>(team_tag->int_val) : 0;
                const Vector2 p = world_to_mini(impl_->grid_x(tr), impl_->grid_y(tr));
                const RlColor uc = color_for_owner(owner);
                DrawPixel(static_cast<int>(p.x), static_cast<int>(p.y),
                          Color{uc.r, uc.g, uc.b, 255});
            }
        }

        const float half_w_grid = static_cast<float>(impl_->window_w) /
                                  (2.0f * impl_->cam_zoom);
        const float half_h_grid = static_cast<float>(impl_->window_h) /
                                 (2.0f * impl_->cam_zoom);
        const float vx0 = impl_->cam_x - half_w_grid;
        const float vx1 = impl_->cam_x + half_w_grid;
        const float vy0 = impl_->cam_y - half_h_grid;
        const float vy1 = impl_->cam_y + half_h_grid;
        const Vector2 vp0 = world_to_mini(
            std::max(0.0f, vx0), std::max(0.0f, vy0));
        const Vector2 vp1 = world_to_mini(
            std::min(1024.0f, vx1), std::min(1024.0f, vy1));
        const Rectangle vp_rect = {
            vp0.x, vp0.y,
            vp1.x - vp0.x, vp1.y - vp0.y
        };
        DrawRectangleLines(static_cast<int>(vp_rect.x),
                           static_cast<int>(vp_rect.y),
                           static_cast<int>(vp_rect.width),
                           static_cast<int>(vp_rect.height),
                           Color{255, 255, 100, 220});

        DrawText("minimap", mm_x + 2, mm_y - 14, 9,
                 Color{200, 200, 200, 200});

        if (!ImGui::GetIO().WantCaptureMouse) {
            const Vector2 mouse = GetMousePosition();
            const bool over_minimap = (mouse.x >= mm_x && mouse.x < mm_x + mm_size &&
                                        mouse.y >= mm_y && mouse.y < mm_y + mm_size);
            if (over_minimap && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
                const float fx = (mouse.x - mm_x) / static_cast<float>(mm_size);
                const float fy = 1.0f - (mouse.y - mm_y) / static_cast<float>(mm_size);
                impl_->cam_x = fx * 1024.0f;
                impl_->cam_y = fy * 1024.0f;
            }
        }
    }
}

} // namespace f4::viewer
