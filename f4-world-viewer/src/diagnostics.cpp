// f4-world-viewer/src/diagnostics.cpp
//
// Diagnostic-report builders — free functions (not ViewerApp members)
// that take a const Installation& (and optional context strings for the
// campaign-load error case) and produce a human-readable report.
//
// Split out of the original 1920-LoC viewer_app.cpp god-file (item #5
// of the architecture review). No behavior change — same sections, same
// field ordering, same wording.
//
// Used by:
//   - ViewerApp::set_install_path (the install summary modal)
//   - ViewerApp::open_install_diagnostics (the full diagnostics modal)
//   - ViewerApp::install_diagnostics_text (the --diagnostics CLI flag)
//   - The campaign-load error modal (build_campaign_load_error only)
//
// Kept as free functions (rather than ViewerApp members) because they
// have no Impl access, no side effects, and no raylib/imgui deps — pure
// text assembly. That makes them trivially unit-testable if we ever
// want to add tests for the report formatting.

#include "diagnostics.hpp"

#include <algorithm>
#include <filesystem>
#include <sstream>
#include <string>

namespace f4::viewer {

std::string build_install_diagnostics(const f4::install::Installation& inst) {
    std::ostringstream ss;
    ss << "=== Install Diagnostics ===\n\n";
    ss << "Root: " << inst.root().string() << "\n";
    ss << "Valid: " << (inst.valid() ? "yes" : "no") << "\n\n";

    ss << "--- FALCON4.ct (class table) ---\n";
    if (inst.class_table().empty()) {
        ss << "NOT FOUND. Searched these locations:\n";
        for (const auto& p : inst.diagnostics().class_table_searched) {
            ss << "  " << p.string() << "\n";
        }
        ss << "\nTo fix: place FALCON4.ct in one of the above locations.\n"
           << "The class table maps entity_type values (100+) to ObjectiveType\n"
           << "and unit subtypes. Without it, objectives render as generic\n"
           << "circles instead of type-specific icons.\n";
    } else {
        ss << "Found: " << inst.class_table().string() << "\n";
    }
    ss << "\n";

    ss << "--- theater.lst ---\n";
    if (inst.diagnostics().theater_lst_path.empty()) {
        ss << "Not found in terrdata/. Fell back to directory scan.\n";
    } else {
        ss << "Path: " << inst.diagnostics().theater_lst_path.string() << "\n";
        ss << "Parsed: " << (inst.diagnostics().theater_lst_parsed ? "yes" : "no") << "\n";
        ss << "Keys: " << inst.diagnostics().theater_lst_key_count << "\n";
    }
    ss << "\n";

    ss << "--- Theaters (" << inst.theaters().size() << ") ---\n";
    for (const auto& t : inst.theaters()) {
        ss << "  " << t.display_name << " (" << t.key << ")";
        ss << (t.complete() ? "" : " [INCOMPLETE]");
        ss << "\n";
        ss << "    dir: " << t.dir.string() << "\n";
        ss << "    THEATER.MAP: " << (t.theater_map.empty() ? "MISSING" : "present") << "\n";
        ss << "    THEATER.MEA: " << (t.theater_mea.empty() ? "MISSING" : "present") << "\n";
        ss << "    THEATER.O2:  " << (t.theater_o2.empty() ? "(absent)" : "present") << "\n";
        ss << "    theater.ini: " << (t.theater_ini.empty() ? "(absent)" : "present") << "\n";
        if (!t.theater_files.empty()) {
            ss << "    All THEATER.* files (" << t.theater_files.size() << "):\n";
            for (const auto& f : t.theater_files) {
                ss << "      " << f.filename().string()
                   << "  (" << std::filesystem::file_size(f) << " bytes)\n";
            }
        }
        ss << "\n";
    }

    ss << "--- Theater dirs probed but rejected (no THEATER.MAP) ---\n";
    if (inst.diagnostics().theater_dirs_probed.empty()) {
        ss << "  (none — no subdirs found in terrdata/, or terrdata/ itself missing)\n";
    } else {
        // Show dirs that aren't in theaters() (rejected).
        for (const auto& dir : inst.diagnostics().theater_dirs_probed) {
            bool is_a_theater = false;
            for (const auto& t : inst.theaters()) {
                if (t.dir == dir) { is_a_theater = true; break; }
            }
            if (!is_a_theater) {
                ss << "  " << dir.string() << "\n";
            }
        }
    }
    ss << "\n";

    ss << "--- Campaigns (" << inst.campaigns().size() << ") ---\n";
    ss << "Campaign dir: ";
    if (inst.campaign_dir().empty()) {
        ss << "NOT FOUND\n";
    } else {
        ss << inst.campaign_dir().string() << "\n";
    }
    for (const auto& c : inst.campaigns()) {
        ss << "  " << c.stem;
        if (!c.theater_key.empty()) ss << "  [" << c.theater_key << "]";
        else ss << "  [flat layout]";
        ss << "\n";
        ss << "    path: " << c.cam.string() << "\n";
        ss << "    exists: " << (std::filesystem::exists(c.cam) ? "yes" : "NO") << "\n";
        if (std::filesystem::exists(c.cam)) {
            std::error_code ec;
            const auto sz = std::filesystem::file_size(c.cam, ec);
            if (!ec) ss << "    size: " << sz << " bytes\n";
        }
    }
    ss << "\n";

    ss << "--- Other paths ---\n";
    ss << "sim/ (aircraft): ";
    ss << (inst.aircraft_dir().empty() ? "(absent)" : inst.aircraft_dir().string());
    ss << "\n";
    ss << "terrdata/: ";
    ss << (inst.terrdata_dir().empty() ? "(absent)" : inst.terrdata_dir().string());
    ss << "\n";

    return ss.str();
}

std::string build_campaign_load_error(const f4::install::Installation& inst,
                                       const std::string& theater_key,
                                       const std::string& campaign_stem,
                                       const std::string& exception_msg) {
    std::ostringstream ss;
    ss << "Campaign load failed.\n\n";
    ss << "Error: " << exception_msg << "\n\n";
    ss << "--- Context ---\n";
    ss << "Theater key: " << theater_key << "\n";
    ss << "Campaign stem: " << campaign_stem << "\n\n";

    const auto* theater = inst.find_theater(theater_key);
    if (!theater) {
        ss << "Theater '" << theater_key << "' not found in install.\n";
        ss << "Available theaters:\n";
        for (const auto& t : inst.theaters()) {
            ss << "  - " << t.key << " (" << t.display_name << ")\n";
        }
    } else {
        ss << "Theater: " << theater->display_name << " (" << theater->key << ")\n";
        ss << "  dir: " << theater->dir.string() << "\n";
        ss << "  complete: " << (theater->complete() ? "yes" : "NO") << "\n";
        ss << "  THEATER.MAP: " << (theater->theater_map.empty() ? "MISSING" : "present") << "\n";
        ss << "  THEATER.MEA: " << (theater->theater_mea.empty() ? "MISSING" : "present") << "\n";
    }
    ss << "\n";

    // Find the campaign.
    auto camps = inst.campaigns_for(theater_key);
    const f4::install::Campaign* camp = nullptr;
    for (const auto& c : camps) {
        if (c.stem == campaign_stem) { camp = &c; break; }
    }
    if (!camp) {
        ss << "Campaign '" << campaign_stem << "' not found.\n";
        ss << "Available campaigns for theater '" << theater_key << "':\n";
        if (camps.empty()) {
            ss << "  (none)\n";
        } else {
            for (const auto& c : camps) {
                ss << "  - " << c.stem << "  →  " << c.cam.string() << "\n";
            }
        }
    } else {
        ss << "Campaign file: " << camp->cam.string() << "\n";
        ss << "  exists: " << (std::filesystem::exists(camp->cam) ? "yes" : "NO") << "\n";
        if (std::filesystem::exists(camp->cam)) {
            std::error_code ec;
            const auto sz = std::filesystem::file_size(camp->cam, ec);
            if (!ec) ss << "  size: " << sz << " bytes\n";
        }
    }
    ss << "\n";

    ss << "Class table: ";
    if (inst.class_table().empty()) {
        ss << "NOT FOUND (objectives will lack icons, but campaign should still load)\n";
    } else {
        ss << inst.class_table().string() << "\n";
    }
    ss << "\n";

    ss << "Use Tools > Install Diagnostics for the full diagnostic report.\n";
    return ss.str();
}

} // namespace f4::viewer
