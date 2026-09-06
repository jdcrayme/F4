// f4-world-viewer/src/ground_layout_view.cpp
//
// "Ground Layout" tab content — a dedicated 2D top-down view of the
// selected objective's PHD/PD ground-layout data.
//
// Content-only: the caller (draw_inspector_window) owns the ImGui window
// and tab item. This function draws the layout (or a placeholder if no
// applicable objective is selected).
//
// Migrated from WorldState to EntityWorld (Step 4c).
// Refactored to content-only (INSPECTOR-TABS-1) — the function no longer
// opens its own ImGui::Begin/End; it draws into whatever tab item is
// currently active.

#include "viewer_state.hpp"
#include <f4/viewer/enum_text.hpp>
#include <f4/world_types/campaign_names.hpp>
#include <f4/math/constants.hpp>

#include <imgui.h>
#include <algorithm>
#include <cmath>
#include <cstdio>

namespace f4::viewer {

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
        case 1:  return { IM_COL32( 30,  30,  30, 255), IM_COL32( 60,  60,  60, 255), 4.0f, 3.0f, false };
        case 8:  return { IM_COL32(120, 120, 120, 200), IM_COL32(120, 120, 120, 200), 1.0f, 2.0f, true  };
        case 11: return { IM_COL32( 60, 200,  80, 255), IM_COL32( 60, 200,  80, 255), 1.0f, 4.0f, false };
        case 12: return { IM_COL32(200, 200,  60, 200), IM_COL32(200, 200,  60, 200), 2.0f, 2.0f, false };
        case 13: return { IM_COL32(200, 140,  60, 200), IM_COL32(200, 140,  60, 200), 2.0f, 2.0f, false };
        case 14: return { IM_COL32( 80, 200, 220, 255), IM_COL32( 80, 200, 220, 255), 1.0f, 4.0f, false };
        case 15: return { IM_COL32(220, 180,  60, 200), IM_COL32(220, 180,  60, 200), 1.0f, 3.0f, false };
        case 16: return { IM_COL32( 60, 120, 220, 255), IM_COL32( 60, 120, 220, 255), 1.0f, 4.0f, false };
        case 17: return { IM_COL32(140, 100,  60, 220), IM_COL32(140, 100,  60, 220), 2.0f, 2.0f, false };
        case 4:  return { IM_COL32(220,  60,  60, 255), IM_COL32(220,  60,  60, 255), 0.0f, 5.0f, false };
        case 5:  return { IM_COL32(220, 140,  40, 255), IM_COL32(220, 140,  40, 255), 0.0f, 5.0f, false };
        case 6:  return { IM_COL32(220, 200,  40, 255), IM_COL32(220, 200,  40, 255), 0.0f, 4.0f, false };
        case 10: return { IM_COL32(180,  60, 220, 255), IM_COL32(180,  60, 220, 255), 1.0f, 5.0f, false };
        default: return { IM_COL32(160, 160, 160, 180), IM_COL32(160, 160, 160, 180), 1.0f, 2.0f, false };
    }
}

} // namespace

