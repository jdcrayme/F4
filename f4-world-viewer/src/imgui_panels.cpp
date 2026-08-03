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
#include <f4/viewer/file_dialog.hpp>
#include <f4/world/world_state.hpp>

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
            if (ImGui::MenuItem("Exit", "Alt+F4")) {
                // Raylib's WindowShouldClose() will pick this up — but we
                // need to actually break the loop. Simplest: close window.
                // ( rlImGui shutdown happens after the loop.)
            }
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("View")) {
            ImGui::Checkbox("Terrain",     &impl_->show_terrain);
            ImGui::Checkbox("Routes",      &impl_->show_routes);
            ImGui::Checkbox("Objectives",  &impl_->show_objectives);
            ImGui::Checkbox("Units",       &impl_->show_units);
            ImGui::Checkbox("Grid",        &impl_->show_grid);
            ImGui::Checkbox("Legend",      &impl_->show_legend);
            ImGui::Separator();
            if (ImGui::MenuItem("Fit to World")) impl_->fit_to_world();
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
            ImGui::Separator();
            // Install Diagnostics — shows the full diagnostic report
            // (where we looked for FALCON4.ct, every theater dir probed,
            // every campaign path + exists check). The "what's actually
            // wrong with my install" tool.
            if (ImGui::MenuItem("Install Diagnostics...")) {
                open_install_diagnostics();
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
        ImGui::Checkbox("Terrain",     &impl_->show_terrain);
        ImGui::Checkbox("Routes",      &impl_->show_routes);
        ImGui::Checkbox("Objectives",  &impl_->show_objectives);
        ImGui::Checkbox("Units",       &impl_->show_units);
        ImGui::Checkbox("Grid",        &impl_->show_grid);
        ImGui::Checkbox("Legend",      &impl_->show_legend);

        ImGui::Separator();
        ImGui::Text("Camera");
        ImGui::SliderFloat("Zoom", &impl_->cam_zoom, 0.1f, 30.0f, "%.2f");
        if (ImGui::Button("Fit to World")) impl_->fit_to_world();

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
            const char* team_names[] = {
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
                ImGui::TextUnformatted(team_names[i]);
            }
            ImGui::Separator();
            ImGui::TextUnformatted("Unit subtypes");
            ImGui::TextDisabled("(ground units use subtype icons)");
            ImGui::TextUnformatted("Armor  Artillery  Infantry");
            ImGui::TextUnformatted("Supply Engineer  HARTS");
            ImGui::TextDisabled("(air units use subtype icons)");
            ImGui::TextUnformatted("Fighter Bomber  Transport");
            ImGui::TextUnformatted("Helicopter  Carrier");
            ImGui::Separator();
            ImGui::TextUnformatted("Generic shapes (fallback)");
            ImGui::TextUnformatted("[] Battalion  <> Brigade");
            ImGui::TextUnformatted("o  Squadron  ^  TaskForce");
        }
        ImGui::End();
    }

    // --- Inspector (right side, below legend) ---
    ImGui::SetNextWindowPos(ImVec2(impl_->window_w - 320, 250), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(310, 380), ImGuiCond_FirstUseEver);
    if (ImGui::Begin("Inspector", nullptr, ImGuiWindowFlags_NoCollapse)) {
        if (impl_->sel_kind == Impl::SelectionKind::None || impl_->sel_index < 0) {
            ImGui::TextDisabled("Nothing selected");
            ImGui::TextDisabled("Click an objective or unit to inspect.");
        } else if (impl_->sel_kind == Impl::SelectionKind::Objective) {
            const auto& o = impl_->world.objectives[impl_->sel_index];
            ImGui::Text("Objective #%d", impl_->sel_index);
            ImGui::Separator();
            ImGui::Text("Obj Type:  %d", o.objective_type);
            ImGui::Text("Entity:    %d", o.type);
            ImGui::Text("Position:   (%d, %d, %.0f ft)", o.x, o.y, o.z);
            ImGui::Text("Owner:      %d", o.owner);
            ImGui::Text("Priority:   %d", o.priority);
            ImGui::Text("Links:     %d (road/rail connections)", static_cast<int>(o.links.size()));
            ImGui::Text("Camp ID:    %d", o.camp_id);
            ImGui::Text("Entity:     %d", o.entity_type);
            ImGui::Text("VU_ID:      0x%08x/0x%08x", o.id_creator, o.id_num);
        } else if (impl_->sel_kind == Impl::SelectionKind::Unit) {
            const auto& u = impl_->world.units[impl_->sel_index];
            ImGui::Text("Unit #%d", impl_->sel_index);
            ImGui::Separator();
            ImGui::Text("Class:     %s (type %d)",
                        f4::world::unit_class_name(u.unit_class), u.type);
            ImGui::Text("Position:  (%d, %d, %.0f ft)", u.x, u.y, u.z);
            ImGui::Text("Owner:     %d", u.owner);
            ImGui::Text("Destination:(%d, %d)", u.dest_x, u.dest_y);
            ImGui::Text("Name ID:   %d", u.name_id);
            ImGui::Text("Camp ID:   %d", u.camp_id);
            ImGui::Text("Reinforc.: %d", u.reinforcement);
            ImGui::Text("Waypoints: %d", u.wp_count);
            ImGui::Text("Losses:    %d", u.losses);
            ImGui::Text("Entity:    %d", u.entity_type);
            ImGui::Text("VU_ID:     0x%08x/0x%08x", u.id_creator, u.id_num);
            // Subclass-specific fields:
            ImGui::Separator();
            switch (u.unit_class) {
                case f4::world::UnitClass::Battalion:
                    ImGui::Text("Supply:    %d%%", u.supply);
                    ImGui::Text("Morale:    %d%%", u.morale);
                    ImGui::Text("Fatigue:   %d%%", u.fatigue);
                    ImGui::Text("Parent:    %d", u.parent_id);
                    break;
                case f4::world::UnitClass::Brigade:
                    ImGui::Text("Supply:    %d%%", u.supply);
                    ImGui::Text("Morale:    %d%%", u.morale);
                    ImGui::Text("Fatigue:   %d%%", u.fatigue);
                    ImGui::Text("Elements:  %d", u.elements);
                    if (ImGui::TreeNode("Child battalions")) {
                        for (uint32_t eid : u.element_ids) {
                            ImGui::Text("  ID: %d", eid);
                        }
                        ImGui::TreePop();
                    }
                    break;
                case f4::world::UnitClass::Squadron:
                    ImGui::Text("Fuel:      %d lbs", u.fuel);
                    if (ImGui::TreeNode("Pilots", "Pilots (%d)", static_cast<int>(u.pilots.size()))) {
                        ImGui::Text("ID    Skill Status AA  Missions");
                        for (const auto& p : u.pilots) {
                            const char* status_str = "?";
                            switch (p.status) {
                                case 0: status_str = "OK"; break;
                                case 1: status_str = "Dead"; break;
                                case 2: status_str = "Leave"; break;
                                case 3: status_str = "Hosp"; break;
                            }
                            ImGui::Text("%-5d %-5d %-6s %-3d %d",
                                        p.pilot_id, p.skill, status_str,
                                        p.aa_kills, p.missions_flown);
                        }
                        ImGui::TreePop();
                    }
                    break;
                case f4::world::UnitClass::TaskForce:
                    ImGui::Text("Supply:    %d%%", u.supply);
                    break;
                case f4::world::UnitClass::Flight:
                case f4::world::UnitClass::Package:
                case f4::world::UnitClass::Unknown:
                    break;
            }
        }
    }
    ImGui::End();

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
                                impl_->world.objectives.size(),
                                impl_->world.units.size());
        }
    }
    ImGui::End();

    // --- Pending file dialog modal ---
    // NOTE: must be inside the rlImGuiBegin/End block — calling ImGui
    // functions after rlImGuiEnd() crashes because the ImGui frame is
    // already finalized (the ID stack is empty, GetID() dereferences
    // an empty ImVector).
    //
    // BUG FIX: The popup ID in OpenPopup() MUST match the name passed to
    // BeginPopupModal(). Previously we used "FilePicker" in OpenPopup but
    // the title string in BeginPopupModal — the IDs didn't match so the
    // popup never opened. Now both use the title string as the ID.
    if (impl_->pending_dialog_open && !impl_->pending_dialog_title.empty()) {
        const char* popup_id = impl_->pending_dialog_title.c_str();
        if (!ImGui::IsPopupOpen(popup_id)) {
            ImGui::OpenPopup(popup_id);
        }
        ImGui::SetNextWindowSize(ImVec2(500, 160), ImGuiCond_FirstUseEver);
        if (ImGui::BeginPopupModal(popup_id,
                                    &impl_->pending_dialog_open,
                                    ImGuiWindowFlags_NoResize)) {
            ImGui::TextUnformatted("Path:");
            ImGui::SameLine();
            ImGui::PushItemWidth(-120);
            ImGui::InputText("##path", impl_->pending_dialog_path,
                             sizeof(impl_->pending_dialog_path));
            ImGui::PopItemWidth();
            ImGui::SameLine();
            if (ImGui::Button("OK")) {
                std::string p = impl_->pending_dialog_path;
                if (!p.empty()) {
                    auto cb = std::move(impl_->pending_dialog_callback);
                    impl_->pending_dialog_callback = nullptr;
                    impl_->pending_dialog_open = false;
                    impl_->pending_dialog_title.clear();
                    try {
                        cb(p);
                    } catch (const std::exception& e) {
                        impl_->last_error = e.what();
                    }
                }
                ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine();
            if (ImGui::Button("Cancel")) {
                impl_->pending_dialog_open = false;
                impl_->pending_dialog_title.clear();
                impl_->pending_dialog_callback = nullptr;
                ImGui::CloseCurrentPopup();
            }
            if (!impl_->last_world_json_path.empty()) {
                ImGui::Separator();
                ImGui::TextDisabled("Last world JSON: %s",
                                    impl_->last_world_json_path.string().c_str());
            }
            ImGui::EndPopup();
        }
    }

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
            static std::string diag_buf;  // static so it persists across frames
            diag_buf = impl_->install_diagnostics_text;
            diag_buf.resize(diag_buf.size() + 1, '\0');  // room for null terminator
            ImGui::InputTextMultiline("##diag_input",
                                       diag_buf.data(), diag_buf.size(),
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
            static std::string err_buf;
            err_buf = impl_->campaign_load_error_text;
            err_buf.resize(err_buf.size() + 1, '\0');
            ImGui::InputTextMultiline("##err_input",
                                       err_buf.data(), err_buf.size(),
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

    rlImGuiEnd();
}

// ---------------------------------------------------------------------------
// File dialog helper — Raylib doesn't ship a native picker, so we use a
// simple ImGui text-input modal. The user can paste a path or type one in.
// A real file browser will replace this in a future pass — likely via
// tinyfiledialogs (which uses the OS native dialog on Windows/macOS/Linux).
// ---------------------------------------------------------------------------
void ViewerApp::open_file_dialog(const char* title, const char* filters,
                                  std::function<void(const std::string&)> on_ok) {
    impl_->pending_dialog_title = title;
    impl_->pending_dialog_filters = filters ? filters : "";
    impl_->pending_dialog_callback = std::move(on_ok);
    // Pre-fill with the last world JSON path if available — saves typing.
    if (!impl_->last_world_json_path.empty()) {
        std::string s = impl_->last_world_json_path.string();
        std::snprintf(impl_->pending_dialog_path, sizeof(impl_->pending_dialog_path),
                      "%s", s.c_str());
    } else {
        impl_->pending_dialog_path[0] = '\0';
    }
    impl_->pending_dialog_open = true;
}

} // namespace f4::viewer
