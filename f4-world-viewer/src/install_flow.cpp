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
#include <f4/terrain_convert/terrain_converter.hpp>
#include <f4/viewer/file_dialog.hpp>
#include <f4/world_convert/cam_archive.hpp>
#include <f4/world_convert/class_table.hpp>
#include <f4/world_convert/theater_data.hpp>
#include <f4/world_convert/world_json.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <system_error>

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

    // Step 1: convert THEATER.* → terrain JSON in a temp file next to
    // the theater dir. We use a temp file rather than in-memory because
    // f4-terrain's loader expects a path.
    const auto terrain_json = theater->dir / "terrain.json";
    f4::terrain_convert::convert_terrain_dir(theater->dir, terrain_json, theater_key);
    impl_->terrain.load_terrain_json(terrain_json);
    impl_->terrain_loaded = true;
    impl_->last_terrain_json_path = terrain_json;
    impl_->status_msg = "Terrain: " + theater_key + " (" +
        std::to_string(impl_->terrain.header.width) + "x" +
        std::to_string(impl_->terrain.header.height) + ")";

    // Step 2: convert .cam → world JSON using the install's class table.
    // Use the install-aware resolver — finds FALCON4.ct automatically.
    f4::world_convert::CamArchive cam;
    cam.load(camp->cam);
    f4::world_convert::WorldJsonOptions opts;
    opts.theater = theater_key;
    opts.terrain_file = terrain_json.filename().string();

    f4::world_convert::ClassTable class_table;
    const auto ct_path = impl_->install->find_class_table(camp->cam);
    if (!ct_path.empty()) {
        try {
            class_table.load(ct_path);
            opts.class_table = &class_table;
        } catch (const std::exception& e) {
            impl_->last_error = "Class table load failed: " + std::string(e.what());
        }
    } else {
        impl_->last_error = "FALCON4.ct not found — objectives will lack icons";
    }

    // Always load the theater object database (Falcon4.OCD/PHD/PD/UCD/
    // VCD/FCD/FED) so the world JSON carries objective class names,
    // airfield ground layouts (runways, taxiways, parking, helipads,
    // docks), unit class names, and per-group vehicle composition.
    // Without this, the Ground Layout window never lights up because
    // world_json.cpp gates the `ground_layout` and `features` arrays on
    // opts.theater_db being set.
    //
    // Two search locations cover both on-disk layouts used by Falcon
    // variants: install-level `terrdata/objects` (vanilla Falcon 4.0 /
    // FreeFalcon / Allied Force) and per-theater `<terrdata>/<key>/
    // objects` (some community theaters). load_all() skips missing
    // files silently, so probing both is safe; we stop as soon as at
    // least one table is loaded.
    f4::world_convert::TheaterObjectDatabase theater_db;
    const std::array<std::filesystem::path, 2> objects_dirs = {
        impl_->install->terrdata_dir() / "objects",
        theater->dir / "objects",
    };
    for (const auto& d : objects_dirs) {
        if (d.empty() || !std::filesystem::exists(d)) continue;
        theater_db.load_all(d);
        if (theater_db.loaded()) break;
    }
    if (theater_db.loaded()) {
        opts.theater_db = &theater_db;
    }

    const std::string json = f4::world_convert::to_world_json(cam, opts);

    // Write next to the .cam, then load via the normal path.
    auto world_json = camp->cam;
    world_json.replace_extension(".world.json");
    {
        std::ofstream f(world_json);
        if (!f) throw std::runtime_error("cannot write " + world_json.string());
        f << json;
    }
    load_world_json(world_json);

    // Persist the last theater + campaign so the next launch pre-selects them.
    impl_->settings.last_theater_key = theater_key;
    impl_->settings.last_campaign_stem = campaign_stem;
    save_settings(impl_->settings);
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
