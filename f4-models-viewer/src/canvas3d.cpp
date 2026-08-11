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
#include "canvas3d.hpp"

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

// ── Lit shader (Lambertian diffuse + ambient) ──────────────────────────────
// Loaded lazily on first draw_canvas() call when lighting_enabled is true.
// Implements basic per-pixel directional lighting using the mesh's vertex
// normals. Falls back gracefully to Raylib's default unlit shader on
// compilation failure (id == 0).
//
// Vertex shader: standard Raylib vertex pipeline + pass fragNormal in
// world space.
//
// We use `vertexNormal` directly (no mat3(modelView) transform) because:
//   1. Raylib's DrawMesh() does NOT upload a `modelView` uniform (there
//      is no SHADER_LOC_MATRIX_MODELVIEW in rlgl.h). The uniform would
//      default to a zero matrix, zeroing all normals and making the
//      directional light component disappear — leaving only ambient.
//   2. The viewer calls DrawMesh with an identity model matrix, so
//      model-space normals are already in world space. The `lightDir`
//      uniform is uploaded in world space, so dot(N, L) is correct.
//
// Fragment shader: tex * colDiffuse * fragColor * (ambient + lightColor * NdotL)

static const char* kLitShaderVS =
    "#version 330\n"
    "in vec3 vertexPosition;\n"
    "in vec2 vertexTexCoord;\n"
    "in vec4 vertexColor;\n"
    "in vec3 vertexNormal;\n"
    "uniform mat4 mvp;\n"
    "out vec2 fragTexCoord;\n"
    "out vec4 fragColor;\n"
    "out vec3 fragNormal;\n"
    "void main() {\n"
    "    fragTexCoord = vertexTexCoord;\n"
    "    fragColor = vertexColor;\n"
    "    fragNormal = normalize(vertexNormal);\n"
    "    gl_Position = mvp * vec4(vertexPosition, 1.0);\n"
    "}\n";

static const char* kLitShaderFS =
    "#version 330\n"
    "in vec2 fragTexCoord;\n"
    "in vec4 fragColor;\n"
    "in vec3 fragNormal;\n"
    "uniform sampler2D texture0;\n"
    "uniform vec4 colDiffuse;\n"
    "uniform vec3 lightDir;\n"
    "uniform vec4 lightColor;\n"
    "uniform vec4 ambient;\n"
    "out vec4 finalColor;\n"
    "void main() {\n"
    "    vec4 tex = texture(texture0, fragTexCoord);\n"
    "    // Chroma-key transparency: FreeFalcon's .TEX textures mark\n"
    "    // transparent pixels with alpha=0 (set in tex_reader.cpp when\n"
    "    // the palette-resolved color matches the TexBankEntry chroma key).\n"
    "    // discard prevents both color AND depth writes, so transparent\n"
    "    // pixels don't occlude geometry behind them.\n"
    "    if (tex.a < 0.5) discard;\n"
    "    vec3 N = normalize(fragNormal);\n"
    "    vec3 L = normalize(lightDir);\n"
    "    float NdotL = max(dot(N, L), 0.0);\n"
    "    vec4 light = ambient + lightColor * NdotL;\n"
    "    finalColor = tex * colDiffuse * fragColor * light;\n"
    "}\n";

