// f4-models-viewer/src/canvas3d.cpp
//
// 3D canvas rendering. Draws the current model's meshes, the reference
// grid, coordinate axes, and bounding volume overlays inside a
// BeginMode3D / EndMode3D block.

#include "viewer_state.hpp"
#include "canvas3d.hpp"

#include <raylib.h>
#include <raymath.h>
#include <rlgl.h>

#include <cmath>

namespace f4::models_viewer {

// ── Colors ────────────────────────────────────────────────────────────────
static constexpr Color GRID_COLOR     = {60, 60, 60, 255};
static constexpr Color AXIS_X_COLOR   = {220, 60, 60, 255};   // Red = X
static constexpr Color AXIS_Y_COLOR   = {60, 220, 60, 255};   // Green = Y (up)
static constexpr Color AXIS_Z_COLOR   = {60, 60, 220, 255};   // Blue = Z
static constexpr Color BSphere_COLOR  = {255, 200, 60, 80};    // bounding sphere
static constexpr Color AABB_COLOR     = {60, 200, 255, 80};    // AABB

// ── draw_grid ──────────────────────────────────────────────────────────────
// Draw a subtle XZ-plane grid centered at the origin.
static void draw_grid(float extent, float step) {
    for (float i = -extent; i <= extent; i += step) {
        // Lines parallel to Z
        DrawLine3D({i, 0, -extent}, {i, 0, extent}, GRID_COLOR);
        // Lines parallel to X
        DrawLine3D({-extent, 0, i}, {extent, 0, i}, GRID_COLOR);
    }
}

// ── draw_axes ──────────────────────────────────────────────────────────────
// Draw RGB coordinate axes at the origin.
static void draw_axes(float length) {
    DrawLine3D({0, 0, 0}, {length, 0, 0}, AXIS_X_COLOR);  // X = Red
    DrawLine3D({0, 0, 0}, {0, length, 0}, AXIS_Y_COLOR);  // Y = Green
    DrawLine3D({0, 0, 0}, {0, 0, length}, AXIS_Z_COLOR);  // Z = Blue
}

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

// ── draw_canvas ────────────────────────────────────────────────────────────
void ViewerApp::Impl::draw_canvas() {
    // Rebuild meshes if dirty
    if (meshes_dirty) {
        rebuild_meshes();
    }

    BeginMode3D(camera);

    // Grid
    if (show_grid) {
        draw_grid(500.0f, 10.0f);
    }

    // Axes
    if (show_axes) {
        draw_axes(50.0f);
    }

    // Draw meshes with the default material. Raylib's default material is
    // white diffuse + vertex-color support — vertex colors set in scene.cpp
    // are multiplied by the material's diffuse color, so the result is the
    // vertex color (resolved through ColorBank) modulated by white. This
    // makes flat-shaded (untextured) meshes visible with their actual
    // ColorBank colors, and textured meshes appear white pending texture
    // binding (Phase V2 work).
    const Matrix identity = MatrixIdentity();
    Material mat = LoadMaterialDefault();

    // Tint the material diffuse white so vertex colors pass through unchanged.
    // (The default material's diffuse is already white, but be explicit so
    // future changes to LoadMaterialDefault() don't silently darken meshes.)
    mat.maps[MATERIAL_MAP_DIFFUSE].color = WHITE;

    // CRITICAL: Disable backface culling. FreeFalcon's models were designed
    // to render WITHOUT backface culling — many polygons have CCW winding
    // (opposite to the plane normal) and would be invisible if culled.
    // The diagnostic showed 7.7% of triangles are back-facing; without
    // this disable, those triangles (and the surfaces they belong to)
    // would appear as holes in the model.
    rlDisableBackfaceCulling();

    if (show_wireframe) {
        rlEnableWireMode();
    }

    for (const auto& mesh : raylib_meshes) {
        // Only call DrawMesh if the mesh actually has triangles. Meshes with
        // only lines/points (LineF/PointF primitives emitted by far LODs)
        // have triangleCount == 0 and are drawn separately below.
        if (mesh.triangleCount > 0) {
            DrawMesh(mesh, mat, identity);
        }
    }

    if (show_wireframe) {
        rlDisableWireMode();
    }

    // Re-enable backface culling for subsequent draws (grid, axes, etc.
    // don't need it, but it's good practice to restore default state).
    rlEnableBackfaceCulling();

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

    EndMode3D();

    // UnloadMaterial on the default material would leak; LoadMaterialDefault()
    // returns a shared singleton that must NOT be UnloadMaterial'd. Leave it.
}

} // namespace f4::models_viewer
