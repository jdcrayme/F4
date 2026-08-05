// f4-models-viewer/src/viewer_state.hpp
//
// PRIVATE HEADER — internal to the f4-models-viewer library. Not installed,
// not visible to consumers. Every viewer .cpp file includes this so it can
// access ViewerApp::Impl (the pimpl struct that holds all render-loop state).

#pragma once

#include <f4/models_viewer/viewer_app.hpp>

#include <f4/models/model_database.hpp>
#include <f4/models/geometry.hpp>
#include <f4/install/installation.hpp>

#include <raylib.h>

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace f4::models_viewer {

// ── Coordinate conversion ─────────────────────────────────────────────────
// FreeFalcon uses LH Z-up. Raylib uses RH Y-up.
// to_raylib(x, y, z) = (x, z, -y)

inline Vector3 to_raylib(float x, float y, float z) {
    return {x, z, -y};
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

    // ── Display toggles ───────────────────────────────────────────────
    bool show_wireframe = false;
    bool show_grid = true;
    bool show_axes = true;
    bool show_bounding_sphere = false;
    bool show_aabb = false;

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

    // canvas3d.cpp
    void draw_canvas();
    void draw_bounding_volumes();

    // file_ops.cpp
    void load_model_files(const std::filesystem::path& hdr_path,
                          const std::filesystem::path& lod_path);
    void load_from_install();

    // imgui_panels.cpp
    void draw_imgui();
};

} // namespace f4::models_viewer