void ViewerApp::draw_ground_layout_view() {
    // Only meaningful when an objective with ground_layout OR features is
    // selected. When the user is on this tab without an applicable selection,
    // show a placeholder so the tab stays stable (doesn't disappear).
    if (impl_->sel_kind != Impl::SelectionKind::Objective ||
        !impl_->sel_entity.valid()) {
        ImGui::TextDisabled("Select an objective to view its ground layout.");
        return;
    }
    auto h = impl_->handle(impl_->sel_entity);
    auto* gl = h.get<f4::entities::GroundLayoutComponent>();
    auto* fs = h.get<f4::entities::FeatureSetComponent>();
    auto* ot = h.get<f4::entities::ObjectiveTypeComponent>();
    const bool has_layout = gl && !gl->layouts.empty();
    const bool has_features = fs && !fs->features.empty();
    if (!has_layout && !has_features) {
        ImGui::TextDisabled("Selected objective has no ground layout or features.");
        return;
    }
    // Header
    if (ot && !ot->class_name.empty()) {
        ImGui::TextUnformatted(ot->class_name.c_str());
    } else {
        ImGui::Text("Objective");
    }
    ImGui::SameLine();
    ImGui::TextDisabled("(%d lists, %d pts, %d features)",
                        gl ? static_cast<int>(gl->layouts.size()) : 0,
                        [&]{
                            int n = 0;
                            if (gl) for (const auto& layout : gl->layouts)
                                n += static_cast<int>(layout.points.size());
                            return n;
                        }(),
                        fs ? static_cast<int>(fs->features.size()) : 0);
    ImGui::SameLine();
    if (ImGui::Button("Zoom to Layout")) {
        impl_->fit_to_selection_layout();
    }
    ImGui::SameLine();
    ImGui::TextDisabled("(main canvas)");

    // Compute the layout's bounding box (in feet)
    float min_x = 1e30f, min_y = 1e30f, max_x = -1e30f, max_y = -1e30f;
    bool any = false;
    if (gl) {
        for (const auto& layout : gl->layouts) {
            for (const auto& pt : layout.points) {
                min_x = std::min(min_x, pt.x);
                min_y = std::min(min_y, pt.y);
                max_x = std::max(max_x, pt.x);
                max_y = std::max(max_y, pt.y);
                any = true;
            }
        }
    }
    if (fs) {
        for (const auto& f : fs->features) {
            if (f.index == 0 && f.offset_x == 0.0f && f.offset_y == 0.0f) continue;
            min_x = std::min(min_x, f.offset_x);
            min_y = std::min(min_y, f.offset_y);
            max_x = std::max(max_x, f.offset_x);
            max_y = std::max(max_y, f.offset_y);
            any = true;
        }
    }
    if (!any) {
        ImGui::TextDisabled("(no points in any list)");
        return;
    }
    min_x = std::min(min_x, 0.0f);
    min_y = std::min(min_y, 0.0f);
    max_x = std::max(max_x, 0.0f);
    max_y = std::max(max_y, 0.0f);
    const float margin_x = (max_x - min_x) * 0.05f + 50.0f;
    const float margin_y = (max_y - min_y) * 0.05f + 50.0f;
    min_x -= margin_x; min_y -= margin_y;
    max_x += margin_x; max_y += margin_y;
    const float world_w = max_x - min_x;
    const float world_h = max_y - min_y;

    ImVec2 canvas_pos = ImGui::GetCursorScreenPos();
    ImVec2 canvas_size = ImGui::GetContentRegionAvail();
    const float footer_h = 22.0f;
    canvas_size.y = std::max(canvas_size.y - footer_h, 60.0f);
    if (canvas_size.x < 50.0f || canvas_size.y < 50.0f) {
        ImGui::TextDisabled("(window too small)");
        return;
    }

    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(canvas_pos,
                      ImVec2(canvas_pos.x + canvas_size.x,
                             canvas_pos.y + canvas_size.y),
                      IM_COL32(28, 30, 36, 255));
    dl->AddRect(canvas_pos,
               ImVec2(canvas_pos.x + canvas_size.x,
                      canvas_pos.y + canvas_size.y),
               IM_COL32(90, 90, 100, 255));

    const float scale_x = canvas_size.x / world_w;
    const float scale_y = canvas_size.y / world_h;
    const float scale = std::min(scale_x, scale_y);
    const float offset_x = canvas_pos.x + (canvas_size.x - world_w * scale) * 0.5f;
    const float offset_y = canvas_pos.y + (canvas_size.y - world_h * scale) * 0.5f;

    auto wx = [&](float x) -> float {
        return offset_x + (x - min_x) * scale;
    };
    auto wy = [&](float y) -> float {
        return offset_y + (max_y - y) * scale;
    };

    const float grid_step = (world_w > 4000.0f || world_h > 4000.0f) ? 1000.0f : 500.0f;
    const ImU32 grid_col = IM_COL32(60, 62, 70, 200);
    for (float gx = std::ceil(min_x / grid_step) * grid_step; gx <= max_x; gx += grid_step) {
        dl->AddLine(ImVec2(wx(gx), wy(min_y)), ImVec2(wx(gx), wy(max_y)), grid_col, 1.0f);
    }
    for (float gy = std::ceil(min_y / grid_step) * grid_step; gy <= max_y; gy += grid_step) {
        dl->AddLine(ImVec2(wx(min_x), wy(gy)), ImVec2(wx(max_x), wy(gy)), grid_col, 1.0f);
    }

    const float cx = wx(0.0f), cy = wy(0.0f);
    dl->AddLine(ImVec2(cx - 8, cy), ImVec2(cx + 8, cy),
                IM_COL32(255, 255, 0, 200), 1.0f);
    dl->AddLine(ImVec2(cx, cy - 8), ImVec2(cx, cy + 8),
                IM_COL32(255, 255, 0, 200), 1.0f);
    dl->AddText(ImVec2(cx + 10, cy - 8), IM_COL32(255, 255, 0, 200), "0,0");

    if (gl) {
        for (const auto& layout : gl->layouts) {
            const LayoutColors lc = colors_for_list_type(layout.type);
            const std::size_t n = layout.points.size();
            if (n == 0) continue;
            if (n >= 2) {
                if (lc.line_width > 0)
                    if (lc.is_dashed) {
                        for (std::size_t i = 0; i + 1 < n; i += 2) {
                            dl->AddLine(
                                ImVec2(wx(layout.points[i].x), wy(layout.points[i].y)),
                                ImVec2(wx(layout.points[i + 1].x), wy(layout.points[i + 1].y)),
                                lc.stroke, lc.line_width);
                        }
                    }
                    else {
                        for (std::size_t i = 0; i + 1 < n; ++i) {
                            dl->AddLine(
                                ImVec2(wx(layout.points[i].x), wy(layout.points[i].y)),
                                ImVec2(wx(layout.points[i + 1].x), wy(layout.points[i + 1].y)),
                                lc.stroke, lc.line_width);
                        }
                    }
            }
            for (std::size_t i = 0; i < n; ++i) {
                const float px = wx(layout.points[i].x);
                const float py = wy(layout.points[i].y);
                const bool is_first = (layout.points[i].flags & 0x01) != 0;
                const float r = is_first ? lc.marker_radius * 1.6f : lc.marker_radius;
                dl->AddCircleFilled(ImVec2(px, py), r, lc.fill);
                dl->AddCircle(ImVec2(px, py), r, lc.stroke, 12, 1.0f);
            }
        }
    }

    // Feature placements
    if (fs && !fs->features.empty()) {
        for (const auto& f : fs->features) {
            if (f.index == 0 && f.offset_x == 0.0f && f.offset_y == 0.0f) continue;
            const float px = wx(f.offset_x);
            const float py = wy(f.offset_y);
            if (px < canvas_pos.x - 20 || px > canvas_pos.x + canvas_size.x + 20 ||
                py < canvas_pos.y - 20 || py > canvas_pos.y + canvas_size.y + 20) continue;

            ImU32 fill_color;
            const char* dmg_label = "";
            switch (f.damage_state) {
                case 0:  fill_color = IM_COL32( 80, 200,  80, 220); dmg_label = "";       break;
                case 1:  fill_color = IM_COL32(220, 200,  40, 220); dmg_label = " dmg";   break;
                case 2:  fill_color = IM_COL32(220,  80,  40, 220); dmg_label = " dest";  break;
                default: fill_color = IM_COL32(140,  40,  20, 220); dmg_label = " X";     break;
            }

            const float half = 4.0f;
            const float rad = -f.facing * static_cast<float>(f4::math::DEG_TO_RAD);
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

            const ImVec2 facing_tip = rot(half + 3.0f, 0.0f);
            dl->AddLine(ImVec2(px, py), facing_tip,
                        IM_COL32(20, 20, 20, 220), 1.0f);

            if (!f.name.empty()) {
                char label[64];
                std::snprintf(label, sizeof(label), "%s%s",
                              f.name.c_str(), dmg_label);
                dl->AddText(ImVec2(px + 6, py - 6),
                            IM_COL32(220, 220, 220, 220), label);
            }
        }
    }

    // Footer
    ImGui::SetCursorScreenPos(ImVec2(canvas_pos.x,
                                      canvas_pos.y + canvas_size.y + 4));
    ImGui::TextDisabled("bbox: %.0f x %.0f ft   scale: %.1f ft/px   grid: %.0f ft",
                        max_x - min_x, max_y - min_y,
                        1.0f / scale, grid_step);

    // Legend
    if (ImGui::CollapsingHeader("Legend")) {
        static const struct { uint8_t type; const char* name; } legend[] = {
            {1,  "Runway"},   {8,  "Runway dim marks"}, {11, "Parking"},
            {12, "Runway left"}, {13, "Runway right"}, {14, "Helicopter pad"},
            {16, "Dock"},     {17, "Track"},
            {4,  "SAM site"}, {5,  "Artillery"},        {6,  "AAA"},
            {10, "Static radar"},
        };
        for (const auto& e : legend) {
            const LayoutColors lc = colors_for_list_type(e.type);
            const ImVec2 p = ImGui::GetCursorScreenPos();
            const float sy = p.y + ImGui::GetTextLineHeight() * 0.5f;
            dl = ImGui::GetWindowDrawList();
            dl->AddCircleFilled(ImVec2(p.x + 8, sy), lc.marker_radius, lc.fill);
            dl->AddCircle(ImVec2(p.x + 8, sy), lc.marker_radius, lc.stroke, 12, 1.0f);
            ImGui::Dummy(ImVec2(20, ImGui::GetTextLineHeight()));
            ImGui::SameLine();
            ImGui::TextUnformatted(f4::world_types::point_list_type_name(e.type));
            ImGui::SameLine();
            ImGui::TextDisabled("(type %d)", e.type);
        }
        ImGui::Separator();
        ImGui::TextUnformatted("Features (building footprints)");
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

    // List details table
    if (gl && ImGui::CollapsingHeader("Lists")) {
        ImGui::Text("idx  type                       pts  runway  heading  ltrt");
        for (std::size_t i = 0; i < gl->layouts.size(); ++i) {
            const auto& layout = gl->layouts[i];
            char type_buf[64];
            std::snprintf(type_buf, sizeof(type_buf), "%d (%s)",
                          layout.type, f4::world_types::point_list_type_name(layout.type));
            ImGui::Text("%-4zu %-26s %-4d %-7d %-8.0f %s",
                        i, type_buf,
                        static_cast<int>(layout.points.size()),
                        layout.runway_num,
                        layout.heading_deg,
                        f4::viewer::ltrt_name(layout.ltrt));
        }
    }

    // Feature placements table
    if (fs) {
        char feat_header[64];
        std::snprintf(feat_header, sizeof(feat_header),
                      "Features (%d)", static_cast<int>(fs->features.size()));
        if (ImGui::CollapsingHeader(feat_header)) {
            if (fs->features.empty()) {
                ImGui::TextDisabled("(no feature placements — FED not loaded or empty)");
            } else {
                ImGui::Text("idx   name              offset        facing  hp    dmg   value");
                for (std::size_t i = 0; i < fs->features.size(); ++i) {
                    const auto& f = fs->features[i];
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
    // (No ImGui::End() here — caller owns the window.)
}

} // namespace f4::viewer
