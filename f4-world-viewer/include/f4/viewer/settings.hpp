// f4-world-viewer/include/f4/viewer/settings.hpp
//
// Persisted viewer settings — the user's install path + last-loaded
// campaign, so the viewer can restore state across launches.
//
// Storage location (cross-platform):
//   Linux:   $XDG_CONFIG_HOME/f4-viewer/settings.json
//            (falls back to ~/.config/f4-viewer/settings.json)
//   macOS:   ~/Library/Application Support/f4-viewer/settings.json
//   Windows: %APPDATA%/F4Viewer/settings.json
//
// The JSON format is intentionally tiny — three or four fields max —
// so we hand-roll it instead of pulling nlohmann/json into the viewer's
// link chain. Format:
//
//   {
//     "install_path": "/path/to/falcon4",
//     "last_theater_key": "korea",
//     "last_campaign_stem": "save1",
//     "last_world_json": "/optional/path/to/last.world.json",
//     "last_terrain_json": "/optional/path/to/last.terrain.json"
//   }
//
// Missing fields are treated as empty strings — the viewer falls back
// to the bundled fixtures or prompts the user to set an install path.

#pragma once

#include <filesystem>
#include <string>

namespace f4::viewer {

struct ViewerSettings {
    /// Path the user picked for their Falcon 4.0 install. Empty if
    /// never set (first launch, or settings file deleted).
    std::filesystem::path install_path;

    /// The last theater the user selected in the Open Campaign dialog.
    /// Used to pre-select the same theater next launch.
    std::string last_theater_key;

    /// The last campaign stem (filename without .cam) the user loaded.
    std::string last_campaign_stem;

    /// Optional: the last manually-loaded world JSON path. Used by the
    /// File > Advanced > Open World JSON... item to pre-fill the path.
    std::filesystem::path last_world_json;

    /// Optional: the last manually-loaded terrain JSON path.
    std::filesystem::path last_terrain_json;

    /// Equality (used by tests + to detect "did anything change").
    bool operator==(const ViewerSettings&) const = default;
};

/// Returns the platform-appropriate settings directory (creating it if
/// it doesn't exist). Throws std::runtime_error on failure.
[[nodiscard]] std::filesystem::path settings_dir();

/// Returns the full path to the settings file. The file may not exist
/// yet — call load_settings() to get a default-populated ViewerSettings
/// in that case.
[[nodiscard]] std::filesystem::path settings_file_path();

/// Load settings from disk. Returns a default-constructed ViewerSettings
/// if the file doesn't exist or fails to parse (never throws — the
/// viewer should always be able to start, even with a corrupted file).
[[nodiscard]] ViewerSettings load_settings();

/// Save settings to disk. Returns true on success, false on failure
/// (the caller can show an error in the status bar but shouldn't crash).
/// Creates the settings directory if it doesn't exist.
bool save_settings(const ViewerSettings& s);

/// Convert settings to a JSON string. Exposed for testing.
[[nodiscard]] std::string settings_to_json(const ViewerSettings& s);

/// Parse settings from a JSON string. Returns a default-constructed
/// ViewerSettings on parse failure. Exposed for testing.
[[nodiscard]] ViewerSettings settings_from_json(const std::string& json);

} // namespace f4::viewer
