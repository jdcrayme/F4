// f4-world-viewer/src/camera.cpp
//
// World <-> screen transforms + camera fit + objective-index rebuild.
// These are ViewerApp::Impl member functions that touch only Impl's
// camera state (cam_x/cam_y/cam_zoom/window_w/window_h) and the
// world data (for rebuild_objective_index).
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

void ViewerApp::Impl::rebuild_objective_index() {
    obj_id_to_index.clear();
    obj_id_to_index.reserve(world.objectives.size());
    for (int i = 0; i < static_cast<int>(world.objectives.size()); ++i) {
        obj_id_to_index[world.objectives[i].id_num] = i;
    }
}

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

} // namespace f4::viewer
