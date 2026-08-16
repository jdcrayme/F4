// f4-models-viewer/src/canvas3d.cpp
//
// 3D canvas rendering. Draws the current model's meshes, the reference
// grid, coordinate axes, and bounding volume overlays inside a
// BeginMode3D / EndMode3D block.
//
// Lighting: when lighting_enabled is true, a custom shader-based lit
// material is used. FreeFalcon's models were authored for a single
// directional light (the sun), and their vertex normals only make
// sense under that assumption. The lighting panel exposes direction,
// intensity, ambient, and sun color.

#include "viewer_state.hpp"

#include <f4/renderer/draw_3d.hpp>
#include <f4/math/vec3.hpp>

#include <raylib.h>
#include <raymath.h>
#include <rlgl.h>

#include <algorithm>
#include <cstdio>
#include <cmath>
#include <cstring>
#include <string>
#include <vector>

namespace f4::models_viewer {

// ── Colors ────────────────────────────────────────────────────────────────
static constexpr Color GRID_COLOR     = {60, 60, 60, 255};
static constexpr Color AXIS_X_COLOR   = {220, 60, 60, 255};   // Red = X
static constexpr Color AXIS_Y_COLOR   = {60, 220, 60, 255};   // Green = Y (up)
static constexpr Color AXIS_Z_COLOR   = {60, 60, 220, 255};   // Blue = Z
static constexpr Color BSphere_COLOR  = {255, 200, 60, 80};    // bounding sphere
static constexpr Color AABB_COLOR     = {60, 200, 255, 80};    // AABB
static constexpr Color LIGHT_GIZMO_COLOR = {255, 230, 120, 255};

// ── draw_grid / draw_axes ────────────────────────────────────────────────
// Now provided by f4::renderer::draw_grid() and f4::renderer::draw_axes().
// The local implementations have been removed.

// ── draw_bounding_volumes ──────────────────────────────────────────────────
// Draw bounding sphere and/or AABB for the selected model.
void ViewerApp::Impl::draw_bounding_volumes() {
    if (selected_parent < 0 || !doc_loaded) return;

    const auto* rec = db.model(selected_parent);
    if (!rec) return;

    if (show_bounding_sphere && rec->radius > 0) {
        const Vector3 center = to_raylib(rec->bbox.center_x(),
                                         rec->bbox.center_y(),
                                         rec->bbox.center_z());
        DrawSphereWires(center, rec->radius, 16, 16, BSphere_COLOR);
    }

    if (show_aabb) {
        // Convert AABB corners from LH Y-up to RH Y-up
        const Vector3 bmin = to_raylib(rec->bbox.min_x,
                                       rec->bbox.min_y,
                                       rec->bbox.min_z);
        const Vector3 bmax = to_raylib(rec->bbox.max_x,
                                       rec->bbox.max_y,
                                       rec->bbox.max_z);
        const Vector3 center = {
            (bmin.x + bmax.x) * 0.5f,
            (bmin.y + bmax.y) * 0.5f,
            (bmin.z + bmax.z) * 0.5f
        };
        const Vector3 size = {
            bmax.x - bmin.x,
            bmax.y - bmin.y,
            bmax.z - bmin.z
        };
        DrawCubeWires(center, size.x, size.y, size.z, AABB_COLOR);
    }
}

// ── Lit shader ────────────────────────────────────────────────────────────
// Now provided by f4::renderer::LitShader. The duplicated GLSL source
// strings and ensure_lit_shader() function have been removed.

// ── draw_canvas ────────────────────────────────────────────────────────────
void ViewerApp::Impl::draw_canvas() {
    // Reset per-frame stats
    stats_draw_calls = 0;
    stats_meshes_drawn = 0;
    stats_vertices_drawn = 0;

    // Rebuild meshes if dirty
    if (meshes_dirty) {
        rebuild_meshes();
    }

    BeginMode3D(orbit_cam.camera());

    // Grid
    if (show_grid) {
        f4::renderer::draw_grid(500.0f, 10.0f);
    }

    // Axes
    if (show_axes) {
        f4::renderer::draw_axes(50.0f);
    }

    // Lighting setup via LitShader RAII wrapper
    if (lighting_enabled && lit_shader.ensure(&status_msg)) {
        lit_shader.set_lighting(light_direction, light_color, light_intensity, ambient_color);
    }

    // Draw all meshes using f4::renderer::draw_meshes() which handles:
    // - opaque-before-alpha sort
    // - backface culling disable (FreeFalcon winding convention)
    // - lit shader application
    // - alpha blend mode
    f4::renderer::DrawStats draw_stats;
    f4::renderer::draw_meshes(mesh_entries, texture_cache, lit_shader,
                              lighting_enabled, show_wireframe, &draw_stats);
    stats_draw_calls    = draw_stats.draw_calls;
    stats_meshes_drawn  = draw_stats.meshes_drawn;
    stats_vertices_drawn = draw_stats.vertices_drawn;

    // Draw line primitives (LineF) — Raylib's DrawMesh only handles triangle
    // lists, so we draw each line segment via DrawLine3D.
    for (const auto& seg : line_segs) {
        DrawLine3D(seg.a, seg.b, seg.color);
    }

    // Draw point primitives (PointF) as small cubes for visibility.
    for (const auto& pm : point_marks) {
        DrawCube(pm.p, pm.size, pm.size, pm.size, pm.color);
    }

    // Bounding volumes
    draw_bounding_volumes();

    // Light direction gizmo (drawn in 3D so it orbits with the camera)
    if (show_light_gizmo && lighting_enabled) {
        draw_light_gizmo();
    }

    EndMode3D();

    // 2D overlay (stats)
    if (show_stats_overlay) {
        draw_stats_overlay();
    }

    // Individual texture materials in texture_cache are cleaned up in unload_meshes().
}

// ── draw_light_gizmo ───────────────────────────────────────────────────────
// Draws a small sun + arrow in 3D space indicating the lighting direction.
// Positioned at the camera target so it stays in view as the user orbits.
void ViewerApp::Impl::draw_light_gizmo() {
    Vector3 dir = light_direction;
    {
        const f4::math::Vec3f d{dir.x, dir.y, dir.z};
        if (d.length() < 0.0001f) return;
        const f4::math::Vec3f n = d.normalized();
        dir = {n.x, n.y, n.z};
    }

    // Place the gizmo 30 world units from the camera target, in the
    // direction TOWARD the light (so the arrow points from sun → scene).
    const float offset = 30.0f;
    const Vector3 cam_tgt = orbit_cam.target();
    const Vector3 sun_pos = {
        cam_tgt.x - dir.x * offset,
        cam_tgt.y - dir.y * offset,
        cam_tgt.z - dir.z * offset
    };
    const Vector3 arrow_tip = {
        cam_tgt.x - dir.x * (offset - 8.0f),
        cam_tgt.y - dir.y * (offset - 8.0f),
        cam_tgt.z - dir.z * (offset - 8.0f)
    };

    DrawSphere(sun_pos, 3.0f, LIGHT_GIZMO_COLOR);
    DrawLine3D(sun_pos, arrow_tip, LIGHT_GIZMO_COLOR);
    DrawCylinderEx(arrow_tip,
                   { arrow_tip.x + dir.x * 2.0f,
                     arrow_tip.y + dir.y * 2.0f,
                     arrow_tip.z + dir.z * 2.0f },
                   0.0f, 1.5f, 6, LIGHT_GIZMO_COLOR);
}

// ── draw_stats_overlay ──────────────────────────────────────────────────────
// 2D overlay (drawn after EndMode3D) showing per-frame stats.
void ViewerApp::Impl::draw_stats_overlay() {
    const int x = 12;
    int y = 12;
    const int line_h = 18;
    const int pad = 8;

    char buf[256];

    // Build the stats text as a vector of lines (avoids the need to
    // split on \n later, and lets us measure each line's width for the
    // background rect).
    std::vector<std::string> lines;
    if (doc_loaded) {
        std::snprintf(buf, sizeof(buf), "FPS: %d", GetFPS());
        lines.emplace_back(buf);
        std::snprintf(buf, sizeof(buf), "Draws: %d", stats_draw_calls);
        lines.emplace_back(buf);
        std::snprintf(buf, sizeof(buf), "Meshes: %d", stats_meshes_drawn);
        lines.emplace_back(buf);
        std::snprintf(buf, sizeof(buf), "Verts: %zu", stats_vertices_drawn);
        lines.emplace_back(buf);
        std::snprintf(buf, sizeof(buf), "Tris: %zu", total_tri_count);
        lines.emplace_back(buf);
        if (selected_parent >= 0) {
            const auto* rec = db.model(selected_parent);
            if (rec) {
                std::snprintf(buf, sizeof(buf), "Model: %d (%s)",
                              selected_parent, rec->visual_class().data());
                lines.emplace_back(buf);
                std::snprintf(buf, sizeof(buf), "LOD: %d", selected_lod);
                lines.emplace_back(buf);
                std::snprintf(buf, sizeof(buf), "DOFs: %d  Sw: %d  Slots: %d",
                              rec->effective_dofs(),
                              rec->effective_switches(),
                              static_cast<int>(rec->n_slots));
                lines.emplace_back(buf);
            }
        }
    } else {
        lines.emplace_back("No model loaded.");
    }

    // Compute the rect width as the max line width.
    int max_w = 0;
    for (const auto& line : lines) {
        const int w = MeasureText(line.c_str(), 14);
        if (w > max_w) max_w = w;
    }
    const int bg_h = static_cast<int>(lines.size()) * line_h + pad * 2;
    const int bg_w = max_w + pad * 2;

    DrawRectangle(x, y, bg_w, bg_h, { 0, 0, 0, 160 });
    DrawRectangleLines(x, y, bg_w, bg_h, { 255, 255, 255, 80 });

    // Draw each line
    int line_y = y + pad;
    for (const auto& line : lines) {
        DrawText(line.c_str(), x + pad, line_y, 14, RAYWHITE);
        line_y += line_h;
    }
}

} // namespace f4::models_viewer
