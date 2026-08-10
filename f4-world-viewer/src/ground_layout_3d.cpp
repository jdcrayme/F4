// f4-world-viewer/src/ground_layout_3d.cpp
//
// "Ground Layout 3D" ImGui window — a 3D top-down/perspective view of
// the selected objective's airfield geometry, rendered into an offscreen
// RenderTexture2D via Raylib's BeginMode3D and displayed inside the
// ImGui window via rlImGuiImageSize.
//
// The geometry is built by ground_layout_models.cpp (pure function) and
// cached on Impl::ground_layout_3d_geometry. We rebuild only when the
// selected entity changes (compared by EntityId).
//
// Mouse interaction (only when the ImGui image is hovered):
//   - Left-drag: orbit (yaw + pitch)
//   - Scroll:    zoom (distance)
//
// Coordinate conversion: the layout-to-geometry module emits ENU feet
// (X=East, Y=North, Z=Up). Raylib uses RH Y-up (X=right, Y=up, Z=toward
// viewer). The mapping is:
//     raylib_x =  enu_x
//     raylib_y =  enu_z
//     raylib_z = -enu_y
//
// See Docs/SCENARIO_PLAYER_PLAN.md §5.5 for the convention rationale.

#include "viewer_state.hpp"

#include <f4/entities/entity.hpp>
#include <f4/viewer/enum_text.hpp>

// f4-models + f4-world-convert headers MUST come before raylib.h because
// Raylib defines `PI` as a preprocessor macro that would otherwise collide
// with any transitive `using PI` declaration. (The scenario-player uses
// the same include-order pattern — see f4-scenario-player/src/renderer.cpp.)
#include <f4/models/model_database.hpp>
#include <f4/models/geometry.hpp>
#include <f4/models/texture.hpp>
#include <f4/world_convert/class_table.hpp>
#include <f4/install/installation.hpp>

#include <imgui.h>
#include <raylib.h>
#include <raymath.h>
#include <rlgl.h>
#include <rlImGui.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <utility>
#include <vector>

