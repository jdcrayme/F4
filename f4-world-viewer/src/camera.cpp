// f4-world-viewer/src/camera.cpp
//
// World <-> screen transforms + camera fit.
// These are ViewerApp::Impl member functions that touch only Impl's
// camera state (cam_x/cam_y/cam_zoom/window_w/window_h) and the
// entity data (for fit_to_selection_layout).
//
// Split out of the original 1920-LoC viewer_app.cpp god-file (item #5
// of the architecture review). No behavior change.
//
// World frame: a 1024×1024 grid where (0,0) is SW corner, (1024,1024)
// is NE corner. Y increases northward (screen up), X increases eastward
// (screen right). Screen frame: pixels from top-left, so we flip Y.

#include "viewer_state.hpp"

#include <algorithm>

namespace f4::viewer {

Vector2 ViewerApp::Impl::world_to_screen(float gx, float gy) const {
    const float cx = window_w * 0.5f;
    const float cy = window_h * 0.5f;
    return {
        cx + (gx - cam_x) * cam_zoom,
        cy - (gy - cam_y) * cam_zoom   // flip y: world y=north = screen up
    };
}

void ViewerApp::Impl::screen_to_world(float sx, float sy, float* gx, float* gy) const {
    const float cx = window_w * 0.5f;
    const float cy = window_h * 0.5f;
    *gx = cam_x + (sx - cx) / cam_zoom;
    *gy = cam_y - (sy - cy) / cam_zoom;   // un-flip y
}

void ViewerApp::Impl::fit_to_world() {
    const float grid_size = 1024.0f;
    cam_x = grid_size * 0.5f;
    cam_y = grid_size * 0.5f;
    const float zoom_x = static_cast<float>(window_w) / grid_size;
    const float zoom_y = static_cast<float>(window_h) / grid_size;
    cam_zoom = std::min(zoom_x, zoom_y) * 0.95f;
}

void ViewerApp::Impl::fit_to_selection_layout() {
    // Requires a selected objective with ground_layout or features.
    if (sel_kind != SelectionKind::Objective || !sel_entity.valid()) return;
    auto h = handle(sel_entity);
    auto* tr = h.get<f4::entities::TransformComponent>();
    auto* gl = h.get<f4::entities::GroundLayoutComponent>();
    auto* fs = h.get<f4::entities::FeatureSetComponent>();
    if (!tr) return;
    const bool has_layout = gl && !gl->layouts.empty();
    const bool has_features = fs && !fs->features.empty();
    if (!has_layout && !has_features) return;

    // Compute the bbox in FEET relative to objective center.
    float min_x = 1e30f, min_y = 1e30f, max_x = -1e30f, max_y = -1e30f;
    bool any = false;
    if (gl) {
        for (const auto& layout : gl->layouts) {
            for (const auto& pt : layout.points) {
                min_x = std::min(min_x, pt.x);  min_y = std::min(min_y, pt.y);
                max_x = std::max(max_x, pt.x);  max_y = std::max(max_y, pt.y);
                any = true;
            }
        }
    }
    if (fs) {
        for (const auto& f : fs->features) {
            if (f.index == 0 && f.offset_x == 0.0f && f.offset_y == 0.0f) continue;
            min_x = std::min(min_x, f.offset_x);  min_y = std::min(min_y, f.offset_y);
            max_x = std::max(max_x, f.offset_x);  max_y = std::max(max_y, f.offset_y);
            any = true;
        }
    }
    if (!any) return;

    // Add a 10% margin so points don't sit on the screen edge.
    const float margin_x = (max_x - min_x) * 0.10f + 100.0f;
    const float margin_y = (max_y - min_y) * 0.10f + 100.0f;
    min_x -= margin_x;  min_y -= margin_y;
    max_x += margin_x;  max_y += margin_y;
    const float w_ft = max_x - min_x;
    const float h_ft = max_y - min_y;

    // Convert feet → grid units (1 grid unit = 1024 ft) and center
    // the camera on the objective. The bbox is relative to the
    // objective center, so the world-space center of the bbox is
    // (obj.x + bbox_center_x / 1024, obj.y + bbox_center_y / 1024).
    constexpr float FT_PER_GRID = 1024.0f;
    const float bbox_cx_ft = (min_x + max_x) * 0.5f;
    const float bbox_cy_ft = (min_y + max_y) * 0.5f;
    const float obj_gx = grid_x(tr);
    const float obj_gy = grid_y(tr);
    cam_x = obj_gx + bbox_cx_ft / FT_PER_GRID;
    cam_y = obj_gy + bbox_cy_ft / FT_PER_GRID;
    // Zoom: fit the bbox into the window with the margin. Use the
    // larger of the two zoom values to ensure the whole bbox fits.
    const float w_grid = w_ft / FT_PER_GRID;
    const float h_grid = h_ft / FT_PER_GRID;
    const float zoom_x = (w_grid > 0.0f) ? static_cast<float>(window_w)  / w_grid : 100.0f;
    const float zoom_y = (h_grid > 0.0f) ? static_cast<float>(window_h)  / h_grid : 100.0f;
    cam_zoom = std::min(zoom_x, zoom_y) * 0.95f;
    cam_zoom = std::clamp(cam_zoom, 0.05f, 2000.0f);
}

} // namespace f4::viewer
