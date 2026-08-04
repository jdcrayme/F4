// f4-world-viewer/src/ground_layout_view.cpp
//
// "Ground Layout" ImGui window — a dedicated 2D top-down view of the
// selected objective's PHD/PD ground-layout data. Drawn with ImDrawList
// directly (no separate raylib render target), so it composites cleanly
// with the rest of the ImGui frame and supports arbitrary zoom/pan
// inside the window without affecting the main canvas.
//
// When to show: the window auto-opens whenever the current selection is
// an Objective with a non-empty `ground_layout` vector. The user can
// close the window manually (collapsed/closed via the standard ImGui
// close button); it will reappear next time a layout-bearing objective
// is selected.
//
// What it draws:
//   - Each GroundLayoutList (one per PtHeader record) is rendered as a
//     colored polyline through its points. Color is keyed by PointListType:
//       Runway (1)         — thick black line
//       RunwayDim (8)      — dashed gray line (runway length/width marks)
//       Parking (11)       — green dots (one per spot)
//       Helicopter (14)    — cyan dots
//       Dock (16)          — blue dots
//       Track (17)         — brown polyline (ground vehicle route)
//       SAM/Artillery/AAA  — red/orange/yellow dots (placement points)
//       everything else    — thin gray line
//   - Each list's bounding box is shown as a faint rectangle.
//   - The first point of each list (PT_FIRST flag) gets a slightly
//     larger marker so the user can see the list's start.
//   - The objective center is drawn as a small crosshair at (0, 0).
//
// Coordinate system: the PHD/PD points are stored as X/Y offsets in
// FEET from the objective's tile center. We auto-fit the view to the
// layout's bounding box (with a small margin) and convert feet →
// window pixels via a single scale factor. A readout shows the
// current scale (feet per pixel) and the bounding-box dimensions.
//
// Not just airfields: any objective with pt_data_index != 0 in its
// OCD record gets a ground_layout — SAM sites, artillery parks,
// naval docks, vehicle tracks. This view handles all of them.

#include "viewer_state.hpp"
#include <f4/viewer/enum_text.hpp>
#include <f4/world_convert/theater_data.hpp>  // point_list_type_name, point_type_name

#include <imgui.h>
#include <algorithm>
#include <cmath>
#include <cstdio>

namespace f4::viewer {

// ---------------------------------------------------------------------------
// Local helpers — color picker for each PointListType.
// Returns a fill color (for point markers) and a stroke color (for
// connecting lines). Marker size in pixels for the dot-style types.
// ---------------------------------------------------------------------------
namespace {

struct LayoutColors {
    ImU32 stroke;
    ImU32 fill;
    float line_width;
    float marker_radius;
    bool is_dashed;
};

LayoutColors colors_for_list_type(uint8_t type) {
    switch (type) {
        case 1:  // PLT_RUNWAY
            return { IM_COL32( 30,  30,  30, 255), IM_COL32( 60,  60,  60, 255), 4.0f, 3.0f, false };
        case 8:  // PLT_RUNWAY_DIM
            return { IM_COL32(120, 120, 120, 200), IM_COL32(120, 120, 120, 200), 1.0f, 2.0f, true  };
        case 11: // PLT_PARK
            return { IM_COL32( 60, 200,  80, 255), IM_COL32( 60, 200,  80, 255), 1.0f, 4.0f, false };
        case 12: // PLT_RUNWAY_LT
            return { IM_COL32(200, 200,  60, 200), IM_COL32(200, 200,  60, 200), 2.0f, 2.0f, false };
        case 13: // PLT_RUNWAY_RT
            return { IM_COL32(200, 140,  60, 200), IM_COL32(200, 140,  60, 200), 2.0f, 2.0f, false };
        case 14: // PLT_HELICOPTER
            return { IM_COL32( 80, 200, 220, 255), IM_COL32( 80, 200, 220, 255), 1.0f, 4.0f, false };
        case 15: // PLT_FOLLOW_ME
            return { IM_COL32(220, 180,  60, 200), IM_COL32(220, 180,  60, 200), 1.0f, 3.0f, false };
        case 16: // PLT_DOCK
            return { IM_COL32( 60, 120, 220, 255), IM_COL32( 60, 120, 220, 255), 1.0f, 4.0f, false };
        case 17: // PLT_TRACK
            return { IM_COL32(140, 100,  60, 220), IM_COL32(140, 100,  60, 220), 2.0f, 2.0f, false };
        case 4:  // PLT_SAM
            return { IM_COL32(220,  60,  60, 255), IM_COL32(220,  60,  60, 255), 1.0f, 5.0f, false };
        case 5:  // PLT_ARTILLERY
            return { IM_COL32(220, 140,  40, 255), IM_COL32(220, 140,  40, 255), 1.0f, 5.0f, false };
        case 6:  // PLT_AAA
            return { IM_COL32(220, 200,  40, 255), IM_COL32(220, 200,  40, 255), 1.0f, 4.0f, false };
        case 10: // PLT_STATIC_RADAR
            return { IM_COL32(180,  60, 220, 255), IM_COL32(180,  60, 220, 255), 1.0f, 5.0f, false };
        default:
            return { IM_COL32(160, 160, 160, 180), IM_COL32(160, 160, 160, 180), 1.0f, 2.0f, false };
    }
}

} // namespace

// ---------------------------------------------------------------------------
// ViewerApp::draw_ground_layout_view
// ---------------------------------------------------------------------------
void ViewerApp::draw_ground_layout_view() {
    // Only show when an objective with ground_layout OR features is selected.
    if (impl_->sel_kind != Impl::SelectionKind::Objective ||
        impl_->sel_index < 0 ||
        impl_->sel_index >= static_cast<int>(impl_->world.objectives.size())) {
        return;
    }
    const auto& obj = impl_->world.objectives[impl_->sel_index];
    if (obj.ground_layout.empty() && obj.features.empty()) return;

    // Window position: bottom-left of the screen, below the Layers panel.
    // Default size is large enough to see the layout clearly — the user
    // can resize smaller if they want. FirstUseEver means the user's
    // manual resize is preserved across frames.
    ImGui::SetNextWindowPos(ImVec2(10, impl_->window_h - 520),
                            ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(640, 480), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Ground Layout", nullptr,
                      ImGuiWindowFlags_NoCollapse)) {
        ImGui::End();
        return;
    }

