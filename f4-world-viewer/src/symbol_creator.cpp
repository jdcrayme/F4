// f4-world-viewer/src/symbol_creator.cpp
//
// Interactive Symbol Creator panel — lets the user build data-driven
// symbol definitions by dragging points on a 2D canvas, then save/load
// the resulting library to JSON.
//
// The data model + JSON I/O + render helpers live in f4-renderer
// (include/f4/renderer/symbol_library.hpp). This file is purely the
// ImGui view + controller — it edits a SymbolLibrary in place and
// delegates rendering to f4::renderer::draw_library_symbol().
//
// Interaction model
// -----------------
// The canvas shows the [-1, +1] × [-1, +1] symbol space (the same
// coordinate convention used by every other symbol renderer in the
// project). The user can:
//
//   • Click an existing point to select it (the primitives panel
//     highlights which primitive + point is active).
//   • Drag a selected point to move it. Coordinates are clamped to
//     [-1.5, +1.5] so points can sit slightly outside the nominal
//     symbol box (matches the existing procedural symbols which
//     draw battalion markers at y = -1.5 of r).
//   • Click empty canvas to append a new point to the currently
//     selected primitive. If no primitive is selected, the click is
//     ignored (the user must first add a primitive via the right
//     panel's [+ Polyline] / [+ Polygon] buttons).
//   • Right-click a point to delete it from its primitive.
//   • Middle-drag to pan the canvas view; mouse wheel to zoom.
//
// The library browser (left panel) lists every SymbolDefinition in
// the library by key + display_name. Clicking a row switches the
// editor to that symbol. [New] creates a blank symbol with key
// "new_symbol_N". [Duplicate] clones the current symbol with a
// "_copy" suffix on the key. [Delete] removes the current symbol.

#include "viewer_state.hpp"  // gives us access to f4::viewer::* + imgui + raylib

#include <f4/viewer/symbol_creator.hpp>
#include <f4/viewer/file_dialog.hpp>

#include <imgui.h>
#include <rlImGui.h>
#include <raylib.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace f4::viewer {

// ---------------------------------------------------------------------------
// Helpers — copy std::string → fixed char buffer (truncating with NUL).
// ---------------------------------------------------------------------------

namespace {

void str_to_buf(const std::string& s, char* buf, std::size_t buf_sz) {
    if (buf_sz == 0) return;
    const std::size_t n = std::min(s.size(), buf_sz - 1);
    std::memcpy(buf, s.data(), n);
    buf[n] = '\0';
}

std::string buf_to_str(const char* buf) {
    return std::string(buf ? buf : "");
}

// Pick a unique key in the library by appending "_2", "_3", ... to the
// base until the key is unused.
std::string unique_key(const f4::renderer::SymbolLibrary& lib,
                        const std::string& base) {
    if (lib.find(base) == nullptr) return base;
    for (int i = 2; i < 1000; ++i) {
        char candidate[256];
        std::snprintf(candidate, sizeof(candidate), "%s_%d", base.c_str(), i);
        if (lib.find(candidate) == nullptr) return candidate;
    }
    return base + "_x";  // give up (shouldn't happen with <1000 symbols)
}

} // namespace

// ===========================================================================
// Lifecycle + lazy init
// ===========================================================================

SymbolCreator::SymbolCreator() = default;

void SymbolCreator::open() {
    open_ = true;
    status_msg_.clear();
    last_error_.clear();
}

void SymbolCreator::close() {
    open_ = false;
}

void SymbolCreator::ensure_library_initialized() {
    if (library_initialized_) return;
    library_ = f4::renderer::make_default_symbol_library();
    library_initialized_ = true;
    if (!library_.empty()) {
        selected_symbol_idx_ = 0;
        sync_metadata_buffers();
    }
}

// ---------------------------------------------------------------------------
// Selection helpers
// ---------------------------------------------------------------------------

f4::renderer::SymbolDefinition* SymbolCreator::current_symbol() {
    if (selected_symbol_idx_ < 0 ||
        selected_symbol_idx_ >= static_cast<int>(library_.size())) {
        return nullptr;
    }
    return &library_.mutable_symbols()[static_cast<std::size_t>(selected_symbol_idx_)];
}

const f4::renderer::SymbolDefinition* SymbolCreator::current_symbol() const {
    if (selected_symbol_idx_ < 0 ||
        selected_symbol_idx_ >= static_cast<int>(library_.size())) {
        return nullptr;
    }
    return &library_.symbols()[static_cast<std::size_t>(selected_symbol_idx_)];
}

