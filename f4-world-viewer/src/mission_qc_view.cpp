// f4-world-viewer/src/mission_qc_view.cpp
//
// The "Mission QC" window (SHOWCASE-1) — the discovery half of the
// mission-QC user concept: ONE place that lists every flyable mission-QC
// scenario (the world viewer's scenario-template library — tanker_track,
// landing_only, on_glideslope, digi_full_mission, ...) and, for each, the
// recorded FlightRecorder trace the headless runs produce.
//
// The workflow it fronts:
//   1. Record — the per-row Record/Re-record button (MISSION-QC-RECORD)
//      spawns the sibling campaign_qc recorder headlessly, in a
//      background thread, with the SAME arguments the CLI step takes:
//        campaign_qc --scenario <template> --out-dir <root>/qc/<stem>
//        → qc/<stem>/trace.json + scenario_qc_summary.json (gates 20–24)
//      The recorder is located in the build tree next to the viewer
//      (build_root() below); the build guarantees it exists
//      (add_dependencies in f4-world-viewer/CMakeLists.txt). One record
//      job at a time; the button reads Re-record when a trace already
//      exists (the rerun simply overwrites both artifacts). The CLI form
//      still works and writes the same files.
//      The live-watch alternative is unchanged: f4-world-viewer
//      --scenario build/scenarios/<name>.json --run
//      --record qc/<name>/trace.json (watch it in 3D, keep the trace).
//   2. click "Open replay" here → the trace loads in replay mode (the
//      scrubber, per-aircraft trail colored by cross-track error, the
//      dashed intended path, ai_state) — geometry QC without a window.
//
// The scan is deliberately dumb: a directory walk over the scenarios
// dir(s) next to the executable plus the CWD, and a fixed set of trace
// conventions (the ones the recorders actually write). No config file,
// no registry — the filesystem IS the menu.

#include "viewer_state.hpp"
#include "record_runner.hpp"
#include <f4/viewer/file_dialog.hpp>

#include <imgui.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <string>
#include <thread>
#include <vector>

// raylib (GetApplicationDirectory) — the viewer links it anyway.
#include <raylib.h>

