// f4-scenario-player/src/renderer.cpp
//
// Camera + scene drawing for the scenario player. Mirrors the structure
// of f4-models-viewer's camera3d.cpp + canvas3d.cpp, but adapted for the
// larger-scale world (camera distances in hundreds of feet, not tens)
// and the ENU coordinate frame.
//
// CRITICAL: Raylib defines `PI` as a preprocessor macro (raylib.h:110),
// which collides with `f4::math::PI` brought in by f4-flight-model's
// constants.hpp. We must include all f4-flight-model headers BEFORE
// raylib.h so the `using f4::math::PI;` declaration resolves against
// the real constant, not the macro. (Once the macro is defined, the
// `using` declaration breaks with a confusing parse error.)

#include "viewer_state.hpp"

#include <f4/simulation/visual_model_component.hpp>
#include <f4/entities/entity.hpp>
#include <f4/flight/flight_model_component.hpp>
#include <f4/flight/angle.hpp>
#include <f4/models/model_database.hpp>

// Now safe to include Raylib (PI macro won't break the flight headers).
#include <imgui.h>
#include <raylib.h>
#include <raymath.h>
#include <rlgl.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace f4::scenario_player {

// ── Constants ──────────────────────────────────────────────────────────────
static constexpr float MY_DEG2RAD = 3.14159265358979323846f / 180.0f;
static constexpr float MIN_PITCH = -89.0f;
static constexpr float MAX_PITCH =  89.0f;
static constexpr float MIN_DISTANCE = 1.0f;
static constexpr float PAN_SPEED = 0.003f;

// Colors
static constexpr Color GRID_COLOR = {60, 60, 60, 255};
static constexpr Color SKY_COLOR  = {135, 175, 220, 255};  // sky blue
static constexpr Color GROUND_COLOR = {50, 70, 35, 255};   // green grass

// ── Camera ─────────────────────────────────────────────────────────────────

void PlayerApp::Impl::update_camera_from_orbit() {
    const float yaw_rad = cam_yaw * MY_DEG2RAD;
    const float pitch_rad = cam_pitch * MY_DEG2RAD;
    const float cx = cam_distance * std::cos(pitch_rad) * std::sin(yaw_rad);
    const float cy = cam_distance * std::sin(pitch_rad);
    const float cz = cam_distance * std::cos(pitch_rad) * std::cos(yaw_rad);
    camera.position = {cam_target.x + cx, cam_target.y + cy, cam_target.z + cz};
    camera.target = cam_target;
    camera.up = {0, 1, 0};
    camera.fovy = 45.0f;
    camera.projection = CAMERA_PERSPECTIVE;
}

