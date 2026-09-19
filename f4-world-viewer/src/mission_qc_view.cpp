// f4-world-viewer/src/mission_qc_view.cpp
//
// The "Mission QC" window (SHOWCASE-1) — the discovery half of the
// mission-QC user concept: ONE place that lists every flyable mission-QC
// scenario (the world viewer's scenario-template library — tanker_track,
// landing_only, on_glideslope, digi_full_mission, ...) and, for each, the
// recorded FlightRecorder trace the headless runs produce.
//
// The workflow it fronts:
//   1. campaign_qc --scenario build/scenarios/<name>.json [--minutes N]
//        → qc/<name>/trace.json + scenario_qc_summary.json (gates 20–24)
//      or  f4-world-viewer --scenario build/scenarios/<name>.json --run
//          --record qc/<name>/trace.json   (watch it live in 3D, keep the trace)
//   2. click "Open replay" here → the trace loads in replay mode (the
//      scrubber, per-aircraft trail colored by cross-track error, the
//      dashed intended path, ai_state) — geometry QC without a window.
//
// The scan is deliberately dumb: a directory walk over the scenarios
// dir(s) next to the executable plus the CWD, and a fixed set of trace
// conventions (the ones the two tools above actually write). No config
// file, no registry — the filesystem IS the menu.

#include "viewer_state.hpp"
#include <f4/viewer/file_dialog.hpp>

#include <imgui.h>

#include <algorithm>
#include <cstdio>
#include <filesystem>

// raylib (GetApplicationDirectory) — the viewer links it anyway.
#include <raylib.h>

