// f4-world-viewer/src/imgui_panels.cpp
//
// ViewerApp::draw_imgui — the entire ImGui frame: menu bar, layers panel,
// legend panel, inspector panel, status bar, and the five modal popups
// (legacy file dialog, install summary, open campaign, install
// diagnostics, campaign load error). Plus ViewerApp::open_file_dialog
// (the legacy text-input modal back door used by some menu items when
// tinyfiledialogs isn't available).
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

#include <f4/terrain/terrain_data.hpp>
#include <f4/viewer/enum_text.hpp>
#include <f4/viewer/file_dialog.hpp>
#include <f4/world_convert/class_table.hpp>      // unit_subtype_name(), DOMAIN_*
#include <f4/world_convert/objective_decoder.hpp> // objective_type_name()
#include <f4/world_convert/theater_data.hpp>     // point_type_name(), point_list_type_name()

#include <imgui.h>
#include <rlImGui.h>
#include <raylib.h>

#include <cstdio>
#include <filesystem>
#include <string>

namespace f4::viewer {

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
            ImGui::TextDisabled("Base layers");
            ImGui::Checkbox("Terrain",     &impl_->show_terrain);
            ImGui::Checkbox("Objectives",  &impl_->show_objectives);
            ImGui::Checkbox("Units",       &impl_->show_units);
            ImGui::Checkbox("Grid",        &impl_->show_grid);
            ImGui::Checkbox("Legend",      &impl_->show_legend);
            ImGui::Separator();
            ImGui::TextDisabled("Overlays");
            ImGui::Checkbox("Radar arcs",          &impl_->show_radar_arcs);
            ImGui::Checkbox("Ground layout",       &impl_->show_ground_layout_overlay);
            ImGui::Checkbox("Feature 3D models",   &impl_->show_feature_meshes);
            ImGui::Checkbox("Unit destinations",   &impl_->show_unit_destinations);
            ImGui::Checkbox("Waypoints",           &impl_->show_waypoints);
            ImGui::Checkbox("Squadron→Airbase",    &impl_->show_squadron_links);
            // Phase 2: hierarchy lines toggle — was declared but never
            // exposed in the UI. Now wired up so users can actually
            // enable battalion→brigade OOB lines.
            ImGui::Checkbox("Hierarchy lines (BN→BDE)", &impl_->show_hierarchy_lines);
            ImGui::Separator();
            ImGui::TextDisabled("Campaign QC (B.3)");
            ImGui::Checkbox("ATO / Tasking window",  &impl_->show_ato);
            ImGui::Checkbox("Mission→Target links",  &impl_->show_mission_links);
            ImGui::Checkbox("Package→Element links", &impl_->show_package_links);
            ImGui::Checkbox("Bullseye",               &impl_->show_bullseye);
            ImGui::Separator();
            ImGui::TextDisabled("Live session (V-CAMP)");
            ImGui::Checkbox("Campaign Session window", &impl_->show_campaign_window);
            ImGui::Checkbox("Live aircraft layer",     &impl_->show_live_layer);
            ImGui::Checkbox("Live routes",             &impl_->show_live_routes);
            ImGui::Checkbox("Threat map overlay",      &impl_->show_threat_overlay);
            // POLISH-2.4: minimap toggle in View menu.
            ImGui::Checkbox("Minimap", &impl_->show_minimap);
            ImGui::Separator();
            if (ImGui::MenuItem("Fit to World")) impl_->fit_to_world();
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
                    start_campaign_session();
                    if (impl_->session) impl_->show_campaign_window = true;
                }
                if (!impl_->world_loaded) {
                    ImGui::TextDisabled("(no world loaded)");
                }
            } else {
                // V-THREAD: the menu draws inside run()'s frame
                // session-lock scope — flip the runner's ATOMIC flag
                // (set_paused() would re-lock the held mutex) AND
                // mirror the session's own flag directly (consistent:
                // the worker can't be mid-advance while we hold).
                const bool menu_paused = impl_->session_runner
                    ? impl_->session_runner->paused()
                    : impl_->session->paused();
                if (ImGui::MenuItem(menu_paused ? "Play (Space)"
                                                : "Pause (Space)")) {
                    if (impl_->session_runner) {
                        impl_->session_runner->set_paused_flag(!menu_paused);
                    }
                    impl_->session->set_paused(!menu_paused);
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
                    const auto out =
                        impl_->last_world_json_path.parent_path() /
                        "campaign_result.json";
                    FILE* f = std::fopen(out.string().c_str(), "wb");
                    if (f) {
                        const std::string json =
                            impl_->session->ledger_json();
                        std::fwrite(json.data(), 1, json.size(), f);
                        std::fclose(f);
                        impl_->status_msg = "Wrote " + out.string();
                    } else {
                        impl_->status_msg =
                            "Cannot write " + out.string();
                    }
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
            ImGui::TextDisabled("Pan: drag  Zoom: wheel  Select: click");
            ImGui::TextDisabled("Engine-agnostic F4 world inspector");
            ImGui::EndMenu();
        }
        ImGui::EndMainMenuBar();
    }

    // --- Layers panel (left side) ---
    ImGui::SetNextWindowPos(ImVec2(10, 30), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(240, 0), ImGuiCond_FirstUseEver);
    if (ImGui::Begin("Layers", nullptr,
                     ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextDisabled("Base");
        ImGui::Checkbox("Terrain",     &impl_->show_terrain);
        ImGui::Checkbox("Objectives",  &impl_->show_objectives);
        ImGui::Checkbox("Units",       &impl_->show_units);
        ImGui::Checkbox("Grid",        &impl_->show_grid);
        ImGui::Checkbox("Legend",      &impl_->show_legend);
        ImGui::Separator();
        ImGui::TextDisabled("Overlays");
        ImGui::Checkbox("Radar arcs",         &impl_->show_radar_arcs);
        ImGui::Checkbox("Ground layout",      &impl_->show_ground_layout_overlay);
        ImGui::Checkbox("Feature 3D models",  &impl_->show_feature_meshes);
        ImGui::Checkbox("Unit destinations",  &impl_->show_unit_destinations);
        ImGui::Checkbox("Waypoints",          &impl_->show_waypoints);
        ImGui::Checkbox("Squadron→Airbase",   &impl_->show_squadron_links);
        // Phase 2: hierarchy lines toggle — was declared but never exposed.
        ImGui::Checkbox("Hierarchy (BN→BDE)", &impl_->show_hierarchy_lines);
        // POLISH-2.4: minimap toggle in Layers panel too.
        ImGui::Checkbox("Minimap", &impl_->show_minimap);

        ImGui::Separator();
        // Phase 2: objective search/filter. Filters objectives by
        // class_name substring (case-insensitive). Empty = show all.
        ImGui::TextDisabled("Filter");
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
        // Phase 2: team filter dropdown. 0xFF = no filter (show all teams);
        // otherwise dim objectives/units owned by other teams.
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

        ImGui::Separator();
        ImGui::Text("Camera");
        ImGui::SliderFloat("Zoom", &impl_->cam_zoom, 0.1f, 150.0f, "%.1f");
        if (ImGui::Button("Fit to World")) impl_->fit_to_world();
        // Phase 2: keyboard shortcut hint.
        ImGui::SameLine();
        ImGui::TextDisabled("(F)");

        ImGui::Separator();
        ImGui::Text("Status");
        if (!impl_->status_msg.empty()) {
            ImGui::TextWrapped("%s", impl_->status_msg.c_str());
        }
        if (!impl_->last_error.empty()) {
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.4f, 0.4f, 1.0f));
            ImGui::TextWrapped("Error: %s", impl_->last_error.c_str());
            ImGui::PopStyleColor();
        }
    }
    ImGui::End();

    // --- Legend (right side, when toggled) ---
    if (impl_->show_legend) {
        ImGui::SetNextWindowPos(ImVec2(impl_->window_w - 230, 30), ImGuiCond_FirstUseEver);
        if (ImGui::Begin("Legend", &impl_->show_legend, ImGuiWindowFlags_NoCollapse)) {
            ImGui::TextUnformatted("Terrain");
            for (int t = 0; t <= 5; ++t) {
                const auto c = f4::terrain::TerrainData::color_for_tile_type(
                    static_cast<f4::terrain::TileType>(t));
                ImGui::PushStyleColor(ImGuiCol_Text,
                    ImVec4(c.r / 255.0f, c.g / 255.0f, c.b / 255.0f, 1.0f));
                ImGui::TextUnformatted("##");
                ImGui::PopStyleColor();
                ImGui::SameLine();
                ImGui::TextUnformatted(f4::terrain::tile_type_name(
                    static_cast<f4::terrain::TileType>(t)));
            }
            ImGui::Separator();
            ImGui::TextUnformatted("Teams (color)");
            // Resolve team names from the loaded WorldState.teams[] when
            // available. Falls back to the legacy hardcoded names only
            // when no world is loaded. The .cmp file's 8 team slots each
            // carry a 20-byte name string — using them ensures the legend
            // matches whatever campaign is actually loaded (e.g. slot 1
            // is "U.S." in the Korea fixture, NOT "Enemy" as the legacy
            // array claimed).
            const char* fallback_names[] = {
                "0 Neutral", "1 Enemy", "2 Friendly", "3 ROK",
                "4 Japan", "5 DPRK", "6 PRC", "7 Other"
            };
            for (int i = 0; i < 8; ++i) {
                const auto c = color_for_owner(static_cast<uint8_t>(i));
                ImGui::PushStyleColor(ImGuiCol_Text,
                    ImVec4(c.r / 255.0f, c.g / 255.0f, c.b / 255.0f, 1.0f));
                ImGui::TextUnformatted("##");
                ImGui::PopStyleColor();
                ImGui::SameLine();
                if (impl_->world_loaded && i < static_cast<int>(impl_->teams().size())) {
                    auto h = impl_->handle(impl_->teams()[i]);
                    auto* cid = h.get<f4::entities::CampaignIdentityComponent>();
                    auto* tc = h.get<f4::entities::TeamComponent>();
                    const auto& t_name = cid ? cid->callsign : std::string();
                    char label[64];
                    if (t_name.empty() || t_name == "XX") {
                        std::snprintf(label, sizeof(label), "%d (empty)", i);
                    } else {
                        std::snprintf(label, sizeof(label), "%d %s", i, t_name.c_str());
                    }
                    ImGui::TextUnformatted(label);
                } else {
                    ImGui::TextUnformatted(fallback_names[i]);
                }
            }
        }
        ImGui::End();
    }

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
    ImGui::SetNextWindowPos(ImVec2(0, impl_->window_h - 24));
    ImGui::SetNextWindowSize(ImVec2(impl_->window_w, 24));
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
    // Provide the installation and model database for cross-referencing.
    // The model DB is lazy-loaded by draw_ground_layout_3d(); if the user
    // opens the Class Table Browser WITHOUT first opening the 3D ground
    // layout panel, model_db_3d would be std::nullopt and the browser's
    // 3D preview would silently no-op. Force-load it here when the
    // browser is open so visType previews always work.
    impl_->class_table_browser.set_install(
        impl_->install.has_value() ? &*impl_->install : nullptr);
    if (impl_->class_table_browser.is_open()) {
        impl_->ensure_models_3d_loaded();
    }
    impl_->class_table_browser.set_model_db(
        impl_->model_db_3d.has_value() ? &*impl_->model_db_3d : nullptr);
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

    rlImGuiEnd();
}

} // namespace f4::viewer
