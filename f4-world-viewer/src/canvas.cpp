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
        // Allow zooming in deep enough to see airfield ground layout
        // detail (runways/taxiways/parking). An airfield layout is
        // typically ~5000 ft across; one grid unit = 1024 ft, so the
        // layout spans ~5 grid units. To fill a 400px window with that
        // we need zoom ~80; to see individual parking spots (~50 ft)
        // we need zoom ~2000. Cap at 2000 to avoid floating-point
        // precision loss in world_to_screen.
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

// POLISH-2.1: invalidate the terrain cache. Called whenever a new terrain
// is loaded (file_ops.cpp::load_terrain_json) so the next draw_canvas()
// re-renders the cache from the new data. Does NOT free the existing
// RenderTexture — that's done lazily by ensure_terrain_cache() before
// re-allocation, and on shutdown by ~ViewerApp. This way the function
// is safe to call from contexts that don't have a GL context (e.g.
// load_terrain_json called from a CLI mode before run()).
void ViewerApp::Impl::invalidate_terrain_cache() {
    terrain_cache_valid = false;
}

// POLISH-2.1: render the current terrain into terrain_cache. Allocates
// the RenderTexture on first call (or after invalidation), then renders
// each terrain cell as a single pixel into the texture. The texture is
// terrain-sized (e.g. 128×128 for the default Korea theater), NOT
// theater-grid-sized (1024×1024) — keeps GPU memory at ~64 KB instead
// of ~4 MB, and the blit at draw time scales the texture up to the
// full theater via DrawTexturePro (free GPU bilinear/point filtering).
//
// Pixel (x, y) in the texture holds the terrain color at cell (x, y).
// One pixel per cell is sufficient because each cell maps to an 8×8
// block of theater grid units (1024/128 = 8) — the block-uniform color
// is exactly what the old naive loop drew.
void ViewerApp::Impl::ensure_terrain_cache() {
    if (terrain_cache_valid) return;
    if (!world.terrain_loaded) return;
    const auto& td = world.terrain;
    const uint32_t gw = td.header.width;
    const uint32_t gh = td.header.height;
    if (gw == 0 || gh == 0) return;

    // (Re)allocate the RenderTexture if needed. We UnloadRenderTexture
    // first to avoid leaking GPU memory on reload (e.g. user loads a
    // different theater).
    if (terrain_cache.id != 0) {
        UnloadRenderTexture(terrain_cache);
        terrain_cache = {0};
    }
    terrain_cache = LoadRenderTexture(static_cast<int>(gw),
                                       static_cast<int>(gh));
    if (terrain_cache.id == 0) {
        // Allocation failed — leave the cache invalid; draw_canvas will
        // fall back to the naive loop. (Shouldn't happen in practice.)
        return;
    }
    // Point filtering: at high zoom we want crisp pixel boundaries so
    // individual terrain cells are visually distinguishable. Bilinear
    // would smear the colors across cell boundaries.
    SetTextureFilter(terrain_cache.texture, TEXTURE_FILTER_POINT);

    // Render each terrain cell as a single pixel into the texture.
    // BeginTextureMode switches the GL render target to the texture;
    // we then draw 1×1 rectangles. After EndTextureMode the GL target
    // reverts to the screen.
    BeginTextureMode(terrain_cache);
    ClearBackground(Color{0, 0, 0, 255});
    for (uint32_t y = 0; y < gh; ++y) {
        for (uint32_t x = 0; x < gw; ++x) {
            const auto t = td.tile_type_at(x, gh-y-1);
            const auto c = f4::terrain::TerrainData::color_for_tile_type(t);
            // Rectangle at (x, y) with size 1×1 — fills exactly one pixel.
            // NOTE: RenderTexture coordinates are Y-DOWN, matching screen
            // space, so we don't need to flip Y here. The flip happens
            // at blit time in draw_canvas (the world_to_screen transform
            // inverts Y for us).
            DrawRectangle(static_cast<int>(x), static_cast<int>(y),
                          1, 1, Color{c.r, c.g, c.b, c.a});
        }
    }
    EndTextureMode();
    terrain_cache_valid = true;
}