    // --- Header: objective name + layout summary ---
    if (!obj.class_name.empty()) {
        ImGui::TextUnformatted(obj.class_name.c_str());
    } else {
        ImGui::Text("Objective #%d", impl_->sel_index);
    }
    ImGui::SameLine();
    ImGui::TextDisabled("(%d lists, %d pts, %d features)",
                        static_cast<int>(obj.ground_layout.size()),
                        [&]{
                            int n = 0;
                            for (const auto& gl : obj.ground_layout)
                                n += static_cast<int>(gl.points.size());
                            return n;
                        }(),
                        static_cast<int>(obj.features.size()));
    // "Zoom to Layout" button — fits the MAIN canvas to this objective's
    // ground_layout + features bbox. Useful for inspecting the geometry
    // in the context of the surrounding world (e.g., to see which
    // taxiways connect to which runways, or how the airbase sits
    // relative to nearby objectives).
    ImGui::SameLine();
    if (ImGui::Button("Zoom to Layout")) {
        impl_->fit_to_selection_layout();
    }
    ImGui::SameLine();
    ImGui::TextDisabled("(main canvas)");

    // --- Compute the layout's bounding box (in feet) ---
    // Includes both ground_layout points AND feature placements so the
    // auto-fit view shows everything.
    float min_x = 1e30f, min_y = 1e30f, max_x = -1e30f, max_y = -1e30f;
    bool any = false;
    for (const auto& gl : obj.ground_layout) {
        for (const auto& pt : gl.points) {
            min_x = std::min(min_x, pt.x);
            min_y = std::min(min_y, pt.y);
            max_x = std::max(max_x, pt.x);
            max_y = std::max(max_y, pt.y);
            any = true;
        }
    }
    for (const auto& f : obj.features) {
        // Skip the placeholder FED[0] entry (index=0, all-zero offset).
        if (f.index == 0 && f.offset_x == 0.0f && f.offset_y == 0.0f) continue;
        min_x = std::min(min_x, f.offset_x);
        min_y = std::min(min_y, f.offset_y);
        max_x = std::max(max_x, f.offset_x);
        max_y = std::max(max_y, f.offset_y);
        any = true;
    }
    if (!any) {
        ImGui::TextDisabled("(no points in any list)");
        ImGui::End();
        return;
    }
    // Include (0, 0) — the objective center — in the bbox so the user
    // always sees where the objective tile center sits relative to the
    // layout.
    min_x = std::min(min_x, 0.0f);
    min_y = std::min(min_y, 0.0f);
    max_x = std::max(max_x, 0.0f);
    max_y = std::max(max_y, 0.0f);
    // Add a 5% margin so points don't sit right on the edge.
    const float margin_x = (max_x - min_x) * 0.05f + 50.0f;
    const float margin_y = (max_y - min_y) * 0.05f + 50.0f;
    min_x -= margin_x; min_y -= margin_y;
    max_x += margin_x; max_y += margin_y;
    const float world_w = max_x - min_x;
    const float world_h = max_y - min_y;