SymbolCreator::PrimRef SymbolCreator::current_primitive() {
    PrimRef ref;
    auto* s = current_symbol();
    if (!s) return ref;
    if (sel_prim_kind_ == SelectedPrimKind::Polyline &&
        sel_prim_idx_ >= 0 &&
        sel_prim_idx_ < static_cast<int>(s->polylines.size())) {
        ref.polyline = &s->polylines[static_cast<std::size_t>(sel_prim_idx_)];
        ref.points   = &ref.polyline->points;
    } else if (sel_prim_kind_ == SelectedPrimKind::Polygon &&
               sel_prim_idx_ >= 0 &&
               sel_prim_idx_ < static_cast<int>(s->polygons.size())) {
        ref.polygon = &s->polygons[static_cast<std::size_t>(sel_prim_idx_)];
        ref.points  = &ref.polygon->points;
    }
    return ref;
}

void SymbolCreator::sync_metadata_buffers() {
    const auto* s = current_symbol();
    if (!s) {
        key_buf_[0] = display_name_buf_[0] = category_buf_[0] = description_buf_[0] = '\0';
        return;
    }
    str_to_buf(s->key,           key_buf_,           sizeof(key_buf_));
    str_to_buf(s->display_name,  display_name_buf_,  sizeof(display_name_buf_));
    str_to_buf(s->category,      category_buf_,      sizeof(category_buf_));
    str_to_buf(s->description,   description_buf_,   sizeof(description_buf_));
}

void SymbolCreator::apply_metadata_buffers() {
    auto* s = current_symbol();
    if (!s) return;
    // The key is special: changing it must not collide with another symbol's
    // key (that would create a duplicate the library can't distinguish). We
    // silently refuse key edits that collide — the buffer keeps the user's
    // text but the symbol's actual key isn't updated.
    const std::string new_key = buf_to_str(key_buf_);
    if (!new_key.empty() && new_key != s->key) {
        // Only check for collisions with OTHER symbols.
        bool collides = false;
        for (std::size_t i = 0; i < library_.size(); ++i) {
            if (static_cast<int>(i) == selected_symbol_idx_) continue;
            if (library_.symbols()[i].key == new_key) { collides = true; break; }
        }
        if (!collides) {
            s->key = new_key;
        } else {
            // Visual feedback: revert the buffer to the actual key so the
            // user sees their edit was rejected.
            str_to_buf(s->key, key_buf_, sizeof(key_buf_));
            last_error_ = "Key '" + new_key + "' is already in use; reverting.";
        }
    }
    s->display_name = buf_to_str(display_name_buf_);
    s->category     = buf_to_str(category_buf_);
    s->description  = buf_to_str(description_buf_);
}

// ---------------------------------------------------------------------------
// Canvas geometry
// ---------------------------------------------------------------------------

SymbolCreator::CanvasGeom SymbolCreator::compute_canvas_geom() const {
    CanvasGeom g;
    // The canvas is a square that fills the available middle column.
    // The base size is whatever fits in the smaller of (avail_w, avail_h);
    // view_zoom_ multiplies that to allow zoom-in for fine editing.
    const ImVec2 avail = ImGui::GetContentRegionAvail();
    const float base = std::min(avail.x, avail.y);
    if (base <= 0.0f) return g;
    g.size_px = base * view_zoom_;
    g.half    = g.size_px * 0.5f;
    const ImVec2 cursor = ImGui::GetCursorScreenPos();
    g.top_left_x = cursor.x + view_pan_x_;
    g.top_left_y = cursor.y + view_pan_y_;
    return g;
}

// ===========================================================================
// draw() — main entry point, called from draw_imgui() when is_open()
// ===========================================================================