void ViewerApp::draw_canvas() {
    // --- Terrain tiles ---
    // POLISH-2.1: previously this branch drew every terrain cell with
    // a separate DrawRectangleRec call (16,384 calls for a 128×128
    // theater — dominated frame time). Now we render the terrain to a
    // cached RenderTexture (one pixel per cell) on first use, then
    // blit it as a single DrawTexturePro each frame. The cache is
    // invalidated by load_terrain_json.
    if (impl_->show_terrain && impl_->world.terrain_loaded) {
        impl_->ensure_terrain_cache();
        if (impl_->terrain_cache_valid && impl_->terrain_cache.id != 0) {
            // Blit the cached terrain texture across the full theater
            // grid (0,0) → (1024, 1024). world_to_screen converts the
            // grid-space rect to screen pixels, handling pan/zoom.
            const Vector2 p0 = impl_->world_to_screen(0.0f, 0.0f);
            const Vector2 p1 = impl_->world_to_screen(1024.0f, 1024.0f);
            const Rectangle src = {0, 0,
                static_cast<float>(impl_->terrain_cache.texture.width),
                // Negative height flips Y — RenderTexture is rendered
                // Y-down but the world grid is Y-up. world_to_screen
                // inverts Y for the dest rect; we mirror it on src so
                // the texture's top row maps to the top of the dest.
                -static_cast<float>(impl_->terrain_cache.texture.height)};
            const Rectangle dst = {p0.x, p1.y,
                p1.x - p0.x, p0.y - p1.y};
            const Vector2 origin = {0, 0};
            DrawTexturePro(impl_->terrain_cache.texture, src, dst, origin,
                           0.0f, Color{255, 255, 255, 255});
        } else {
            // Cache failed to allocate — fall back to the old naive
            // loop. This is the safety net; in practice it shouldn't
            // run unless the GPU is out of memory.
            const auto& td = impl_->world.terrain;
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
    } else if (!impl_->world.terrain_loaded) {
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
        // Phase 2: draw objective labels (class_name) when zoomed in close
        // enough that text wouldn't overlap with neighbors. The threshold
        // is chosen so labels appear roughly when individual objectives
        // become pickable (icon size > 12px). At lower zoom, labels would
        // be unreadable noise.
        const bool draw_labels = impl_->cam_zoom > 8.0f;
        // Phase 2: pre-compute viewport bounds for culling — skip
        // objectives whose screen position is well outside the window.
        // With 2659 objectives at fit-to-world zoom, this saves ~2k
        // off-screen draws per frame.
        const float cull_margin = base_size + (draw_labels ? 80.0f : 0.0f);
        const float sx_min = -cull_margin;
        const float sx_max = static_cast<float>(impl_->window_w) + cull_margin;
        const float sy_min = -cull_margin;
        const float sy_max = static_cast<float>(impl_->window_h) + cull_margin;

        for (int i = 0; i < static_cast<int>(impl_->world.objectives.size()); ++i) {
            const auto& o = impl_->world.objectives[i];
            const Vector2 p = impl_->world_to_screen(o.x, o.y);
            // Phase 2: viewport culling — skip off-screen objectives.
            if (p.x < sx_min || p.x > sx_max || p.y < sy_min || p.y > sy_max) continue;

            // Phase 2: search filter — if the user typed a search string,
            // skip objectives whose class_name doesn't contain it.
            // Empty search = show all.
            // POLISH-2.2: use the cached lowercase needle instead of
            // allocating a std::string per iteration. The haystack is
            // lowercased in-place into a stack buffer (avoids heap
            // allocation; class_name is at most ~40 chars).
            if (impl_->objective_search_lower[0] != '\0') {
                if (!o.class_name.empty()) {
                    // Lowercase the haystack into a stack buffer. We
                    // can't lowercase in-place (class_name is const).
                    // 256 chars is plenty — class names are short
                    // (e.g. "02_20 Airbase 2", "044 Bridge 6").
                    char haystack_buf[256];
                    const std::size_t hn = std::min<std::size_t>(
                        o.class_name.size(), 255);
                    for (std::size_t k = 0; k < hn; ++k) {
                        char c = o.class_name[k];
                        if (c >= 'A' && c <= 'Z') {
                            c = static_cast<char>(c - 'A' + 'a');
                        }
                        haystack_buf[k] = c;
                    }
                    haystack_buf[hn] = '\0';
                    // Substring search using the cached needle.
                    if (std::strstr(haystack_buf,
                                    impl_->objective_search_lower) == nullptr) {
                        continue;
                    }
                } else {
                    continue;  // no class_name, can't match search
                }
            }
            // Phase 2: team filter — if a specific team is selected,
            // dim objectives owned by other teams.
            RlColor c = color_for_owner(o.owner);
            if (impl_->team_filter != 0xFF && o.owner != impl_->team_filter) {
                // Dim non-matching objectives to 30% alpha.
                c.r = static_cast<unsigned char>(c.r * 0.3f);
                c.g = static_cast<unsigned char>(c.g * 0.3f);
                c.b = static_cast<unsigned char>(c.b * 0.3f);
                c.a = static_cast<unsigned char>(c.a * 0.3f);
            }

            // Procedural symbol — see symbols.hpp. The shape encodes the
            // objective type; the fill color encodes the team.
            const SymbolKind kind = symbol_for_objective_type(o.objective_type);
            // Outline: darker shade of the team color for crispness at
            // small sizes. Falls back to near-black for very dark colors.
            const RlColor outline = {
                static_cast<unsigned char>(c.r * 0.4f),
                static_cast<unsigned char>(c.g * 0.4f),
                static_cast<unsigned char>(c.b * 0.4f),
                255};
            impl_->draw_symbol(kind, p.x, p.y, base_size, c, outline);
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
            // Phase 2: draw class_name label next to the icon at high zoom.
            // Use the loaded class_name if available; fall back to
            // objective_type_name as a less-informative label.
            if (draw_labels) {
                std::string label;
                if (!o.class_name.empty()) {
                    label = o.class_name;
                } else {
                    label = f4::world_convert::objective_type_name(
                        static_cast<int16_t>(o.objective_type));
                }
                if (!label.empty() && label != "Unknown") {
                    // Place label to the right of the icon, vertically
                    // centered. Slight offset so it doesn't overlap the
                    // priority halo.
                    const float lx = p.x + base_size * 0.6f + 4;
                    const float ly = p.y - 6;
                    // Draw a dark shadow for readability against any
                    // terrain background.
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
        // Phase 2: viewport culling for units — same as objectives.
        const float cull_margin = s + 4.0f;
        const float sx_min = -cull_margin;
        const float sx_max = static_cast<float>(impl_->window_w) + cull_margin;
        const float sy_min = -cull_margin;
        const float sy_max = static_cast<float>(impl_->window_h) + cull_margin;
        for (int i = 0; i < static_cast<int>(impl_->world.units.size()); ++i) {
            const auto& u = impl_->world.units[i];
            const Vector2 p = impl_->world_to_screen(u.x, u.y);
            // Phase 2: viewport culling.
            if (p.x < sx_min || p.x > sx_max || p.y < sy_min || p.y > sy_max) continue;
            RlColor c = color_for_owner(u.owner);
            // Phase 2: team filter — dim units owned by other teams.
            if (impl_->team_filter != 0xFF && u.owner != impl_->team_filter) {
                c.r = static_cast<unsigned char>(c.r * 0.3f);
                c.g = static_cast<unsigned char>(c.g * 0.3f);
                c.b = static_cast<unsigned char>(c.b * 0.3f);
                c.a = static_cast<unsigned char>(c.a * 0.3f);
            }
            // Procedural symbol — see symbols.hpp. The frame shape encodes
            // the unit class (rect=battalion, diamond=brigade, circle=
            // squadron, triangle=taskforce); the inner glyph encodes the
            // subtype (armor/fighter/carrier/...). Fill is team color.
            const SymbolKind kind = symbol_for_unit(u.unit_class, u.unit_subtype);
            const RlColor outline = {
                static_cast<unsigned char>(c.r * 0.4f),
                static_cast<unsigned char>(c.g * 0.4f),
                static_cast<unsigned char>(c.b * 0.4f),
                255};
            impl_->draw_symbol(kind, p.x, p.y, s, c, outline);

            // Destination line (only if moved AND toggle enabled)
            if (impl_->show_unit_destinations &&
                (u.dest_x != u.x || u.dest_y != u.y)) {
                const Vector2 d = impl_->world_to_screen(u.dest_x, u.dest_y);
                DrawLineEx(p, d, 1.0f, Color{c.r, c.g, c.b, 160});
            }

            // Waypoint polyline: draws the unit's flight/ground plan as a
            // thin dashed line through each waypoint. Only meaningful when
            // wp_count > 0 (rare in fresh campaign saves, common once
            // missions are assigned). Gated by show_waypoints toggle.
            if (impl_->show_waypoints &&
                u.wp_count > 0 && !u.waypoints.empty()) {
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
            // squadron calls home. Gated by show_squadron_links toggle.
            if (impl_->show_squadron_links &&
                u.unit_class == f4::entities::UnitClass::Squadron && u.airbase_id != 0) {
                auto it = impl_->obj_id_to_index.find(u.airbase_id);
                if (it != impl_->obj_id_to_index.end() && it->second < static_cast<int>(impl_->world.objectives.size())) {
                    const auto& ab = impl_->world.objectives[it->second];
                    const Vector2 a = impl_->world_to_screen(ab.x, ab.y);
                    DrawLineEx(p, a, 0.5f, Color{c.r, c.g, c.b, 90});
                }
            }

            // Phase 2: Battalion → Brigade hierarchy lines.
            // Each Battalion has a parent_id pointing at its parent Brigade's
            // VU_ID.num. We draw a thin dotted-style line from the battalion
            // to its parent brigade so the user can see the OOB hierarchy at
            // a glance. Gated by show_hierarchy_lines toggle (off by default
            // — it adds significant visual clutter for theaters with many
            // battalions).
            if (impl_->show_hierarchy_lines &&
                u.unit_class == f4::entities::UnitClass::Battalion && u.parent_id != 0) {
                auto it = impl_->unit_id_to_index.find(u.parent_id);
                if (it != impl_->unit_id_to_index.end() && it->second < static_cast<int>(impl_->world.units.size())) {
                    const auto& parent = impl_->world.units[it->second];
                    const Vector2 pp = impl_->world_to_screen(parent.x, parent.y);
                    // Drawn slightly thicker than squadron links and more
                    // opaque — hierarchy is more important than basing.
                    DrawLineEx(p, pp, 1.0f, Color{c.r, c.g, c.b, 140});
                }
            }

            // Phase 2: Brigade → child Battalion elements (drawn from the
            // brigade side too, so the link appears even if only the brigade
            // is selected). Same visual style as battalion→brigade above.
            if (impl_->show_hierarchy_lines &&
                u.unit_class == f4::entities::UnitClass::Brigade &&
                !u.element_ids.empty()) {
                for (uint32_t child_id : u.element_ids) {
                    if (child_id == 0) continue;
                    auto it = impl_->unit_id_to_index.find(child_id);
                    if (it != impl_->unit_id_to_index.end() && it->second < static_cast<int>(impl_->world.units.size())) {
                        const auto& child = impl_->world.units[it->second];
                        const Vector2 cp = impl_->world_to_screen(child.x, child.y);
                        DrawLineEx(p, cp, 1.0f, Color{c.r, c.g, c.b, 140});
                    }
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

    // --- Radar detection arcs overlay ---
    // Each objective with has_radar==true has a detect_ratio[8] array
    // covering 360° in 8 azimuthal arcs (45° each). We draw each arc as
    // a filled sector whose radius encodes the detection ratio (0..1)
    // and whose alpha encodes detection strength. The radar range is
    // in "grid units". One theater grid unit = 1024 ft ≈ 312 m ≈ 0.312 km,
    // so km → grid units = km / 0.312 ≈ km * 3.2.
    //
    // Phase 3: previously we used a fabricated constant (32 grid units ≈
    // 10 km) for ALL radar objectives because Falcon4.RCD was not parsed.
    // Now the world JSON carries the real radar_range_km for each radar
    // objective (resolved via OCD → FED → FCD → RCD at world-convert time).
    // We fall back to the old 32-grid-unit constant only when the real
    // range is unavailable (e.g. when theater_db wasn't loaded by cam2json).
    //
    // Arcs are drawn for ALL radar objectives, not just the selected
    // one — this gives the user a strategic overview of radar coverage
    // across the theater. The selected objective's arcs are highlighted.
    // Gated by show_radar_arcs toggle (off by default to reduce clutter).
    if (impl_->world_loaded && impl_->show_objectives &&
        impl_->show_radar_arcs) {
        // Fallback radius when the real RCD range wasn't resolved.
        const float fallback_radius_grid = 32.0f;  // ~10 km
        // 1 grid unit = 1024 ft = 0.312 km → km / 0.312 = km * 3.2.
        constexpr float KM_TO_GRID = 1.0f / 0.312f;
        for (int i = 0; i < static_cast<int>(impl_->world.objectives.size()); ++i) {
            const auto& o = impl_->world.objectives[i];
            if (!o.has_radar) continue;
            const Vector2 origin = impl_->world_to_screen(
                static_cast<float>(o.x), static_cast<float>(o.y));
            // Phase 2: viewport culling for radar arcs — skip if origin
            // is well off-screen (the arcs can be huge — use a generous
            // margin based on the radar's range).
            const float radar_range_grid = (o.radar_range_km > 0.0f)
                ? o.radar_range_km * KM_TO_GRID
                : fallback_radius_grid;
            const float cull_radius_px = radar_range_grid * impl_->cam_zoom;
            if (origin.x < -cull_radius_px ||
                origin.x > impl_->window_w + cull_radius_px ||
                origin.y < -cull_radius_px ||
                origin.y > impl_->window_h + cull_radius_px) continue;

            const bool is_selected =
                (impl_->sel_kind == Impl::SelectionKind::Objective &&
                 impl_->sel_index == i);
            // Each arc covers 45° (360/8). Arc 0 starts at 0° (north).
            // We draw arcs as filled wedges with alpha-coded detection
            // strength. Color follows the owner color so team affiliation
            // is visible.
            const RlColor owner = color_for_owner(o.owner);
            for (int arc = 0; arc < 8; ++arc) {
                const float ratio = o.detect_ratio[arc];
                if (ratio <= 0.0f) continue;
                const float radius_px = radar_range_grid * impl_->cam_zoom * ratio;
                if (radius_px < 2.0f) continue;
                // Arc `arc` covers angles [arc*45 - 22.5, arc*45 + 22.5].
                // Raylib's DrawCircleSector uses start_angle and sweep
                // counter-clockwise from east (0° = +X). Falcon's arcs
                // are 0=north, clockwise. Convert: north=0° → east=-90°
                // offset, and clockwise → counter-clockwise (negate).
                const float center_angle = arc * 45.0f - 90.0f;  // north=0 → east=-90
                const float start_angle = center_angle - 22.5f;
                const float end_angle = center_angle + 22.5f;
                // Alpha: stronger detection = more opaque. Selected
                // objective gets a brighter overlay.
                const uint8_t base_alpha = is_selected ? 80 : 50;
                const uint8_t alpha = static_cast<uint8_t>(
                    base_alpha * std::min(1.0f, ratio));
                const Color fill = {owner.r, owner.g, owner.b, alpha};
                DrawCircleSector(origin, radius_px, start_angle, end_angle,
                                  8, fill);
                // Outline the arc edge so the wedge is visible even at
                // low alpha.
                if (is_selected) {
                    DrawCircleSectorLines(origin, radius_px,
                                           start_angle, end_angle, 8,
                                           Color{owner.r, owner.g, owner.b, 200});
                }
            }
        }
    }

    // --- Ground Layout overlay (selected objective only, zoom-gated) ---
    // When the selected objective has a non-empty ground_layout AND the
    // user has zoomed in close enough that the layout's foot-level detail
    // would be visible, draw the runway/taxiway/parking/SAM-site geometry
    // directly on the main canvas. At low zoom this would just be a
    // sub-pixel smudge — the dedicated "Ground Layout" ImGui window
    // is the better tool at that scale.
    //
    // The geometry is in FEET relative to the objective center; one
    // theater grid unit = 1024 ft, so we convert feet → grid units
    // by dividing by 1024. Then world_to_screen gives us pixel coords.
    // Gated by show_ground_layout_overlay toggle.
    if (impl_->world_loaded &&
        impl_->show_ground_layout_overlay &&
        impl_->sel_kind == Impl::SelectionKind::Objective &&
        impl_->sel_index >= 0 &&
        impl_->sel_index < static_cast<int>(impl_->world.objectives.size()) &&
        impl_->cam_zoom > 4.0f) {
        const auto& obj = impl_->world.objectives[impl_->sel_index];
        if (!obj.ground_layout.empty()) {
            // Feet → grid-units conversion: 1 grid unit = 1024 ft.
            constexpr float FT_PER_GRID = 1024.0f;
            // The objective's screen position is the anchor for all the
            // layout's foot-offsets.
            const Vector2 origin = impl_->world_to_screen(
                static_cast<float>(obj.x), static_cast<float>(obj.y));
            // 1 foot in screen pixels = (1 / FT_PER_GRID) * cam_zoom.
            const float px_per_ft = impl_->cam_zoom / FT_PER_GRID;
            // Only draw if at least one point would be > 2px from origin.
            // (Below this threshold the overlay is just noise.)
            bool worth_drawing = false;
            for (const auto& gl : obj.ground_layout) {
                for (const auto& pt : gl.points) {
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
                for (const auto& gl : obj.ground_layout) {
                    // Color by list type — match the dedicated window's
                    // palette so the two views are visually consistent.
                    Color stroke;
                    float line_w;
                    switch (gl.type) {
                        case 1:  stroke = Color{ 30,  30,  30, 230}; line_w = 2.0f; break; // Runway
                        case 8:  stroke = Color{120, 120, 120, 180}; line_w = 1.0f; break; // RunwayDim
                        case 11: stroke = Color{ 60, 200,  80, 230}; line_w = 1.0f; break; // Parking
                        case 14: stroke = Color{ 80, 200, 220, 230}; line_w = 1.0f; break; // Helicopter
                        case 16: stroke = Color{ 60, 120, 220, 230}; line_w = 1.0f; break; // Dock
                        case 17: stroke = Color{140, 100,  60, 220}; line_w = 1.0f; break; // Track
                        case 4:  stroke = Color{220,  60,  60, 230}; line_w = 1.0f; break; // SAM
                        case 5:  stroke = Color{220, 140,  40, 230}; line_w = 1.0f; break; // Artillery
                        case 6:  stroke = Color{220, 200,  40, 230}; line_w = 1.0f; break; // AAA
                        case 10: stroke = Color{180,  60, 220, 230}; line_w = 1.0f; break; // StaticRadar
                        default: stroke = Color{160, 160, 160, 180}; line_w = 1.0f; break;
                    }
                    const std::size_t n = gl.points.size();
                    if (n < 2) {
                        // Single point — just draw a dot.
                        if (n == 1) {
                            const float px = origin.x + gl.points[0].x * px_per_ft;
                            // Y is flipped: world Y up = screen Y down,
                            // but world_to_screen already inverts Y for
                            // the objective position. So we subtract the
                            // layout's y-offset (in screen pixels) from
                            // origin.y to get "up = up".
                            const float py = origin.y - gl.points[0].y * px_per_ft;
                            DrawCircleV({px, py}, 3.0f, stroke);
                        }
                        continue;
                    }
                    // Polyline through all points.
                    for (std::size_t i = 0; i + 1 < n; ++i) {
                        const float x0 = origin.x + gl.points[i].x * px_per_ft;
                        const float y0 = origin.y - gl.points[i].y * px_per_ft;
                        const float x1 = origin.x + gl.points[i + 1].x * px_per_ft;
                        const float y1 = origin.y - gl.points[i + 1].y * px_per_ft;
                        DrawLineEx({x0, y0}, {x1, y1}, line_w, stroke);
                    }
                    // Markers at each point.
                    for (const auto& pt : gl.points) {
                        const float px = origin.x + pt.x * px_per_ft;
                        const float py = origin.y - pt.y * px_per_ft;
                        DrawCircleV({px, py}, 2.0f, stroke);
                    }
                }
            }
        }
    }

    // --- HUD overlay (top-left corner of canvas) ---
    // POLISH-2.3: lightweight heads-up display drawn directly on the
    // canvas (not in an ImGui window) so it doesn't take up panel space
    // and doesn't compete with the Layers panel for left-side real
    // estate. Shows:
    //   FPS (color-coded: green >=55, yellow 30-54, red <30)
    //   Cursor world coords (grid x, y) — useful for cross-referencing
    //     with the .cam file or other tools
    //   Counts: objectives / units / teams (so the user can see at a
    //     glance whether a world is loaded and how big it is)
    //   Current selection (one-line summary) so the user doesn't have
    //     to glance at the Inspector for the basics
    //
    // Skipped entirely when an ImGui window is being dragged over the
    // top-left corner (rlImGui sets WantCaptureMouse; we don't want to
    // draw under the cursor in that case).
    {
        const int pad = 6;
        int y = 30 + pad;  // below the menu bar
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

        // FPS, color-coded.
        const int fps = GetFPS();
        char buf[160];
        const Color fps_c = (fps >= 55) ? ok_color
                          : (fps >= 30) ? warn_color
                                        : bad_color;
        snprintf(buf, sizeof(buf), "FPS %d", fps);
        draw_text(buf, fps_c);

        if (impl_->world_loaded) {
            // Cursor world coords.
            const Vector2 mouse = GetMousePosition();
            float gx = 0.0f, gy = 0.0f;
            impl_->screen_to_world(mouse.x, mouse.y, &gx, &gy);
            snprintf(buf, sizeof(buf), "Cursor  grid (%.1f, %.1f)", gx, gy);
            draw_text(buf, accent);

            // Counts.
            snprintf(buf, sizeof(buf), "Objectives %zu   Units %zu   Teams %zu",
                     impl_->world.objectives.size(),
                     impl_->world.units.size(),
                     impl_->world.teams.size());
            draw_text(buf, text);

            // Camera info.
            snprintf(buf, sizeof(buf), "Cam  (%.1f, %.1f)  zoom %.2fx",
                     impl_->cam_x, impl_->cam_y, impl_->cam_zoom);
            draw_text(buf, text);

            // Selection summary (one line).
            if (impl_->sel_kind == Impl::SelectionKind::Objective &&
                impl_->sel_index >= 0 &&
                impl_->sel_index < static_cast<int>(impl_->world.objectives.size())) {
                const auto& o = impl_->world.objectives[impl_->sel_index];
                std::string sel_name;
                if (!o.class_name.empty()) {
                    sel_name = o.class_name;
                } else {
                    sel_name = f4::world_convert::objective_type_name(
                        static_cast<int16_t>(o.objective_type));
                }
                snprintf(buf, sizeof(buf), "Sel: [Obj %d] %s  owner=%u",
                         impl_->sel_index, sel_name.c_str(), o.owner);
                draw_text(buf, accent);
            } else if (impl_->sel_kind == Impl::SelectionKind::Unit &&
                       impl_->sel_index >= 0 &&
                       impl_->sel_index < static_cast<int>(impl_->world.units.size())) {
                const auto& u = impl_->world.units[impl_->sel_index];
                const char* name = u.class_name.empty() ? "(no class)" : u.class_name.c_str();
                snprintf(buf, sizeof(buf), "Sel: [Unit %d] %s  owner=%u",
                         impl_->sel_index, name, u.owner);
                draw_text(buf, accent);
            }
        } else {
            draw_text("No world loaded", warn_color);
        }

        // Hovered-entity hint (when not clicking, just hovering). Helps
        // the user find an objective by name without clicking it. Only
        // shown when no selection is active (otherwise the selection
        // summary already takes the line).
        if (impl_->world_loaded &&
            impl_->sel_kind == Impl::SelectionKind::None &&
            !ImGui::GetIO().WantCaptureMouse) {
            const Vector2 mouse = GetMousePosition();
            float gx = 0.0f, gy = 0.0f;
            impl_->screen_to_world(mouse.x, mouse.y, &gx, &gy);
            // Find the nearest objective within a small screen-space
            // radius (10px). This is O(N) per frame; for 2659 objectives
            // at 60fps that's ~160k checks/sec — cheap (no allocation).
            // We could spatial-index this if it ever shows up in a profile.
            int best_idx = -1;
            float best_dist_sq = 100.0f;  // 10px radius squared
            for (int i = 0; i < static_cast<int>(impl_->world.objectives.size()); ++i) {
                const auto& o = impl_->world.objectives[i];
                const Vector2 p = impl_->world_to_screen(o.x, o.y);
                const float dx = p.x - mouse.x;
                const float dy = p.y - mouse.y;
                const float d2 = dx * dx + dy * dy;
                if (d2 < best_dist_sq) {
                    best_dist_sq = d2;
                    best_idx = i;
                }
            }
            if (best_idx >= 0) {
                const auto& o = impl_->world.objectives[best_idx];
                std::string hover_name;
                if (!o.class_name.empty()) {
                    hover_name = o.class_name;
                } else {
                    hover_name = f4::world_convert::objective_type_name(
                        static_cast<int16_t>(o.objective_type));
                }
                snprintf(buf, sizeof(buf), "Hover: [Obj %d] %s",
                         best_idx, hover_name.c_str());
                draw_text(buf, text);
            }
        }
    }

    // --- Minimap (bottom-right corner) ---
    // POLISH-2.4: small overview of the entire 1024×1024 theater with
    // the current main-canvas viewport highlighted. Click anywhere on
    // the minimap to pan the main canvas there. The minimap reuses the
    // cached terrain texture (free), then overlays objective dots and
    // unit dots colored by owner.
    //
    // Layout: bottom-right corner, square, with a 4px margin. The
    // minimap is drawn UNDER the Legend panel (which by default sits
    // top-right) — when both are visible the minimap is below the
    // legend. If the window is too short to fit both, the minimap
    // slides up; if it would overlap the status bar at the bottom, it
    // slides up further.
    if (impl_->show_minimap && impl_->world_loaded) {
        const int mm_size = impl_->minimap_size;
        const int mm_pad = 8;
        const int mm_x = impl_->window_w - mm_size - mm_pad;
        // Reserve 24px at the bottom for the status bar.
        const int mm_y = impl_->window_h - mm_size - mm_pad - 24;

        // Background panel (semi-transparent dark rect so the minimap
        // is legible against any underlying canvas content).
        DrawRectangle(mm_x - 2, mm_y - 2, mm_size + 4, mm_size + 4,
                      Color{0, 0, 0, 180});
        // Border.
        DrawRectangleLines(mm_x - 2, mm_y - 2, mm_size + 4, mm_size + 4,
                           Color{180, 180, 180, 200});

        // Helper: world (0..1024, 0..1024) → minimap pixel.
        auto world_to_mini = [&](float gx, float gy) -> Vector2 {
            // Minimap covers grid (0..1024) → pixel (mm_x..mm_x+mm_size).
            // Y is inverted (world up = screen up).
            const float fx = gx / 1024.0f;
            const float fy = gy / 1024.0f;
            return {
                mm_x + fx * mm_size,
                mm_y + mm_size - fy * mm_size
            };
        };

        // Terrain thumbnail — reuse the cached terrain texture if valid.
        // We blit it as a tiny mm_size×mm_size image. (Point filtering
        // keeps the terrain cells crisp even at this small size.)
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
            // No terrain cache yet — fill with dark gray.
            DrawRectangle(mm_x, mm_y, mm_size, mm_size,
                          Color{40, 42, 48, 255});
        }

        // Objective dots (small — 1-2px each).
        if (impl_->show_objectives) {
            for (const auto& o : impl_->world.objectives) {
                const Vector2 p = world_to_mini(o.x, o.y);
                const RlColor oc = color_for_owner(o.owner);
                // Selected objective: bigger + yellow outline.
                if (impl_->sel_kind == Impl::SelectionKind::Objective &&
                    impl_->sel_index >= 0 &&
                    impl_->sel_index < static_cast<int>(impl_->world.objectives.size()) &&
                    &impl_->world.objectives[impl_->sel_index] == &o) {
                    DrawCircleV(p, 3.0f, Color{255, 255, 100, 255});
                } else {
                    DrawPixel(static_cast<int>(p.x), static_cast<int>(p.y),
                              Color{oc.r, oc.g, oc.b, 255});
                }
            }
        }

        // Unit dots.
        if (impl_->show_units) {
            for (const auto& u : impl_->world.units) {
                const Vector2 p = world_to_mini(u.x, u.y);
                const RlColor uc = color_for_owner(u.owner);
                DrawPixel(static_cast<int>(p.x), static_cast<int>(p.y),
                          Color{uc.r, uc.g, uc.b, 255});
            }
        }

        // Viewport rectangle: compute the world-space rect visible in
        // the main canvas, then map it to minimap coords.
        // Main canvas spans (cam_x - w/(2*zoom)) to (cam_x + w/(2*zoom))
        // in world X, similarly for Y.
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

        // Label.
        DrawText("minimap", mm_x + 2, mm_y - 14, 9,
                 Color{200, 200, 200, 200});

        // Click-to-pan: if the mouse is over the minimap AND the user
        // clicks (not drags — we don't want to fight the main-canvas
        // drag), recenter the main camera on the clicked location.
        // We check ImGui::GetIO().WantCaptureMouse so clicks on ImGui
        // windows over the minimap don't trigger pan.
        if (!ImGui::GetIO().WantCaptureMouse) {
            const Vector2 mouse = GetMousePosition();
            const bool over_minimap = (mouse.x >= mm_x && mouse.x < mm_x + mm_size &&
                                        mouse.y >= mm_y && mouse.y < mm_y + mm_size);
            if (over_minimap && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
                // Map mouse → world (0..1024).
                const float fx = (mouse.x - mm_x) / static_cast<float>(mm_size);
                const float fy = 1.0f - (mouse.y - mm_y) / static_cast<float>(mm_size);
                impl_->cam_x = fx * 1024.0f;
                impl_->cam_y = fy * 1024.0f;
            }
        }
    }
}

} // namespace f4::viewer