    // --- Available canvas area ---
    ImVec2 canvas_pos = ImGui::GetCursorScreenPos();
    ImVec2 canvas_size = ImGui::GetContentRegionAvail();
    // Reserve a small strip at the bottom for the readout.
    const float footer_h = 22.0f;
    canvas_size.y = std::max(canvas_size.y - footer_h, 60.0f);
    if (canvas_size.x < 50.0f || canvas_size.y < 50.0f) {
        ImGui::TextDisabled("(window too small)");
        ImGui::End();
        return;
    }

    // --- Background + border ---
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(canvas_pos,
                      ImVec2(canvas_pos.x + canvas_size.x,
                             canvas_pos.y + canvas_size.y),
                      IM_COL32(28, 30, 36, 255));
    dl->AddRect(canvas_pos,
               ImVec2(canvas_pos.x + canvas_size.x,
                      canvas_pos.y + canvas_size.y),
               IM_COL32(90, 90, 100, 255));

    // --- Compute feet→pixel scale (uniform, preserve aspect ratio) ---
    const float scale_x = canvas_size.x / world_w;
    const float scale_y = canvas_size.y / world_h;
    const float scale = std::min(scale_x, scale_y);
    // Center the world inside the canvas.
    const float offset_x = canvas_pos.x + (canvas_size.x - world_w * scale) * 0.5f;
    const float offset_y = canvas_pos.y + (canvas_size.y - world_h * scale) * 0.5f;

    // World (feet) → screen (pixels) transform. Note: world Y is NORTH
    // (up) in F4's convention, but screen Y is DOWN — so we flip Y.
    auto wx = [&](float x) -> float {
        return offset_x + (x - min_x) * scale;
    };
    auto wy = [&](float y) -> float {
        return offset_y + (max_y - y) * scale;
    };

    // --- Grid: light gridlines every 500 ft (or 1000 ft if very zoomed out) ---
    const float grid_step = (world_w > 4000.0f || world_h > 4000.0f) ? 1000.0f : 500.0f;
    const ImU32 grid_col = IM_COL32(60, 62, 70, 200);
    for (float gx = std::ceil(min_x / grid_step) * grid_step; gx <= max_x; gx += grid_step) {
        dl->AddLine(ImVec2(wx(gx), wy(min_y)), ImVec2(wx(gx), wy(max_y)), grid_col, 1.0f);
    }
    for (float gy = std::ceil(min_y / grid_step) * grid_step; gy <= max_y; gy += grid_step) {
        dl->AddLine(ImVec2(wx(min_x), wy(gy)), ImVec2(wx(max_x), wy(gy)), grid_col, 1.0f);
    }

    // --- Objective center crosshair (0, 0) ---
    const float cx = wx(0.0f), cy = wy(0.0f);
    dl->AddLine(ImVec2(cx - 8, cy), ImVec2(cx + 8, cy),
                IM_COL32(255, 255, 0, 200), 1.0f);
    dl->AddLine(ImVec2(cx, cy - 8), ImVec2(cx, cy + 8),
                IM_COL32(255, 255, 0, 200), 1.0f);
    dl->AddText(ImVec2(cx + 10, cy - 8), IM_COL32(255, 255, 0, 200), "0,0");

    // --- Draw each layout list ---
    for (const auto& gl : obj.ground_layout) {
        const LayoutColors lc = colors_for_list_type(gl.type);
        const std::size_t n = gl.points.size();
        if (n == 0) continue;

        // Build the polyline vertices.
        // (We could reserve, but ImVector grows fine for our sizes.)
        // For dashed lines (runway dim marks), draw segments alternately.
        if (n >= 2) {
            if (lc.is_dashed) {
                for (std::size_t i = 0; i + 1 < n; i += 2) {
                    dl->AddLine(
                        ImVec2(wx(gl.points[i].x), wy(gl.points[i].y)),
                        ImVec2(wx(gl.points[i + 1].x), wy(gl.points[i + 1].y)),
                        lc.stroke, lc.line_width);
                }
            } else {
                // Draw the polyline as connected segments.
                for (std::size_t i = 0; i + 1 < n; ++i) {
                    dl->AddLine(
                        ImVec2(wx(gl.points[i].x), wy(gl.points[i].y)),
                        ImVec2(wx(gl.points[i + 1].x), wy(gl.points[i + 1].y)),
                        lc.stroke, lc.line_width);
                }
            }
        }

        // Markers at each point. Size scales with PT_FIRST flag.
        for (std::size_t i = 0; i < n; ++i) {
            const float px = wx(gl.points[i].x);
            const float py = wy(gl.points[i].y);
            const bool is_first = (gl.points[i].flags & 0x01) != 0;
            const float r = is_first ? lc.marker_radius * 1.6f : lc.marker_radius;
            dl->AddCircleFilled(ImVec2(px, py), r, lc.fill);
            dl->AddCircle(ImVec2(px, py), r, lc.stroke, 12, 1.0f);
        }
    }