void SymbolCreator::draw() {
    if (!open_) return;

    ensure_library_initialized();

    // Default window position: right of the Layers panel, below the menu.
    ImGui::SetNextWindowSize(ImVec2(960, 640), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowPos(ImVec2(280, 80), ImGuiCond_FirstUseEver);

    const ImGuiWindowFlags flags = ImGuiWindowFlags_NoCollapse;
    if (!ImGui::Begin("Symbol Creator", &open_, flags)) {
        ImGui::End();
        return;
    }

    // --- Toolbar ---
    if (ImGui::Button("New Symbol")) {
        f4::renderer::SymbolDefinition s;
        s.key = unique_key(library_, "new_symbol");
        s.display_name = "New Symbol";
        s.category = "custom";
        library_.add_or_replace(std::move(s));
        selected_symbol_idx_ = static_cast<int>(library_.size()) - 1;
        sel_prim_kind_ = SelectedPrimKind::None;
        sel_prim_idx_ = -1;
        sel_point_idx_ = -1;
        sync_metadata_buffers();
        status_msg_ = "Created new symbol.";
    }
    ImGui::SameLine();
    if (ImGui::Button("Duplicate") && current_symbol()) {
        f4::renderer::SymbolDefinition copy = *current_symbol();
        copy.key = unique_key(library_, copy.key + "_copy");
        copy.display_name = copy.display_name + " (copy)";
        library_.add_or_replace(std::move(copy));
        selected_symbol_idx_ = static_cast<int>(library_.size()) - 1;
        sync_metadata_buffers();
        status_msg_ = "Duplicated symbol.";
    }
    ImGui::SameLine();
    if (ImGui::Button("Delete") && current_symbol()) {
        const std::string key = current_symbol()->key;
        library_.erase(key);
        if (!library_.empty()) {
            if (selected_symbol_idx_ >= static_cast<int>(library_.size())) {
                selected_symbol_idx_ = static_cast<int>(library_.size()) - 1;
            }
        } else {
            selected_symbol_idx_ = -1;
        }
        sel_prim_kind_ = SelectedPrimKind::None;
        sel_prim_idx_ = -1;
        sel_point_idx_ = -1;
        sync_metadata_buffers();
        status_msg_ = "Deleted symbol '" + key + "'.";
    }
    ImGui::SameLine();
    ImGui::Dummy(ImVec2(8, 0));  // spacer instead of vertical separator (not in ImGui 1.91)
    ImGui::SameLine();
    if (ImGui::Button("Load Library...")) {
        pick_load_library_dialog();
    }
    ImGui::SameLine();
    if (ImGui::Button("Save Library")) {
        pick_save_library_dialog();
    }

    ImGui::Separator();

    // --- Body: 3-column layout (library | canvas | primitives) ---
    // We use a fixed-height content region for the 3 columns so the
    // metadata strip below stays anchored to the bottom of the window.
    const float metadata_h = 110.0f;
    const float avail_h = ImGui::GetContentRegionAvail().y;
    const float panel_h = std::max(avail_h - metadata_h, 100.0f);

    draw_library_panel(panel_h);
    ImGui::SameLine();
    draw_canvas_panel(panel_h);
    ImGui::SameLine();
    draw_primitives_panel(panel_h);

    // Bottom: metadata + hints
    draw_metadata_panel();

    ImGui::End();
}

// ===========================================================================
// Library browser (left column)
// ===========================================================================

void SymbolCreator::draw_library_panel(float panel_h) {
    ImGui::BeginChild("##library_browser", ImVec2(200, panel_h), true);
    ImGui::TextDisabled("Library (%zu)", library_.size());
    ImGui::Separator();

    if (library_.empty()) {
        ImGui::TextWrapped("No symbols. Click 'New Symbol' above to create one.");
    } else {
        // Use a child region with a fixed height so the list scrolls
        // independently of the toolbar above.
        if (ImGui::BeginChild("##lib_list", ImVec2(0, 0), false,
                               ImGuiWindowFlags_AlwaysVerticalScrollbar)) {
            for (int i = 0; i < static_cast<int>(library_.size()); ++i) {
                const auto& s = library_.symbols()[static_cast<std::size_t>(i)];
                const bool selected = (i == selected_symbol_idx_);
                // Show key + display_name. The Selectable's label is the
                // display_name (or key if display_name is empty).
                char label[256];
                const std::string& name = s.display_name.empty() ? s.key : s.display_name;
                std::snprintf(label, sizeof(label), "%s###%d", name.c_str(), i);
                if (ImGui::Selectable(label, selected)) {
                    selected_symbol_idx_ = i;
                    sel_prim_kind_ = SelectedPrimKind::None;
                    sel_prim_idx_ = -1;
                    sel_point_idx_ = -1;
                    sync_metadata_buffers();
                }
                // Tooltip with the key + category + description.
                if (ImGui::IsItemHovered()) {
                    ImGui::SetTooltip("key: %s\ncategory: %s\n%s",
                                       s.key.c_str(),
                                       s.category.empty() ? "(none)" : s.category.c_str(),
                                       s.description.c_str());
                }
            }
        }
        ImGui::EndChild();
    }
    ImGui::EndChild();
}

// ===========================================================================
// Canvas (middle column)
// ===========================================================================

void SymbolCreator::draw_canvas_panel(float panel_h) {
    ImGui::BeginChild("##canvas_panel", ImVec2(0, panel_h), true);

    if (!current_symbol()) {
        ImGui::TextDisabled("Select or create a symbol to edit.");
        ImGui::EndChild();
        return;
    }

    // Compute canvas geometry from the available region.
    const ImVec2 avail = ImGui::GetContentRegionAvail();
    const float base = std::min(avail.x, avail.y);
    if (base <= 10.0f) {
        ImGui::TextDisabled("(canvas too small)");
        ImGui::EndChild();
        return;
    }
    const float size_px = base * view_zoom_;
    const float half = size_px * 0.5f;
    const ImVec2 cursor = ImGui::GetCursorScreenPos();
    const float cx = cursor.x + view_pan_x_ + half;
    const float cy = cursor.y + view_pan_y_ + half;

    // --- Draw the canvas background + grid + bounding box ---
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImU32 bg_col     = IM_COL32(28, 28, 32, 255);
    const ImU32 grid_col   = IM_COL32(60, 60, 70, 200);
    const ImU32 box_col    = IM_COL32(120, 120, 140, 255);
    const ImU32 axis_col   = IM_COL32(80, 80, 100, 200);
    const ImU32 pt_col     = IM_COL32(240, 220, 80, 255);
    const ImU32 pt_sel_col = IM_COL32(80, 200, 255, 255);

    // Background (square, centered on (cx, cy))
    dl->AddRectFilled(ImVec2(cx - half, cy - half),
                       ImVec2(cx + half, cy + half), bg_col);

    // Subtle grid: lines every 0.25 in symbol space (= 1/8 of the box).
    const float step = 0.25f;
    for (float v = -1.0f; v <= 1.0f + 0.001f; v += step) {
        // Vertical line at x = v
        const float sx = cx + v * half;
        dl->AddLine(ImVec2(sx, cy - half), ImVec2(sx, cy + half), grid_col);
        // Horizontal line at y = v
        const float sy = cy + v * half;
        dl->AddLine(ImVec2(cx - half, sy), ImVec2(cx + half, sy), grid_col);
    }
    // Bounding box (the [-1, +1] × [-1, +1] square)
    dl->AddRect(ImVec2(cx - half, cy - half), ImVec2(cx + half, cy + half),
                 box_col, 0.0f, 0, 1.5f);
    // Axes (center cross)
    dl->AddLine(ImVec2(cx - half, cy), ImVec2(cx + half, cy), axis_col);
    dl->AddLine(ImVec2(cx, cy - half), ImVec2(cx, cy + half), axis_col);

    // --- Render the current symbol via the shared library renderer ---
    // We use the ImGui path with fill = team-blue, outline = white so
    // the user sees the symbol as it would appear on a friendly team.
    // NOTE: must use the non-const current_symbol() overload because we
    // mutate the symbol's point vectors in the click/drag handlers below.
    auto* s = current_symbol();
    if (!s) {
        ImGui::TextDisabled("(no symbol selected)");
        ImGui::EndChild();
        return;
    }
    const ImU32 fill_col    = IM_COL32(60, 140, 220, 180);
    const ImU32 outline_col = IM_COL32(255, 255, 255, 255);
    f4::renderer::draw_library_symbol(dl, library_, s->key,
                                       ImVec2(cx, cy), size_px,
                                       fill_col, outline_col, true);

    // --- Draw point handles on top of the rendered symbol ---
    // All primitives' points are drawn as small dots; the selected point
    // is drawn larger + brighter.
    auto draw_points = [&](const std::vector<f4::renderer::SymbolPoint>& pts,
                            SelectedPrimKind kind, int prim_idx) {
        for (std::size_t i = 0; i < pts.size(); ++i) {
            const ImVec2 p(cx + pts[i].x * half, cy + pts[i].y * half);
            const bool is_sel = (kind == sel_prim_kind_ &&
                                  prim_idx == sel_prim_idx_ &&
                                  static_cast<int>(i) == sel_point_idx_);
            const float r = is_sel ? 5.0f : 3.5f;
            const ImU32 col = is_sel ? pt_sel_col : pt_col;
            dl->AddCircleFilled(p, r, col);
            dl->AddCircle(p, r, IM_COL32(0, 0, 0, 200), 0, 1.0f);
        }
    };
    // s is the non-const SymbolDefinition* (declared above, before the
    // render-library call). It's used here for iteration and below for
    // mutation in the click/drag handlers.
    for (std::size_t i = 0; i < s->polylines.size(); ++i) {
        draw_points(s->polylines[i].points, SelectedPrimKind::Polyline, static_cast<int>(i));
    }
    for (std::size_t i = 0; i < s->polygons.size(); ++i) {
        draw_points(s->polygons[i].points, SelectedPrimKind::Polygon, static_cast<int>(i));
    }

    // --- Capture mouse interactions on the canvas ---
    // We use an InvisibleButton to capture clicks inside the canvas area.
    // The button's bounding rect is the visible square.
    ImGui::SetCursorScreenPos(ImVec2(cx - half, cy - half));
    ImGui::InvisibleButton("##canvas_interaction",
                            ImVec2(size_px, size_px));
    const bool canvas_hovered = ImGui::IsItemHovered();
    const ImVec2 mouse = ImGui::GetMousePos();
    const float mx = mouse.x - cx;
    const float my = mouse.y - cy;

    // Helper: convert mouse offset from center to normalized symbol coords.
    auto to_normalized = [&](float dx, float dy) -> f4::renderer::SymbolPoint {
        return { std::clamp(dx / half, -1.5f, 1.5f),
                 std::clamp(dy / half, -1.5f, 1.5f) };
    };

    // Helper: find the nearest point within a pixel radius.
    // Returns the primitive + point index, or {None, -1, -1} if nothing
    // is within `pick_px` pixels of the mouse.
    auto find_nearest_point = [&](float pick_px) {
        struct Hit { SelectedPrimKind kind; int prim_idx; int point_idx; float dist2; };
        Hit best { SelectedPrimKind::None, -1, -1, pick_px * pick_px };
        const auto* sym = current_symbol();
        if (!sym) return best;
        auto consider = [&](const std::vector<f4::renderer::SymbolPoint>& pts,
                              SelectedPrimKind kind, int prim_idx) {
            for (std::size_t i = 0; i < pts.size(); ++i) {
                const float px = pts[i].x * half;
                const float py = pts[i].y * half;
                const float dx = px - mx;
                const float dy = py - my;
                const float d2 = dx * dx + dy * dy;
                if (d2 < best.dist2) {
                    best = { kind, prim_idx, static_cast<int>(i), d2 };
                }
            }
        };
        for (std::size_t i = 0; i < sym->polylines.size(); ++i) {
            consider(sym->polylines[i].points, SelectedPrimKind::Polyline, static_cast<int>(i));
        }
        for (std::size_t i = 0; i < sym->polygons.size(); ++i) {
            consider(sym->polygons[i].points, SelectedPrimKind::Polygon, static_cast<int>(i));
        }
        return best;
    };

    const float pick_px = 8.0f;  // 8-pixel pick radius

    // Right-click on a point: delete it.
    if (canvas_hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
        auto hit = find_nearest_point(pick_px);
        if (hit.kind != SelectedPrimKind::None) {
            // Delete the point from its primitive. We use the `s` pointer
            // (non-const, declared above) to mutate the symbol directly.
            if (hit.kind == SelectedPrimKind::Polyline) {
                auto& pl = s->polylines[static_cast<std::size_t>(hit.prim_idx)];
                if (hit.point_idx >= 0 &&
                    hit.point_idx < static_cast<int>(pl.points.size())) {
                    pl.points.erase(pl.points.begin() + hit.point_idx);
                    if (sel_point_idx_ == hit.point_idx) sel_point_idx_ = -1;
                    else if (sel_point_idx_ > hit.point_idx) --sel_point_idx_;
                }
            } else {
                auto& pg = s->polygons[static_cast<std::size_t>(hit.prim_idx)];
                if (hit.point_idx >= 0 &&
                    hit.point_idx < static_cast<int>(pg.points.size())) {
                    pg.points.erase(pg.points.begin() + hit.point_idx);
                    if (sel_point_idx_ == hit.point_idx) sel_point_idx_ = -1;
                    else if (sel_point_idx_ > hit.point_idx) --sel_point_idx_;
                }
            }
            status_msg_ = "Deleted point.";
        }
    }

    // Left-click: select an existing point, or (if a primitive is
    // selected and Shift is held) append a new point.
    if (canvas_hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        auto hit = find_nearest_point(pick_px);
        if (hit.kind != SelectedPrimKind::None) {
            // Select the hit point + primitive.
            sel_prim_kind_ = hit.kind;
            sel_prim_idx_  = hit.prim_idx;
            sel_point_idx_ = hit.point_idx;
            dragging_point_ = true;
            status_msg_.clear();
        } else {
            // No nearby point. If a primitive is selected, append a new
            // point to it (normal click) — but only when the click is
            // inside the bounding box (so we don't add points far off-canvas).
            if (std::fabs(mx) <= half && std::fabs(my) <= half) {
                auto ref = current_primitive();
                if (ref.points) {
                    ref.points->push_back(to_normalized(mx, my));
                    sel_point_idx_ = static_cast<int>(ref.points->size()) - 1;
                    dragging_point_ = true;
                    status_msg_ = "Added point.";
                } else {
                    status_msg_ = "Select or add a primitive first (right panel).";
                }
            }
        }
    }

    // Drag: move the selected point with the mouse.
    if (dragging_point_ && ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
        auto ref = current_primitive();
        if (ref.points && sel_point_idx_ >= 0 &&
            sel_point_idx_ < static_cast<int>(ref.points->size())) {
            (*ref.points)[static_cast<std::size_t>(sel_point_idx_)] = to_normalized(mx, my);
        }
    }
    if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
        dragging_point_ = false;
    }

    // Middle-drag: pan the canvas view.
    if (ImGui::IsMouseDragging(ImGuiMouseButton_Middle)) {
        const ImVec2 delta = ImGui::GetMouseDragDelta(ImGuiMouseButton_Middle);
        view_pan_x_ += delta.x;
        view_pan_y_ += delta.y;
        ImGui::ResetMouseDragDelta(ImGuiMouseButton_Middle);
    }

    // Wheel: zoom the canvas view (only when hovered, so we don't steal
    // wheel events from the rest of the viewer).
    if (canvas_hovered) {
        const float wheel = ImGui::GetIO().MouseWheel;
        if (wheel != 0.0f) {
            view_zoom_ *= (wheel > 0) ? 1.15f : (1.0f / 1.15f);
            view_zoom_ = std::clamp(view_zoom_, 0.25f, 8.0f);
        }
    }

    // --- Canvas header + footer hints ---
    // Re-position cursor ABOVE the canvas (where we already drew) to
    // show a small status line. We use a separate child region at the
    // top for the header so the canvas stays at the natural cursor pos.
    // For simplicity, we just render the status text after the canvas
    // (it'll appear below the InvisibleButton).
    ImGui::TextDisabled("click = add/select • drag = move • right-click = delete • middle-drag = pan • wheel = zoom");
    if (sel_prim_kind_ != SelectedPrimKind::None && sel_point_idx_ >= 0) {
        auto ref = current_primitive();
        if (ref.points && sel_point_idx_ < static_cast<int>(ref.points->size())) {
            const auto& p = (*ref.points)[static_cast<std::size_t>(sel_point_idx_)];
            ImGui::TextDisabled("selected point [%d]: (%.3f, %.3f)", sel_point_idx_, p.x, p.y);
        }
    }

    ImGui::EndChild();
}