void PlayerApp::Impl::handle_camera_input() {
    const ImGuiIO& io = ImGui::GetIO();
    if (!io.WantCaptureKeyboard) {
        if (IsKeyPressed(KEY_F)) fit_to_aircraft();
        if (IsKeyPressed(KEY_R)) reset_camera();
        if (IsKeyPressed(KEY_SPACE)) {
            paused = !paused;
            status_msg = paused ? "Paused" : "Running";
        }
    }
    if (io.WantCaptureMouse) return;

    const Vector2 mouse = GetMousePosition();

    if (IsMouseButtonDown(MOUSE_BUTTON_LEFT)) {
        if (!orbit_dragging) {
            orbit_dragging = true;
            drag_start = mouse;
            drag_yaw0 = cam_yaw;
            drag_pitch0 = cam_pitch;
        } else {
            const float dx = mouse.x - drag_start.x;
            const float dy = mouse.y - drag_start.y;
            cam_yaw = drag_yaw0 - dx * 0.3f;
            cam_pitch = drag_pitch0 + dy * 0.3f;
            if (cam_pitch < MIN_PITCH) cam_pitch = MIN_PITCH;
            if (cam_pitch > MAX_PITCH) cam_pitch = MAX_PITCH;
            update_camera_from_orbit();
        }
    } else {
        orbit_dragging = false;
    }

    if (IsMouseButtonDown(MOUSE_BUTTON_RIGHT)) {
        if (!pan_dragging) {
            pan_dragging = true;
            drag_start = mouse;
            drag_target0 = cam_target;
        } else {
            const float dx = mouse.x - drag_start.x;
            const float dy = mouse.y - drag_start.y;
            const Vector3 delta = {camera.target.x - camera.position.x,
                                     camera.target.y - camera.position.y,
                                     camera.target.z - camera.position.z};
            const float len = std::sqrt(delta.x*delta.x + delta.y*delta.y + delta.z*delta.z);
            const Vector3 fwd = {delta.x/len, delta.y/len, delta.z/len};
            const Vector3 world_up = {0, 1, 0};
            const Vector3 r = {fwd.y*world_up.z - fwd.z*world_up.y,
                               fwd.z*world_up.x - fwd.x*world_up.z,
                               fwd.x*world_up.y - fwd.y*world_up.x};
            const float rlen = std::sqrt(r.x*r.x + r.y*r.y + r.z*r.z);
            const Vector3 right = {r.x/rlen, r.y/rlen, r.z/rlen};
            const Vector3 u = {right.y*fwd.z - right.z*fwd.y,
                               right.z*fwd.x - right.x*fwd.z,
                               right.x*fwd.y - right.y*fwd.x};
            const float ulen = std::sqrt(u.x*u.x + u.y*u.y + u.z*u.z);
            const Vector3 up = {u.x/ulen, u.y/ulen, u.z/ulen};
            const float pan_scale = cam_distance * PAN_SPEED;
            cam_target.x = drag_target0.x - right.x * dx * pan_scale + up.x * dy * pan_scale;
            cam_target.y = drag_target0.y - right.y * dx * pan_scale + up.y * dy * pan_scale;
            cam_target.z = drag_target0.z - right.z * dx * pan_scale + up.z * dy * pan_scale;
            update_camera_from_orbit();
        }
    } else {
        pan_dragging = false;
    }

    const float wheel = GetMouseWheelMove();
    if (wheel != 0.0f) {
        cam_distance *= (1.0f - wheel * 0.1f);
        if (cam_distance < MIN_DISTANCE) cam_distance = MIN_DISTANCE;
        update_camera_from_orbit();
    }
}

void PlayerApp::Impl::fit_to_aircraft() {
    if (!sim_initialized) return;
    auto h = f4::entities::EntityHandle(sim->aircraft_entity(), &sim->world());
    auto* tf = h.get<f4::entities::TransformComponent>();
    if (!tf) return;
    cam_target = enu_to_raylib_v3(tf->position.x, tf->position.y, tf->position.z);
    cam_distance = 80.0f;  // close enough to see the F-16 in detail
    update_camera_from_orbit();
}

void PlayerApp::Impl::reset_camera() {
    cam_yaw = 45.0f;
    cam_pitch = 25.0f;
    // Default target: the parking spot (scenario aircraft's spawn position).
    if (sim_initialized && !scenario.aircraft.empty()) {
        const auto& p = scenario.aircraft.front().parking_spot;
        cam_target = enu_to_raylib_v3(p.x, p.y, p.z);
    } else {
        cam_target = {0, 0, 0};
    }
    cam_distance = 250.0f;
    update_camera_from_orbit();
}

// ── Lit shader (same source as f4-models-viewer's canvas3d.cpp) ────────────
// Forward-declared here so we can lazily compile it on first draw_scene().

static const char* kLitShaderVS =
    "#version 330\n"
    "in vec3 vertexPosition;\n"
    "in vec2 vertexTexCoord;\n"
    "in vec4 vertexColor;\n"
    "in vec3 vertexNormal;\n"
    "uniform mat4 mvp;\n"
    "uniform mat4 matModel;\n"
    "out vec2 fragTexCoord;\n"
    "out vec4 fragColor;\n"
    "out vec3 fragNormal;\n"
    "void main() {\n"
    "    fragTexCoord = vertexTexCoord;\n"
    "    fragColor = vertexColor;\n"
    "    fragNormal = normalize(mat3(matModel) * vertexNormal);\n"
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
    "    if (tex.a < 0.5) discard;\n"
    "    vec3 N = normalize(fragNormal);\n"
    "    vec3 L = normalize(lightDir);\n"
    "    float NdotL = max(dot(N, L), 0.0);\n"
    "    vec4 light = ambient + lightColor * NdotL;\n"
    "    finalColor = tex * colDiffuse * fragColor * light;\n"
    "}\n";