    // --- Feature placements (from Falcon4.FED + FCD) ---
    // Each feature is a building/structure placed at (offset_x, offset_y)
    // feet from the objective center, rotated to `facing` degrees. We
    // draw them as small filled squares rotated to their facing, color-
    // coded by damage state (green=intact, yellow=damaged, red=destroyed).
    // The feature's name (when resolved from FCD) is shown as a tiny
    // label next to the marker.
    if (!obj.features.empty()) {
        // Include feature positions in the bbox so they're always visible
        // (already done above, but worth noting — the initial bbox calc
        // includes only ground_layout points). We re-extend the bbox here
        // would be wrong since we already drew using the existing bbox;
        // instead, we just clamp drawn features to the canvas viewport.
        for (const auto& f : obj.features) {
            // Skip the placeholder FED[0] entry (index=0, all-zero offset).
            if (f.index == 0 && f.offset_x == 0.0f && f.offset_y == 0.0f) continue;
            const float px = wx(f.offset_x);
            const float py = wy(f.offset_y);
            // Skip if outside the canvas viewport.
            if (px < canvas_pos.x - 20 || px > canvas_pos.x + canvas_size.x + 20 ||
                py < canvas_pos.y - 20 || py > canvas_pos.y + canvas_size.y + 20) continue;

            // Damage state color: 0=intact (green), 1=damaged (yellow),
            // 2=destroyed (red), 3=heavily destroyed (dark red).
            ImU32 fill_color;
            const char* dmg_label = "";
            switch (f.damage_state) {
                case 0:  fill_color = IM_COL32( 80, 200,  80, 220); dmg_label = "";       break;
                case 1:  fill_color = IM_COL32(220, 200,  40, 220); dmg_label = " dmg";   break;
                case 2:  fill_color = IM_COL32(220,  80,  40, 220); dmg_label = " dest";  break;
                default: fill_color = IM_COL32(140,  40,  20, 220); dmg_label = " X";     break;
            }

            // Draw a small rotated square (6x6 px) representing the
            // building footprint. Rotation is `facing` degrees.
            // ImDrawList doesn't have a rotated-rect primitive, so we
            // build the 4 corners manually.
            const float half = 4.0f;
            const float rad = -f.facing * 3.14159265f / 180.0f;  // radians
            const float cos_r = std::cos(rad);
            const float sin_r = std::sin(rad);
            auto rot = [&](float dx, float dy) -> ImVec2 {
                return ImVec2(px + dx * cos_r - dy * sin_r,
                               py + dx * sin_r + dy * cos_r);
            };
            const ImVec2 c0 = rot(-half, -half);
            const ImVec2 c1 = rot(+half, -half);
            const ImVec2 c2 = rot(+half, +half);
            const ImVec2 c3 = rot(-half, +half);
            dl->AddQuadFilled(c0, c1, c2, c3, fill_color);
            dl->AddQuad(c0, c1, c2, c3, IM_COL32(20, 20, 20, 220), 1.0f);

            // Draw the facing direction as a short line from center.
            const ImVec2 facing_tip = rot(half + 3.0f, 0.0f);
            dl->AddLine(ImVec2(px, py), facing_tip,
                        IM_COL32(20, 20, 20, 220), 1.0f);

            // Label: feature name (when resolved) + damage state.
            // Only show labels for features with names to avoid clutter.
            if (!f.name.empty()) {
                char label[64];
                std::snprintf(label, sizeof(label), "%s%s",
                              f.name.c_str(), dmg_label);
                dl->AddText(ImVec2(px + 6, py - 6),
                            IM_COL32(220, 220, 220, 220), label);
            }
        }
    }

    // --- Footer readout ---
    ImGui::SetCursorScreenPos(ImVec2(canvas_pos.x,
                                      canvas_pos.y + canvas_size.y + 4));
    ImGui::TextDisabled("bbox: %.0f x %.0f ft   scale: %.1f ft/px   grid: %.0f ft",
                        max_x - min_x, max_y - min_y,
                        1.0f / scale, grid_step);