// ===========================================================================
// Primitives list (right column)
// ===========================================================================

void SymbolCreator::draw_primitives_panel(float panel_h) {
    ImGui::BeginChild("##primitives_panel", ImVec2(240, panel_h), true);

    if (!current_symbol()) {
        ImGui::TextDisabled("No symbol selected.");
        ImGui::EndChild();
        return;
    }

    ImGui::TextDisabled("Primitives");
    ImGui::Separator();

    auto* s = current_symbol();

    // Polylines section
    ImGui::TextUnformatted("Polylines");
    for (std::size_t i = 0; i < s->polylines.size(); ++i) {
        const auto& pl = s->polylines[i];
        char label[128];
        std::snprintf(label, sizeof(label), "[%zu] width %.2f, %zu pts, %s",
                       i, pl.width, pl.points.size(),
                       pl.closed ? "closed" : "open");
        const bool selected = (sel_prim_kind_ == SelectedPrimKind::Polyline &&
                                sel_prim_idx_ == static_cast<int>(i));
        if (ImGui::Selectable(label, selected)) {
            sel_prim_kind_ = SelectedPrimKind::Polyline;
            sel_prim_idx_ = static_cast<int>(i);
            sel_point_idx_ = -1;
        }
        // Right-click on the row to delete the whole primitive.
        if (ImGui::BeginPopupContextItem(("ctx_pl_" + std::to_string(i)).c_str())) {
            if (ImGui::MenuItem("Delete Polyline")) {
                s->polylines.erase(s->polylines.begin() + static_cast<long>(i));
                if (sel_prim_kind_ == SelectedPrimKind::Polyline) {
                    if (sel_prim_idx_ == static_cast<int>(i)) {
                        sel_prim_kind_ = SelectedPrimKind::None;
                        sel_prim_idx_ = -1;
                        sel_point_idx_ = -1;
                    } else if (sel_prim_idx_ > static_cast<int>(i)) {
                        --sel_prim_idx_;
                    }
                }
            }
            ImGui::EndPopup();
        }
    }
    if (ImGui::Button("+ Polyline")) {
        s->polylines.push_back(f4::renderer::SymbolPolyline{});
        sel_prim_kind_ = SelectedPrimKind::Polyline;
        sel_prim_idx_ = static_cast<int>(s->polylines.size()) - 1;
        sel_point_idx_ = -1;
    }

    ImGui::Spacing();
    ImGui::TextUnformatted("Polygons");
    for (std::size_t i = 0; i < s->polygons.size(); ++i) {
        const auto& pg = s->polygons[i];
        char label[128];
        std::snprintf(label, sizeof(label), "[%zu] %s, %zu pts",
                       i, pg.filled ? "filled" : "outline",
                       pg.points.size());
        const bool selected = (sel_prim_kind_ == SelectedPrimKind::Polygon &&
                                sel_prim_idx_ == static_cast<int>(i));
        if (ImGui::Selectable(label, selected)) {
            sel_prim_kind_ = SelectedPrimKind::Polygon;
            sel_prim_idx_ = static_cast<int>(i);
            sel_point_idx_ = -1;
        }
        if (ImGui::BeginPopupContextItem(("ctx_pg_" + std::to_string(i)).c_str())) {
            if (ImGui::MenuItem("Delete Polygon")) {
                s->polygons.erase(s->polygons.begin() + static_cast<long>(i));
                if (sel_prim_kind_ == SelectedPrimKind::Polygon) {
                    if (sel_prim_idx_ == static_cast<int>(i)) {
                        sel_prim_kind_ = SelectedPrimKind::None;
                        sel_prim_idx_ = -1;
                        sel_point_idx_ = -1;
                    } else if (sel_prim_idx_ > static_cast<int>(i)) {
                        --sel_prim_idx_;
                    }
                }
            }
            ImGui::EndPopup();
        }
    }
    if (ImGui::Button("+ Polygon (filled)")) {
        f4::renderer::SymbolPolygon pg;
        pg.filled = true;
        s->polygons.push_back(std::move(pg));
        sel_prim_kind_ = SelectedPrimKind::Polygon;
        sel_prim_idx_ = static_cast<int>(s->polygons.size()) - 1;
        sel_point_idx_ = -1;
    }
    ImGui::SameLine();
    if (ImGui::Button("+ Polygon (outline)")) {
        f4::renderer::SymbolPolygon pg;
        pg.filled = false;
        s->polygons.push_back(std::move(pg));
        sel_prim_kind_ = SelectedPrimKind::Polygon;
        sel_prim_idx_ = static_cast<int>(s->polygons.size()) - 1;
        sel_point_idx_ = -1;
    }

    ImGui::Separator();
    if (ImGui::Button("Delete Primitive") &&
        sel_prim_kind_ != SelectedPrimKind::None &&
        sel_prim_idx_ >= 0) {
        if (sel_prim_kind_ == SelectedPrimKind::Polyline) {
            s->polylines.erase(s->polylines.begin() + sel_prim_idx_);
        } else {
            s->polygons.erase(s->polygons.begin() + sel_prim_idx_);
        }
        sel_prim_kind_ = SelectedPrimKind::None;
        sel_prim_idx_ = -1;
        sel_point_idx_ = -1;
    }

    // Selected primitive properties
    if (sel_prim_kind_ != SelectedPrimKind::None && sel_prim_idx_ >= 0) {
        ImGui::Separator();
        ImGui::TextDisabled("Properties");
        if (sel_prim_kind_ == SelectedPrimKind::Polyline) {
            auto& pl = s->polylines[static_cast<std::size_t>(sel_prim_idx_)];
            ImGui::PushItemWidth(-1);
            ImGui::TextUnformatted("Width:");
            ImGui::SliderFloat("##pl_width", &pl.width, 0.5f, 8.0f, "%.2f");
            ImGui::TextUnformatted("Closed:");
            ImGui::Checkbox("##pl_closed", &pl.closed);
            ImGui::PopItemWidth();
        } else {
            auto& pg = s->polygons[static_cast<std::size_t>(sel_prim_idx_)];
            ImGui::TextUnformatted("Filled:");
            ImGui::Checkbox("##pg_filled", &pg.filled);
        }

        // Selected point operations
        if (sel_point_idx_ >= 0) {
            ImGui::Separator();
            ImGui::TextDisabled("Point [%d]", sel_point_idx_);
            auto ref = current_primitive();
            if (ref.points && sel_point_idx_ < static_cast<int>(ref.points->size())) {
                auto& p = (*ref.points)[static_cast<std::size_t>(sel_point_idx_)];
                ImGui::PushItemWidth(-1);
                ImGui::SliderFloat("x##pt_x", &p.x, -1.5f, 1.5f, "%.3f");
                ImGui::SliderFloat("y##pt_y", &p.y, -1.5f, 1.5f, "%.3f");
                ImGui::PopItemWidth();
                if (ImGui::Button("Delete Point")) {
                    ref.points->erase(ref.points->begin() + sel_point_idx_);
                    sel_point_idx_ = -1;
                }
            }
        }
    }

    ImGui::EndChild();
}