bool PlayerApp::Impl::ensure_lit_shader() {
    if (lit_shader_loaded) return lit_shader.id != 0;
    lit_shader = LoadShaderFromMemory(kLitShaderVS, kLitShaderFS);
    lit_shader_loaded = true;
    if (lit_shader.id == 0) {
        status_msg = "warning: lit shader failed to compile (falling back to unlit)";
        return false;
    }
    lit_shader_dir_loc     = GetShaderLocation(lit_shader, "lightDir");
    lit_shader_color_loc   = GetShaderLocation(lit_shader, "lightColor");
    lit_shader_ambient_loc = GetShaderLocation(lit_shader, "ambient");
    return true;
}

// ── Mesh building ──────────────────────────────────────────────────────────
// Reuses f4-models-viewer's scene.cpp logic in simplified form: convert
// each f4::models::Mesh to a Raylib ::Mesh, resolving vertex colors
// through the ColorBank (Prim.rgba is an index, NOT packed ABGR).

static Color resolve_vertex_color(uint32_t color_index,
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

void PlayerApp::Impl::build_aircraft_meshes() {
    // Phase 2A: this is now a thin wrapper that ensures the aircraft's
    // mesh is in the cache. The actual mesh-building logic lives in
    // build_mesh_for_model(), shared between aircraft and airfield features.
    if (!sim_initialized) { meshes_built = true; return; }

    auto h = f4::entities::EntityHandle(sim->aircraft_entity(), &sim->world());
    auto* vis = h.get<f4::simulation::VisualModelComponent>();
    if (!vis || !vis->model_record) {
        status_msg = "No visual model on aircraft entity";
        meshes_built = true;
        return;
    }

    auto& db = const_cast<f4::models::ModelDatabase&>(sim->model_db());
    const auto* base = db.model(0);
    const int parent_index = base ? static_cast<int>(vis->model_record - base) : -1;
    if (parent_index < 0) {
        status_msg = "Cannot resolve model index from pointer";
        meshes_built = true;
        return;
    }

    build_mesh_for_model(parent_index);

    meshes_built = true;
    auto it = mesh_cache.find(parent_index);
    if (it != mesh_cache.end()) {
        int n_textured = 0;
        for (const auto& me : it->second.meshes) if (me.tex_id >= 0) ++n_textured;
        status_msg = "F-16 loaded: " + std::to_string(it->second.meshes.size()) +
                     " meshes, " + std::to_string(n_textured) + " textured";
    }
}

void PlayerApp::Impl::build_mesh_for_model(int parent_index) {
    // Phase 2A: build (or skip if already cached) the Raylib Mesh objects
    // for one KoreaObj model. The result is stored in mesh_cache[parent_index]
    // so multiple entities sharing the same vis_type reuse one upload.
    //
    // Requires the GL context (UploadMesh). Called lazily from
    // draw_visual_entities() the first time an entity with a previously-
    // unseen parent_index is encountered, and eagerly from
    // build_aircraft_meshes() at startup for the primary aircraft.
    if (parent_index < 0) return;
    auto it = mesh_cache.find(parent_index);
    if (it != mesh_cache.end() && it->second.built) return;  // already cached

    auto& db = const_cast<f4::models::ModelDatabase&>(sim->model_db());
    const auto* rec = db.model(parent_index);
    if (!rec || rec->lods.empty()) {
        if (it != mesh_cache.end()) it->second.built = true;
        else mesh_cache[parent_index].built = true;
        return;
    }
    const int lod = 0;  // lock to LOD 0 (highest detail) for now
    auto err = db.parse_lod(parent_index, lod);
    if (!err.empty()) {
        if (it != mesh_cache.end()) it->second.built = true;
        else mesh_cache[parent_index].built = true;
        return;
    }

    // Use a default ModelState (texture_set=0, no switches). The aircraft's
    // per-instance gear switch is baked into the extracted geometry at
    // build time, so a future phase that needs to animate the gear will
    // need to invalidate and rebuild the cache entry. For Phase 2A we
    // accept static gear (already down at spawn, stays down during taxi).
    f4::models::ModelState default_state;
    default_state.texture_set = 0;
    default_state.n_texture_sets = std::max(1, static_cast<int>(rec->n_texture_sets));

    auto geom = db.extract_model_geometry(parent_index, lod, default_state);
    if (geom.meshes.empty()) {
        mesh_cache[parent_index].built = true;
        return;
    }

    const auto& cb = db.color_bank();

    MeshCacheEntry entry;
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
            const Vector3 pos = model_vertex_to_raylib_v3(v.position.x, v.position.y, v.position.z);
            rm.vertices[i*3+0] = pos.x;
            rm.vertices[i*3+1] = pos.y;
            rm.vertices[i*3+2] = pos.z;
            const Vector3 nrm = model_vertex_to_raylib_v3(v.normal.x, v.normal.y, v.normal.z);
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

        MeshEntry me;
        me.mesh = rm;
        me.tex_id = src.tex_id;
        entry.meshes.push_back(me);
    }

    entry.built = true;
    mesh_cache[parent_index] = std::move(entry);

    // Upload any new textures referenced by this model's meshes.
    upload_textures();
}

