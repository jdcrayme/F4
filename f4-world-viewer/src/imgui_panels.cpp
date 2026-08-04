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
#include <f4/world/world_state.hpp>
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
            if (ImGui::MenuItem("Exit", "Alt+F4")) {
                // Raylib's WindowShouldClose() will pick this up — but we
                // need to actually break the loop. Simplest: close window.
                // ( rlImGui shutdown happens after the loop.)
            }
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("View")) {
            ImGui::TextDisabled("Base layers");
            ImGui::Checkbox("Terrain",     &impl_->show_terrain);
            ImGui::Checkbox("Routes",      &impl_->show_routes);
            ImGui::Checkbox("Objectives",  &impl_->show_objectives);
            ImGui::Checkbox("Units",       &impl_->show_units);
            ImGui::Checkbox("Grid",        &impl_->show_grid);
            ImGui::Checkbox("Legend",      &impl_->show_legend);
            ImGui::Separator();
            ImGui::TextDisabled("Overlays");
            ImGui::Checkbox("Radar arcs",          &impl_->show_radar_arcs);
            ImGui::Checkbox("Ground layout",       &impl_->show_ground_layout_overlay);
            ImGui::Checkbox("Unit destinations",   &impl_->show_unit_destinations);
            ImGui::Checkbox("Waypoints",           &impl_->show_waypoints);
            ImGui::Checkbox("Squadron→Airbase",    &impl_->show_squadron_links);
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
            ImGui::Separator();
            // Snapshot Install Files — TEMPORARY diagnostic tool for the
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
        ImGui::Checkbox("Routes",      &impl_->show_routes);
        ImGui::Checkbox("Objectives",  &impl_->show_objectives);
        ImGui::Checkbox("Units",       &impl_->show_units);
        ImGui::Checkbox("Grid",        &impl_->show_grid);
        ImGui::Checkbox("Legend",      &impl_->show_legend);
        ImGui::Separator();
        ImGui::TextDisabled("Overlays");
        ImGui::Checkbox("Radar arcs",         &impl_->show_radar_arcs);
        ImGui::Checkbox("Ground layout",      &impl_->show_ground_layout_overlay);
        ImGui::Checkbox("Unit destinations",  &impl_->show_unit_destinations);
        ImGui::Checkbox("Waypoints",          &impl_->show_waypoints);
        ImGui::Checkbox("Squadron→Airbase",   &impl_->show_squadron_links);

        ImGui::Separator();
        ImGui::Text("Camera");
        ImGui::SliderFloat("Zoom", &impl_->cam_zoom, 0.1f, 150.0f, "%.1f");
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
                if (impl_->world_loaded && i < static_cast<int>(impl_->world.teams.size())) {
                    const auto& t = impl_->world.teams[i];
                    char label[64];
                    if (t.name.empty() || t.name == "XX") {
                        std::snprintf(label, sizeof(label), "%d (empty)", i);
                    } else {
                        std::snprintf(label, sizeof(label), "%d %s", i, t.name.c_str());
                    }
                    ImGui::TextUnformatted(label);
                } else {
                    ImGui::TextUnformatted(fallback_names[i]);
                }
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
            // Resolve the team name from WorldState.teams[] when available.
            // Falls back to "(unknown)" if no world loaded or owner out of range.
            const char* team_name = "(no world)";
            if (impl_->world_loaded && o.owner < impl_->world.teams.size()) {
                const auto& t = impl_->world.teams[o.owner];
                team_name = t.name.empty() ? "(empty)" : t.name.c_str();
            }
            // Resolve the objective type name (e.g. "Airbase"). Falls back to
            // "Unknown" when objective_type == 0 (no class table loaded).
            const std::string obj_type_name_str =
                (o.objective_type > 0)
                    ? f4::world_convert::objective_type_name(
                          static_cast<int16_t>(o.objective_type))
                    : std::string("Unknown");
            ImGui::Text("Objective #%d", impl_->sel_index);
            ImGui::Separator();
            // Show the objective's class name (e.g. "02_20 Airbase 2") when
            // available — much more useful than just "Airbase". Falls back
            // to the objective_type name when no class_name was loaded.
            if (!o.class_name.empty()) {
                ImGui::Text("Name:      %s", o.class_name.c_str());
            }
            ImGui::Text("Type:      %s (%d)", obj_type_name_str.c_str(), o.objective_type);
            ImGui::Text("Entity:    %d", o.type);
            ImGui::Text("Position:  (%d, %d, %.0f ft)", o.x, o.y, o.z);
            ImGui::Text("Owner:     %d (%s)", o.owner, team_name);
            ImGui::Text("Priority:  %d", o.priority);
            ImGui::Text("Camp ID:   %d", o.camp_id);
            ImGui::Text("Name ID:   %d", o.nameid);
            // first_owner uses the same Control enum as owner — decode it.
            ImGui::Text("First own: %d (%s)", o.first_owner,
                        f4::viewer::control_name(o.first_owner));
            // parent_id resolves to the parent objective's name when we can
            // find it in the VU_ID → objective index map.
            {
                const char* parent_label = "(none)";
                std::string parent_buf;
                auto it = impl_->obj_id_to_index.find(o.parent_id);
                if (o.parent_id != 0 &&
                    it != impl_->obj_id_to_index.end() &&
                    it->second < static_cast<int>(impl_->world.objectives.size())) {
                    const auto& par = impl_->world.objectives[it->second];
                    if (!par.class_name.empty()) {
                        parent_buf = par.class_name + " (#" +
                                     std::to_string(it->second) + ")";
                        parent_label = parent_buf.c_str();
                    } else {
                        parent_buf = "objective #" + std::to_string(it->second);
                        parent_label = parent_buf.c_str();
                    }
                }
                ImGui::Text("Parent ID: 0x%08x  %s", o.parent_id, parent_label);
            }
            ImGui::Text("Links:     %d (road/rail)", static_cast<int>(o.links.size()));
            ImGui::Text("VU_ID:     0x%08x/0x%08x", o.id_creator, o.id_num);
            ImGui::Separator();
            ImGui::Text("Supply:    %d", o.supply);
            ImGui::Text("Fuel:      %d", o.fuel);
            ImGui::Text("Losses:    %d", o.losses);
            ImGui::Text("Last rep:  %d", o.last_repair);
            // obj_flags is a bitmap — decode the well-known bits.
            {
                char flag_buf[128];
                f4::viewer::obj_flags_text(o.obj_flags, flag_buf, sizeof(flag_buf));
                ImGui::Text("Obj flags: 0x%08x (%s)", o.obj_flags, flag_buf);
            }
            // Theater static-data enrichment (from Falcon4.OCD):
            if (o.features_count > 0 || o.deag_distance > 0 || o.pt_data_index > 0) {
                ImGui::Separator();
                ImGui::TextUnformatted("Objective class data (OCD):");
                ImGui::Text("Features:  %d", o.features_count);
                ImGui::Text("Deag dist: %d", o.deag_distance);
                ImGui::Text("Radar feat:%d", o.radar_feature);
                ImGui::Text("PT index:  %d", o.pt_data_index);
            }
            if (o.has_radar) {
                ImGui::Separator();
                ImGui::TextUnformatted("Radar detection arcs:");
                for (int i = 0; i < 8; ++i) {
                    ImGui::Text("  arc %d: %.3f", i, o.detect_ratio[i]);
                }
            }
            // Airbase ground layout (from Falcon4.PHD/PD): show runway/
            // taxiway/parking lists with their points.
            if (!o.ground_layout.empty()) {
                ImGui::Separator();
                if (ImGui::TreeNode("Ground Layout", "Ground Layout (%d lists)", static_cast<int>(o.ground_layout.size()))) {
                    for (std::size_t li = 0; li < o.ground_layout.size(); ++li) {
                        const auto& gl = o.ground_layout[li];
                        // Use the proper PointListType decoder from
                        // f4-world-convert instead of the inline switch.
                        const char* type_str =
                            f4::world_convert::point_list_type_name(gl.type);
                        char label[96];
                        std::snprintf(label, sizeof(label), "[%zu] %s (runway %d, %d pts, %.0f deg)",
                                      li, type_str, gl.runway_num,
                                      static_cast<int>(gl.points.size()), gl.heading_deg);
                        if (ImGui::TreeNode(label)) {
                            // Decode ltrt: -1=Left, +1=Right, 0=Center.
                            ImGui::Text("  type: %d (%s)  count: %d  ltrt: %d (%s)",
                                        gl.type, type_str, gl.count, gl.ltrt,
                                        f4::viewer::ltrt_name(gl.ltrt));
                            int pi = 0;
                            for (const auto& pt : gl.points) {
                                // Decode point type (1=Runway, 3=Taxi,
                                // 11=SmallPark, ...) and point flags bitmap
                                // (PT_FIRST / PT_LAST / PT_OCCUPIED).
                                char flag_buf[32];
                                f4::viewer::point_flags_text(
                                    pt.flags, flag_buf, sizeof(flag_buf));
                                ImGui::Text("  pt %d: (%.0f, %.0f) type=%d (%s) flags=0x%02x (%s)",
                                            pi++, pt.x, pt.y, pt.type,
                                            f4::world_convert::point_type_name(pt.type),
                                            pt.flags, flag_buf);
                            }
                            ImGui::TreePop();
                        }
                    }
                    ImGui::TreePop();
                }
            }
        } else if (impl_->sel_kind == Impl::SelectionKind::Unit) {
            const auto& u = impl_->world.units[impl_->sel_index];
            const char* team_name = "(no world)";
            if (impl_->world_loaded && u.owner < impl_->world.teams.size()) {
                const auto& t = impl_->world.teams[u.owner];
                team_name = t.name.empty() ? "(empty)" : t.name.c_str();
            }
            // Subtype name (e.g. "Armor", "Fighter-Bomber"). Uses the
            // domain+subtype pair emitted by the converter. Falls back to
            // "Unknown" when subtype == 0 (no class table loaded).
            const char* subtype_str = f4::world_convert::unit_subtype_name(
                u.domain, u.unit_subtype);
            ImGui::Text("Unit #%d", impl_->sel_index);
            ImGui::Separator();
            // Show the unit's class name (e.g. "Patrol", "Armor Battalion")
            // when available — much more useful than just "battalion".
            if (!u.class_name.empty()) {
                ImGui::Text("Name:      %s", u.class_name.c_str());
            }
            ImGui::Text("Class:     %s (%s)",
                        f4::world::unit_class_name(u.unit_class),
                        subtype_str);
            ImGui::Text("Type:      %d", u.type);
            ImGui::Text("Subtype:   %d (%s)", u.unit_subtype, subtype_str);
            // Domain: 2=Air, 3=Land, 4=Sea — decode so the user doesn't have
            // to keep the enum table in their head.
            ImGui::Text("Domain:    %d (%s)", u.domain,
                        f4::viewer::domain_name(u.domain));
            ImGui::Text("Position:  (%d, %d, %.0f ft)", u.x, u.y, u.z);
            ImGui::Text("Owner:     %d (%s)", u.owner, team_name);
            ImGui::Text("Destination:(%d, %d)", u.dest_x, u.dest_y);
            ImGui::Text("Name ID:   %d", u.name_id);
            ImGui::Text("Camp ID:   %d", u.camp_id);
            ImGui::Text("Reinforc.: %d", u.reinforcement);
            ImGui::Text("Waypoints: %d", u.wp_count);
            ImGui::Text("Losses:    %d", u.losses);
            // Movement specs (from Falcon4.UCD):
            if (u.movement_type > 0 || u.movement_speed > 0) {
                ImGui::Separator();
                ImGui::TextUnformatted("Movement (UCD):");
                if (!u.movement_type_name.empty()) {
                    ImGui::Text("  Type:     %s (%d)", u.movement_type_name.c_str(), u.movement_type);
                } else {
                    ImGui::Text("  Type:     %d", u.movement_type);
                }
                ImGui::Text("  Speed:    %d", u.movement_speed);
                ImGui::Text("  Range:    %d km", u.max_range);
            }
            // Roster: 16 groups × 2 bits. Show live vehicle count per group.
            // GetNumVehicles(vg) = (roster >> (vg*2)) & 0x03 — max 3/group.
            {
                int total_vehicles = 0;
                for (int vg = 0; vg < 16; ++vg) {
                    total_vehicles += (u.roster >> (vg * 2)) & 0x03;
                }
                ImGui::Text("Roster:    0x%08x (%d vehicles)", u.roster, total_vehicles);
            }
            // Vehicle composition (from Falcon4.UCD + VCD): per-group vehicle
            // types and names, with live counts from the roster.
            if (!u.vehicle_groups.empty()) {
                if (ImGui::TreeNode("Vehicle Groups", "Vehicle Groups (%d)", static_cast<int>(u.vehicle_groups.size()))) {
                    ImGui::Text("grp  type  count  live  name         HP   speed");
                    int nominal_total = 0;
                    int live_total = 0;
                    for (const auto& vg : u.vehicle_groups) {
                        ImGui::Text("%-4d %-5d %-6d %-5d %-12s %-4d %d",
                                    vg.group, vg.vehicle_type, vg.count,
                                    vg.live_count,
                                    vg.vehicle_name.empty() ? "?" : vg.vehicle_name.c_str(),
                                    vg.hit_points, vg.max_speed);
                        nominal_total += vg.count;
                        live_total += vg.live_count;
                    }
                    ImGui::Separator();
                    ImGui::Text("Total:     %d nominal, %d live", nominal_total, live_total);
                    ImGui::TreePop();
                }
            }
            ImGui::Text("Entity:    %d", u.entity_type);
            ImGui::Text("VU_ID:     0x%08x/0x%08x", u.id_creator, u.id_num);
            // Subclass-specific fields:
            ImGui::Separator();
            switch (u.unit_class) {
                case f4::world::UnitClass::Battalion:
                    ImGui::Text("Supply:    %d%%", u.supply);
                    ImGui::Text("Morale:    %d%%", u.morale);
                    ImGui::Text("Fatigue:   %d%%", u.fatigue);
                    ImGui::Text("Heading:   %d deg", static_cast<int>(u.heading * 360 / 256));
                    ImGui::Text("Final hdg: %d deg", static_cast<int>(u.final_heading * 360 / 256));
                    ImGui::Text("Last move: %d", u.last_move);
                    ImGui::Text("Last cmbt: %d", u.last_combat);
                    // Resolve parent_id (VU_ID.num of the brigade) to a
                    // unit name via the unit_id_to_index lookup table.
                    {
                        const char* parent_label = "(none)";
                        std::string parent_buf;
                        auto it = impl_->unit_id_to_index.find(u.parent_id);
                        if (u.parent_id != 0 &&
                            it != impl_->unit_id_to_index.end() &&
                            it->second < static_cast<int>(impl_->world.units.size())) {
                            const auto& par = impl_->world.units[it->second];
                            if (!par.class_name.empty()) {
                                parent_buf = par.class_name + " (unit #" +
                                             std::to_string(it->second) + ")";
                            } else {
                                parent_buf = "unit #" + std::to_string(it->second);
                            }
                            parent_label = parent_buf.c_str();
                        }
                        ImGui::Text("Parent:    0x%08x  %s", u.parent_id, parent_label);
                    }
                    break;
                case f4::world::UnitClass::Brigade:
                    ImGui::Text("Supply:    %d%%", u.supply);
                    ImGui::Text("Morale:    %d%%", u.morale);
                    ImGui::Text("Fatigue:   %d%%", u.fatigue);
                    ImGui::Text("Elements:  %d", u.elements);
                    if (ImGui::TreeNode("Child battalions")) {
                        for (uint32_t eid : u.element_ids) {
                            // Resolve each child battalion VU_ID.num to
                            // its name via the unit_id_to_index map.
                            const char* child_label = "(missing)";
                            std::string child_buf;
                            auto it = impl_->unit_id_to_index.find(eid);
                            if (it != impl_->unit_id_to_index.end() &&
                                it->second < static_cast<int>(impl_->world.units.size())) {
                                const auto& child = impl_->world.units[it->second];
                                if (!child.class_name.empty()) {
                                    child_buf = child.class_name + " (unit #" +
                                                std::to_string(it->second) + ")";
                                } else {
                                    child_buf = "unit #" + std::to_string(it->second);
                                }
                                child_label = child_buf.c_str();
                            }
                            ImGui::Text("  ID 0x%08x  %s", eid, child_label);
                        }
                        ImGui::TreePop();
                    }
                    break;
                case f4::world::UnitClass::Squadron:
                    ImGui::Text("Fuel:      %d lbs", u.fuel);
                    // Resolve airbase_id (VU_ID.num) to the airbase
                    // objective's name via the obj_id_to_index map.
                    {
                        const char* ab_label = "(none)";
                        std::string ab_buf;
                        auto it = impl_->obj_id_to_index.find(u.airbase_id);
                        if (u.airbase_id != 0 &&
                            it != impl_->obj_id_to_index.end() &&
                            it->second < static_cast<int>(impl_->world.objectives.size())) {
                            const auto& ab = impl_->world.objectives[it->second];
                            if (!ab.class_name.empty()) {
                                ab_buf = ab.class_name + " (obj #" +
                                         std::to_string(it->second) + ")";
                            } else {
                                ab_buf = "objective #" + std::to_string(it->second);
                            }
                            ab_label = ab_buf.c_str();
                        }
                        ImGui::Text("Airbase:   0x%08x  %s", u.airbase_id, ab_label);
                    }
                    ImGui::Text("Specialty: %d (%s)", u.specialty,
                                f4::viewer::squadron_specialty_name(u.specialty));
                    ImGui::Separator();
                    ImGui::Text("AA kills:  %d", u.aa_kills);
                    ImGui::Text("AG kills:  %d", u.ag_kills);
                    ImGui::Text("AS kills:  %d", u.as_kills);
                    ImGui::Text("AN kills:  %d", u.an_kills);
                    ImGui::Text("Missions:  %d", u.missions_flown);
                    ImGui::Text("Score:     %d", u.mission_score);
                    ImGui::Text("Total loss:%d", u.total_losses);
                    ImGui::Text("Pilot loss:%d", u.pilot_losses);
                    ImGui::Text("Patch:     %d", u.squadron_patch);
                    if (ImGui::TreeNode("Pilots", "Pilots (%d)", static_cast<int>(u.pilots.size()))) {
                        ImGui::Text("ID    Skill        Rating     Status  AA  AG  Missions");
                        for (const auto& p : u.pilots) {
                            // Decode skill (0=Recruit..3=Ace) and rating
                            // (same enum) instead of bare ints.
                            char skill_buf[32];
                            std::snprintf(skill_buf, sizeof(skill_buf),
                                          "%d (%s)", p.skill,
                                          f4::viewer::pilot_skill_name(p.skill));
                            char rating_buf[32];
                            std::snprintf(rating_buf, sizeof(rating_buf),
                                          "%d (%s)", p.rating,
                                          f4::viewer::pilot_skill_name(p.rating));
                            ImGui::Text("%-5ld %-12s %-10s %-7s %-3d %-3d %d",
                                        static_cast<long>(p.pilot_id),
                                        skill_buf, rating_buf,
                                        f4::viewer::pilot_status_name(p.status),
                                        p.aa_kills, p.ag_kills, p.missions_flown);
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
            // Waypoint list (only if non-empty)
            if (!u.waypoints.empty()) {
                ImGui::Separator();
                if (ImGui::TreeNode("Waypoints", "Waypoints (%d)", static_cast<int>(u.waypoints.size()))) {
                    ImGui::Text("idx  x    y    z    action              depart");
                    int wi = 0;
                    for (const auto& w : u.waypoints) {
                        // Decode WP_ACTION (Takeoff/Land/Strike/CAP/...)
                        char action_buf[40];
                        std::snprintf(action_buf, sizeof(action_buf),
                                      "%d (%s)", static_cast<int>(w.action),
                                      f4::viewer::wp_action_name(w.action));
                        ImGui::Text("%-4d %-4d %-4d %-4d %-19s %d",
                                    wi++, w.x, w.y, w.z,
                                    action_buf, w.depart);
                    }
                    ImGui::TreePop();
                }
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

    // --- Ground Layout view (auto-opens when an objective with
    // ground_layout is selected — shows runway/taxiway/parking/SAM-site
    // geometry in a dedicated 2D window).
    draw_ground_layout_view();

    // --- Campaign + Teams panels (auto-open when a world is loaded —
    // show CampaignState fields and the .tea-enriched team roster
    // with stance matrix, country memberships, experience, and command
    // chain).
    draw_campaign_and_teams_view();

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
