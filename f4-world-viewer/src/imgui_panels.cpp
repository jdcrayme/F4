// f4-world-viewer/src/imgui_panels.cpp
//
// ViewerApp::draw_imgui — the entire ImGui frame: menu bar (File, View,
// Windows, Campaign, Tools, Help), the Layers panel (collapsible layer
// groups + filters + camera + map legend + status), inspector panel,
// status bar, and the modal popups (legacy file dialog, install summary,
// open campaign, install diagnostics, campaign load error). Plus
// ViewerApp::open_file_dialog (the legacy text-input modal back door
// used by some menu items when tinyfiledialogs isn't available).
//
// Split out of the original 1920-LoC viewer_app.cpp god-file (item #5
// of the architecture review). No behavior change.
//
// This file is the largest of the split (~620 LoC) because draw_imgui
// is one cohesive function that walks the entire panel/modal set in a
// single ImGui frame. Further splitting would require either many new
// private member declarations on the public header (bloating the API
// surface) or a helper-struct-of-function-pointers indirection — both
// of which add friction without obvious payoff. The current shape is
// "one function, one file, one concern: rendering the UI".

#include "viewer_state.hpp"
#include "diagnostics.hpp"

#include <f4/viewer/file_dialog.hpp>
#include <f4/world_types/class_table.hpp>        // unit_subtype_name(), DOMAIN_*

#include <imgui.h>
#include <rlImGui.h>
#include <raylib.h>

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <string>