/// Lazily load the lit shader if not already loaded. Returns true if the
/// shader is available (id != 0).
static bool ensure_lit_shader(ViewerApp::Impl& impl) {
    if (impl.lit_shader_loaded) {
        return impl.lit_shader.id != 0;
    }
    impl.lit_shader = LoadShaderFromMemory(kLitShaderVS, kLitShaderFS);
    impl.lit_shader_loaded = true;
    if (impl.lit_shader.id == 0) {
        // Shader compile failed — log to status_msg once
        impl.status_msg = "warning: lit shader failed to compile (falling back to unlit)";
        return false;
    }
    impl.lit_shader_dir_loc     = GetShaderLocation(impl.lit_shader, "lightDir");
    impl.lit_shader_color_loc   = GetShaderLocation(impl.lit_shader, "lightColor");
    impl.lit_shader_ambient_loc = GetShaderLocation(impl.lit_shader, "ambient");
    return true;
}

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

    BeginMode3D(camera);

    // Grid
    if (show_grid) {
        draw_grid(500.0f, 10.0f);
    }

    // Axes
    if (show_axes) {
        draw_axes(50.0f);
    }

    // Draw meshes with per-mesh materials. Textured meshes use the material
    // from the texture cache (which has the decoded TEX bound as diffuse).
    // Untextured meshes (tex_id < 0) use the default white material so
    // vertex colors (resolved through ColorBank) pass through unchanged.
    const Matrix identity = MatrixIdentity();
    Material default_mat = LoadMaterialDefault();
    default_mat.maps[MATERIAL_MAP_DIFFUSE].color = WHITE;

    // Lighting setup: if enabled, swap in the lit shader on both the
    // default material and any textured material that uses the default
    // shader. Set the light uniforms once per frame (cheap).
    bool lighting_active = false;
    Vector3 light_dir = light_direction;
    const float dlen = std::sqrt(light_dir.x*light_dir.x +
                                  light_dir.y*light_dir.y +
                                  light_dir.z*light_dir.z);
    if (dlen > 0.0001f) {
        light_dir.x /= dlen; light_dir.y /= dlen; light_dir.z /= dlen;
    } else {
        light_dir = {0.5f, -1.0f, 0.3f};
    }

    if (lighting_enabled && ensure_lit_shader(*this)) {
        default_mat.shader = lit_shader;
        lighting_active = true;

        if (lit_shader_dir_loc >= 0) {
            // lightDir is the direction FROM the surface TO the light
            // (i.e., the direction a surface should face to be fully lit).
            // Our convention: light_direction points from scene toward
            // the sun, which is exactly this. Pass as-is.
            const float dir[3] = { light_dir.x, light_dir.y, light_dir.z };
            SetShaderValue(lit_shader, lit_shader_dir_loc, dir, SHADER_UNIFORM_VEC3);
        }
        if (lit_shader_color_loc >= 0) {
            const float col[4] = {
                light_color.r / 255.0f * light_intensity,
                light_color.g / 255.0f * light_intensity,
                light_color.b / 255.0f * light_intensity,
                light_color.a / 255.0f
            };
            SetShaderValue(lit_shader, lit_shader_color_loc, col, SHADER_UNIFORM_VEC4);
        }
        if (lit_shader_ambient_loc >= 0) {
            const float amb[4] = {
                ambient_color.r / 255.0f,
                ambient_color.g / 255.0f,
                ambient_color.b / 255.0f,
                ambient_color.a / 255.0f
            };
            SetShaderValue(lit_shader, lit_shader_ambient_loc, amb, SHADER_UNIFORM_VEC4);
        }
    }

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

    // Draw opaque meshes first, then alpha meshes. FreeFalcon's .TEX
    // chroma-key textures have alpha=0 on transparent pixels. Without
    // depth-sorted transparency, drawing alpha meshes before opaque
    // ones causes transparent pixels to write depth and occlude the
    // opaque geometry behind them. Drawing opaque first, then alpha,
    // ensures transparent pixels are blended correctly.
    //
    // The lit shader also has `if (tex.a < 0.5) discard;` which
    // prevents transparent pixels from writing color OR depth — so
    // even without perfect depth sorting, chroma-key cutouts render
    // correctly. This sort is a belt-and-suspenders safety net for
    // the unlit path (Raylib's default shader doesn't discard).
    std::vector<std::size_t> opaque_order;
    std::vector<std::size_t> alpha_order;
    opaque_order.reserve(mesh_entries.size());
    alpha_order.reserve(mesh_entries.size());
    for (std::size_t i = 0; i < mesh_entries.size(); ++i) {
        const auto& entry = mesh_entries[i];
        bool has_alpha = false;
        if (entry.tex_id >= 0) {
            auto it = texture_cache.find(entry.tex_id);
            if (it != texture_cache.end() && it->second.uploaded) {
                has_alpha = it->second.has_alpha;
            }
        }
        if (has_alpha) {
            alpha_order.push_back(i);
        } else {
            opaque_order.push_back(i);
        }
    }

    // Enable alpha blending for the whole mesh pass. Opaque pixels
    // (alpha=255) are unaffected; transparent pixels (alpha=0) blend
    // to the background. This is required for the unlit path; the lit
    // shader's discard handles it more efficiently but blend mode is
    // still safe to leave on.
    BeginBlendMode(BLEND_ALPHA);

    auto draw_entry = [&](std::size_t idx) {
        const auto& entry = mesh_entries[idx];
        const auto& mesh = entry.mesh;
        if (mesh.triangleCount <= 0) return;

        Material* mat_to_use = &default_mat;
        if (entry.tex_id >= 0) {
            auto it = texture_cache.find(entry.tex_id);
            if (it != texture_cache.end() && it->second.uploaded) {
                mat_to_use = &it->second.material;
                if (lighting_active) {
                    mat_to_use->shader = lit_shader;
                }
            }
        }
        DrawMesh(mesh, *mat_to_use, identity);
        ++stats_draw_calls;
        ++stats_meshes_drawn;
        stats_vertices_drawn += static_cast<std::size_t>(mesh.vertexCount);
    };

    // Opaque first
    for (auto idx : opaque_order) draw_entry(idx);
    // Alpha last
    for (auto idx : alpha_order) draw_entry(idx);

    EndBlendMode();

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

    // Light direction gizmo (drawn in 3D so it orbits with the camera)
    if (show_light_gizmo && lighting_enabled) {
        draw_light_gizmo();
    }

    EndMode3D();

    // 2D overlay (stats)
    if (show_stats_overlay) {
        draw_stats_overlay();
    }

    // UnloadMaterial on the default material would leak; LoadMaterialDefault()
    // returns a shared singleton that must NOT be UnloadMaterial'd. Leave it.
    // Individual texture materials in texture_cache are cleaned up in unload_textures().
}

// ── draw_light_gizmo ───────────────────────────────────────────────────────
// Draws a small sun + arrow in 3D space indicating the lighting direction.
// Positioned at the camera target so it stays in view as the user orbits.
void ViewerApp::Impl::draw_light_gizmo() {
    Vector3 dir = light_direction;
    const float dlen = std::sqrt(dir.x*dir.x + dir.y*dir.y + dir.z*dir.z);
    if (dlen < 0.0001f) return;
    dir.x /= dlen; dir.y /= dlen; dir.z /= dlen;

    // Place the gizmo 30 world units from the camera target, in the
    // direction TOWARD the light (so the arrow points from sun → scene).
    const float offset = 30.0f;
    const Vector3 sun_pos = {
        cam_target.x - dir.x * offset,
        cam_target.y - dir.y * offset,
        cam_target.z - dir.z * offset
    };
    const Vector3 arrow_tip = {
        cam_target.x - dir.x * (offset - 8.0f),
        cam_target.y - dir.y * (offset - 8.0f),
        cam_target.z - dir.z * (offset - 8.0f)
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