// ===========================================================================
// Metadata editor + hints (bottom strip)
// ===========================================================================

void SymbolCreator::draw_metadata_panel() {
    ImGui::Separator();
    if (!current_symbol()) {
        ImGui::TextDisabled("No symbol selected — pick one from the left, or click 'New Symbol'.");
        return;
    }

    // 4-column row: key | display name | category | (description below)
    ImGui::PushItemWidth(180);
    if (ImGui::InputText("Key", key_buf_, sizeof(key_buf_))) {
        apply_metadata_buffers();
    }
    ImGui::PopItemWidth();
    ImGui::SameLine();
    ImGui::PushItemWidth(220);
    if (ImGui::InputText("Display Name", display_name_buf_, sizeof(display_name_buf_))) {
        apply_metadata_buffers();
    }
    ImGui::PopItemWidth();
    ImGui::SameLine();
    ImGui::PushItemWidth(160);
    if (ImGui::InputText("Category", category_buf_, sizeof(category_buf_))) {
        apply_metadata_buffers();
    }
    ImGui::PopItemWidth();

    ImGui::PushItemWidth(-1);
    if (ImGui::InputText("Description", description_buf_, sizeof(description_buf_))) {
        apply_metadata_buffers();
    }
    ImGui::PopItemWidth();

    // Status / error line
    if (!last_error_.empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.4f, 0.4f, 1.0f));
        ImGui::TextWrapped("Error: %s", last_error_.c_str());
        ImGui::PopStyleColor();
    } else if (!status_msg_.empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.5f, 0.9f, 0.6f, 1.0f));
        ImGui::TextUnformatted(status_msg_.c_str());
        ImGui::PopStyleColor();
    } else {
        ImGui::TextDisabled("click empty canvas to add point • drag point to move • right-click point to delete");
    }

    // Errors auto-clear after a few seconds; status messages until next action.
    // (We could use ImGui's time, but a manual reset on next action suffices.)
    // Clear the error after the user types in any field — the next action
    // may produce a new error.
    if (!last_error_.empty()) {
        // Don't auto-clear; let it stay until the next action.
    }
}