void PlayerApp::Impl::upload_textures() {
    if (!sim_initialized) return;
    auto& db = const_cast<f4::models::ModelDatabase&>(sim->model_db());

    // Phase 2A: walk every cached mesh entry across all models. The
    // texture_cache keys by tex_id, so shared textures across models are
    // only uploaded once.
    for (auto& [parent_idx, cache_entry] : mesh_cache) {
        for (auto& me : cache_entry.meshes) {
            if (me.tex_id < 0) continue;
            if (texture_cache.count(me.tex_id)) continue;

            const auto* decoded = db.fetch_texture(me.tex_id);
            if (!decoded || !decoded->valid()) {
                TexCacheEntry ce; ce.uploaded = false;
                texture_cache[me.tex_id] = ce;
                continue;
            }

            Image img = {};
            img.data = RL_MALLOC(decoded->width * decoded->height * 4);
            if (!img.data) {
                TexCacheEntry ce; ce.uploaded = false;
                texture_cache[me.tex_id] = ce;
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

            TexCacheEntry ce;
            ce.texture = tex;
            ce.material = mat;
            ce.has_alpha = decoded->has_alpha;
            ce.uploaded = true;
            texture_cache[me.tex_id] = ce;
            UnloadImage(img);
        }
    }
}

void PlayerApp::Impl::unload_textures() {
    for (auto& [id, ce] : texture_cache) {
        if (ce.uploaded) {
            UnloadTexture(ce.texture);
            ce.material.maps[MATERIAL_MAP_DIFFUSE].texture = {};
        }
    }
    texture_cache.clear();
}

void PlayerApp::Impl::unload_meshes() {
    unload_textures();
    // Phase 2A: walk every model in the mesh cache.
    for (auto& [parent_idx, cache_entry] : mesh_cache) {
        for (auto& me : cache_entry.meshes) {
            UnloadMesh(me.mesh);
        }
        cache_entry.meshes.clear();
        cache_entry.built = false;
    }
    mesh_cache.clear();
    meshes_built = false;
}

// ── draw_scene ─────────────────────────────────────────────────────────────

static void draw_grid_enu(float extent, float step) {
    // Grid on the Y=0 plane (Raylib up). Each line is one row/col of the grid.
    for (float i = -extent; i <= extent; i += step) {
        DrawLine3D({i, 0, -extent}, {i, 0, extent}, GRID_COLOR);
        DrawLine3D({-extent, 0, i}, {extent, 0, i}, GRID_COLOR);
    }
}

// Helper: convert an ENU WorldPosition to a Raylib Vector3.
static inline Vector3 to_rh(const f4::geo::WorldPosition& p) {
    return enu_to_raylib_v3(p.x, p.y, p.z);
}

// Helper: convert a Raylib Color from float RGBA (0..1) to ::Color.
// Reads r, g, blue, a_ (the struct uses `blue` instead of `b` to avoid
// collision with GeoLine::b which is the segment endpoint).
static inline Color to_raylib_color(const float c[4]) {
    return Color{
        static_cast<unsigned char>(c[0] * 255.0f),  // r
        static_cast<unsigned char>(c[1] * 255.0f),  // g
        static_cast<unsigned char>(c[2] * 255.0f),  // blue
        static_cast<unsigned char>(c[3] * 255.0f)   // a_
    };
}

// Draw a flat quad (assumed coplanar) on the ground. Raylib has no
// DrawQuad3D, so we draw it as two triangles via rlgl primitives.
static void draw_quad_3d(const GeoQuad& q) {
    const Color c = to_raylib_color(&q.r);
    // Two triangles: (p0,p1,p2) and (p0,p2,p3)
    DrawTriangle3D(to_rh(q.p[0]), to_rh(q.p[1]), to_rh(q.p[2]), c);
    DrawTriangle3D(to_rh(q.p[0]), to_rh(q.p[2]), to_rh(q.p[3]), c);
}

// Draw a small cube at a marker position.
static void draw_marker(const GeoMarker& m) {
    const Vector3 c = to_rh(m.center);
    const float s = m.size_ft;
    const Color col = to_raylib_color(&m.r);
    DrawCube(c, s, s, s, col);
    DrawCubeWires(c, s, s, s, BLACK);
}

void PlayerApp::Impl::draw_airport() {
    if (!airport_built || !show_airport) return;

    // Runway surface
    draw_quad_3d(airport.runway_surface);

    // Threshold bars
    for (const auto& b : airport.threshold_bars) {
        draw_quad_3d(b);
    }

    // Centerline dashes
    for (const auto& d : airport.centerline_dashes) {
        draw_quad_3d(d);
    }

    // Taxi route lines
    if (show_taxi_route) {
        for (const auto& l : airport.taxi_route_lines) {
            const Color c = to_raylib_color(&l.r);
            DrawLine3D(to_rh(l.a), to_rh(l.b), c);
        }
    }

    // Markers
    draw_marker(airport.parking_spot);
    draw_marker(airport.hold_short);
    draw_marker(airport.runway_end);

    // Compass rose
    if (show_compass) {
        for (const auto& l : airport.compass_rose) {
            const Color c = to_raylib_color(&l.r);
            DrawLine3D(to_rh(l.a), to_rh(l.b), c);
        }
    }
}

void PlayerApp::Impl::draw_aircraft() {
    // Phase 2A: legacy entry point — draws only the primary aircraft.
    // Kept for compatibility; the real work now happens in draw_visual_entities().
    if (!show_aircraft || !sim_initialized) return;
    draw_visual_entities();
}

void PlayerApp::Impl::draw_visual_entities() {
    // Phase 2A: walk every entity that has a VisualModelComponent and draw
    // it. This unifies the aircraft and airfield-feature draw paths — both
    // are just entities with a TransformComponent + VisualModelComponent,
    // and the renderer doesn't care whether they also have a FlightModelComponent.
    //
    // The mesh cache is keyed by parent_index (the KoreaObj model number),
    // so multiple entities sharing the same vis_type reuse one GPU upload.
    // The cache is built lazily here — the first time we see a new
    // parent_index, build_mesh_for_model() uploads it.
    if (!sim_initialized) return;
    if (!show_aircraft && !show_airport) return;  // both toggles off → skip

    // Collect all VMC-bearing entities. with_component<>() returns a fresh
    // vector each call (O(N) walk over the world), so we do it once per
    // frame, not once per mesh.
    const auto entities = sim->world().with_component<f4::simulation::VisualModelComponent>();
    if (entities.empty()) return;

    auto& db = const_cast<f4::models::ModelDatabase&>(sim->model_db());
    const auto* base = db.model(0);

    // Set up lighting once per frame (shared across all entities).
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

    if (ensure_lit_shader()) {
        lighting_active = true;
        if (lit_shader_dir_loc >= 0) {
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

    // CRITICAL: Disable backface culling (same as f4-models-viewer).
    // FreeFalcon's models were authored without consistent winding.
    rlDisableBackfaceCulling();
    BeginBlendMode(BLEND_ALPHA);

    Material default_mat = LoadMaterialDefault();
    default_mat.maps[MATERIAL_MAP_DIFFUSE].color = WHITE;
    if (lighting_active) default_mat.shader = lit_shader;

    // Determine which entity is the "primary aircraft" so we can apply the
    // show_aircraft toggle only to it (features are gated by show_airport).
    const auto primary_aircraft_id = sim->aircraft_entity();

    for (const auto eid : entities) {
        auto h = f4::entities::EntityHandle(eid, &sim->world());
        auto* vis = h.get<f4::simulation::VisualModelComponent>();
        auto* tf  = h.get<f4::entities::TransformComponent>();
        if (!vis || !vis->model_record || !tf) continue;

        // Toggle gating: aircraft ↔ show_aircraft, features ↔ show_airport.
        // The primary aircraft's entity ID matches aircraft_entity(); all
        // other VMC-bearing entities are features.
        const bool is_aircraft = (eid.value == primary_aircraft_id.value);
        if (is_aircraft && !show_aircraft) continue;
        if (!is_aircraft && !show_airport) continue;

        // Resolve parent_index from the model_record pointer.
        const int parent_index = base ? static_cast<int>(vis->model_record - base) : -1;
        if (parent_index < 0) continue;

        // Lazy mesh build: if this model isn't cached yet, build it now.
        // This handles features that weren't pre-built at startup.
        build_mesh_for_model(parent_index);

        auto cache_it = mesh_cache.find(parent_index);
        if (cache_it == mesh_cache.end() || cache_it->second.meshes.empty()) continue;

        // Convert ENU position to Raylib RH Y-up.
        const Vector3 pos_rh = enu_to_raylib_v3(tf->position.x, tf->position.y, tf->position.z);

        // Convert ENU quaternion to Raylib RH Y-up quaternion.
        // The TransformComponent's quaternion (qw, qx, qy, qz) is body→world
        // in ENU (x=east, y=north, z=up). The basis change ENU → RH Y-up is
        // (x, y, z)_enu → (x, z, -y)_rh, which gives the Hamilton-form rule
        //   q_rh (Hamilton w,x,y,z) = (qw, qx, qz, -qy)
        //
        // IMPORTANT: Raylib's Quaternion struct is {x, y, z, w} — NOT {w,x,y,z}.
        // The previous code initialized it as {qw, qx, qz, -qy} which put the
        // scalar `qw` into the x field and produced a 180° X-rotation for an
        // identity input. The correct initialization puts each Hamilton
        // component into the matching Raylib field.
        Quaternion q_rh = {
            static_cast<float>(tf->qx),    // q.x  (was tf->qw — the bug)
            static_cast<float>(tf->qz),    // q.y  (was tf->qx)
            static_cast<float>(-tf->qy),   // q.z  (was tf->qz)
            static_cast<float>(tf->qw)     // q.w  (was -tf->qy)
        };
        const float qlen = std::sqrt(q_rh.x*q_rh.x + q_rh.y*q_rh.y + q_rh.z*q_rh.z + q_rh.w*q_rh.w);
        if (qlen > 0.0001f) {
            q_rh.x /= qlen; q_rh.y /= qlen; q_rh.z /= qlen; q_rh.w /= qlen;
        }

        const Matrix model_matrix = MatrixMultiply(
            MatrixTranslate(pos_rh.x, pos_rh.y, pos_rh.z),
            QuaternionToMatrix(q_rh)
        );

        for (const auto& me : cache_it->second.meshes) {
            if (me.mesh.triangleCount <= 0) continue;
            const Material* mat_to_use = &default_mat;
            if (me.tex_id >= 0) {
                auto tex_it = texture_cache.find(me.tex_id);
                if (tex_it != texture_cache.end() && tex_it->second.uploaded) {
                    mat_to_use = &tex_it->second.material;
                    if (lighting_active) {
                        const_cast<Material*>(mat_to_use)->shader = lit_shader;
                    }
                }
            }
            DrawMesh(me.mesh, *mat_to_use, model_matrix);
        }
    }

    EndBlendMode();
    rlEnableBackfaceCulling();
}

void PlayerApp::Impl::draw_hud() {
    if (!show_hud) return;

    const int x = 12;
    int y = 12;
    const int line_h = 18;
    const int pad = 8;

    std::vector<std::string> lines;
    char buf[256];

    std::snprintf(buf, sizeof(buf), "Scenario: %s", scenario.name.c_str());
    lines.emplace_back(buf);

    if (sim_initialized) {
        std::snprintf(buf, sizeof(buf), "Tick: %llu   Sim time: %.1fs",
                      static_cast<unsigned long long>(sim->tick_count()),
                      sim->sim_time_s());
        lines.emplace_back(buf);
        std::snprintf(buf, sizeof(buf), "State: %s   FPS: %d",
                      paused ? "PAUSED" : "RUNNING", GetFPS());
        lines.emplace_back(buf);

        // Aircraft state
        auto h = f4::entities::EntityHandle(sim->aircraft_entity(), &sim->world());
        auto* fm = h.get<f4::flight::FlightModelComponent>();
        if (fm) {
            const auto& s = fm->state();
            std::snprintf(buf, sizeof(buf), "KCAS: %.1f   AGL: %.0fft",
                          s.vcas, -s.kin.z - s.gear.groundZ_ft);
            lines.emplace_back(buf);
            std::snprintf(buf, sizeof(buf), "Hdg: %.0f\u00B0   Pitch: %.1f\u00B0   Roll: %.1f\u00B0",
                          f4::flight::to_degrees(s.kin.psi),
                          f4::flight::to_degrees(s.kin.theta),
                          f4::flight::to_degrees(s.kin.phi));
            lines.emplace_back(buf);
            std::snprintf(buf, sizeof(buf), "Gear: %s   On ground: %s",
                          s.aero.gearPos > 0.5 ? "DOWN" : "UP",
                          !s.gear.inAir ? "yes" : "no");
            lines.emplace_back(buf);
        }

        if (!scenario.aircraft.empty()) {
            std::snprintf(buf, sizeof(buf), "Callsign: %s   (%s)",
                          scenario.aircraft.front().callsign.c_str(),
                          scenario.aircraft.front().aircraft_name.c_str());
            lines.emplace_back(buf);
        }
    }

    if (!status_msg.empty()) {
        lines.emplace_back("");
        lines.emplace_back("Status: " + status_msg);
    }

    // Controls hint
    lines.emplace_back("");
    lines.emplace_back("Space: pause/resume   F: focus aircraft   R: reset view");

    int max_w = 0;
    for (const auto& line : lines) {
        const int w = MeasureText(line.c_str(), 14);
        if (w > max_w) max_w = w;
    }
    const int bg_h = static_cast<int>(lines.size()) * line_h + pad * 2;
    const int bg_w = max_w + pad * 2;

    DrawRectangle(x, y, bg_w, bg_h, { 0, 0, 0, 180 });
    DrawRectangleLines(x, y, bg_w, bg_h, { 255, 255, 255, 80 });

    int line_y = y + pad;
    for (const auto& line : lines) {
        DrawText(line.c_str(), x + pad, line_y, 14, RAYWHITE);
        line_y += line_h;
    }
}

void PlayerApp::Impl::draw_scene() {
    // Background sky + ground
    ClearBackground(SKY_COLOR);

    BeginMode3D(camera);

    // Ground plane (a big green quad at Y=0)
    DrawPlane({0, 0, 0}, {8000, 8000}, GROUND_COLOR);

    if (show_grid) {
        draw_grid_enu(2000.0f, 100.0f);
    }

    if (show_axes) {
        // RGB axes at the parking spot (or origin)
        Vector3 origin = {0, 0, 0};
        if (sim_initialized && !scenario.aircraft.empty()) {
            const auto& p = scenario.aircraft.front().parking_spot;
            origin = enu_to_raylib_v3(p.x, p.y, p.z);
        }
        DrawLine3D(origin, {origin.x + 50, origin.y, origin.z}, RED);
        DrawLine3D(origin, {origin.x, origin.y + 50, origin.z}, GREEN);
        DrawLine3D(origin, {origin.x, origin.y, origin.z + 50}, BLUE);
    }

    draw_airport();
    draw_visual_entities();

    EndMode3D();

    draw_hud();
}

} // namespace f4::scenario_player
