// f4-world-viewer/include/f4/viewer/viewer_app.hpp
//
// Interactive Raylib + Dear ImGui application for inspecting F4 world data.
//
// The viewer loads:
//   - A terrain JSON (produced by f4-terrain-convert) for the tile grid
//   - A world JSON (produced by f4-world-convert's cam2json) for objectives,
//     units, and campaign state
//
// Both can be loaded from the File menu, or passed as CLI args. The viewer
// also wraps the CLI converters in-process, so the user can directly import
// THEATER.* binary files or a .cam archive without leaving the app — this
// makes it a starting point for a future world editor.

#pragma once

#include <filesystem>
#include <functional>
#include <memory>
#include <string>

namespace f4::viewer {

class ViewerApp {
public:
    ViewerApp();
    ~ViewerApp();

    // Lifecycle
    void run();   // blocking — enters the Raylib event loop

    // File operations (callable from ImGui menus)
    void load_terrain_json(const std::filesystem::path& path);
    void load_world_json(const std::filesystem::path& path);
    void import_terrain_binary(const std::filesystem::path& terrain_dir);
    void import_cam_archive(const std::filesystem::path& cam_path);

    /// Set the initial camera position (grid coordinates) and zoom (pixels
    /// per grid unit). Call before run(). Useful for screenshots and for
    /// launching the viewer focused on a region of interest.
    void set_initial_camera(float center_x, float center_y, float zoom);

    /// Test/smoke-test helper: schedule a screenshot to be taken after `delay_sec`
    /// seconds. Useful for headless verification on CI / Linux dev boxes.
    void schedule_screenshot(float delay_sec, const std::string& path);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;

    // Internal helpers
    void handle_input();
    void draw_canvas();
    void draw_imgui();
    void open_file_dialog(const char* title, const char* filters,
                          std::function<void(const std::string&)> on_ok);
};

} // namespace f4::viewer