    // --- Legend (collapsible, below the footer) ---
    if (ImGui::CollapsingHeader("Legend")) {
        static const struct { uint8_t type; const char* name; } legend[] = {
            {1,  "Runway"},
            {8,  "Runway dim marks"},
            {11, "Parking"},
            {12, "Runway left"},
            {13, "Runway right"},
            {14, "Helicopter pad"},
            {16, "Dock"},
            {17, "Track"},
            {4,  "SAM site"},
            {5,  "Artillery"},
            {6,  "AAA"},
            {10, "Static radar"},
        };
        for (const auto& e : legend) {
            const LayoutColors lc = colors_for_list_type(e.type);
            // Draw a small swatch + the name.
            const ImVec2 p = ImGui::GetCursorScreenPos();
            const float sy = p.y + ImGui::GetTextLineHeight() * 0.5f;
            dl = ImGui::GetWindowDrawList();
            dl->AddCircleFilled(ImVec2(p.x + 8, sy), lc.marker_radius, lc.fill);
            dl->AddCircle(ImVec2(p.x + 8, sy), lc.marker_radius, lc.stroke, 12, 1.0f);
            ImGui::Dummy(ImVec2(20, ImGui::GetTextLineHeight()));
            ImGui::SameLine();
            // Decode the type name via the proper decoder for consistency.
            ImGui::TextUnformatted(f4::world_convert::point_list_type_name(e.type));
            ImGui::SameLine();
            ImGui::TextDisabled("(type %d)", e.type);
        }
        ImGui::Separator();
        ImGui::TextUnformatted("Features (building footprints)");
        // Damage state color swatches.
        static const struct { const char* name; ImU32 color; } feat_colors[] = {
            {"Intact",           IM_COL32( 80, 200,  80, 220)},
            {"Damaged",          IM_COL32(220, 200,  40, 220)},
            {"Destroyed",        IM_COL32(220,  80,  40, 220)},
            {"Heavily destroyed",IM_COL32(140,  40,  20, 220)},
        };
        for (const auto& fc : feat_colors) {
            const ImVec2 p = ImGui::GetCursorScreenPos();
            const float sy = p.y + ImGui::GetTextLineHeight() * 0.5f;
            dl = ImGui::GetWindowDrawList();
            // Draw a small filled square as the swatch.
            dl->AddQuadFilled(
                ImVec2(p.x + 2, sy - 4),
                ImVec2(p.x + 10, sy - 4),
                ImVec2(p.x + 10, sy + 4),
                ImVec2(p.x + 2, sy + 4), fc.color);
            ImGui::Dummy(ImVec2(14, ImGui::GetTextLineHeight()));
            ImGui::SameLine();
            ImGui::TextUnformatted(fc.name);
        }
    }

    // --- List details table (collapsible) ---
    if (ImGui::CollapsingHeader("Lists")) {
        ImGui::Text("idx  type                       pts  runway  heading  ltrt");
        for (std::size_t i = 0; i < obj.ground_layout.size(); ++i) {
            const auto& gl = obj.ground_layout[i];
            char type_buf[64];
            std::snprintf(type_buf, sizeof(type_buf), "%d (%s)",
                          gl.type, f4::world_convert::point_list_type_name(gl.type));
            ImGui::Text("%-4zu %-26s %-4d %-7d %-8.0f %s",
                        i, type_buf,
                        static_cast<int>(gl.points.size()),
                        gl.runway_num,
                        gl.heading_deg,
                        f4::viewer::ltrt_name(gl.ltrt));
        }
    }

    // --- Feature placements table (collapsible) ---
    {
        char feat_header[64];
        std::snprintf(feat_header, sizeof(feat_header),
                      "Features (%d)", static_cast<int>(obj.features.size()));
        if (ImGui::CollapsingHeader(feat_header)) {
            if (obj.features.empty()) {
                ImGui::TextDisabled("(no feature placements — FED not loaded or empty)");
            } else {
                ImGui::Text("idx   name              offset        facing  hp    dmg   value");
                for (std::size_t i = 0; i < obj.features.size(); ++i) {
                    const auto& f = obj.features[i];
                    const char* dmg_label =
                        f.damage_state == 0 ? "OK" :
                        f.damage_state == 1 ? "dmg" :
                        f.damage_state == 2 ? "dest" : "X";
                    ImGui::Text("%-5ld %-16s (%5.0f,%5.0f) %-7d %-5d %-4s %d",
                                static_cast<long>(i),
                                f.name.empty() ? "?" : f.name.c_str(),
                                f.offset_x, f.offset_y,
                                f.facing, f.hit_points, dmg_label,
                                static_cast<int>(f.value));
                }
            }
        }
    }

    ImGui::End();
}

} // namespace f4::viewer