namespace f4::viewer {

namespace {

namespace fs = std::filesystem;

// The scenarios directories to scan, most-specific first. The viewer
// binary lives at <build>/f4-world-viewer/f4-world-viewer and the
// templates at <build>/scenarios — hence the ../scenarios walk-up.
std::vector<fs::path> scenario_dirs() {
    std::vector<fs::path> dirs;
    const fs::path exe_dir = GetApplicationDirectory();
    dirs.push_back(exe_dir / ".." / "scenarios");
    dirs.push_back(exe_dir / "scenarios");
    dirs.push_back("scenarios");          // CWD (run from the build root)
    return dirs;
}

// The trace conventions the two recorders actually write, in order:
//   * qc/<stem>/trace.json            — campaign_qc --scenario (CWD and
//                                       build-root forms)
//   * <stem>_trace.json beside the    — the tanker_track template's own
//     build root                        record_path convention
std::vector<fs::path> trace_candidates(const std::string& stem) {
    std::vector<fs::path> out;
    const fs::path exe_dir = GetApplicationDirectory();
    out.push_back(exe_dir / ".." / "qc" / stem / "trace.json");
    out.push_back("qc" / fs::path(stem) / "trace.json");
    out.push_back(exe_dir / ".." / (stem + "_trace.json"));
    return out;
}

} // namespace

void ViewerApp::draw_mission_qc_view() {
    if (!impl_->show_mission_qc) return;

    ImGui::SetNextWindowSize(ImVec2(620, 440), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Mission QC", &impl_->show_mission_qc)) {
        ImGui::End();
        return;
    }

    // Scan lazily (first open + the Rescan button). The directory walk
    // is tiny (18 templates); caching just avoids per-frame stat noise.
    if (!impl_->mission_qc_scanned) scan_mission_qc();

    ImGui::TextDisabled(
        "Fly the scenario live (3D) or open its recorded trace (2D replay).");
    ImGui::Separator();

    if (ImGui::Button("Rescan")) {
        impl_->mission_qc_entries.clear();
        impl_->mission_qc_scanned = false;
    }
    ImGui::SameLine();
    if (ImGui::Button("Open trace file...")) {
        auto path = pick_open_file(
            "Open Flight Recording (trace.json)",
            "Flight Recording JSON (*.json)|All files (*.*)", {});
        if (!path.empty()) {
            std::string err;
            if (!load_replay(path, &err)) {
                impl_->last_error = err;
                impl_->status_msg = "Replay load failed: " + err;
            }
        }
    }
    ImGui::Separator();

    if (impl_->mission_qc_entries.empty()) {
        ImGui::TextDisabled(
            "No scenario templates found (looked in <exe>/../scenarios, "
            "<exe>/scenarios, ./scenarios).");
        ImGui::TextDisabled("Build the tree first — the templates are "
                            "generated into build/scenarios.");
    }

    // One row per template: name, trace status, the action button.
    // The table stays tiny; no clipper needed.
    if (ImGui::BeginTable("mission_qc", 3,
                          ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableSetupColumn("scenario", ImGuiTableColumnFlags_WidthStretch, 2.0f);
        ImGui::TableSetupColumn("trace", ImGuiTableColumnFlags_WidthStretch, 3.0f);
        ImGui::TableSetupColumn("action", ImGuiTableColumnFlags_WidthFixed, 120.0f);
        ImGui::TableHeadersRow();
        for (const auto& e : impl_->mission_qc_entries) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(e.name.c_str());
            ImGui::TableNextColumn();
            if (e.has_trace) {
                ImGui::TextUnformatted(e.trace_path.c_str());
            } else {
                ImGui::TextDisabled("(not recorded)");
            }
            ImGui::TableNextColumn();
            if (e.has_trace) {
                if (ImGui::Button(("Open replay##" + e.name).c_str())) {
                    std::string err;
                    if (!load_replay(e.trace_path, &err)) {
                        impl_->last_error = err;
                        impl_->status_msg = "Replay load failed: " + err;
                    } else {
                        impl_->status_msg =
                            "Mission QC: opened " + e.name + " replay";
                    }
                }
            } else {
                ImGui::TextDisabled("run step 1 below");
            }
        }
        ImGui::EndTable();
    }

    ImGui::Separator();
    // The two commands, spelled out — the menu is a MENU, but the user
    // concept is file-based and the commands belong on the screen.
    ImGui::TextDisabled("1. record:   campaign_qc --scenario "
                        "build/scenarios/<name>.json [--minutes N]");
    ImGui::TextDisabled("   (or:      f4-world-viewer --scenario "
                        "build/scenarios/<name>.json --run "
                        "--record qc/<name>/trace.json)");
    ImGui::TextDisabled("2. replay:   this window's Open replay — trail "
                        "colored by cross-track error, ai_state, fuel.");

    ImGui::End();
}

void ViewerApp::scan_mission_qc() {
    impl_->mission_qc_scanned = true;
    impl_->mission_qc_entries.clear();

    // Collect scenarios from all candidate dirs (deduped by stem — the
    // first hit wins; the exe-relative one is the build tree's own).
    std::vector<MissionQcEntry> found;
    for (const auto& dir : scenario_dirs()) {
        std::error_code ec;
        if (!fs::is_directory(dir, ec)) continue;
        for (const auto& p : fs::directory_iterator(dir, ec)) {
            if (p.path().extension() != ".json") continue;
            const std::string stem = p.path().stem().string();
            bool dup = false;
            for (const auto& e : found)
                if (e.name == stem) { dup = true; break; }
            if (dup) continue;
            MissionQcEntry entry;
            entry.name = stem;
            entry.scenario_path = p.path().string();
            for (const auto& t : trace_candidates(stem)) {
                std::error_code ec2;
                if (fs::is_regular_file(t, ec2)) {
                    entry.trace_path = t.string();
                    entry.has_trace = true;
                    break;
                }
            }
            found.push_back(std::move(entry));
        }
    }
    std::sort(found.begin(), found.end(),
              [](const MissionQcEntry& a, const MissionQcEntry& b) {
                  return a.name < b.name;
              });
    impl_->mission_qc_entries = std::move(found);
}

} // namespace f4::viewer