namespace f4::viewer {

namespace {

// ---------------------------------------------------------------------------
// Constants
// ---------------------------------------------------------------------------

// Offscreen render target size. Larger = sharper but slower. 800x600 is
// a good balance for an embedded panel — the user can resize the ImGui
// window, and rlImGuiImageSize scales the texture to fit.
constexpr int RT_W = 800;
constexpr int RT_H = 600;

constexpr float MIN_PITCH = -1.55f;   // ~-89°
constexpr float MAX_PITCH =  1.55f;   // ~+89°
constexpr float MIN_DISTANCE = 50.0f;
constexpr float MAX_DISTANCE = 50000.0f;

constexpr float ORBIT_YAW_SPEED   = 0.005f;   // rad/px
constexpr float ORBIT_PITCH_SPEED = 0.005f;   // rad/px
constexpr float ZOOM_SPEED       = 0.001f;    // log-scale per scroll unit

// Background + grid colors.
constexpr Color BG_COLOR     = { 22,  24,  30, 255};
constexpr Color GRID_COLOR   = { 50,  54,  62, 255};
constexpr Color ORIGIN_COLOR = {255, 255,   0, 200};

// ---------------------------------------------------------------------------
// ENU feet → Raylib Vector3
// ---------------------------------------------------------------------------

inline Vector3 enu_to_rl(float enu_x, float enu_y, float enu_z) {
    return { enu_x, enu_z, -enu_y };
}

// ---------------------------------------------------------------------------
// FreeFalcon BSP model-space → Raylib RH Y-up
// ---------------------------------------------------------------------------
//
// KoreaObj .LOD mesh vertices are stored in FreeFalcon's model-local
// frame. The conversion to Raylib RH Y-up is (x, -z, y) — verified
// against the f4-scenario-player renderer (which renders aircraft and
// airfield features correctly with this transform). Ported from
// f4-scenario-player/include/f4/scenario_player/coordinate_transform.hpp
// ::model_vertex_to_raylib.
inline Vector3 model_vertex_to_rl(float x, float y, float z) {
    return { x, -z, y };
}

// ---------------------------------------------------------------------------
// Resolve a FreeFalcon vertex color index → Raylib Color
// ---------------------------------------------------------------------------
//
// FreeFalcon's `Prim.rgba` is an INT index into the ColorBank (NOT packed
// ABGR). Indices < 4096 look up the ColorBank; index 0 is a sentinel
// ("no color") that we map to white for textured meshes or grey for
// untextured. Out-of-range indices fall back to treating the value as
// packed ABGR (rare; some legacy models).
//
// Ported from f4-scenario-player/src/renderer.cpp:resolve_vertex_color.
Color resolve_vertex_color(uint32_t color_index,
                            const f4::models::ColorBank& color_bank,
                            bool mesh_is_textured) noexcept {
    if (color_index == 0) {
        return mesh_is_textured ? Color{255, 255, 255, 255}
                                : Color{180, 180, 180, 255};
    }
    if (color_index < 4096) {
        const int idx = static_cast<int>(color_index);
        const uint32_t rgba = color_bank.rgba_at(idx);
        if (rgba != 0) {
            return Color{
                static_cast<unsigned char>((rgba >> 24) & 0xFF),
                static_cast<unsigned char>((rgba >> 16) & 0xFF),
                static_cast<unsigned char>((rgba >> 8)  & 0xFF),
                static_cast<unsigned char>(rgba & 0xFF)
            };
        }
    }
    return Color{
        static_cast<unsigned char>(color_index & 0xFF),
        static_cast<unsigned char>((color_index >> 8) & 0xFF),
        static_cast<unsigned char>((color_index >> 16) & 0xFF),
        static_cast<unsigned char>((color_index >> 24) & 0xFF)
    };
}

// ---------------------------------------------------------------------------
// Lit shader source (same as f4-scenario-player's renderer.cpp)
// ---------------------------------------------------------------------------
//
// Forward-declared here so we can lazily compile it on first draw. The
// shader adds a single directional light + ambient term to Raylib's
// default mesh pipeline. If compilation fails (e.g. headless GL stub
// with no shader support), we fall back to LoadMaterialDefault() —
// features still render, just unlit.
const char* const kLitShaderVS_3d =
    "#version 330\n"
    "in vec3 vertexPosition;\n"
    "in vec2 vertexTexCoord;\n"
    "in vec4 vertexColor;\n"
    "in vec3 vertexNormal;\n"
    "uniform mat4 mvp;\n"
    "uniform mat4 modelView;\n"
    "out vec2 fragTexCoord;\n"
    "out vec4 fragColor;\n"
    "out vec3 fragNormal;\n"
    "void main() {\n"
    "    fragTexCoord = vertexTexCoord;\n"
    "    fragColor = vertexColor;\n"
    "    fragNormal = normalize(mat3(modelView) * vertexNormal);\n"
    "    gl_Position = mvp * vec4(vertexPosition, 1.0);\n"
    "}\n";

const char* const kLitShaderFS_3d =
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
    "    if (tex.a < 0.5) discard;\n"
    "    vec3 N = normalize(fragNormal);\n"
    "    vec3 L = normalize(-lightDir);\n"
    "    float NdotL = max(dot(N, L), 0.0);\n"
    "    vec4 light = ambient + lightColor * NdotL;\n"
    "    finalColor = tex * colDiffuse * fragColor * light;\n"
    "}\n";

// ---------------------------------------------------------------------------
// Drawing — flat ground quads (runway surface, taxiway strip, threshold
// bars, building footprints). Corners are CCW from above with +Z up.
// ---------------------------------------------------------------------------

void draw_ground_quad(const LayoutQuad& q) {
    const Vector3 v0 = enu_to_rl(q.x[0], q.y[0], q.z);
    const Vector3 v1 = enu_to_rl(q.x[1], q.y[1], q.z);
    const Vector3 v2 = enu_to_rl(q.x[2], q.y[2], q.z);
    const Vector3 v3 = enu_to_rl(q.x[3], q.y[3], q.z);
    const Color c = {q.r, q.g, q.b, q.a};
    // Two triangles (CCW from above). Raylib's DrawTriangle3D expects
    // CCW winding for front-facing triangles (with default culling),
    // but we disabled backface culling below so winding doesn't matter.
    DrawTriangle3D(v0, v1, v2, c);
    DrawTriangle3D(v0, v2, v3, c);
}

// ---------------------------------------------------------------------------
// Drawing — line segments (centerline dashes, taxiway centerlines)
// ---------------------------------------------------------------------------

void draw_layout_line(const LayoutLine& l) {
    const Vector3 a = enu_to_rl(l.x0, l.y0, l.z);
    const Vector3 b = enu_to_rl(l.x1, l.y1, l.z);
    const Color c = {l.r, l.g, l.b, l.a};
    DrawLine3D(a, b, c);
}

// ---------------------------------------------------------------------------
// Drawing — labeled markers
// ---------------------------------------------------------------------------

void draw_layout_marker(const LayoutMarker& m) {
    const Vector3 center = enu_to_rl(m.x, m.y, m.z);
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
// Drawing — ground grid for orientation reference
// ---------------------------------------------------------------------------

void draw_ground_grid(float center_x, float center_y,
                      float extent_ft, float step_ft) {
    for (float gx = std::ceil((-extent_ft + center_x) / step_ft) * step_ft;
         gx <= extent_ft + center_x; gx += step_ft) {
        const Vector3 a = enu_to_rl(gx, center_y - extent_ft, 0.0f);
        const Vector3 b = enu_to_rl(gx, center_y + extent_ft, 0.0f);
        DrawLine3D(a, b, GRID_COLOR);
    }
    for (float gy = std::ceil((-extent_ft + center_y) / step_ft) * step_ft;
         gy <= extent_ft + center_y; gy += step_ft) {
        const Vector3 a = enu_to_rl(center_x - extent_ft, gy, 0.0f);
        const Vector3 b = enu_to_rl(center_x + extent_ft, gy, 0.0f);
        DrawLine3D(a, b, GRID_COLOR);
    }
    // Origin marker (objective center, z=0).
    const Vector3 o = enu_to_rl(center_x, center_y, 0.0f);
    DrawLine3D({o.x - 8, o.y, o.z}, {o.x + 8, o.y, o.z}, ORIGIN_COLOR);
    DrawLine3D({o.x, o.y, o.z - 8}, {o.x, o.y, o.z + 8}, ORIGIN_COLOR);
}

// ---------------------------------------------------------------------------
// Drawing — labels (project 3D marker position to 2D screen, draw text
// in the overlay pass after EndMode3D)
// ---------------------------------------------------------------------------

struct Label2D {
    int   x, y;          // screen pixel position
    bool  visible;
    char  text[64];
    Color color;
};

void collect_labels(const Camera3D& cam,
                    const AirfieldGeometry3D& g,
                    bool show_parking,
                    std::vector<Label2D>& out) {
    const auto add = [&](const LayoutMarker& m) {
        Label2D lbl{};
        const Vector3 rl = enu_to_rl(m.x, m.y, m.z + m.size_ft * 1.5f);
        const Vector2 s = GetWorldToScreen(rl, cam);
        lbl.x = static_cast<int>(s.x);
        lbl.y = static_cast<int>(s.y);
        lbl.visible = (s.x >= 0 && s.x < RT_W && s.y >= 0 && s.y < RT_H);
        std::snprintf(lbl.text, sizeof(lbl.text), "%s", m.label.c_str());
        lbl.color = Color{255, 255, 255, 220};
        out.push_back(lbl);
    };
    if (show_parking) {
        for (const auto& m : g.parking_spots) add(m);
    }
    for (const auto& m : g.helipads) add(m);
    for (const auto& m : g.runway_ends) add(m);
}

void draw_labels(const std::vector<Label2D>& labels) {
    for (const auto& lbl : labels) {
        if (!lbl.visible) continue;
        DrawText(lbl.text, lbl.x + 1, lbl.y + 1, 12, Color{0, 0, 0, 220});
        DrawText(lbl.text, lbl.x, lbl.y, 12, lbl.color);
    }
}

} // anonymous namespace

// ---------------------------------------------------------------------------
// Impl method definitions — KoreaObj model loading + mesh building
// ---------------------------------------------------------------------------
//
// These mirror the f4-scenario-player's PlayerApp::Impl methods of the
// same name (build_mesh_for_model, upload_textures, unload_textures,
// unload_meshes), but adapted to live inside the world-viewer's Impl
// struct. The mesh + texture cache is shared across all selected
// objectives — switching selection does NOT invalidate the cache (the
// models are keyed by KoreaObj parent_index, not by entity).
//
// All methods require the GL context (rlImGuiSetup has been called).
// No-op when no Installation is configured.

bool ViewerApp::Impl::ensure_models_3d_loaded() {
    if (models_3d_load_attempted) return models_3d_loaded;
    models_3d_load_attempted = true;

    // Resolve KoreaObj.HDR + .LOD + .TEX + Falcon4.ct from the install.
    if (!install.has_value()) {
        models_3d_error = "No installation configured — set the install path to load feature models.";
        return false;
    }
    const auto& inst = *install;
    if (inst.terrdata_dir().empty()) {
        models_3d_error = "Installation has no terrdata/ — cannot locate KoreaObj files.";
        return false;
    }

    // Locate KoreaObj.HDR/.LOD (or .DXH/.DXL) under the install root.
    auto [hdr_path, lod_path] =
        f4::models::ModelDatabase::find_koreaobj_files(inst.root());
    if (hdr_path.empty() || lod_path.empty()) {
        // Try terrdata/ root as fallback (some installs nest objects there).
        const auto& td = inst.terrdata_dir();
        auto [hdr2, lod2] = f4::models::ModelDatabase::find_koreaobj_files(td);
        if (!hdr2.empty() && !lod2.empty()) {
            hdr_path = hdr2;
            lod_path = lod2;
        }
    }
    if (hdr_path.empty() || lod_path.empty()) {
        models_3d_error = "KoreaObj.HDR/.LOD not found under install root.";
        return false;
    }

    // Locate the .TEX file (try install root first, then terrdata/).
    auto tex_path = f4::models::ModelDatabase::find_tex_file(inst.root());
    if (tex_path.empty()) {
        tex_path = f4::models::ModelDatabase::find_tex_file(inst.terrdata_dir());
    }

    // Load the class table (FALCON4.ct) — needed to map FeatureEntryState.index
    // (entity_type) → vis_type[0] (KoreaObj model index).
    const auto& ct_path = inst.class_table();
    if (ct_path.empty()) {
        models_3d_error = "FALCON4.ct not found — cannot resolve feature entity_type to model.";
        return false;
    }
    try {
        class_table_3d.load(ct_path);
    } catch (const std::exception& e) {
        models_3d_error = std::string("Failed to load FALCON4.ct: ") + e.what();
        return false;
    }

    // Load the model database.
    model_db_3d.emplace();
    auto err = model_db_3d->load(hdr_path, lod_path);
    if (!err.empty()) {
        model_db_3d.reset();
        models_3d_error = "ModelDatabase::load failed: " + err;
        return false;
    }
    if (!tex_path.empty()) {
        auto tex_err = model_db_3d->load_tex(tex_path);
        if (!tex_err.empty()) {
            // Textures are optional — features will render with vertex
            // colors only. Don't treat this as a hard failure.
            // (Note: many FF installs don't ship KoreaObj.TEX at all —
            //  the textures live in the theater's texture atlas instead.)
        }
    }

    models_3d_loaded = true;
    return true;
}

bool ViewerApp::Impl::ensure_lit_shader_3d() {
    if (lit_shader_3d_loaded) return lit_shader_3d.id != 0;
    lit_shader_3d_loaded = true;
    lit_shader_3d = LoadShaderFromMemory(kLitShaderVS_3d, kLitShaderFS_3d);
    if (lit_shader_3d.id == 0) return false;
    lit_shader_3d_dir_loc     = GetShaderLocation(lit_shader_3d, "lightDir");
    lit_shader_3d_color_loc   = GetShaderLocation(lit_shader_3d, "lightColor");
    lit_shader_3d_ambient_loc = GetShaderLocation(lit_shader_3d, "ambient");
    return true;
}

bool ViewerApp::Impl::ensure_default_material_3d() {
    // Idempotent — once valid, stay valid (until unload_meshes_3d()).
    if (default_mat_3d_valid) return true;

    // 1) Create the 1x1 opaque-white fallback texture.
    if (!fallback_white_tex_3d_valid) {
        Image img = {};
        img.data = RL_MALLOC(4);  // 1 pixel * 4 bytes (RGBA8)
        if (!img.data) return false;
        unsigned char* px = static_cast<unsigned char*>(img.data);
        px[0] = 255; px[1] = 255; px[2] = 255; px[3] = 255;
        img.width = 1;
        img.height = 1;
        img.mipmaps = 1;
        img.format = PIXELFORMAT_UNCOMPRESSED_R8G8B8A8;
        fallback_white_tex_3d = LoadTextureFromImage(img);
        UnloadImage(img);
        fallback_white_tex_3d_valid = (fallback_white_tex_3d.id != 0);
        if (!fallback_white_tex_3d_valid) return false;
    }

    // 2) Create the cached default material, bind the white texture to
    //    its diffuse map so the lit shader samples (1,1,1,1) instead of
    //    undefined data. Also assign the lit shader (if it compiled —
    //    otherwise we fall back to Raylib's default shader, which still
    //    works without lighting).
    default_mat_3d = LoadMaterialDefault();
    default_mat_3d.maps[MATERIAL_MAP_DIFFUSE].texture = fallback_white_tex_3d;
    default_mat_3d.maps[MATERIAL_MAP_DIFFUSE].color = WHITE;
    if (lit_shader_3d.id != 0) {
        default_mat_3d.shader = lit_shader_3d;
    }
    default_mat_3d_valid = true;
    return true;
}

void ViewerApp::Impl::build_mesh_3d(int parent_index) {
    if (parent_index < 0) return;
    auto it = mesh_cache_3d.find(parent_index);
    if (it != mesh_cache_3d.end() && it->second.built) return;  // already cached

    if (!model_db_3d.has_value()) {
        if (it != mesh_cache_3d.end()) it->second.built = true;
        else mesh_cache_3d[parent_index].built = true;
        return;
    }
    auto& db = *model_db_3d;
    const auto* rec = db.model(parent_index);
    if (!rec || rec->lods.empty()) {
        if (it != mesh_cache_3d.end()) it->second.built = true;
        else mesh_cache_3d[parent_index].built = true;
        return;
    }
    const int lod = 0;  // lock to LOD 0 (highest detail)
    auto err = db.parse_lod(parent_index, lod);
    if (!err.empty()) {
        if (it != mesh_cache_3d.end()) it->second.built = true;
        else mesh_cache_3d[parent_index].built = true;
        return;
    }

    // Default ModelState: texture_set=0, no DOFs/switches active.
    f4::models::ModelState default_state;
    default_state.texture_set = 0;
    default_state.n_texture_sets = std::max(1, static_cast<int>(rec->n_texture_sets));

    auto geom = db.extract_model_geometry(parent_index, lod, default_state);
    if (geom.meshes.empty()) {
        mesh_cache_3d[parent_index].built = true;
        return;
    }

    const auto& cb = db.color_bank();

    MeshCacheEntry3D entry;
    entry.meshes.reserve(geom.meshes.size());
    for (const auto& src : geom.meshes) {
        if (src.vertices.empty()) continue;
        if (src.kind == f4::models::PrimitiveKind::Triangles && src.triangles.empty()) continue;

        const bool mesh_is_textured = (src.tex_id >= 0);
        const int vert_count = static_cast<int>(src.vertices.size());
        const int tri_count = static_cast<int>(src.triangles.size());

        ::Mesh rm = {};
        rm.vertexCount = vert_count;
        rm.triangleCount = tri_count;
        rm.vertices = static_cast<float*>(RL_MALLOC(vert_count * 3 * sizeof(float)));
        rm.normals  = static_cast<float*>(RL_MALLOC(vert_count * 3 * sizeof(float)));
        rm.texcoords = static_cast<float*>(RL_MALLOC(vert_count * 2 * sizeof(float)));
        rm.colors   = static_cast<unsigned char*>(RL_MALLOC(vert_count * 4 * sizeof(unsigned char)));
        if (tri_count > 0) {
            rm.indices = static_cast<unsigned short*>(RL_MALLOC(tri_count * 3 * sizeof(unsigned short)));
        }

        for (int i = 0; i < vert_count; ++i) {
            const auto& v = src.vertices[static_cast<std::size_t>(i)];
            const Vector3 pos = model_vertex_to_rl(v.position.x, v.position.y, v.position.z);
            rm.vertices[i*3+0] = pos.x;
            rm.vertices[i*3+1] = pos.y;
            rm.vertices[i*3+2] = pos.z;
            const Vector3 nrm = model_vertex_to_rl(v.normal.x, v.normal.y, v.normal.z);
            rm.normals[i*3+0] = nrm.x;
            rm.normals[i*3+1] = nrm.y;
            rm.normals[i*3+2] = nrm.z;
            rm.texcoords[i*2+0] = v.uv.u;
            rm.texcoords[i*2+1] = v.uv.v;
            const Color c = resolve_vertex_color(v.color, cb, mesh_is_textured);
            rm.colors[i*4+0] = c.r;
            rm.colors[i*4+1] = c.g;
            rm.colors[i*4+2] = c.b;
            rm.colors[i*4+3] = c.a;
        }
        if (tri_count > 0) {
            for (int i = 0; i < tri_count; ++i) {
                const auto& tri = src.triangles[static_cast<std::size_t>(i)];
                rm.indices[i*3+0] = static_cast<unsigned short>(tri.v0);
                rm.indices[i*3+1] = static_cast<unsigned short>(tri.v1);
                rm.indices[i*3+2] = static_cast<unsigned short>(tri.v2);
            }
        }
        UploadMesh(&rm, false);

        MeshEntry3D me;
        me.mesh = rm;
        me.tex_id = src.tex_id;
        entry.meshes.push_back(me);
    }

    entry.built = true;
    mesh_cache_3d[parent_index] = std::move(entry);

    // Upload any new textures referenced by this model's meshes.
    upload_textures_3d();
}

void ViewerApp::Impl::upload_textures_3d() {
    if (!model_db_3d.has_value()) return;
    auto& db = *model_db_3d;

    for (auto& [parent_idx, cache_entry] : mesh_cache_3d) {
        for (auto& me : cache_entry.meshes) {
            if (me.tex_id < 0) continue;
            if (texture_cache_3d.count(me.tex_id)) continue;

            const auto* decoded = db.fetch_texture(me.tex_id);
            if (!decoded || !decoded->valid()) {
                TexCacheEntry3D ce; ce.uploaded = false;
                texture_cache_3d[me.tex_id] = ce;
                continue;
            }

            Image img = {};
            img.data = RL_MALLOC(decoded->width * decoded->height * 4);
            if (!img.data) {
                TexCacheEntry3D ce; ce.uploaded = false;
                texture_cache_3d[me.tex_id] = ce;
                continue;
            }
            std::memcpy(img.data, decoded->rgba.data(),
                        static_cast<std::size_t>(decoded->width * decoded->height * 4));
            img.width = decoded->width;
            img.height = decoded->height;
            img.mipmaps = 1;
            img.format = PIXELFORMAT_UNCOMPRESSED_R8G8B8A8;

            Texture2D tex = LoadTextureFromImage(img);
            Material mat = LoadMaterialDefault();
            mat.maps[MATERIAL_MAP_DIFFUSE].texture = tex;
            mat.maps[MATERIAL_MAP_DIFFUSE].color = WHITE;
            // Assign the lit shader if it has been compiled. This is
            // normally the case because ensure_lit_shader_3d() is called
            // BEFORE the first build_mesh_3d() in the render loop. If
            // the shader isn't loaded yet (e.g. it failed to compile),
            // the material falls back to Raylib's default shader, which
            // still renders the mesh (just without directional lighting).
            if (lit_shader_3d.id != 0) {
                mat.shader = lit_shader_3d;
            }

            TexCacheEntry3D ce;
            ce.texture = tex;
            ce.material = mat;
            ce.uploaded = true;
            texture_cache_3d[me.tex_id] = ce;
            UnloadImage(img);
        }
    }

    // If the lit shader was compiled AFTER some texture materials were
    // already cached (e.g. shader compilation succeeded on a later frame
    // than the first texture upload), walk the cache and assign the
    // shader retroactively. Idempotent.
    if (lit_shader_3d.id != 0) {
        for (auto& [id, ce] : texture_cache_3d) {
            if (ce.uploaded && ce.material.shader.id != lit_shader_3d.id) {
                ce.material.shader = lit_shader_3d;
            }
        }
    }
}

void ViewerApp::Impl::unload_meshes_3d() {
    // Free textures first (meshes don't reference textures, but materials do).
    for (auto& [id, ce] : texture_cache_3d) {
        if (ce.uploaded) {
            UnloadTexture(ce.texture);
            ce.material.maps[MATERIAL_MAP_DIFFUSE].texture = {};
        }
    }
    texture_cache_3d.clear();

    // Free meshes.
    for (auto& [parent_idx, cache_entry] : mesh_cache_3d) {
        for (auto& me : cache_entry.meshes) {
            UnloadMesh(me.mesh);
        }
        cache_entry.meshes.clear();
        cache_entry.built = false;
    }
    mesh_cache_3d.clear();

    // Free the cached default material + 1x1 fallback texture. We must
    // unset the texture reference on the material BEFORE unloading the
    // texture, otherwise UnloadMaterial may try to free a texture that's
    // about to be freed separately.
    if (default_mat_3d_valid) {
        default_mat_3d.maps[MATERIAL_MAP_DIFFUSE].texture = {};
        UnloadMaterial(default_mat_3d);
        default_mat_3d = {};
        default_mat_3d_valid = false;
    }
    if (fallback_white_tex_3d_valid) {
        UnloadTexture(fallback_white_tex_3d);
        fallback_white_tex_3d = {};
        fallback_white_tex_3d_valid = false;
    }

    // Unload the lit shader.
    if (lit_shader_3d.id != 0) {
        UnloadShader(lit_shader_3d);
        lit_shader_3d = {};
        lit_shader_3d_loaded = false;
        lit_shader_3d_dir_loc = -1;
        lit_shader_3d_color_loc = -1;
        lit_shader_3d_ambient_loc = -1;
    }
}

// ---------------------------------------------------------------------------
// Public method — ViewerApp::draw_ground_layout_3d
// ---------------------------------------------------------------------------

void ViewerApp::draw_ground_layout_3d() {
    // Only show when an objective with ground_layout OR features is selected.
    if (impl_->sel_kind != Impl::SelectionKind::Objective ||
        !impl_->sel_entity.valid()) {
        return;
    }
    auto h = impl_->handle(impl_->sel_entity);
    auto* gl = h.get<f4::entities::GroundLayoutComponent>();
    auto* fs = h.get<f4::entities::FeatureSetComponent>();
    auto* ot = h.get<f4::entities::ObjectiveTypeComponent>();
    const bool has_layout = gl && !gl->layouts.empty();
    const bool has_features = fs && !fs->features.empty();
    if (!has_layout && !has_features) return;

    // Lazily load KoreaObj models + FALCON4.ct the first time we have a
    // feature-bearing objective selected. The load is ~50-150ms; once
    // loaded, subsequent calls are no-ops. On failure, we silently fall
    // back to flat footprint rendering (the toggle just won't do
    // anything visible).
    if (has_features && impl_->ground_layout_3d_show_models &&
        !impl_->models_3d_load_attempted) {
        impl_->ensure_models_3d_loaded();
    }
    const bool models_ready = impl_->models_3d_loaded &&
                              impl_->model_db_3d.has_value() &&
                              impl_->class_table_3d.loaded();

    // Local lambdas that close over impl_ — keeps Impl private while
    // still letting us structure the camera + geometry logic into
    // small, named helpers.
    auto update_camera_orbit = [&]() {
        const float cy = std::cos(impl_->ground_layout_3d_yaw);
        const float sy = std::sin(impl_->ground_layout_3d_yaw);
        const float cp = std::cos(impl_->ground_layout_3d_pitch);
        const float sp = std::sin(impl_->ground_layout_3d_pitch);
        const float d  = impl_->ground_layout_3d_distance;
        const float cx = impl_->ground_layout_3d_center_x;
        const float cy_g = impl_->ground_layout_3d_center_y;
        const Vector3 target = enu_to_rl(cx, cy_g, 0.0f);
        impl_->ground_layout_3d_camera.position = {
            target.x + d * cp * sy,
            target.y + d * sp,
            target.z + d * cp * cy
        };
        impl_->ground_layout_3d_camera.target = target;
        impl_->ground_layout_3d_camera.up = {0.0f, 1.0f, 0.0f};
        impl_->ground_layout_3d_camera.fovy = 45.0f;
        impl_->ground_layout_3d_camera.projection = CAMERA_PERSPECTIVE;
    };

    auto default_orbit_for_bbox = [&](const AirfieldGeometry3D& g) {
        if (g.empty) return;
        const float cx = (g.min_x + g.max_x) * 0.5f;
        const float cy = (g.min_y + g.max_y) * 0.5f;
        const float w = g.max_x - g.min_x;
        const float h = g.max_y - g.min_y;
        const float diag = std::sqrt(w * w + h * h);
        impl_->ground_layout_3d_center_x = cx;
        impl_->ground_layout_3d_center_y = cy;
        // Default: 35° pitch, ~30° yaw, distance = 1.5x diagonal.
        impl_->ground_layout_3d_pitch    = 0.6f;
        impl_->ground_layout_3d_yaw       = 0.5f;
        impl_->ground_layout_3d_distance  = std::max(diag * 1.5f, 500.0f);
    };

    // Rebuild the geometry when the selection changes. We compare by
    // EntityId — the geometry is purely a function of the layout/features
    // data, which doesn't mutate at runtime (the user can't edit it).
    if (impl_->ground_layout_3d_cached_entity != impl_->sel_entity) {
        impl_->ground_layout_3d_geometry = build_airfield_geometry_3d(
            gl ? gl->layouts : std::vector<f4::entities::GroundLayoutList>{},
            (fs && impl_->ground_layout_3d_show_features) ? &fs->features : nullptr);
        impl_->ground_layout_3d_cached_entity = impl_->sel_entity;
        default_orbit_for_bbox(impl_->ground_layout_3d_geometry);
    }
    // If the user toggled "show features" since the last rebuild, force
    // a rebuild (cheap — pure function).
    if (fs && !fs->features.empty() &&
        (impl_->ground_layout_3d_geometry.feature_footprints.empty() ==
         impl_->ground_layout_3d_show_features)) {
        impl_->ground_layout_3d_geometry = build_airfield_geometry_3d(
            gl ? gl->layouts : std::vector<f4::entities::GroundLayoutList>{},
            impl_->ground_layout_3d_show_features ? &fs->features : nullptr);
        impl_->ground_layout_3d_cached_entity = impl_->sel_entity;
    }
    const auto& g = impl_->ground_layout_3d_geometry;
    if (g.empty) return;  // nothing to render

    // Update the camera from orbit parameters BEFORE the render pass.
    // Without this, the first frame after a selection change would
    // render with a stale/default-constructed camera (all zeros),
    // producing a blank panel. The original code called update_camera_orbit()
    // at the END of the function — which meant the FIRST frame of every
    // selection always rendered nothing, and only subsequent frames
    // showed the geometry. Moving it here makes the very first frame
    // correct.
    update_camera_orbit();

    // ImGui window setup.
    ImGui::SetNextWindowPos(ImVec2(660, 80), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(700, 540), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Ground Layout 3D", nullptr, ImGuiWindowFlags_NoCollapse)) {
        ImGui::End();
        return;
    }

    // Header — class name + counts.
    if (ot && !ot->class_name.empty()) {
        ImGui::TextUnformatted(ot->class_name.c_str());
    } else {
        ImGui::TextUnformatted("Objective");
    }
    ImGui::SameLine();
    ImGui::TextDisabled("(%d runways, %d parking, %d taxi strips, %d features)",
        static_cast<int>(g.runway_surfaces.size()),
        static_cast<int>(g.parking_spots.size()),
        static_cast<int>(g.taxiway_strips.size()),
        static_cast<int>(g.feature_footprints.size()));

    // Layer toggles (compact, on one line).
    ImGui::Checkbox("Runway",   &impl_->ground_layout_3d_show_runway);
    ImGui::SameLine();
    ImGui::Checkbox("Taxiways", &impl_->ground_layout_3d_show_taxiways);
    ImGui::SameLine();
    ImGui::Checkbox("Parking",  &impl_->ground_layout_3d_show_parking);
    ImGui::SameLine();
    ImGui::Checkbox("Features", &impl_->ground_layout_3d_show_features);
    ImGui::SameLine();
    ImGui::Checkbox("3D Models", &impl_->ground_layout_3d_show_models);
    ImGui::SameLine();
    ImGui::Checkbox("Labels",   &impl_->ground_layout_3d_show_labels);
    ImGui::SameLine();
    ImGui::Checkbox("Grid",     &impl_->ground_layout_3d_show_grid);

    // Reset-view button.
    ImGui::SameLine();
    if (ImGui::Button("Reset View")) {
        default_orbit_for_bbox(g);
    }

    // Model-loading status indicator (only shown when 3D Models is on
    // AND we have features). Helps the user understand why nothing is
    // rendering as 3D — common cases: no install set, KoreaObj.HDR not
    // found, FALCON4.ct missing.
    if (impl_->ground_layout_3d_show_models && has_features) {
        if (!impl_->install.has_value()) {
            ImGui::TextDisabled("[3D models: no install set — using footprints]");
        } else if (!models_ready) {
            const char* err = impl_->models_3d_error.empty()
                ? "(loading…)" : impl_->models_3d_error.c_str();
            ImGui::TextDisabled("[3D models: %s]", err);
        } else {
            const int cached = static_cast<int>(impl_->mesh_cache_3d.size());
            const int textures = static_cast<int>(impl_->texture_cache_3d.size());
            ImGui::TextDisabled("[3D models: %d cached, %d textures]",
                                cached, textures);
            // Per-frame diagnostic — shows where features are being
            // dropped in the pipeline. The numbers reset every frame so
            // they always reflect the CURRENT selection. If "drawn" is 0
            // but "total" is non-zero, the bug is in the feature → vis_type
            // → mesh pipeline (visible in the skipped counters).
            ImGui::TextDisabled(
                "[features: %d total | drawn=%d | no_vistype=%d | no_mesh=%d | placeholder=%d]",
                impl_->diag_3d_features_total,
                impl_->diag_3d_features_drawn,
                impl_->diag_3d_features_no_vistype,
                impl_->diag_3d_features_no_mesh,
                impl_->diag_3d_features_skipped_placeholder);
            ImGui::TextDisabled("[meshes drawn: %d, triangles: %d]",
                                impl_->diag_3d_meshes_drawn,
                                impl_->diag_3d_triangles_drawn);
        }
    }

    // Available content region for the embedded image.
    const ImVec2 avail = ImGui::GetContentRegionAvail();
    const int img_w = std::max(static_cast<int>(avail.x), 64);
    const int img_h = std::max(static_cast<int>(avail.y) - 20, 64);  // -20 for footer

    // Lazily allocate the RenderTexture2D on first use.
    if (!impl_->ground_layout_3d_target_valid) {
        if (impl_->ground_layout_3d_target.id != 0) {
            UnloadRenderTexture(impl_->ground_layout_3d_target);
            impl_->ground_layout_3d_target = {0};
        }
        impl_->ground_layout_3d_target = LoadRenderTexture(RT_W, RT_H);
        SetTextureFilter(impl_->ground_layout_3d_target.texture,
                         TEXTURE_FILTER_BILINEAR);
        impl_->ground_layout_3d_target_valid = true;
    }

    // --- Render the 3D scene into the RenderTexture2D --------------------
    BeginTextureMode(impl_->ground_layout_3d_target);
    {
        ClearBackground(BG_COLOR);
        BeginMode3D(impl_->ground_layout_3d_camera);
        {
            // CRITICAL: disable backface culling. Our ground quads have
            // arbitrary winding (the layout data doesn't guarantee CCW),
            // and we draw both sides anyway. Mirrors the f4-models-viewer
            // approach for FreeFalcon's .LOD meshes.
            rlDisableBackfaceCulling();
            rlSetBlendMode(BLEND_ALPHA);

            // --- Diagnostic: origin axis triad (always drawn) ---------------
            //
            // Draw a small RGB axis triad at the camera target so the user
            // can ALWAYS see SOMETHING in the viewport — even if no
            // features/runways render. If the triad is invisible, the bug
            // is in the camera/projection (BeginMode3D not setting up
            // matrices correctly). If the triad is visible but nothing
            // else is, the bug is in the feature/mesh pipeline.
            //
            // +X = red (East in ENU), +Y = green (Up), +Z = blue (-North).
            // Length scales with bbox size so it stays proportional.
            {
                const float axis_len = std::max(
                    (g.max_x - g.min_x) * 0.05f,
                    (g.max_y - g.min_y) * 0.05f);
                const float al = std::max(axis_len, 50.0f);
                const Vector3 o = enu_to_rl(
                    impl_->ground_layout_3d_center_x,
                    impl_->ground_layout_3d_center_y, 0.0f);
                DrawLine3D(o, {o.x + al, o.y, o.z}, Color{220, 60, 60, 255});
                DrawLine3D(o, {o.x, o.y + al, o.z}, Color{60, 220, 60, 255});
                DrawLine3D(o, {o.x, o.y, o.z + al}, Color{60, 100, 220, 255});
            }

            // Ground grid (orientation reference).
            if (impl_->ground_layout_3d_show_grid) {
                const float ext = std::max(
                    (g.max_x - g.min_x) * 0.6f,
                    (g.max_y - g.min_y) * 0.6f);
                const float step = ext > 4000.0f ? 1000.0f :
                                   ext > 1000.0f ?  500.0f : 100.0f;
                draw_ground_grid(impl_->ground_layout_3d_center_x,
                                 impl_->ground_layout_3d_center_y,
                                 ext, step);
            }

            // Runway surfaces + threshold bars + centerline dashes + end markers.
            if (impl_->ground_layout_3d_show_runway) {
                for (const auto& q : g.runway_surfaces) draw_ground_quad(q);
                for (const auto& q : g.threshold_bars) draw_ground_quad(q);
                for (const auto& q : g.centerline_dashes) draw_ground_quad(q);
                for (const auto& m : g.runway_ends) draw_layout_marker(m);
            }

            // Taxiway strips + centerlines.
            if (impl_->ground_layout_3d_show_taxiways) {
                for (const auto& q : g.taxiway_strips)      draw_ground_quad(q);
                for (const auto& l : g.taxiway_centerlines) draw_layout_line(l);
            }

            // Parking markers + helipads.
            if (impl_->ground_layout_3d_show_parking) {
                for (const auto& m : g.parking_spots) draw_layout_marker(m);
            }
            for (const auto& m : g.helipads) draw_layout_marker(m);

            // Feature footprints — used as a fallback when 3D models are
            // disabled or unavailable. When 3D models ARE active and
            // loaded, we skip the footprints (they would z-fight with the
            // models and add visual clutter).
            const bool draw_footprints = !has_features ||
                !impl_->ground_layout_3d_show_models ||
                !models_ready;
            if (draw_footprints) {
                for (const auto& q : g.feature_footprints) draw_ground_quad(q);
            }

            // --- Real KoreaObj 3D feature models ------------------------
            //
            // For each FeatureEntryState on the selected objective, resolve
            // its vis_type via FALCON4.ct (entity_type → vis_type[0]),
            // build the Raylib Mesh for that KoreaObj parent_index (cached),
            // and DrawMesh it at the feature's offset_xyz rotated by
            // facing degrees around the vertical axis.
            //
            // The mesh cache is keyed by vis_type, so multiple features
            // of the same type (e.g. three hangars of type 169) share
            // one GPU upload. Textures are cached by tex_id across all
            // meshes — shared across features and across selections.

            // Reset per-frame diagnostic counters (always, even when we
            // won't draw models — so the panel shows 0/0/0 instead of
            // stale values from a previous selection).
            impl_->diag_3d_features_total = 0;
            impl_->diag_3d_features_skipped_placeholder = 0;
            impl_->diag_3d_features_no_vistype = 0;
            impl_->diag_3d_features_no_mesh = 0;
            impl_->diag_3d_features_drawn = 0;
            impl_->diag_3d_meshes_drawn = 0;
            impl_->diag_3d_triangles_drawn = 0;

            if (impl_->ground_layout_3d_show_models && models_ready && fs) {
                // Compile the lit shader on first use (idempotent).
                const bool lighting_active = impl_->ensure_lit_shader_3d();
                if (lighting_active) {
                    // Set shader uniforms once for this draw batch.
                    Vector3 light_dir = impl_->light_3d_direction;
                    const float dlen = std::sqrt(
                        light_dir.x * light_dir.x +
                        light_dir.y * light_dir.y +
                        light_dir.z * light_dir.z);
                    if (dlen > 0.0001f) {
                        light_dir.x /= dlen; light_dir.y /= dlen; light_dir.z /= dlen;
                    } else {
                        light_dir = {0.5f, -1.0f, 0.3f};
                    }
                    if (impl_->lit_shader_3d_dir_loc >= 0) {
                        const float dir[3] = { light_dir.x, light_dir.y, light_dir.z };
                        SetShaderValue(impl_->lit_shader_3d,
                                        impl_->lit_shader_3d_dir_loc,
                                        dir, SHADER_UNIFORM_VEC3);
                    }
                    if (impl_->lit_shader_3d_color_loc >= 0) {
                        const float col[4] = {
                            impl_->light_3d_color.r / 255.0f * impl_->light_3d_intensity,
                            impl_->light_3d_color.g / 255.0f * impl_->light_3d_intensity,
                            impl_->light_3d_color.b / 255.0f * impl_->light_3d_intensity,
                            impl_->light_3d_color.a / 255.0f
                        };
                        SetShaderValue(impl_->lit_shader_3d,
                                        impl_->lit_shader_3d_color_loc,
                                        col, SHADER_UNIFORM_VEC4);
                    }
                    if (impl_->lit_shader_3d_ambient_loc >= 0) {
                        const float amb[4] = {
                            impl_->ambient_3d_color.r / 255.0f,
                            impl_->ambient_3d_color.g / 255.0f,
                            impl_->ambient_3d_color.b / 255.0f,
                            impl_->ambient_3d_color.a / 255.0f
                        };
                        SetShaderValue(impl_->lit_shader_3d,
                                        impl_->lit_shader_3d_ambient_loc,
                                        amb, SHADER_UNIFORM_VEC4);
                    }
                }

                // Cached default material — created ONCE (not every frame).
                // The material binds a 1x1 opaque-white fallback texture
                // to MATERIAL_MAP_DIFFUSE so that UTEXTURED meshes sample
                // (1,1,1,1) instead of undefined data. Without this, the
                // lit shader's `if (tex.a < 0.5) discard;` would kill
                // every fragment of every untextured mesh (which is most
                // of them — buildings, signs, vehicles typically don't
                // have textures in KoreaObj).
                if (!impl_->ensure_default_material_3d()) {
                    // Failed to build the fallback texture — can't safely
                    // draw meshes (the shader would discard them all).
                    // Skip the model pass; the grid + axis triad + flat
                    // geometry are still visible so the user knows the
                    // panel is alive.
                } else {
                    // Walk features and draw each one's model.
                    impl_->diag_3d_features_total =
                        static_cast<int>(fs->features.size());
                    for (const auto& f : fs->features) {
                        // Skip empty placeholder features (the bridge emits
                        // these when the FED entry is unused).
                        if (f.index == 0 && f.offset_x == 0.0f && f.offset_y == 0.0f) {
                            ++impl_->diag_3d_features_skipped_placeholder;
                            continue;
                        }

                        // Resolve entity_type → vis_type[0] via the class table.
                        const auto vis_type =
                            impl_->class_table_3d.vis_type_for(
                                static_cast<uint16_t>(f.index), 0);
                        if (vis_type <= 0) {
                            ++impl_->diag_3d_features_no_vistype;
                            continue;  // no model for this feature
                        }

                        // Lazy mesh build (cached by vis_type).
                        impl_->build_mesh_3d(vis_type);

                        auto cache_it = impl_->mesh_cache_3d.find(vis_type);
                        if (cache_it == impl_->mesh_cache_3d.end() ||
                            cache_it->second.meshes.empty()) {
                            ++impl_->diag_3d_features_no_mesh;
                            continue;
                        }

                        // Build the model matrix: translate to feature's ENU
                        // position, then rotate around the vertical (Raylib Y)
                        // axis by the feature's facing. The facing sign matches
                        // the existing footprint code (which uses
                        // rad = -facing_deg * pi/180 in the ENU X-Y plane —
                        // equivalent to Raylib Y-axis rotation by the same
                        // angle, since ENU's +Z_up corresponds to Raylib's
                        // +Y_up under the enu_to_rl swap).
                        const Vector3 pos_rh = enu_to_rl(
                            f.offset_x, f.offset_y, f.offset_z);
                        const float facing_rad = -f.facing *
                            (3.14159265358979323846f / 180.0f);
                        const Matrix rot = MatrixRotateY(facing_rad);
                        const Matrix model_matrix = MatrixMultiply(
                            MatrixTranslate(pos_rh.x, pos_rh.y, pos_rh.z), rot);

                        // Draw every mesh in the cache entry. Use the cached
                        // default material (with the white fallback texture)
                        // for untextured meshes; use the per-tex_id cached
                        // material for textured meshes. Either way the
                        // material's shader is the lit shader (set once at
                        // material creation in ensure_default_material_3d /
                        // upload_textures_3d).
                        int meshes_for_this_feature = 0;
                        int tris_for_this_feature = 0;
                        for (const auto& me : cache_it->second.meshes) {
                            if (me.mesh.triangleCount <= 0) continue;
                            const Material* mat_to_use = &impl_->default_mat_3d;
                            if (me.tex_id >= 0) {
                                auto tex_it = impl_->texture_cache_3d.find(me.tex_id);
                                if (tex_it != impl_->texture_cache_3d.end() &&
                                    tex_it->second.uploaded) {
                                    mat_to_use = &tex_it->second.material;
                                }
                            }
                            DrawMesh(me.mesh, *mat_to_use, model_matrix);
                            ++meshes_for_this_feature;
                            tris_for_this_feature += me.mesh.triangleCount;
                        }
                        if (meshes_for_this_feature > 0) {
                            ++impl_->diag_3d_features_drawn;
                            impl_->diag_3d_meshes_drawn += meshes_for_this_feature;
                            impl_->diag_3d_triangles_drawn += tris_for_this_feature;
                        } else {
                            ++impl_->diag_3d_features_no_mesh;
                        }
                    }
                }
            }

            rlEnableBackfaceCulling();
        }
        EndMode3D();

        // 2D overlay — labels projected from 3D positions.
        if (impl_->ground_layout_3d_show_labels) {
            std::vector<Label2D> labels;
            labels.reserve(64);
            collect_labels(impl_->ground_layout_3d_camera, g,
                           impl_->ground_layout_3d_show_parking, labels);
            draw_labels(labels);
        }
    }
    EndTextureMode();

    // --- Display the rendered texture in the ImGui window -----------------
    //
    // rlImGuiImageSize takes a Texture* (not RenderTexture*) and a width/
    // height. We pass the RenderTexture's .texture field and the
    // content-region size so the image scales to fit the window.
    rlImGuiImageSize(&impl_->ground_layout_3d_target.texture, img_w, img_h);

    // --- Mouse input (orbit + zoom) — only when the image is hovered ------
    //
    // We use ImGui::IsItemHovered() to detect hover on the last-drawn
    // image (rlImGuiImageSize acts like ImGui::Image). Once the user
    // starts dragging, we keep the drag active even if the cursor
    // briefly leaves the image bounds.
    const bool img_hovered = ImGui::IsItemHovered();
    if (img_hovered) {
        const Vector2 wheel = GetMouseWheelMoveV();
        if (wheel.y != 0.0f) {
            // Log-scale zoom.
            const float factor = std::exp(-wheel.y * ZOOM_SPEED * 30.0f);
            impl_->ground_layout_3d_distance =
                std::clamp(impl_->ground_layout_3d_distance * factor,
                           MIN_DISTANCE, MAX_DISTANCE);
        }
    }
    static bool s_dragging_3d = false;
    if (img_hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        s_dragging_3d = true;
    }
    if (s_dragging_3d && ImGui::IsMouseReleased(ImGuiMouseButton_Left)) {
        s_dragging_3d = false;
    }
    if (s_dragging_3d) {
        const ImVec2 delta = ImGui::GetIO().MouseDelta;
        impl_->ground_layout_3d_yaw   += delta.x * ORBIT_YAW_SPEED;
        impl_->ground_layout_3d_pitch -= delta.y * ORBIT_PITCH_SPEED;
        impl_->ground_layout_3d_pitch =
            std::clamp(impl_->ground_layout_3d_pitch, MIN_PITCH, MAX_PITCH);
    }

    // Note: update_camera_orbit() is now called at the TOP of the
    // function (before BeginTextureMode) so the very first frame after
    // a selection change renders correctly. We re-run it here so any
    // drag that happened during this frame is reflected immediately —
    // but the actual visible render happens on the NEXT frame using
    // these updated values. (Same one-frame latency as before for
    // interactive drag, but the initial render is now correct.)
    update_camera_orbit();

    // Footer — bbox + camera info.
    ImGui::TextDisabled("bbox: %.0f x %.0f ft   yaw: %.0f°   pitch: %.0f°   d: %.0f ft",
                        g.max_x - g.min_x, g.max_y - g.min_y,
                        impl_->ground_layout_3d_yaw   * 57.29578f,
                        impl_->ground_layout_3d_pitch * 57.29578f,
                        impl_->ground_layout_3d_distance);
    ImGui::TextDisabled("drag = orbit, scroll = zoom   (close window to free GPU texture)");

    ImGui::End();
}

} // namespace f4::viewer
