// f4-models-viewer/src/viewer_state.hpp
//
// PRIVATE HEADER — internal to the f4-models-viewer library. Not installed,
// not visible to consumers. Every viewer .cpp file includes this so it can
// access ViewerApp::Impl (the pimpl struct that holds all render-loop state).

#pragma once

#include <f4/models_viewer/viewer_app.hpp>

#include <f4/models/model_database.hpp>
#include <f4/models/geometry.hpp>
#include <f4/models/texture.hpp>
#include <f4/install/installation.hpp>

#include <raylib.h>

#include <filesystem>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace f4::models_viewer {

// ── Coordinate conversion ─────────────────────────────────────────────────
// FreeFalcon's BSP model data uses LH Y-up (DirectX rendering convention):
//   +X = right,  +Y = up,  +Z = forward (into screen)
// Raylib uses RH Y-up:
//   +X = right,  +Y = up,  +Z = toward viewer (out of screen)
//
// Conversion: to_raylib(x, y, z) = (x, y, -z)
//   X → X   (right stays right)
//   Y → Y   (up stays up)
//   Z → -Z  (LH forward → RH backward; negating one axis flips handedness)
//
// NOTE: The world/terrain system uses LH Z-up (+X north, +Y east, +Z up),
// but the 3D model vertex data in BSP trees follows the rendering convention
// (LH Y-up), which is what this conversion handles. The docs in
// MODEL_VIEWER_IMPLEMENTATION_PLAN.md §6.6 incorrectly state the model data
// is Z-up; the DOF rotation matrices prove it is Y-up (dof_rotation maps
// local Z → parent Y for rotors, which only makes sense in Y-up).

inline Vector3 to_raylib(float x, float y, float z) {
    return {x, -z, y};
}

// ── ViewerApp::Impl ───────────────────────────────────────────────────────
// All state the render loop touches, in one struct. Fields are grouped
// by concern so a reader can find what they need without scanning the
// whole struct.

struct ViewerApp::Impl {
    // ── Window ────────────────────────────────────────────────────────
    int window_w = 1600;
    int window_h = 900;
    bool should_exit = false;

    // ── Orbit Camera ──────────────────────────────────────────────────
    Camera3D camera = {};
    float cam_yaw = 45.0f;          // degrees, horizontal orbit
    float cam_pitch = 30.0f;        // degrees, vertical orbit (clamped)
    float cam_distance = 100.0f;    // distance from target
    Vector3 cam_target = {0, 0, 0}; // point the camera orbits around
    bool orbit_dragging = false;    // left-drag in progress
    bool pan_dragging = false;      // right-drag in progress
    Vector2 drag_start = {0, 0};    // mouse position at drag start
    float drag_yaw0 = 0;            // yaw at drag start
    float drag_pitch0 = 0;          // pitch at drag start
    Vector3 drag_target0 = {};      // target at drag start
    bool initial_camera_set = false;

    // ── Data ──────────────────────────────────────────────────────────
    f4::models::ModelDatabase db;
    bool doc_loaded = false;
    int selected_parent = -1;
    int selected_lod = 0;
    f4::models::ModelState model_state;

    // ── Render cache ──────────────────────────────────────────────────
    std::vector<::Mesh> raylib_meshes;
    bool meshes_dirty = true;
    std::size_t total_tri_count = 0;  // across all meshes

    // ── Texture cache ─────────────────────────────────────────────────
    // Each mesh entry tracks its tex_id for per-mesh material lookup.
    struct RaylibMeshEntry {
        ::Mesh mesh = {};
        int tex_id = -1;              ///< texture bank index for this mesh
    };
    std::vector<RaylibMeshEntry> mesh_entries;

    // GPU texture cache: tex_id → uploaded Texture2D.
    // Populated lazily in upload_textures() when a mesh needs a texture.
    struct TexCacheEntry {
        ::Texture2D texture = {};
        ::Material material = {};
        bool has_alpha = false;
        bool uploaded = false;        ///< true once texture is on GPU
    };
    std::unordered_map<int, TexCacheEntry> texture_cache;

    // ── Texture set selection ─────────────────────────────────────────
    int selected_texture_set = 0;     ///< 0=summer, 1=winter, 2=desert

    // ── Lines + Points (LineF / PointF primitives) ───────────────────
    // These don't fit Raylib's ::Mesh triangle-list model, so we draw them
    // separately via DrawLine3D / DrawCube in canvas3d.cpp.
    struct LineSeg {
        Vector3 a, b;
        Color   color;
    };
    struct PointMark {
        Vector3 p;
        Color   color;
        float   size;
    };
    std::vector<LineSeg>   line_segs;
    std::vector<PointMark> point_marks;

