// f4-models-viewer/include/f4/models_viewer/viewer_app.hpp
//
// Top-level 3D model viewer application. Uses Raylib + Dear ImGui
// for rendering and the f4-models library for KoreaObj.HDR/LOD parsing.
//
// Usage:
//   f4::models_viewer::ViewerApp app;
//   app.load_model("KoreaObj.HDR", "KoreaObj.LOD");
//   app.run();   // blocking Raylib event loop

#pragma once

#include <filesystem>
#include <memory>
#include <optional>

namespace f4::install { class Installation; }

namespace f4::models_viewer {

class ViewerApp {
public:
    ViewerApp();
    ~ViewerApp();

    /// Blocking Raylib event loop. Call after setting initial state.
    void run();

    // ── Install-aware API ─────────────────────────────────────────────

    /// Set the Falcon 4.0 install path and auto-locate KoreaObj files.
    /// Returns true if the install was detected successfully.
    bool set_install_path(const std::filesystem::path& path);

    /// Access the current installation (if any).
    const std::optional<f4::install::Installation>& installation() const noexcept;

    // ── Direct file API ───────────────────────────────────────────────

    /// Load model database from explicit HDR + LOD paths.
    void load_model(const std::filesystem::path& hdr_path,
                    const std::filesystem::path& lod_path);

    // ── Parent / LOD selection ────────────────────────────────────────

    /// Select a parent model by index (-1 = none).
    void select_parent(int index);

    /// Select a LOD level (0 = highest detail).
    void select_lod(int lod_index);

    // ── Test helpers ──────────────────────────────────────────────────

    /// Override the initial camera position (before run()).
    void set_initial_camera(const float eye[3], const float target[3]);

    /// Schedule a screenshot after `delay_sec` seconds, saved to `path`.
    /// Used by headless smoke tests.
    void schedule_screenshot(float delay_sec, const std::filesystem::path& path);

    // Impl is defined in the private viewer_state.hpp header.
    // It needs to be accessible by internal .cpp files that include that header.
    struct Impl;

private:
    std::unique_ptr<Impl> impl_;
};

} // namespace f4::models_viewer
