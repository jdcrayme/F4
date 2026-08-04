// f4-world-viewer/include/f4/viewer/viewer_app.hpp
//
// Interactive Raylib + Dear ImGui application for inspecting F4 world data.
//
// CONOPS — the primary user-facing flow is:
//
//   1. First launch: File > Set Install Path... → native folder picker.
//      The viewer calls f4::install::Installation::detect() and shows
//      a summary (theaters + campaigns found, FALCON4.ct located).
//      The path is cached to settings.json so the user only picks it once.
//
//   2. File > Open Campaign... → modal with Theater + Campaign dropdowns
//      populated from the Installation. On OK: the viewer loads
//      THEATER.* (in-process terrain2json), the chosen .cam (in-process
//      cam2json with FALCON4.ct auto-resolved), and renders.
//
//   3. File > Advanced > ... — the original four menu items (Open World
//      JSON, Open Terrain JSON, Import .cam, Import THEATER.*) kept for
//      the dev / un-bundled-fixtures workflow.
//
// Both can be loaded from the File menu, or passed as CLI args. The viewer
// also wraps the CLI converters in-process, so the user can directly import
// THEATER.* binary files or a .cam archive without leaving the app — this
// makes it a starting point for a future world editor.

#pragma once

#include <f4/install/installation.hpp>
#include <f4/viewer/settings.hpp>

#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>

namespace f4::viewer {

class ViewerApp {
public:
    ViewerApp();
    ~ViewerApp();

    // Lifecycle
    void run();   // blocking — enters the Raylib event loop

    // --- Install-aware API (new primary flow) ---

    /// Open a native folder picker, call Installation::detect() on the
    /// chosen path, cache it to settings, and return whether the
    /// installation validated. On success, callers can call
    /// open_campaign_dialog() to let the user pick a campaign.
    /// On cancel, returns false and leaves any existing installation
    /// unchanged.
    bool set_install_path_dialog();

    /// Set the install path directly (no dialog). Used by CLI flags
    /// (--install <path>) and by tests. Returns whether detect()
    /// validated the path.
    bool set_install_path(const std::filesystem::path& path);

    /// The currently-configured installation (may be std::nullopt if the
    /// user hasn't set one yet, or if detect() failed).
    [[nodiscard]] const std::optional<f4::install::Installation>& installation() const noexcept;

    /// Open the Theater + Campaign picker modal. Populated from the
    /// current installation. No-op if no installation is set (the
    /// caller should call set_install_path_dialog() first).
    void open_campaign_dialog();

    // --- File operations (callable from ImGui menus) ---

    void load_terrain_json(const std::filesystem::path& path);
    void load_world_json(const std::filesystem::path& path);
    void import_terrain_binary(const std::filesystem::path& terrain_dir);
    void import_cam_archive(const std::filesystem::path& cam_path);

    /// Install-aware campaign load: takes a Theater key + Campaign
    /// stem, resolves them to on-disk paths via the Installation,
    /// and runs the in-process converters (terrain2json + cam2json
    /// with auto-resolved FALCON4.ct) to populate WorldState. This
    /// is the "one-click load" the new CONOPS promises.
    /// Throws on missing files or parse errors — caller catches and
    /// shows the error in the status bar.
    void load_campaign_from_install(const std::string& theater_key,
                                     const std::string& campaign_stem);

    /// Open the Hex Inspector panel (Tools > Hex Inspector) with a file
    /// pre-loaded. Used by the --hex-inspect CLI flag for scripted use
    /// and for headless smoke tests. Throws on I/O error.
    void open_hex_inspector_with_file(const std::filesystem::path& path);

    /// Open the Install Diagnostics modal (Tools > Install Diagnostics).
    /// Builds the full diagnostic report from the current Installation
    /// and shows it in a scrollable, copyable text panel.
    /// No-op if no install is set (the modal will say so).
    void open_install_diagnostics();

    /// Get the current install diagnostic report as a string. Used by
    /// the --diagnostics CLI flag to print to stderr. Returns a "no
    /// install set" message if no install is configured.
    [[nodiscard]] std::string install_diagnostics_text() const;

    /// Open a native save-file dialog and write a snapshot of the
    /// current install to the chosen path. The snapshot is a plain
    /// ASCII text file containing hex+ASCII dumps of the first N bytes
    /// of every interesting Falcon4 data file (Falcon4.PHD, .PD, .OCD,
    /// .UCD, .VCD, .FED, .FCD, .AII, FALCON4.ct, etc.). Intended for
    /// sharing with the dev team to ground-truth binary-format reverse
    /// engineering — see Docs/FALCON4_FILE_LAYOUT.md.
    ///
    /// No-op (shows an error in the status bar) if no install is set.
    /// Throws std::runtime_error on I/O failure.
    void open_snapshot_dialog();

    /// Write an install snapshot to `output_path` directly (no dialog).
    /// Used by the --snapshot CLI flag for headless use. Returns true
    /// on success; on failure, sets `err_out` (if non-null) and returns
    /// false. No-op (returns false, sets err_out) if no install is set.
    bool snapshot_install_files(const std::filesystem::path& output_path,
                                 std::string* err_out = nullptr);

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
