// f4-world-viewer/src/install_flow.cpp
//
// Install-aware ViewerApp methods — the "primary user flow" per the
// viewer_app.hpp CONOPS:
//
//   File > Set Install Path...     → set_install_path_dialog / set_install_path
//   File > Open Campaign...        → open_campaign_dialog / load_campaign_from_install
//   Tools > Install Diagnostics... → open_install_diagnostics / install_diagnostics_text
//   --hex-inspect <file> CLI flag  → open_hex_inspector_with_file
//
// Split out of the original 1920-LoC viewer_app.cpp god-file (item #5
// of the architecture review). No behavior change.
//
// The actual report-building logic lives in diagnostics.cpp (free
// functions build_install_diagnostics and build_campaign_load_error,
// declared in diagnostics.hpp). This file just dispatches to them and
// manages the modal state.

#include "viewer_state.hpp"
#include "diagnostics.hpp"
#include "snapshot.hpp"

#include <f4/install/installation.hpp>
#include <f4/viewer/file_dialog.hpp>
#include <f4/viewer/pipeline_io.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <system_error>
#include <thread>

namespace f4::viewer {

bool ViewerApp::set_install_path_dialog() {
    // Use the current install path (or last world JSON dir) as the
    // starting point for the folder picker — saves navigation.
    std::filesystem::path start = impl_->settings.install_path;
    if (start.empty() && !impl_->last_world_json_path.empty()) {
        start = impl_->last_world_json_path.parent_path();
    }

    auto path = pick_folder("Select Falcon 4.0 Install Directory", start);
    if (path.empty()) return false;  // user cancelled

    return set_install_path(path);
}

bool ViewerApp::set_install_path(const std::filesystem::path& path) {
    try {
        auto inst = f4::install::Installation::detect(path);
        if (!inst.valid()) {
            impl_->last_error = "Not a Falcon 4.0 install: " + path.string() +
                "\n\nExpected: a directory containing FALCON4.ct and/or a terrdata/ subdirectory.";
            show_message_box("Invalid Install Path", impl_->last_error, "warning");
            return false;
        }
        impl_->install = std::move(inst);
        impl_->settings.install_path = path;
        save_settings(impl_->settings);

        // Reset the KoreaObj model load flag so the Ground Layout 3D
        // panel re-attempts to load on next selection. The previous
        // attempt may have failed because no install was set; now that
        // we have one, retry. Existing mesh + texture caches are kept
        // (they're still valid if models were loaded before — switching
        // installs doesn't invalidate the in-memory model data).
        impl_->models_3d_load_attempted = false;
        impl_->models_3d_error.clear();

        // If a world was already loaded (naming a theater) but the
        // theater binaries couldn't load for lack of an install, retry
        // now — the 2D map + 3D panel pick up tile art next frame.
        impl_->try_load_theater_tiles();

        // Build a summary for the confirmation modal.
        std::ostringstream ss;
        ss << "Install detected successfully.\n\n";
        ss << "Root: " << impl_->install->root().string() << "\n\n";

        ss << "Theaters: " << impl_->install->theaters().size() << "\n";
        for (const auto& t : impl_->install->theaters()) {
            ss << "  - " << t.display_name << " (" << t.key << ")";
            ss << (t.complete() ? "" : " [INCOMPLETE]");
            ss << "\n";
            // Show which THEATER.* files are present (helps diagnose
            // incomplete theaters — missing THEATER.MAP or .MEA breaks
            // campaign loading for every campaign in that theater).
            ss << "      files: ";
            if (t.theater_files.empty()) {
                ss << "(none)";
            } else {
                bool first = true;
                for (const auto& f : t.theater_files) {
                    if (!first) ss << ", ";
                    ss << f.filename().string();
                    first = false;
                }
            }
            ss << "\n";
        }

        ss << "\nCampaigns: " << impl_->install->campaigns().size() << "\n";
        // Show the first few campaigns with their paths so the user can
        // verify the layout (flat vs. nested) is what we expect.
        const std::size_t max_camp_show = 5;
        for (std::size_t i = 0;
             i < std::min(impl_->install->campaigns().size(), max_camp_show); ++i) {
            const auto& c = impl_->install->campaigns()[i];
            ss << "  - " << c.stem;
            if (!c.theater_key.empty()) ss << "  [" << c.theater_key << "]";
            ss << "\n      " << c.cam.string() << "\n";
        }
        if (impl_->install->campaigns().size() > max_camp_show) {
            ss << "  ... and " << (impl_->install->campaigns().size() - max_camp_show)
               << " more\n";
        }

        ss << "\nClass table: ";
        if (impl_->install->class_table().empty()) {
            ss << "NOT FOUND\n";
            ss << "  Searched:\n";
            for (const auto& p : impl_->install->diagnostics().class_table_searched) {
                ss << "    " << p.string() << "\n";
            }
            ss << "  (Objectives will lack icons. Use Tools > Install Diagnostics\n"
               << "   for more detail, or place FALCON4.ct in one of these locations.)\n";
        } else {
            ss << impl_->install->class_table().string() << "\n";
        }
        impl_->install_summary_text = ss.str();
        impl_->install_summary_open = true;
        impl_->status_msg = "Install: " + path.string();
        return true;
    } catch (const std::exception& e) {
        impl_->last_error = std::string("Install detection failed: ") + e.what();
        show_message_box("Install Detection Failed", impl_->last_error, "error");
        return false;
    }
}

const std::optional<f4::install::Installation>&
ViewerApp::installation() const noexcept {
    return impl_->install;
}

void ViewerApp::open_campaign_dialog() {
    if (!impl_->install || !impl_->install->valid()) {
        // No install set — prompt the user to pick one first.
        show_message_box("No Install Set",
                          "You need to set the Falcon 4.0 install path first.\n"
                          "Use File > Set Install Path... to pick the directory.",
                          "warning");
        return;
    }
    if (impl_->install->theaters().empty()) {
        show_message_box("No Theaters Found",
                          "The install at " + impl_->install->root().string() +
                          "\ncontains no theaters under terrdata/.\n"
                          "Make sure the install is intact.",
                          "warning");
        return;
    }

    // Pre-select the last theater the user picked, if it's still present.
    impl_->campaign_dialog_theater_idx = 0;
    if (!impl_->settings.last_theater_key.empty()) {
        for (size_t i = 0; i < impl_->install->theaters().size(); ++i) {
            if (impl_->install->theaters()[i].key == impl_->settings.last_theater_key) {
                impl_->campaign_dialog_theater_idx = static_cast<int>(i);
                break;
            }
        }
    }

    // Populate the campaigns list for the selected theater.
    const auto& theater = impl_->install->theaters()[impl_->campaign_dialog_theater_idx];
    impl_->campaign_dialog_campaigns = impl_->install->campaigns_for(theater.key);
    impl_->campaign_dialog_campaign_idx = 0;

    // Pre-select last campaign stem, if still present.
    if (!impl_->settings.last_campaign_stem.empty()) {
        for (size_t i = 0; i < impl_->campaign_dialog_campaigns.size(); ++i) {
            if (impl_->campaign_dialog_campaigns[i].stem == impl_->settings.last_campaign_stem) {
                impl_->campaign_dialog_campaign_idx = static_cast<int>(i);
                break;
            }
        }
    }

    impl_->campaign_dialog_open = true;
}

void ViewerApp::load_campaign_from_install(const std::string& theater_key,
                                             const std::string& campaign_stem) {
    if (!impl_->install) {
        throw std::runtime_error("load_campaign_from_install: no install set");
    }
    const auto* theater = impl_->install->find_theater(theater_key);
    if (!theater) {
        throw std::runtime_error("theater not found: " + theater_key);
    }
    if (!theater->complete()) {
        throw std::runtime_error("theater '" + theater_key +
            "' is incomplete (missing THEATER.MAP or .MEA)");
    }

    // Find the campaign in this theater with the matching stem.
    auto camps = impl_->install->campaigns_for(theater_key);
    const f4::install::Campaign* camp = nullptr;
    for (const auto& c : camps) {
        if (c.stem == campaign_stem) { camp = &c; break; }
    }
    if (!camp) {
        throw std::runtime_error("campaign '" + campaign_stem +
            "' not found in theater '" + theater_key + "'");
    }

    // Step 0: class table JSON — canonical Data/Classes export, else a
    // one-time ct2json conversion into Data/Temp. Everything on the
    // runtime side of the P2 boundary reads the JSON (class table
    // browser, 3D model path, campaign session); cam2json below is the
    // only consumer of the install's BINARY .ct, passed separately.
    impl_->class_table_json_path = ensure_class_table_json(*impl_->install);

    // Step 1: convert THEATER.* → terrain JSON via the terrain2json CLI
    // when the canonical Data/Theater/<key>/terrain.json export is
    // absent — the conversion lands in Data/Temp/Theater/<key>/ and
    // never touches committed Data/ files.
    bool terrain_converted = false;
    const auto terrain_json =
        ensure_terrain_json(theater_key, theater->dir, &terrain_converted);
    impl_->terrain.load_terrain_json(terrain_json);
    impl_->terrain_loaded = true;
    impl_->last_terrain_json_path = terrain_json;
    impl_->status_msg = "Terrain: " + theater_key + " (" +
        std::to_string(impl_->terrain.header.width) + "x" +
        std::to_string(impl_->terrain.header.height) + ")";

    // Also load the raw theater binaries through the shared WorldView
    // (post levels + tile art) for TEXTURED terrain in the 3D panel.
    // Non-fatal when the theater lacks tile data — the panel falls back
    // to the terrain-JSON vertex-color path. (try_load_theater_tiles may
    // already have loaded this theater from an earlier world JSON —
    // skip the multi-second reload then.)
    const bool tiles_already = impl_->theater_tiles_loaded &&
        impl_->current_theater_dir == theater->dir;
    impl_->current_theater_dir = theater->dir;
    if (!tiles_already) {
        try {
            impl_->theater_tiles_loaded = impl_->world.load_theater(theater->dir);
            if (!impl_->theater_tiles_loaded) {
                impl_->status_msg += " [no tile art — untextured]";
            }
        } catch (const std::exception& e) {
            impl_->theater_tiles_loaded = false;
            impl_->status_msg += std::string(" [tile load failed: ") + e.what() + "]";
        }
    }

    // Step 3: convert .cam → world JSON via the cam2json CLI when no
    // canonical export covers it (manifest campaign:<stem> entry, then
    // the well-known Data/World names). Conversions land in
    // Data/Temp/World/<stem>.world.json. --theater-data points at the
    // install's objects dir so the world JSON carries objective class
    // names, airfield ground layouts (runways, taxiways, parking,
    // helipads, docks), unit class names, and per-group vehicle
    // composition — the same inputs the build's korea-real-world-json
    // target passes. Two search locations cover both on-disk layouts
    // used by Falcon variants: install-level `terrdata/objects`
    // (vanilla Falcon 4.0 / FreeFalcon / Allied Force) and per-theater
    // `<terrdata>/<key>/objects` (some community theaters).
    std::filesystem::path objects_dir;
    for (const auto& d : { impl_->install->terrdata_dir() / "objects",
                           theater->dir / "objects" }) {
        if (!d.empty() && std::filesystem::exists(d)) {
            objects_dir = d;
            break;
        }
    }
    bool world_converted = false;
    const auto world_json = ensure_world_json(
        camp->cam, objects_dir,
        impl_->install->class_table(), &world_converted);
    load_world_json(world_json);

    if (terrain_converted || world_converted) {
        impl_->status_msg += "  [converted into Data/Temp:";
        if (terrain_converted) impl_->status_msg += " terrain";
        if (world_converted) impl_->status_msg += " world";
        impl_->status_msg += "]";
    }

    // Step 4: glTF models + textures. The canonical Data/Models/
    // koreaobj export (or a previous Temp conversion) makes this a
    // no-op; otherwise the multi-minute f4import run starts in the
    // background and poll_pipeline_job() adopts the result when done.
    if (find_models_data_root(discover_data_dir()).empty()) {
        start_models_conversion();
    }

    // Persist the last theater + campaign so the next launch pre-selects them.
    impl_->settings.last_theater_key = theater_key;
    impl_->settings.last_campaign_stem = campaign_stem;
    save_settings(impl_->settings);
}

void ViewerApp::start_models_conversion() {
    if (!impl_->install || !impl_->install->valid()) return;
    if (impl_->models_job) return;  // already running

    auto job = std::make_shared<ModelsImportJob>();
    impl_->models_job = job;
    // The worker gets a copy of the Installation — no lifetime tie to
    // Impl, so quitting mid-conversion can't leave it dereferencing a
    // dead viewer. It only touches the shared job struct.
    const auto install = *impl_->install;
    std::thread([job, install]() {
        try {
            auto root = ensure_models_root(
                install, [job](const std::string& line) {
                    std::lock_guard<std::mutex> g(job->mutex);
                    job->progress = line;
                });
            std::lock_guard<std::mutex> g(job->mutex);
            job->ok = true;
            job->models_root = root;
        } catch (const std::exception& e) {
            std::lock_guard<std::mutex> g(job->mutex);
            job->ok = false;
            job->error = e.what();
        }
        job->done.store(true);
    }).detach();
    impl_->status_msg = "Converting 3D models/textures into Data/Temp "
                        "(one-time, several minutes)...";
}

void ViewerApp::poll_pipeline_job() {
    if (!impl_->models_job) return;
    auto& job = *impl_->models_job;
    if (!job.done.load()) {
        {
            std::lock_guard<std::mutex> g(job.mutex);
            if (!job.progress.empty()) {
                impl_->status_msg = "Converting 3D models/textures into "
                                    "Data/Temp: " + job.progress;
            }
        }
        return;
    }

    const auto finished = impl_->models_job;
    impl_->models_job.reset();
    if (finished->ok) {
        impl_->models_data_dir_override = finished->models_root;
        // Re-arm the lazy 3D load — the next draw_ground_layout_3d /
        // canvas feature-mesh pass discovers the Temp models root.
        impl_->models_3d_load_attempted = false;
        impl_->models_3d_error.clear();
        impl_->status_msg = "3D models/textures ready (Data/Temp).";
    } else {
        impl_->last_error = "3D model conversion failed: " + finished->error;
        impl_->status_msg = "3D model conversion failed — see Error.";
    }
}

void ViewerApp::open_hex_inspector_with_file(const std::filesystem::path& path) {
    impl_->hex_inspector.open();
    impl_->hex_inspector.load_file(path);
}

std::string ViewerApp::install_diagnostics_text() const {
    if (!impl_->install) {
        return "No install set. Use File > Set Install Path... to configure one.\n";
    }
    return build_install_diagnostics(*impl_->install);
}

void ViewerApp::open_install_diagnostics() {
    if (!impl_->install) {
        impl_->install_diagnostics_text =
            "No install set.\n\nUse File > Set Install Path... to pick your "
            "Falcon 4.0 install directory first.";
    } else {
        impl_->install_diagnostics_text = build_install_diagnostics(*impl_->install);
    }
    impl_->install_diagnostics_open = true;
}

void ViewerApp::open_snapshot_dialog() {
    if (!impl_->install) {
        impl_->last_error =
            "No install set. Use File > Set Install Path... to pick your "
            "Falcon 4.0 install directory first.";
        show_message_box("No Install Set", impl_->last_error, "warning");
        return;
    }

    // Default the save picker to a sensible location: next to the
    // install root, with a timestamped default filename.
    const auto ts = []() {
        using std::chrono::system_clock;
        const auto now = system_clock::now();
        const std::time_t t = system_clock::to_time_t(now);
        std::tm tm{};
#if defined(_WIN32)
        gmtime_s(&tm, &t);
#else
        gmtime_r(&t, &tm);
#endif
        char buf[32];
        std::strftime(buf, sizeof(buf), "%Y%m%d_%H%M%S", &tm);
        return std::string(buf);
    }();

    const auto default_name = "f4_install_snapshot_" + ts + ".txt";
    auto default_path = impl_->install->root() / default_name;

    auto out = pick_save_file(
        "Save Install Snapshot",
        "Text files (*.txt)|All files (*.*)",
        default_path);
    if (out.empty()) return;  // user cancelled

    std::string err;
    if (!snapshot_install_files(out, &err)) {
        impl_->last_error = "Snapshot failed: " + err;
        show_message_box("Snapshot Failed", impl_->last_error, "error");
        return;
    }

    impl_->status_msg = "Snapshot saved: " + out.string() +
        " (" + std::to_string(std::filesystem::file_size(out)) + " bytes)";
    show_message_box("Snapshot Saved",
        ("Saved install snapshot to:\n" + out.string() +
         "\n\nMail or upload this file to the dev team — it contains hex "
         "dumps of every interesting Falcon4 data file in your install.").c_str(),
        "info");
}

bool ViewerApp::snapshot_install_files(const std::filesystem::path& output_path,
                                        std::string* err_out) {
    if (!impl_->install) {
        if (err_out) *err_out = "no install set";
        return false;
    }
    SnapshotOptions opts;  // defaults: 8 KB per file, no tail, with listings
    return write_install_snapshot(*impl_->install, output_path, opts, err_out);
}

void ViewerApp::open_list_files_dialog() {
    if (!impl_->install) {
        impl_->last_error =
            "No install set. Use File > Set Install Path... to pick your "
            "Falcon 4.0 install directory first.";
        show_message_box("No Install Set", impl_->last_error, "warning");
        return;
    }

    // Default the save picker to a sensible location: next to the
    // install root, with a timestamped default filename.
    const auto ts = []() {
        using std::chrono::system_clock;
        const auto now = system_clock::now();
        const std::time_t t = system_clock::to_time_t(now);
        std::tm tm{};
#if defined(_WIN32)
        gmtime_s(&tm, &t);
#else
        gmtime_r(&t, &tm);
#endif
        char buf[32];
        std::strftime(buf, sizeof(buf), "%Y%m%d_%H%M%S", &tm);
        return std::string(buf);
    }();

    const auto default_name = "f4_install_filelisting_" + ts + ".txt";
    auto default_path = impl_->install->root() / default_name;

    auto out = pick_save_file(
        "Save Install File Listing",
        "Text files (*.txt)|All files (*.*)",
        default_path);
    if (out.empty()) return;  // user cancelled

    std::string err;
    if (!list_install_files(out, &err)) {
        impl_->last_error = "File listing failed: " + err;
        show_message_box("File Listing Failed", impl_->last_error, "error");
        return;
    }

    impl_->status_msg = "File listing saved: " + out.string() +
        " (" + std::to_string(std::filesystem::file_size(out)) + " bytes)";
    show_message_box("File Listing Saved",
        ("Saved install file listing to:\n" + out.string() +
         "\n\nMail or upload this file to the dev team — it contains the "
         "name, path, and size of every file under the install root. "
         "Running it on multiple installs (vanilla / FreeFalcon / BMS) "
         "lets us document each layout and simplify the file-search "
         "logic in f4-install.").c_str(),
        "info");
}

bool ViewerApp::list_install_files(const std::filesystem::path& output_path,
                                    std::string* err_out) {
    if (!impl_->install) {
        if (err_out) *err_out = "no install set";
        return false;
    }
    // Listing-only mode: skip curated hex dumps, enable the full
    // recursive walk. Disables the per-directory catch-all listings
    // too (the full walk already covers them).
    SnapshotOptions opts;
    opts.full_recursive_listing = true;
    opts.skip_curated_dumps = true;
    opts.list_terrdata_files = false;  // redundant under full walk
    return write_install_snapshot(*impl_->install, output_path, opts, err_out);
}

} // namespace f4::viewer