    // ── Display toggles ───────────────────────────────────────────────
    bool show_wireframe = false;
    bool show_grid = true;
    bool show_axes = true;
    bool show_bounding_sphere = false;
    bool show_aabb = false;
    bool show_stats_overlay = true;       ///< top-left canvas overlay
    bool show_light_gizmo = true;          ///< directional-light arrow

    // ── Lighting ──────────────────────────────────────────────────────
    // When lighting_enabled is true, meshes are drawn with a custom lit
    // shader (loaded lazily on first draw_canvas call) that implements
    // lambertian diffuse + ambient. Most FreeFalcon models look
    // dramatically better with a single directional light because their
    // vertex normals were authored for it. When the shader fails to
    // compile (e.g. on a headless GL context), the viewer silently
    // falls back to Raylib's default unlit shader.
    bool   lighting_enabled = true;
    Vector3 light_direction = { 0.65f, -1.0f, 0.35f };  // world-space, normalized lazily
    float  light_intensity  = 1.0f;
    Color  ambient_color    = { 60, 60, 70, 255 };
    Color  light_color      = { 255, 250, 235, 255 };
    Shader lit_shader       = {};
    bool   lit_shader_loaded = false;  ///< true once we've tried (id may still be 0 on failure)
    int    lit_shader_dir_loc       = -1;
    int    lit_shader_color_loc     = -1;
    int    lit_shader_ambient_loc   = -1;

    // ── Animation ─────────────────────────────────────────────────────
    // Per-DOF auto-animation. Lets the user click "Auto" on a DOF slider
    // (typically the rotor) to make it spin continuously. Useful for
    // visually validating the DOF transform pipeline.
    struct AnimationTrack {
        int   dof_number = -1;     ///< which DOF this track drives
        bool  enabled    = false;  ///< auto-animate this DOF
        float speed      = 1.0f;   ///< cycles per second (1 = 2π rad/s)
        float phase      = 0.0f;   ///< accumulated phase (radians)
        bool  wrap_2pi   = true;   ///< true: 0..2π; false: min..max ping-pong
    };
    std::vector<AnimationTrack> animations;
    bool animation_paused = false;

    // ── Per-frame stats (reset every frame in draw_canvas) ────────────
    int   stats_draw_calls = 0;
    int   stats_meshes_drawn = 0;
    std::size_t stats_vertices_drawn = 0;

    // ── ColorBank thumbnail cache ─────────────────────────────────────
    // A single RGBA8 Image (Texture2D on the GPU) holding a horizontal
    // strip of every ColorBank entry, rebuilt lazily when the bank changes.
    // Used by the Materials panel to render clickable swatches.
    Texture2D colorbank_texture = {};
    bool      colorbank_dirty   = true;
    int       colorbank_cols    = 32;  ///< swatches per row

    // ── Status bar ────────────────────────────────────────────────────
    std::string status_msg;

    // ── Screenshot ────────────────────────────────────────────────────
    bool screenshot_pending = false;
    double screenshot_at = 0.0;
    std::filesystem::path screenshot_path;

    // ── Install ───────────────────────────────────────────────────────
    std::optional<f4::install::Installation> install;

    // ── Model list scroll ─────────────────────────────────────────────
    int model_list_scroll_to = -1;  // set by select_parent to auto-scroll

    // ── Functions defined in other .cpp files ─────────────────────────

    // camera3d.cpp
    void update_camera_from_orbit();
    void handle_camera_input();
    void fit_to_model();
    void reset_camera();

    // scene.cpp
    void rebuild_meshes();
    void unload_meshes();
    void upload_textures();
    void unload_textures();

    // canvas3d.cpp
    void draw_canvas();
    void draw_bounding_volumes();
    void draw_stats_overlay();
    void draw_light_gizmo();

    // file_ops.cpp
    void load_model_files(const std::filesystem::path& hdr_path,
                          const std::filesystem::path& lod_path);
    void load_from_install();

    // animation.cpp logic (lives in viewer_app.cpp for now — small)
    void tick_animation(float dt);
    void reset_animations();

    // colorbank.cpp logic (lives in imgui_panels.cpp for now)
    void rebuild_colorbank_texture();
    void unload_colorbank_texture();

    // imgui_panels.cpp
    void draw_imgui();
};

} // namespace f4::models_viewer