namespace f4::viewer {

namespace {

namespace fs = std::filesystem;

fs::path exe_dir() {
    return fs::path(GetApplicationDirectory());
}

std::string recorder_exe_name() {
#ifdef _WIN32
    return "campaign_qc.exe";
#else
    return "campaign_qc";
#endif
}

// The build root — the directory that holds the f4-simulation target dir
// (where the campaign_qc recorder lives). Single-config generators put
// every target dir directly under <build>/, so exe_dir/.. IS the root;
// multi-config (MSVC) adds a per-config layer (exe_dir =
// <build>/f4-world-viewer/Debug), so the root is two up. Walk up from
// the exe looking for the f4-simulation sibling; fall back to exe_dir/..
// (the single-config form, and the best guess for an installed layout).
fs::path build_root() {
    const fs::path base = exe_dir();
    fs::path p = base;
    for (int depth = 0; depth < 4 && p.has_parent_path(); ++depth) {
        std::error_code ec;
        if (fs::is_directory(p / "f4-simulation", ec)) return p;
        if (p.parent_path() == p) break;
        p = p.parent_path();
    }
    return base / "..";
}

// The scenarios directories to scan, most-specific first. The viewer
// binary lives at <build>/f4-world-viewer[/Debug] and the templates at
// <build>/scenarios.
std::vector<fs::path> scenario_dirs() {
    std::vector<fs::path> dirs;
    const fs::path base = exe_dir();
    dirs.push_back(base / ".." / "scenarios");
    dirs.push_back(base / "scenarios");
    dirs.push_back("scenarios");          // CWD (run from the build root)
    return dirs;
}

// The trace conventions the recorders actually write, in order:
//   * <build root>/qc/<stem>/trace.json — campaign_qc --scenario (the
//     Record button AND a CLI run from the build root land here — under
//     multi-config generators this is the only build-tree form that
//     matches the CLI's CWD-relative output)
//   * qc/<stem>/trace.json              — a CLI run's CWD form
//   * <exe>/../qc/<stem>/trace.json     — legacy pre-multi-config form
//   * <stem>_trace.json beside the      — the tanker_track template's own
//     build root                          record_path convention
std::vector<fs::path> trace_candidates(const std::string& stem) {
    std::vector<fs::path> out;
    out.push_back(build_root() / "qc" / stem / "trace.json");
    out.push_back("qc" / fs::path(stem) / "trace.json");
    out.push_back(exe_dir() / ".." / "qc" / stem / "trace.json");
    out.push_back(exe_dir() / ".." / (stem + "_trace.json"));
    return out;
}

// Where the Record button's recorder writes for a template — the first
// trace convention, so the rescan finds the artifacts by construction.
fs::path mission_qc_out_dir(const std::string& stem) {
    return build_root() / "qc" / stem;
}

// Locate the sibling campaign_qc recorder in the build tree. Prefer the
// viewer's own config (multi-config layouts name it in exe_dir's leaf)
// so a Debug viewer never pairs with a stale Release recorder.
std::string resolve_campaign_qc_tool() {
    static const char* kConfigs[] = {
        "Debug", "Release", "RelWithDebInfo", "MinSizeRel", "",
    };
    const fs::path base = build_root() / "f4-simulation";
    const std::string leaf = exe_dir().filename().string();
    std::vector<fs::path> candidates;
    for (const char* c : kConfigs) {
        if (*c && leaf == c) {
            candidates.push_back(base / c / recorder_exe_name());
            break;
        }
    }
    for (const char* c : kConfigs) {
        candidates.push_back(*c ? base / c / recorder_exe_name()
                                : base / recorder_exe_name());
    }
    std::error_code ec;
    for (const auto& c : candidates)
        if (fs::is_regular_file(c, ec)) return c.string();
    return {};
}

// The recorder's exit codes are the QC verdict (run_scenario's gate
// ladder) — name them the way the cookbook does.
const char* gate_text(int code) {
    switch (code) {
        case 0:  return "pass";
        case 1:  return "scenario load/init failed";
        case 20: return "gate 20 — no aircraft spawned";
        case 21: return "gate 21 — FROZEN (nothing flew)";
        case 22: return "gate 22 — AAR never made contact";
        case 23: return "gate 23 — AAR incomplete";
        case 24: return "gate 24 — no touchdown";
        default: return "recorder failed";
    }
}

// Spawn one detached record job. The thread owns the process and the
// shared result flags; the UI only polls. Detached at spawn so a viewer
// closed mid-record leaves the recorder running to finish its write.
MissionQcJob spawn_record_job(const std::string& stem, const fs::path& tool,
                              const fs::path& scenario,
                              const fs::path& out_dir) {
    MissionQcJob job;
    job.stem = stem;
    job.done = std::make_shared<std::atomic_bool>(false);
    job.exit_code = std::make_shared<std::atomic_int>(-1);
    job.diag = std::make_shared<std::string>();
    auto done = job.done;
    auto code = job.exit_code;
    auto diag = job.diag;
    std::thread([done, code, diag, tool, scenario, out_dir]() {
        code->store(run_recorder(tool, scenario, out_dir, diag.get()));
        done->store(true);
    }).detach();
    return job;
}

} // namespace

void ViewerApp::draw_mission_qc_view() {
    if (!impl_->show_mission_qc) return;

    ImGui::SetNextWindowSize(ImVec2(760, 440), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Mission QC", &impl_->show_mission_qc)) {
        ImGui::End();
        return;
    }

    // Scan lazily (first open + the Rescan button). The directory walk
    // is tiny (18 templates); caching just avoids per-frame stat noise.
    if (!impl_->mission_qc_scanned) scan_mission_qc();

    // Resolve the recorder once (a filesystem walk; cheap, but no need
    // to repeat it per frame).
    if (!impl_->mission_qc_tool_checked) {
        impl_->mission_qc_tool_checked = true;
        impl_->mission_qc_tool = resolve_campaign_qc_tool();
    }

    // Report finished record jobs exactly once, then drop them. A
    // finished job invalidates the cached scan (its trace/summary just
    // appeared under the conventions above).
    for (auto& j : impl_->mission_qc_jobs) {
        if (j.reported || !j.done || !j.done->load()) continue;
        j.reported = true;
        impl_->mission_qc_scanned = false;
        const int code = j.exit_code ? j.exit_code->load() : -1;
        impl_->status_msg =
            code == 0
                ? "Mission QC: recorded " + j.stem + " (pass) — open its replay"
                : "Mission QC: recorded " + j.stem + " — " + gate_text(code);
        if (code != 0 && j.diag && !j.diag->empty())
            impl_->last_error = "Mission QC record of " + j.stem + ": " + *j.diag;
    }
    impl_->mission_qc_jobs.erase(
        std::remove_if(impl_->mission_qc_jobs.begin(),
                       impl_->mission_qc_jobs.end(),
                       [](const MissionQcJob& j) { return j.reported; }),
        impl_->mission_qc_jobs.end());
    // One record at a time (each run is CPU-bound; serial keeps the
    // results comparable and the box responsive).
    const MissionQcJob* active = nullptr;
    for (const auto& j : impl_->mission_qc_jobs)
        if (j.running()) { active = &j; break; }

    ImGui::TextDisabled(
        "Record a trace (headless), fly it live (3D), or open the replay "
        "(2D).");
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
                show_message_box("Replay Load Failed", err, "error");
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

    // One row per template: name, trace status, the two action buttons.
    // The table stays tiny; no clipper needed.
    if (ImGui::BeginTable("mission_qc", 4,
                          ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableSetupColumn("scenario", ImGuiTableColumnFlags_WidthStretch, 2.0f);
        ImGui::TableSetupColumn("trace", ImGuiTableColumnFlags_WidthStretch, 3.0f);
        ImGui::TableSetupColumn("replay", ImGuiTableColumnFlags_WidthFixed, 110.0f);
        ImGui::TableSetupColumn("record", ImGuiTableColumnFlags_WidthFixed, 110.0f);
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
                        show_message_box("Replay Load Failed", err, "error");
                    } else {
                        impl_->status_msg =
                            "Mission QC: opened " + e.name + " replay";
                    }
                }
            } else {
                ImGui::TextDisabled("-");
            }
            ImGui::TableNextColumn();
            const bool mine = active && active->stem == e.name;
            if (mine) {
                ImGui::TextDisabled("recording...");
            } else if (active) {
                ImGui::TextDisabled("(busy)");
            } else if (impl_->mission_qc_tool.empty()) {
                ImGui::TextDisabled("(no recorder)");
            } else {
                const char* label = e.has_trace ? "Re-record" : "Record";
                if (ImGui::Button((std::string(label) + "##" + e.name).c_str())) {
                    impl_->mission_qc_jobs.push_back(spawn_record_job(
                        e.name, impl_->mission_qc_tool, e.scenario_path,
                        mission_qc_out_dir(e.name)));
                    impl_->status_msg = "Mission QC: recording " + e.name +
                                        " (headless campaign_qc)...";
                }
            }
        }
        ImGui::EndTable();
    }

    ImGui::Separator();
    if (impl_->mission_qc_tool.empty()) {
        ImGui::TextDisabled(
            "Recorder not found: build the campaign_qc target "
            "(f4-simulation) — it sits beside this viewer in the build "
            "tree.");
    }
    // The two commands, spelled out — the menu is a MENU, but the user
    // concept is file-based and the commands belong on the screen.
    ImGui::TextDisabled("1. record:   this window's Record button — or: "
                        "campaign_qc --scenario "
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

void ViewerApp::open_mission_qc_window() {
    impl_->show_mission_qc = true;
}

} // namespace f4::viewer
