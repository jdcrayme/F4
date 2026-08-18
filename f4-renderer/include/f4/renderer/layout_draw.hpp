// f4-renderer/include/f4/renderer/layout_draw.hpp
//
// Shared Raylib draw primitives for AirfieldGeometry3D (see
// ground_layout_models.hpp). Both f4-world-viewer (ground-layout panel)
// and f4-scenario-player (real-airbase rendering) consume these — the
// drawing logic lives here once instead of per-app.
//
// All functions take an ENU origin offset (feet): the geometry from
// build_airfield_geometry_3d() is objective-local, and the offset places
// it anywhere in the world (e.g. the objective's ENU center). Internally
// each point is converted with enu_to_raylib(x + ox, y + oy, z + oz).
//
// Requires an active 3D mode (BeginMode3D) for the draw calls; the label
// helpers run in 2D (after EndMode3D).
//
// Dependencies: Raylib, f4-renderer (coord_transform, ground_layout_models).
// C++20.

#pragma once

#include <f4/renderer/coord_transform.hpp>
#include <f4/renderer/ground_layout_models.hpp>

#include <raylib.h>

#include <algorithm>
#include <cstdio>
#include <vector>

namespace f4::renderer {

namespace detail {
// enu_to_raylib returns f4::math::Vec3f; Raylib draw calls want Vector3.
inline Vector3 to_v3(const math::Vec3f& v) noexcept {
    return Vector3{v.x, v.y, v.z};
}
} // namespace detail

// ---------------------------------------------------------------------------
// 3D draw primitives (call inside BeginMode3D)
// ---------------------------------------------------------------------------

/// Flat ground quad (runway surface, taxiway strip, threshold bar,
/// footprint). Two triangles; winding is irrelevant under disabled
/// backface culling (callers disable it for FreeFalcon models anyway).
inline void draw_layout_quad(const LayoutQuad& q,
                             float ox = 0.0f, float oy = 0.0f,
                             float oz = 0.0f) {
    const Vector3 v0 = detail::to_v3(enu_to_raylib(q.x[0] + ox, q.y[0] + oy, q.z + oz));
    const Vector3 v1 = detail::to_v3(enu_to_raylib(q.x[1] + ox, q.y[1] + oy, q.z + oz));
    const Vector3 v2 = detail::to_v3(enu_to_raylib(q.x[2] + ox, q.y[2] + oy, q.z + oz));
    const Vector3 v3 = detail::to_v3(enu_to_raylib(q.x[3] + ox, q.y[3] + oy, q.z + oz));
    const Color c = {q.r, q.g, q.b, q.a};
    DrawTriangle3D(v0, v1, v2, c);
    DrawTriangle3D(v0, v2, v3, c);
}

/// Line segment (taxiway centerline, dim marks).
inline void draw_layout_line(const LayoutLine& l,
                             float ox = 0.0f, float oy = 0.0f,
                             float oz = 0.0f) {
    const Vector3 a = detail::to_v3(enu_to_raylib(l.x0 + ox, l.y0 + oy, l.z + oz));
    const Vector3 b = detail::to_v3(enu_to_raylib(l.x1 + ox, l.y1 + oy, l.z + oz));
    const Color c = {l.r, l.g, l.b, l.a};
    DrawLine3D(a, b, c);
}

/// Labeled marker: cube (parking/runway end), cylinder (helipad), or
/// cone (placement points).
inline void draw_layout_marker(const LayoutMarker& m,
                               float ox = 0.0f, float oy = 0.0f,
                               float oz = 0.0f) {
    const Vector3 center = detail::to_v3(enu_to_raylib(m.x + ox, m.y + oy, m.z + oz));
    const Color c = {m.r, m.g, m.b, m.a};
    const float s = m.size_ft;
    if (m.shape == 1) {
        // Cylinder — helipad.
        const Vector3 top = {center.x, center.y + s * 0.5f, center.z};
        const Vector3 bot = {center.x, center.y - s * 0.05f, center.z};
        DrawCylinderEx(bot, top, s, s, 16, c);
        DrawCylinderWiresEx(bot, top, s, s, 16, Color{20, 20, 20, 220});
    } else if (m.shape == 2) {
        // Cone — placement marker (SAM, AAA, etc.).
        const Vector3 tip = {center.x, center.y + s * 1.5f, center.z};
        const Vector3 bot = center;
        DrawCylinderEx(bot, tip, s, 0.0f, 12, c);
    } else {
        // Cube — parking spot / runway end.
        const Vector3 size = {s * 2, s * 2, s * 2};
        DrawCubeV(center, size, c);
        DrawCubeWiresV(center, size, Color{20, 20, 20, 220});
    }
}

// ---------------------------------------------------------------------------
// Labels (project markers to screen; draw in the 2D overlay pass)
// ---------------------------------------------------------------------------

/// One projected label (fill via collect_layout_labels, draw via
/// draw_layout_labels after EndMode3D).
struct LayoutLabel2D {
    int x = 0, y = 0;        // screen pixels
    bool visible = false;
    char text[64] = {};
    Color color{255, 255, 255, 220};
};

/// Project the labeled markers of `g` to screen space using `cam`.
/// `viewport_w/h` gate visibility. Set `show_parking` false to skip the
/// (usually numerous) parking labels.
inline void collect_layout_labels(const Camera3D& cam,
                                  const AirfieldGeometry3D& g,
                                  bool show_parking,
                                  int viewport_w, int viewport_h,
                                  float ox, float oy, float oz,
                                  std::vector<LayoutLabel2D>& out) {
    const auto add = [&](const LayoutMarker& m) {
        LayoutLabel2D lbl;
        const Vector3 rl = detail::to_v3(enu_to_raylib(m.x + ox, m.y + oy,
                                         m.z + oz + m.size_ft * 1.5f));
        const Vector2 s = GetWorldToScreen(rl, cam);
        lbl.x = static_cast<int>(s.x);
        lbl.y = static_cast<int>(s.y);
        lbl.visible = (s.x >= 0 && s.x < viewport_w &&
                       s.y >= 0 && s.y < viewport_h);
        std::snprintf(lbl.text, sizeof(lbl.text), "%s", m.label.c_str());
        out.push_back(lbl);
    };
    if (show_parking) {
        for (const auto& m : g.parking_spots) add(m);
    }
    for (const auto& m : g.helipads) add(m);
    for (const auto& m : g.runway_ends) add(m);
}

/// Draw collected labels (2D pass). Skips invisible entries.
inline void draw_layout_labels(const std::vector<LayoutLabel2D>& labels) {
    for (const auto& lbl : labels) {
        if (!lbl.visible) continue;
        DrawText(lbl.text, lbl.x + 1, lbl.y + 1, 12, Color{0, 0, 0, 220});
        DrawText(lbl.text, lbl.x, lbl.y, 12, lbl.color);
    }
}

} // namespace f4::renderer
