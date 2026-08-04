// f4-world-viewer/src/file_ops.cpp
//
// ViewerApp file-load / import operations. These wrap the cam2json and
// terrain2json CLIs' libraries in-process (no fork/exec) so the user
// can import raw FreeFalcon binary files without leaving the app.
//
// Split out of the original 1920-LoC viewer_app.cpp god-file (item #5
// of the architecture review). No behavior change — same paths, same
// FALCON4.ct auto-search, same status-message format.
//
// The four public methods here are called from:
//   - File menu (File > Advanced > Open World JSON / Open Terrain JSON /
//     Import .cam Archive / Import THEATER.* Binary)
//   - load_campaign_from_install() in install_flow.cpp (which calls
//     load_world_json after writing the converted JSON next to the .cam)

#include "viewer_state.hpp"

#include <f4/terrain_convert/terrain_converter.hpp>
#include <f4/world_convert/cam_archive.hpp>
#include <f4/world_convert/class_table.hpp>
#include <f4/world_convert/world_json.hpp>

#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>

namespace f4::viewer {

void ViewerApp::load_terrain_json(const std::filesystem::path& path) {
    impl_->world.terrain.load_terrain_json(path);
    impl_->world.terrain_loaded = true;
    impl_->last_terrain_json_path = path;
    impl_->status_msg = "Loaded terrain: " + path.string();
    // POLISH-2.1: invalidate the terrain RenderTexture cache so the
    // next draw_canvas() re-renders it from the newly-loaded data.
    // (Safe to call before GL context exists — just sets a flag.)
    impl_->invalidate_terrain_cache();
    impl_->fit_to_world();
}

void ViewerApp::load_world_json(const std::filesystem::path& path) {
    impl_->world.load(path);
    impl_->world_loaded = true;
    impl_->world_path_display = path.string();
    impl_->last_world_json_path = path;
    impl_->status_msg = "Loaded world: " + path.string() +
        "  (" + std::to_string(impl_->world.objectives.size()) + " objectives, " +
        std::to_string(impl_->world.units.size()) + " units)";
    impl_->rebuild_objective_index();

    // Try to auto-load the referenced terrain file — but only if terrain
    // isn't already loaded. This prevents a spurious "auto-load failed"
    // error when load_campaign_from_install() has already loaded terrain
    // before calling us: the world JSON's terrain_file field is relative
    // to the .cam's directory (e.g. "terrain.json"), and that path may
    // not resolve correctly from CWD.
    if (!impl_->world.terrain_file.empty() && !impl_->world.terrain_loaded) {
        try {
            impl_->world.load_terrain();
            impl_->status_msg += "  + terrain: " + impl_->world.terrain_file;
        } catch (const std::exception& e) {
            impl_->last_error = "Auto-load terrain failed: " + std::string(e.what());
        }
    }
    impl_->fit_to_world();
}

void ViewerApp::import_terrain_binary(const std::filesystem::path& terrain_dir) {
    // Use the in-process library so we don't shell out to terrain2json.
    const auto out = terrain_dir / "terrain.json";
    f4::terrain_convert::convert_terrain_dir(terrain_dir, out, "korea");
    load_terrain_json(out);
}

void ViewerApp::import_cam_archive(const std::filesystem::path& cam_path) {
    f4::world_convert::CamArchive cam;
    cam.load(cam_path);
    f4::world_convert::WorldJsonOptions opts;
    opts.theater = "korea";
    opts.terrain_file = "korea.terrain.json";

    // Auto-search for FALCON4.ct (the class table) in a few standard
    // locations. Without it, objectives carry only their raw entity_type
    // and the viewer can't pick icons — they all fall back to circles.
    // The class table is a binary file shipped with the game data; the
    // repo bundles a copy in f4-world-convert/tests/fixtures/FALCON4.ct
    // so we can resolve types out-of-the-box.
    f4::world_convert::ClassTable class_table;
    const auto ct_path = f4::world_convert::find_class_table(cam_path);
    if (!ct_path.empty()) {
        try {
            class_table.load(ct_path);
            opts.class_table = &class_table;
            impl_->status_msg = "Loaded class table: " + ct_path.string() +
                " (" + std::to_string(class_table.size()) + " entries)";
        } catch (const std::exception& e) {
            impl_->last_error = "Class table load failed: " + std::string(e.what());
        }
    } else {
        impl_->last_error =
            "FALCON4.ct not found — objectives will render as fallback circles "
            "(no icon mapping). Place FALCON4.ct next to the .cam or in assets/.";
    }

    const std::string json = f4::world_convert::to_world_json(cam, opts);

    // Write to a temp file next to the .cam, then load via the normal path.
    auto out = cam_path;
    out.replace_extension(".world.json");
    {
        std::ofstream f(out);
        if (!f) throw std::runtime_error("cannot write " + out.string());
        f << json;
    }
    load_world_json(out);
}

} // namespace f4::viewer