// ===========================================================================
// File dialogs
// ===========================================================================

void SymbolCreator::pick_load_library_dialog() {
    auto path = pick_open_file(
        "Load Symbol Library",
        "Symbol Library JSON (*.json)|JSON (*.json)|All files (*.*)",
        last_library_path_);
    if (path.empty()) return;
    try {
        auto loaded = f4::renderer::load_symbol_library(path);
        library_ = std::move(loaded);
        selected_symbol_idx_ = library_.empty() ? -1 : 0;
        sel_prim_kind_ = SelectedPrimKind::None;
        sel_prim_idx_ = -1;
        sel_point_idx_ = -1;
        sync_metadata_buffers();
        last_library_path_ = path;
        last_error_.clear();
        status_msg_ = "Loaded library: " + path.string();
    } catch (const std::exception& e) {
        last_error_ = e.what();
        status_msg_.clear();
    }
}

void SymbolCreator::pick_save_library_dialog() {
    auto path = pick_save_file(
        "Save Symbol Library",
        "Symbol Library JSON (*.json)|JSON (*.json)|All files (*.*)",
        last_library_path_);
    if (path.empty()) return;
    // Ensure .json extension if the user didn't type one.
    if (path.has_extension() == false) {
        path.replace_extension(".json");
    }
    try {
        f4::renderer::save_symbol_library(library_, path);
        last_library_path_ = path;
        last_error_.clear();
        status_msg_ = "Saved library: " + path.string();
    } catch (const std::exception& e) {
        last_error_ = e.what();
        status_msg_.clear();
    }
}

} // namespace f4::viewer
