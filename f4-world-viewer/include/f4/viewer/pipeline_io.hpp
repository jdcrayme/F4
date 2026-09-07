// f4-world-viewer/include/f4/viewer/pipeline_io.hpp
//
// Pipeline I/O — the "Data first, else convert into Data/Temp" layer
// between the viewer and the converter CLIs (Tranche 0d, P2 boundary:
// the runtime never links the parser libraries, and the Falcon install
// is treated as read-only source material).
//
// Resolution rule for every artifact:
//   1. The canonical export under Data/ (produced by
//      scripts/export-game-data and committed with the repo) wins when
//      present.
//   2. Otherwise the converter CLI runs, writing into Data/Temp/ (same
//      sub-layout as Data/), and the Temp path is returned. Temp
//      outputs are reused on later loads, never registered in
//      Data/manifest.json, and never overwrite canonical Data/ files —
//      so a personal install's data can't clobber the curated exports.
//
// All functions throw std::runtime_error on failure with the captured
// CLI output appended, so the campaign-load error modal shows the
// converter's own diagnostics.

#pragma once

#include <filesystem>
#include <functional>
#include <string>

namespace f4::install {
class Installation;
}

namespace f4::viewer {

// Location of the Data/ directory: AssetRoot::discover() when it finds
// one (walks up from the working directory), else <F4_SOURCE_DIR>/Data.
// Empty when neither exists — callers show their own guidance message.
std::filesystem::path discover_data_dir();

// Data/Temp — scratch area for install-derived conversions.
std::filesystem::path temp_dir(const std::filesystem::path& data_dir);

// Quote one path as a single command-line argument (paths may contain
// spaces). Shared by every shell-out in the viewer.
std::string quote_arg(const std::filesystem::path& p);

// Per-line progress callback for run_converter (f4import --all takes
// minutes; the viewer surfaces these lines as status text).
using ProgressFn = std::function<void(const std::string& line)>;

// Run a converter CLI: <exe> <args>. Returns the process exit code
// (0 = success). Captures combined stdout+stderr into *captured_output
// (when non-null) so callers can include the converter's own error text
// in thrown messages. On Windows the child runs with CREATE_NO_WINDOW —
// no console flash from a GUI app, and no cmd.exe forward-slash quirks
// (std::system returned exit 1 for a missing exe, indistinguishable
// from a real converter failure).
int run_converter(const std::filesystem::path& exe,
                  const std::string& args,
                  std::string* captured_output = nullptr,
                  const ProgressFn& on_line = {});

// ── Resolvers ─────────────────────────────────────────────────────────
//
// Each returns a usable artifact path, converting into Data/Temp when
// the canonical export is missing. converted_to_temp (when non-null)
// reports whether the returned path came from Data/Temp.

// Class table JSON: Data/Classes/falcon4.ct.json, else ct2json on the
// install's binary FALCON4.ct → Data/Temp/Classes/falcon4.ct.json.
std::filesystem::path ensure_class_table_json(
    const f4::install::Installation& install,
    bool* converted_to_temp = nullptr);

// Terrain JSON: Data/Theater/<key>/terrain.json, else terrain2json on
// the theater dir → Data/Temp/Theater/<key>/terrain.json.
std::filesystem::path ensure_terrain_json(
    const std::string& theater_key,
    const std::filesystem::path& theater_dir,
    bool* converted_to_temp = nullptr);

// World JSON for a .cam archive: manifest campaign:<lowercased stem>
// entry, then Data/World/<stem>.world.json, then a previous
// Data/Temp/World/<stem>.world.json, else cam2json →
// Data/Temp/World/<stem>.world.json. objects_dir and binary_ct may be
// empty (passed to cam2json as --theater-data / --class-table when
// present — the binary .ct is what cam2json parses, not the JSON).
std::filesystem::path ensure_world_json(
    const std::filesystem::path& cam_path,
    const std::filesystem::path& objects_dir,
    const std::filesystem::path& binary_ct,
    bool* converted_to_temp = nullptr);

// Data/ root whose Models/koreaobj subtree holds the glTF exports:
// canonical Data/Models/koreaobj, else Data/Temp (run through the
// f4import textures+models --all conversion into a staging dir first,
// so an interrupted run can't leave a half-written Temp/Models behind).
// MINUTES on first run — call from a worker thread, not the UI thread.
// on_line receives f4import's progress output.
std::filesystem::path ensure_models_root(
    const f4::install::Installation& install,
    const ProgressFn& on_line = {});

// Locate Models/koreaobj without converting: canonical
// Data/Models/koreaobj, else Data/Temp/Models/koreaobj when present.
// Empty when neither exists (the cue for ensure_models_root).
std::filesystem::path find_models_data_root(
    const std::filesystem::path& data_dir);

} // namespace f4::viewer