namespace f4::viewer {

// ---------------------------------------------------------------------------
// Layer-toggle checkboxes (single source of truth)
// ---------------------------------------------------------------------------

// The View menu and the Layers panel render the same toggle groups through
// for_each_layer_group(), so the two lists cannot drift apart again.
// Window toggles (ATO, Campaign Session, Minimap…) deliberately live in
// the Windows menu instead — these groups are canvas LAYERS only.
namespace {

struct LayerToggle {
    const char* label;
    bool* value;
};

// Walk every canvas layer group in display order, handing each to `fn`
// as (group title, group index, toggles). The index lets the panel put
// the first group open by default. ImplT stays a deduced template
// parameter — ViewerApp::Impl is a private nested type that file-scope
// code cannot name (see campaign_qc_view.cpp).
template <typename ImplT, typename Fn>
void for_each_layer_group(ImplT* impl, Fn&& fn) {
    fn("Base layers", 0, {
        {"Terrain",    &impl->show_terrain},
        {"Objectives", &impl->show_objectives},
        {"Units",      &impl->show_units},
        {"Grid",       &impl->show_grid},
    });
    fn("Overlays", 1, {
        {"Radar arcs (static)",      &impl->show_radar_arcs},
        {"Front line (FLOT)",        &impl->show_flot},
        {"Supply state",             &impl->show_supply},
        {"All flight plans",         &impl->show_all_routes},
        {"Squadron→Airbase",         &impl->show_squadron_links},
    });
    fn("Campaign QC", 2, {
        {"Mission→Target links",  &impl->show_mission_links},
        {"Package→Element links", &impl->show_package_links},
        {"Bullseye",              &impl->show_bullseye},
    });
    fn("Live session", 3, {
        {"Live aircraft layer", &impl->show_live_layer},
        {"Live routes",         &impl->show_live_routes},
        {"Threat map overlay",  &impl->show_threat_overlay},
    });
}

// View-menu rendering: one submenu per group (toggle checkmarks).
template <typename ImplT>
void draw_layer_groups_menu(ImplT* impl) {
    for_each_layer_group(impl,
        [](const char* title, int,
           std::initializer_list<LayerToggle> toggles) {
            if (ImGui::BeginMenu(title)) {
                for (const auto& t : toggles)
                    ImGui::MenuItem(t.label, nullptr, t.value);
                ImGui::EndMenu();
            }
        });
}

// Layers-panel rendering: one collapsing header per group. Only the
// first group (Base layers) is open on first use; ImGui persists the
// open state per header from there on.
template <typename ImplT>
void draw_layer_groups_panel(ImplT* impl) {
    for_each_layer_group(impl,
        [](const char* title, int group_idx,
           std::initializer_list<LayerToggle> toggles) {
            if (group_idx == 0)
                ImGui::SetNextItemOpen(true, ImGuiCond_FirstUseEver);
            if (ImGui::CollapsingHeader(title)) {
                for (const auto& t : toggles)
                    ImGui::Checkbox(t.label, t.value);
            }
        });
}

} // namespace

// ---------------------------------------------------------------------------
// ImGui panels
// ---------------------------------------------------------------------------
void ViewerApp::draw_imgui() {
    rlImGuiBegin();

    // --- Menu bar ---
    if (ImGui::BeginMainMenuBar()) {
        if (ImGui::BeginMenu("File")) {
            // --- Primary flow (install-aware) ---
            if (ImGui::MenuItem("Set Install Path...")) {
                set_install_path_dialog();
            }
            if (ImGui::MenuItem("Open Campaign...", nullptr, false,
                                 impl_->install.has_value())) {
                open_campaign_dialog();
            }
            // Show the current install path as a disabled hint so the
            // user can see at a glance whether an install is configured.
            if (impl_->install) {
                ImGui::TextDisabled("    %s",
                    impl_->install->root().string().c_str());
            } else {
                ImGui::TextDisabled("    (no install set)");
            }
            ImGui::Separator();

            // --- Advanced / dev path (legacy + manual file picking) ---
            if (ImGui::BeginMenu("Advanced")) {
                if (ImGui::MenuItem("Open World JSON...")) {
                    auto path = pick_open_file(
                        "Open World JSON",
                        "World JSON (*.world.json)|JSON (*.json)|All files (*.*)",
                        impl_->last_world_json_path);
                    if (!path.empty()) {
                        try { load_world_json(path); }
                        catch (const std::exception& e) {
                            impl_->last_error = e.what();
                        }
                    }
                }
                if (ImGui::MenuItem("Open Terrain JSON...")) {
                    auto path = pick_open_file(
                        "Open Terrain JSON",
                        "Terrain JSON (*.terrain.json)|JSON (*.json)|All files (*.*)",
                        impl_->last_terrain_json_path);
                    if (!path.empty()) {
                        try { load_terrain_json(path); }
                        catch (const std::exception& e) {
                            impl_->last_error = e.what();
                        }
                    }
                }
                ImGui::Separator();
                if (ImGui::MenuItem("Import .cam Archive...")) {
                    auto path = pick_open_file(
                        "Import .cam",
                        "Campaign Archive (*.cam)|All files (*.*)",
                        impl_->last_world_json_path);
                    if (!path.empty()) {
                        try { import_cam_archive(path); }
                        catch (const std::exception& e) {
                            impl_->last_error = e.what();
                        }
                    }
                }
                if (ImGui::MenuItem("Import THEATER.* Binary...")) {
                    // Now that we have a real folder picker, this Just Works.
                    auto dir = pick_folder("Select THEATER.* Directory");
                    if (!dir.empty()) {
                        try { import_terrain_binary(dir); }
                        catch (const std::exception& e) {
                            impl_->last_error = e.what();
                        }
                    }
                }
                ImGui::EndMenu();
            }
            ImGui::Separator();
            // Path B2: open a FlightRecorder trace JSON for replay.
            // Uses the same pick_open_file pattern as the other Open
            // menu items. On success, switches the viewer to replay
            // mode (run() will dispatch to draw_replay_canvas +
            // draw_replay_panel).
            if (ImGui::MenuItem("Open Replay...", "Ctrl+R")) {
                auto path = pick_open_file(
                    "Open Flight Recording (trace.json)",
                    "Flight Recording JSON (*.json)|All files (*.*)",
                    {});
                if (!path.empty()) {
                    std::string err;
                    if (!load_replay(path, &err)) {
                        impl_->last_error = err;
                        impl_->status_msg = "Replay load failed: " + err;
                    }
                }
            }
            // SHOWCASE-1: the mission-QC discovery window — the scenario
            // template roster + their recorded traces, one click from a
            // geometry replay. See mission_qc_view.cpp.
            if (ImGui::MenuItem("Mission QC...")) {
                impl_->show_mission_qc = true;
            }
            ImGui::Separator();
            if (ImGui::MenuItem("Exit", "Alt+F4")) {
                // Phase 2 fix: was a no-op (the comment admitted it).
                // Now sets the should_exit flag, which run() checks each
                // frame to break the loop. We don't call CloseWindow()
                // directly because that would skip the rlImGui + Raylib
                // shutdown sequence that run() performs after the loop.
                impl_->should_exit = true;
            }
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("View")) {
            // Canvas layers, grouped into submenus (same table as the
            // Layers panel).
            draw_layer_groups_menu(impl_.get());
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Windows")) {
            // Every panel in one place: show/hide anything without
            // hunting for its checkbox in a layer list. The Tools
            // panels keep their own Tools menu entries.
            ImGui::MenuItem("Layers", nullptr, &impl_->show_layers_panel);
            ImGui::MenuItem("Inspector", nullptr, &impl_->show_inspector);
            ImGui::Separator();
            ImGui::MenuItem("Campaign Info", nullptr, &impl_->show_campaign_info);
            ImGui::MenuItem("ATO / Tasking", nullptr, &impl_->show_ato);
            ImGui::MenuItem("Campaign Session", nullptr, &impl_->show_campaign_window);
            ImGui::MenuItem("Mission QC", nullptr, &impl_->show_mission_qc);
            ImGui::MenuItem("Minimap", nullptr, &impl_->show_minimap);
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Campaign")) {
            // V-CAMP: the live campaign loop menu — Start/Stop/Reset
            // mirror the Campaign Session window's own controls (the
            // window carries the full filter + speed UI; this is the
            // quick path).
            if (!impl_->session) {
                if (ImGui::MenuItem("Start Session...", nullptr, false,
                                    impl_->world_loaded)) {
                    // The start is ASYNC — impl_->session is still null
                    // when this returns, so open the window now and let
                    // it show the "Starting session…" progress row.
                    impl_->show_campaign_window = true;
                    start_campaign_session();
                }
                if (!impl_->world_loaded) {
                    ImGui::TextDisabled("(no world loaded)");
                }
            } else {
                const bool menu_paused = impl_->session_runner
                    ? impl_->session_runner->paused()
                    : true;  // dead branch: a session always adopts with its runner
                if (ImGui::MenuItem(menu_paused ? "Play (Space)"
                                                : "Pause (Space)")) {
                    // The pause contract lives in set_session_paused()
                    // (we hold the frame lock here, which is what it
                    // expects).
                    set_session_paused(!menu_paused);
                }
                if (ImGui::MenuItem("Reset Session")) {
                    stop_campaign_session();
                    start_campaign_session();
                }
                if (ImGui::MenuItem("Stop Session")) {
                    stop_campaign_session();
                }
                ImGui::Separator();
                if (ImGui::MenuItem("Write Result JSON")) {
                    write_result_json();
                }
            }
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Tools")) {
            // Hex Inspector — opens a panel for inspecting raw bytes of
            // any file (FALCON4.ct, .cam, THEATER.*, etc.) with decoder
            // overlays. The primary RE tool.
            const bool hex_open = impl_->hex_inspector.is_open();
            if (ImGui::MenuItem("Hex Inspector...", nullptr, hex_open)) {
                if (!hex_open) impl_->hex_inspector.open();
            }
            // Class Table Browser — browsable, filterable, exportable view
            // over the Falcon4.ct class table + OCD/UCD/VCD/FCD data.
            const bool ctb_open = impl_->class_table_browser.is_open();
            if (ImGui::MenuItem("Class Table Browser...", nullptr, ctb_open)) {
                if (!ctb_open) impl_->class_table_browser.open();
            }
            // Symbol Creator — interactive editor for the data-driven
            // symbol library. Lets the user build symbol definitions
            // (lists of polylines + polygons) by dragging points on a
            // 2D canvas, then save/load the library to JSON. The
            // eventual refactor of symbols.cpp will consume the same
            // library data model (see f4/renderer/symbol_library.hpp).
            const bool sc_open = impl_->symbol_creator.is_open();
            if (ImGui::MenuItem("Symbol Creator...", nullptr, sc_open)) {
                if (!sc_open) impl_->symbol_creator.open();
            }
            ImGui::Separator();
            // Install Diagnostics — shows the full diagnostic report
            // (where we looked for FALCON4.ct, every theater dir probed,
            // every campaign path + exists check). The "what's actually
            // wrong with my install" tool.
            if (ImGui::MenuItem("Install Diagnostics...")) {
                open_install_diagnostics();
            }
            ImGui::Separator();
            // Snapshot Install Files — diagnostic tool for ground-truthing
            // static-data parsing milestone. Walks the install, dumps the
            // first 8 KB of every interesting Falcon4 data file (PHD/PD/
            // OCD/UCD/VCD/FED/FCD/AII/ct) as hex+ASCII to a single text
            // file the user can email back for ground-truth RE. See
            // Docs/FALCON4_FILE_LAYOUT.md and snapshot.hpp.
            if (ImGui::MenuItem("Snapshot Install Files...",
                                 nullptr, false,
                                 impl_->install.has_value())) {
                open_snapshot_dialog();
            }
            // List All Install Files — Walks the ENTIRE install root
            // recursively and lists every regular file (relative path
            // + size) to a single text file. No hex dumps — much smaller
            // than the snapshot. Used to document install layouts across
            // vanilla / FreeFalcon / BMS installs side-by-side and to
            // spot files our curated snapshot list missed.
            if (ImGui::MenuItem("List All Install Files...",
                                 nullptr, false,
                                 impl_->install.has_value())) {
                open_list_files_dialog();
            }
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Help")) {
            ImGui::TextDisabled("F4 World Viewer");
            ImGui::TextDisabled("Pan: drag  Zoom: wheel  Select: click  Fit: F");
            ImGui::TextDisabled("Engine-agnostic F4 world inspector");
            ImGui::EndMenu();
        }
        ImGui::EndMainMenuBar();
    }

    // --- Layers panel (left side) ---
    // Organized as collapsing sections so the default view is a few
    // headers instead of a checkbox wall: Base layers open, every
    // optional group (overlays, QC, live session, filters) collapsed
    // until wanted.
    ImGui::SetNextWindowPos(ImVec2(10, 30), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(240, 0), ImGuiCond_FirstUseEver);
    if (ImGui::Begin("Layers", &impl_->show_layers_panel,
                     ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_AlwaysAutoResize)) {
        // Same toggle groups as the View menu (single source of truth).
        draw_layer_groups_panel(impl_.get());

        // Phase 2: objective search/filter. Filters objectives by
        // class_name substring (case-insensitive). Empty = show all.
        // Phase 2: team filter dropdown. 0xFF = no filter (show all
        // teams); otherwise dim objectives/units owned by other teams.
        if (ImGui::CollapsingHeader("Filters")) {
            ImGui::TextUnformatted("Search objectives:");
            ImGui::PushItemWidth(220);
            // ImGui::InputText returns true if the text changed this frame.
            // POLISH-2.2: when the text changes, refresh the cached
            // lowercase needle so the canvas loop doesn't have to lowercase
            // the search string per-objective per-frame.
            if (ImGui::InputText("##obj_search", impl_->objective_search,
                                 sizeof(impl_->objective_search))) {
                impl_->update_search_cache();
            }
            ImGui::PopItemWidth();
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("Case-insensitive substring match on objective class_name");
            }
            ImGui::TextUnformatted("Team filter:");
            const char* team_labels[] = {
                "All teams", "0 Neutral", "1 Enemy", "2 Friendly",
                "3 ROK", "4 Japan", "5 DPRK", "6 PRC", "7 Other"
            };
            int tf_idx = (impl_->team_filter == 0xFF) ? 0 : static_cast<int>(impl_->team_filter) + 1;
            if (ImGui::Combo("##team_filter", &tf_idx, team_labels, 9)) {
                impl_->team_filter = (tf_idx == 0) ? 0xFF
                                                  : static_cast<uint8_t>(tf_idx - 1);
            }
        }

        // (The old Camera zoom slider + Fit-to-World button are gone:
        // the wheel zoom clamps to the fit extent, and the map scale
        // reference line replaces the numeric zoom readout.)

        // Status (only takes rows when there is something to say).
        if (!impl_->status_msg.empty() || !impl_->last_error.empty()) {
            ImGui::Separator();
            if (ImGui::CollapsingHeader("Status",
                                        ImGuiTreeNodeFlags_DefaultOpen)) {
                if (!impl_->status_msg.empty()) {
                    ImGui::TextWrapped("%s", impl_->status_msg.c_str());
                }
                if (!impl_->last_error.empty()) {
                    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.4f, 0.4f, 1.0f));
                    ImGui::TextWrapped("Error: %s", impl_->last_error.c_str());
                    ImGui::PopStyleColor();
                }
            }
        }
    }
    ImGui::End();

    // --- Inspector window (right side, below legend) ---
    // INSPECTOR-TABS-1: replaces three separate windows (Inspector,
    // Ground Layout, Ground Layout 3D) with a single window that hosts
    // all three views as tabs. See inspector_panel.cpp::
    // draw_inspector_window() for the tabbed layout. The content
    // functions (draw_inspector, draw_ground_layout_view,
    // draw_ground_layout_3d) are now content-only — they no longer
    // open their own ImGui::Begin/End; they draw into whatever tab
    // item is currently active.
    draw_inspector_window();

    // --- Status bar (bottom) ---
    ImGui::SetNextWindowPos(ImVec2(0.0f, static_cast<float>(impl_->window_h - 24)));
    ImGui::SetNextWindowSize(ImVec2(static_cast<float>(impl_->window_w), 24.0f));
    if (ImGui::Begin("##status", nullptr,
                     ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoMove |
                     ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoTitleBar |
                     ImGuiWindowFlags_NoBringToFrontOnFocus)) {
        const Vector2 mouse = GetMousePosition();
        float gx, gy;
        impl_->screen_to_world(mouse.x, mouse.y, &gx, &gy);
        ImGui::Text("Cursor: (%.1f, %.1f)  Zoom: %.2fx  FPS: %d",
                    gx, gy, impl_->cam_zoom, GetFPS());
        if (impl_->world_loaded) {
            ImGui::SameLine();
            ImGui::TextDisabled("|  %d objectives  %d units",
                                impl_->objectives().size(),
                                impl_->units().size());
        }
    }
    ImGui::End();

    // --- Pending file dialog modal ---
    // NOTE: must be inside the rlImGuiBegin/End block — calling ImGui
    // functions after rlImGuiEnd() crashes because the ImGui frame is
    // already finalized (the ID stack is empty, GetID() dereferences
    // an empty ImVector).
    //
    // --- Install summary modal (shown after Set Install Path succeeds) ---
    if (impl_->install_summary_open) {
        if (!ImGui::IsPopupOpen("Install Summary")) {
            ImGui::OpenPopup("Install Summary");
        }
        ImGui::SetNextWindowSize(ImVec2(500, 360), ImGuiCond_FirstUseEver);
        if (ImGui::BeginPopupModal("Install Summary",
                                    &impl_->install_summary_open,
                                    ImGuiWindowFlags_NoResize)) {
            ImGui::TextUnformatted(impl_->install_summary_text.c_str());
            ImGui::Separator();
            if (ImGui::Button("Open Campaign...")) {
                impl_->install_summary_open = false;
                ImGui::CloseCurrentPopup();
                open_campaign_dialog();
            }
            ImGui::SameLine();
            if (ImGui::Button("Close")) {
                impl_->install_summary_open = false;
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }
    }

    // --- Open Campaign modal (Theater + Campaign dropdowns) ---
    if (impl_->campaign_dialog_open && impl_->install) {
        if (!ImGui::IsPopupOpen("Open Campaign")) {
            ImGui::OpenPopup("Open Campaign");
        }
        ImGui::SetNextWindowSize(ImVec2(440, 220), ImGuiCond_FirstUseEver);
        if (ImGui::BeginPopupModal("Open Campaign",
                                    &impl_->campaign_dialog_open,
                                    ImGuiWindowFlags_NoResize)) {
            // --- Theater dropdown ---
            const auto& theaters = impl_->install->theaters();
            const auto* cur_theater =
                (impl_->campaign_dialog_theater_idx >= 0 &&
                 impl_->campaign_dialog_theater_idx < static_cast<int>(theaters.size()))
                    ? &theaters[impl_->campaign_dialog_theater_idx] : nullptr;
            const std::string theater_preview = cur_theater
                ? (cur_theater->display_name + " (" + cur_theater->key + ")")
                : "(none)";
            if (ImGui::BeginCombo("Theater", theater_preview.c_str())) {
                for (int i = 0; i < static_cast<int>(theaters.size()); ++i) {
                    const bool sel = (i == impl_->campaign_dialog_theater_idx);
                    const std::string label = theaters[i].display_name + " (" +
                                              theaters[i].key + ")";
                    if (ImGui::Selectable(label.c_str(), sel)) {
                        impl_->campaign_dialog_theater_idx = i;
                        // Theater changed — refresh the campaigns list.
                        impl_->campaign_dialog_campaigns =
                            impl_->install->campaigns_for(theaters[i].key);
                        impl_->campaign_dialog_campaign_idx = 0;
                    }
                    if (sel) ImGui::SetItemDefaultFocus();
                }
                ImGui::EndCombo();
            }

            // --- Campaign dropdown (depends on selected theater) ---
            const auto& camps = impl_->campaign_dialog_campaigns;
            const std::string camp_preview =
                (impl_->campaign_dialog_campaign_idx >= 0 &&
                 impl_->campaign_dialog_campaign_idx < static_cast<int>(camps.size()))
                    ? camps[impl_->campaign_dialog_campaign_idx].display_name
                    : "(no campaigns)";
            if (ImGui::BeginCombo("Campaign", camp_preview.c_str())) {
                if (camps.empty()) {
                    ImGui::TextDisabled("No .cam saves found in this theater");
                }
                for (int i = 0; i < static_cast<int>(camps.size()); ++i) {
                    const bool sel = (i == impl_->campaign_dialog_campaign_idx);
                    if (ImGui::Selectable(camps[i].display_name.c_str(), sel)) {
                        impl_->campaign_dialog_campaign_idx = i;
                    }
                    if (sel) ImGui::SetItemDefaultFocus();
                }
                ImGui::EndCombo();
            }

            ImGui::Separator();
            if (ImGui::Button("Load") &&
                impl_->campaign_dialog_campaign_idx >= 0 &&
                impl_->campaign_dialog_campaign_idx < static_cast<int>(camps.size())) {
                const auto& theater = theaters[impl_->campaign_dialog_theater_idx];
                const auto& camp = camps[impl_->campaign_dialog_campaign_idx];
                try {
                    load_campaign_from_install(theater.key, camp.stem);
                    impl_->campaign_dialog_open = false;
                    ImGui::CloseCurrentPopup();
                } catch (const std::exception& e) {
                    // Build a detailed error report so the user can see
                    // exactly what failed (incomplete theater? missing
                    // .cam? parse error?) without having to open the
                    // diagnostics panel separately.
                    impl_->last_error = e.what();
                    impl_->campaign_load_error_text = build_campaign_load_error(
                        *impl_->install, theater.key, camp.stem, e.what());
                    impl_->campaign_load_error_open = true;
                }
            }
            ImGui::SameLine();
            if (ImGui::Button("Cancel")) {
                impl_->campaign_dialog_open = false;
                ImGui::CloseCurrentPopup();
            }

            // Helpful hint when no campaigns are present.
            if (camps.empty() && cur_theater) {
                ImGui::Separator();
                ImGui::TextWrapped(
                    "No .cam files found under campaign/%s/ or campaign/.\n"
                    "Start a new campaign in Falcon 4.0 first, then refresh.",
                    cur_theater->key.c_str());
            }
            ImGui::EndPopup();
        }
    }

    // --- Install Diagnostics modal (Tools > Install Diagnostics) ---
    if (impl_->install_diagnostics_open) {
        if (!ImGui::IsPopupOpen("Install Diagnostics")) {
            ImGui::OpenPopup("Install Diagnostics");
        }
        ImGui::SetNextWindowSize(ImVec2(700, 500), ImGuiCond_FirstUseEver);
        if (ImGui::BeginPopupModal("Install Diagnostics",
                                    &impl_->install_diagnostics_open,
                                    ImGuiWindowFlags_NoResize)) {
            // Render the diagnostic text in a scrollable, selectable
            // (copyable) read-only text box. The user can select-all +
            // copy to share the full report.
            ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.10f, 0.10f, 0.12f, 1.0f));
            ImGui::BeginChild("diag_text", ImVec2(0, -40), true,
                               ImGuiWindowFlags_HorizontalScrollbar);
            // Use InputTextMultiline as a read-only text viewer — it
            // supports selection + copy out of the box, unlike
            // ImGui::TextUnformatted which doesn't allow selection.
            // We use a sufficiently large buffer and disable editing.
            // (The text is in impl_->install_diagnostics_text, which we
            // need to copy into a mutable buffer for InputTextMultiline.)
            // Phase 2: was `static std::string diag_buf` — moved to Impl
            // member to fix thread-safety + reentrancy.
            impl_->diag_buf = impl_->install_diagnostics_text;
            impl_->diag_buf.resize(impl_->diag_buf.size() + 1, '\0');
            ImGui::InputTextMultiline("##diag_input",
                                       impl_->diag_buf.data(), impl_->diag_buf.size(),
                                       ImVec2(-1, -1),
                                       ImGuiInputTextFlags_ReadOnly |
                                       ImGuiInputTextFlags_AllowTabInput);
            ImGui::EndChild();
            ImGui::PopStyleColor();
            ImGui::Separator();
            if (ImGui::Button("Copy to Clipboard")) {
                ImGui::SetClipboardText(impl_->install_diagnostics_text.c_str());
            }
            ImGui::SameLine();
            if (ImGui::Button("Close")) {
                impl_->install_diagnostics_open = false;
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }
    }

    // --- Campaign Load Error modal ---
    // Shown when load_campaign_from_install throws. Shows the full error
    // message + theater/campaign/class-table context so the user can
    // diagnose the failure without having to open the diagnostics panel.
    if (impl_->campaign_load_error_open) {
        if (!ImGui::IsPopupOpen("Campaign Load Failed")) {
            ImGui::OpenPopup("Campaign Load Failed");
        }
        ImGui::SetNextWindowSize(ImVec2(600, 450), ImGuiCond_FirstUseEver);
        if (ImGui::BeginPopupModal("Campaign Load Failed",
                                    &impl_->campaign_load_error_open,
                                    ImGuiWindowFlags_NoResize)) {
            ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.18f, 0.10f, 0.10f, 1.0f));
            ImGui::BeginChild("err_text", ImVec2(0, -40), true,
                               ImGuiWindowFlags_HorizontalScrollbar);
            // Same InputTextMultiline trick for selectability.
            // Phase 2: was `static std::string err_buf` — moved to Impl.
            impl_->err_buf = impl_->campaign_load_error_text;
            impl_->err_buf.resize(impl_->err_buf.size() + 1, '\0');
            ImGui::InputTextMultiline("##err_input",
                                       impl_->err_buf.data(), impl_->err_buf.size(),
                                       ImVec2(-1, -1),
                                       ImGuiInputTextFlags_ReadOnly |
                                       ImGuiInputTextFlags_AllowTabInput);
            ImGui::EndChild();
            ImGui::PopStyleColor();
            ImGui::Separator();
            if (ImGui::Button("Copy to Clipboard")) {
                ImGui::SetClipboardText(impl_->campaign_load_error_text.c_str());
            }
            ImGui::SameLine();
            if (ImGui::Button("Open Install Diagnostics")) {
                impl_->campaign_load_error_open = false;
                ImGui::CloseCurrentPopup();
                open_install_diagnostics();
            }
            ImGui::SameLine();
            if (ImGui::Button("Close")) {
                impl_->campaign_load_error_open = false;
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }
    }

    // --- Hex Inspector panel (Tools > Hex Inspector) ---
    impl_->hex_inspector.draw();

    // --- Class Table Browser panel (Tools > Class Table Browser) ---
    // Provide the shared render resources (glTF model cache + textures +
    // lit shader) for the 3D visType preview. The glTF data is
    // lazy-discovered by ensure_models_3d_loaded(); force-load it here
    // when the browser is open so visType previews always work.
    if (impl_->class_table_browser.is_open()) {
        impl_->ensure_models_3d_loaded();
    }
    impl_->class_table_browser.set_render_resources(&impl_->render_res_3d);
    // The browser derives objective feature collections (its CLASS_OBJECTIVE
    // detail section) from the loaded world; the generation counter tells it
    // when the world changed and the collections must be rebuilt.
    impl_->class_table_browser.set_entity_world(&impl_->eworld,
                                                impl_->world_generation);
    impl_->class_table_browser.draw();

    // --- Symbol Creator panel (Tools > Symbol Creator) ---
    // Interactive editor for the data-driven symbol library. Drawn
    // after the other Tools panels so it can take focus when opened.
    impl_->symbol_creator.draw();

    // --- Campaign + Teams panels (auto-open when a world is loaded —
    // show CampaignState fields and the .tea-enriched team roster
    // with stance matrix, country memberships, experience, and command
    // chain).
    draw_campaign_and_teams_view();

    // --- ATO / Tasking window (B.3 campaign QC) ---
    // The sortable flight table: callsign, mission, team, package, TOT,
    // target, squadron. Click-to-select + camera focus; shares the
    // mission/team filters with the canvas overlays. See
    // campaign_qc_view.cpp.
    draw_campaign_qc_view();

    // V-CAMP: the live campaign session window (draw last — the
    // generated-missions table reads the same tick run() just drained).
    draw_campaign_session_view();

    // SHOWCASE-1: the Mission QC discovery window (scenario roster +
    // recorded traces → replay). See mission_qc_view.cpp.
    draw_mission_qc_view();

    rlImGuiEnd();
}

} // namespace f4::viewer
