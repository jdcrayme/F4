// f4-renderer/src/draw_3d.cpp
//
// 3D drawing helpers implementation.

#include <f4/renderer/draw_3d.hpp>

#include <raylib.h>
#include <raymath.h>
#include <rlgl.h>

#include <vector>

namespace f4::renderer {

// ── Colors ────────────────────────────────────────────────────────────────────
static constexpr Color GRID_COLOR   = {60, 60, 60, 255};
static constexpr Color AXIS_X_COLOR = {220, 60, 60, 255};   // Red = X
static constexpr Color AXIS_Y_COLOR = {60, 220, 60, 255};   // Green = Y (up)
static constexpr Color AXIS_Z_COLOR = {60, 60, 220, 255};   // Blue = Z

// ── draw_grid ─────────────────────────────────────────────────────────────────

void draw_grid(float extent, float step) {
    for (float i = -extent; i <= extent; i += step) {
        DrawLine3D({i, 0, -extent}, {i, 0, extent}, GRID_COLOR);
        DrawLine3D({-extent, 0, i}, {extent, 0, i}, GRID_COLOR);
    }
}

// ── draw_axes ─────────────────────────────────────────────────────────────────

void draw_axes(float length) {
    DrawLine3D({0, 0, 0}, {length, 0, 0}, AXIS_X_COLOR);  // X = Red
    DrawLine3D({0, 0, 0}, {0, length, 0}, AXIS_Y_COLOR);  // Y = Green
    DrawLine3D({0, 0, 0}, {0, 0, length}, AXIS_Z_COLOR);  // Z = Blue
}

// ── draw_single_mesh ──────────────────────────────────────────────────────────

void draw_single_mesh(const ::Mesh& mesh, const ::Material& material,
                       const ::Matrix& transform) {
    rlDisableBackfaceCulling();
    BeginBlendMode(BLEND_ALPHA);
    DrawMesh(mesh, material, transform);
    EndBlendMode();
    rlEnableBackfaceCulling();
}

// ── draw_meshes ───────────────────────────────────────────────────────────────

void draw_meshes(
    const std::vector<MeshEntry>& entries,
    const TextureCache& tex_cache,
    const LitShader& lit_shader,
    bool lighting_enabled,
    bool wireframe,
    DrawStats* stats)
{
    const Matrix identity = MatrixIdentity();
    Material default_mat = LoadMaterialDefault();
    default_mat.maps[MATERIAL_MAP_DIFFUSE].color = WHITE;

    // Apply lit shader to default material if lighting is active
    bool lighting_active = false;
    if (lighting_enabled && lit_shader.is_loaded()) {
        default_mat.shader = lit_shader.shader();
        lighting_active = true;
    }

    // CRITICAL: Disable backface culling. FreeFalcon's models were designed
    // to render WITHOUT backface culling — many polygons have CCW winding
    // (opposite to the plane normal) and would be invisible if culled.
    rlDisableBackfaceCulling();

    if (wireframe) {
        rlEnableWireMode();
    }

    // Sort into opaque and alpha order
    std::vector<std::size_t> opaque_order;
    std::vector<std::size_t> alpha_order;
    opaque_order.reserve(entries.size());
    alpha_order.reserve(entries.size());
    for (std::size_t i = 0; i < entries.size(); ++i) {
        if (tex_cache.has_alpha(entries[i].tex_id)) {
            alpha_order.push_back(i);
        } else {
            opaque_order.push_back(i);
        }
    }

    BeginBlendMode(BLEND_ALPHA);

    auto draw_entry = [&](std::size_t idx) {
        const auto& entry = entries[idx];
        const auto& mesh = entry.mesh;
        if (mesh.triangleCount <= 0) return;

        const Material* mat_to_use = &default_mat;
        Material lit_mat = {};
        if (entry.tex_id >= 0) {
            auto* ce = tex_cache.lookup(entry.tex_id);
            if (ce && ce->uploaded) {
                mat_to_use = &ce->material;
                if (lighting_active) {
                    lit_mat = ce->material;
                    lit_mat.shader = lit_shader.shader();
                    mat_to_use = &lit_mat;
                }
            }
        }
        DrawMesh(mesh, *mat_to_use, identity);
        if (stats) {
            ++stats->draw_calls;
            ++stats->meshes_drawn;
            stats->vertices_drawn += static_cast<std::size_t>(mesh.vertexCount);
        }
    };

    // Opaque first, alpha last
    for (auto idx : opaque_order) draw_entry(idx);
    for (auto idx : alpha_order) draw_entry(idx);

    EndBlendMode();

    if (wireframe) {
        rlDisableWireMode();
    }

    rlEnableBackfaceCulling();
}

} // namespace f4::renderer
