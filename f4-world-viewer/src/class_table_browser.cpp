// f4-world-viewer/src/class_table_browser.cpp
//
// ClassTableBrowser -- a browsable, filterable, exportable view over the
// Falcon4.ct class table + OCD/UCD/VCD/FCD theater object data.
//
// Follows the HexInspector pattern: self-contained panel with open/close/
// draw, owns its own state, accessed via Tools menu.

#include <f4/viewer/class_table_browser.hpp>
#include <f4/viewer/enum_text.hpp>
#include <f4/viewer/file_dialog.hpp>

#include <f4/world_convert/class_table.hpp>      // unit_subtype_name, DOMAIN_*, CLASS_*
#include <f4/world_convert/objective_decoder.hpp> // objective_type_name
#include <f4/world_convert/theater_data.hpp>     // movement_type_name

#include <imgui.h>

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <string>

namespace f4::viewer {

using f4::world_convert::unit_subtype_name;
using f4::world_convert::movement_type_name;

// ---------------------------------------------------------------------------
// Corrected data_type_name -- uses the verified DataType enum from
// class_table.hpp, NOT the stale mapping in enum_text.hpp.
// ---------------------------------------------------------------------------
const char* ClassTableBrowser::ct_data_type_name(uint8_t dt) noexcept {
    using namespace f4::world_convert;
    switch (static_cast<DataType>(dt)) {
        case DTYPE_NOTHING:       return "Nothing";
        case DTYPE_FEATURE:       return "Feature(FCD)";
        case DTYPE_OBJECTIVE:     return "Objective(OCD)";
        case DTYPE_UNIT:          return "Unit(UCD)";
        case DTYPE_VEHICLE:       return "Vehicle(VCD)";
        case DTYPE_WEAPON:        return "Weapon";
        case DTYPE_SQUAD_STORES:  return "SquadStores";
        default:                  return "Unknown";
    }
}

// ---------------------------------------------------------------------------
// Data loading
// ---------------------------------------------------------------------------
void ClassTableBrowser::ensure_data_loaded() {
    if (data_load_attempted_) return;
    data_load_attempted_ = true;

    if (!install_) {
        load_error_ = "No installation configured. Use File > Set Install Path.";
        return;
    }

    // Load class table.
    auto ct_path = install_->class_table();
    if (ct_path.empty()) {
        ct_path = install_->find_class_table();
    }
    if (ct_path.empty()) {
        load_error_ = "FALCON4.ct not found in install.";
        return;
    }
    try {
        class_table_.load(ct_path);
    } catch (const std::exception& e) {
        load_error_ = std::string("Error loading class table: ") + e.what();
        return;
    }

    // Load theater object database (OCD/UCD/VCD/FCD/etc.)
    // Same two search locations as install_flow.cpp:
    //   1. install-level terrdata/objects  (vanilla FF, FreeFalcon, Allied Force)
    //   2. per-theater <theater.dir>/objects  (some community theaters)
    // theater.dir is an ABSOLUTE path (e.g. /install/terrdata/korea).
    const std::array<std::filesystem::path, 2> base_dirs = {
        install_->terrdata_dir() / "objects",
        (!install_->theaters().empty())
            ? install_->theaters()[0].dir / "objects"
            : std::filesystem::path{},
    };
    for (const auto& d : base_dirs) {
        if (d.empty() || !std::filesystem::exists(d)) continue;
        try {
            theater_db_.load_all(d);
            if (theater_db_.loaded()) break;
        } catch (...) {
            // Continue trying other dirs.
        }
    }

    data_loaded_ = class_table_.loaded();
    if (!data_loaded_) {
        load_error_ = "Class table loaded 0 entries.";
    }
}

// ---------------------------------------------------------------------------
// Filter logic
// ---------------------------------------------------------------------------
bool ClassTableBrowser::passes_filter(
    uint16_t entity_type,
    const f4::world_convert::ClassTableEntry& entry) const
{
    if (filter_domain_ != 0 && entry.domain != filter_domain_) return false;
    if (filter_class_ != 0 && entry.cls != filter_class_) return false;

    if (search_buf_[0] != '\0') {
        std::string search_lower(search_buf_);
        std::transform(search_lower.begin(), search_lower.end(),
                       search_lower.begin(), ::tolower);

        std::string id_str = std::to_string(entity_type);
        std::transform(id_str.begin(), id_str.end(), id_str.begin(), ::tolower);
        if (id_str.find(search_lower) != std::string::npos) return true;

        const char* sub = unit_subtype_name(entry.domain, entry.stype);
        std::string sub_lower(sub);
        std::transform(sub_lower.begin(), sub_lower.end(),
                       sub_lower.begin(), ::tolower);
        if (sub_lower.find(search_lower) != std::string::npos) return true;

        const char* cls = vu_class_name(entry.cls);
        std::string cls_lower(cls);
        std::transform(cls_lower.begin(), cls_lower.end(),
                       cls_lower.begin(), ::tolower);
        if (cls_lower.find(search_lower) != std::string::npos) return true;

        if (entry.data_type == f4::world_convert::DTYPE_VEHICLE) {
            const auto* vcd = theater_db_.vehicles.at(entry.data_ptr_index);
            if (vcd && !vcd->name.empty()) {
                std::string name_lower(vcd->name);
                std::transform(name_lower.begin(), name_lower.end(),
                               name_lower.begin(), ::tolower);
                if (name_lower.find(search_lower) != std::string::npos) return true;
            }
        }

        if (entry.data_type == f4::world_convert::DTYPE_OBJECTIVE) {
            const auto* ocd = theater_db_.objectives.at(entry.data_ptr_index);
            if (ocd && !ocd->name.empty()) {
                std::string name_lower(ocd->name);
                std::transform(name_lower.begin(), name_lower.end(),
                               name_lower.begin(), ::tolower);
                if (name_lower.find(search_lower) != std::string::npos) return true;
            }
        }

        return false;
    }

    return true;
}

// ---------------------------------------------------------------------------
// Pre-compute filtered entries whenever filters change.
// This avoids the IM_ASSERT from ImGuiListClipper when skipping
// filtered-out rows inside the clip loop (clipper requires a 1:1
// mapping between row index and displayed row).
// ---------------------------------------------------------------------------
void ClassTableBrowser::rebuild_filtered_entries() {
    filtered_entries_.clear();
    if (!class_table_.loaded()) return;

    const int n = static_cast<int>(class_table_.size());
    const int base = f4::world_convert::VU_LAST_ENTITY_TYPE;

    for (int i = 0; i < n; ++i) {
        const uint16_t et = static_cast<uint16_t>(base + i);
        const auto* entry = class_table_.lookup(et);
        if (entry && passes_filter(et, *entry)) {
            filtered_entries_.push_back({et, entry});
        }
    }

    filter_dirty_ = false;
}

// ---------------------------------------------------------------------------
// Drawing
// ---------------------------------------------------------------------------
void ClassTableBrowser::draw() {
    if (!open_) return;

    ensure_data_loaded();

    ImGui::SetNextWindowSize(ImVec2(1000, 700), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Class Table Browser", &open_,
                       ImGuiWindowFlags_NoCollapse)) {
        ImGui::End();
        return;
    }

    draw_toolbar();

    if (data_loaded_) {
        draw_filter_bar();
        draw_table();
        draw_detail_panel();
        draw_export_bar();
    } else if (!load_error_.empty()) {
        ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "Error: %s",
                            load_error_.c_str());
    } else {
        ImGui::TextDisabled("No data loaded. Set an install path and re-open.");
    }

    ImGui::End();
}

void ClassTableBrowser::draw_toolbar() {
    if (data_loaded_) {
        ImGui::Text("FALCON4.ct: %zu entries", class_table_.size());
        if (theater_db_.loaded()) {
            ImGui::SameLine();
            ImGui::TextDisabled("  Theater DB: OCD=%zu UCD=%zu VCD=%zu FCD=%zu",
                                theater_db_.objectives.size(),
                                theater_db_.units.size(),
                                theater_db_.vehicles.size(),
                                theater_db_.features.size());
        }
    } else if (!load_error_.empty()) {
        ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "%s",
                            load_error_.c_str());
    }

    ImGui::SameLine(ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x - 70);
    if (ImGui::Button("Reload")) {
        data_load_attempted_ = false;
        data_loaded_ = false;
        load_error_.clear();
        class_table_ = f4::world_convert::ClassTable{};
        theater_db_ = f4::world_convert::TheaterObjectDatabase{};
        selected_entity_type_ = -1;
        filtered_entries_.clear();
        filter_dirty_ = true;
    }
}

void ClassTableBrowser::draw_filter_bar() {
    ImGui::Text("Domain:");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(80);
    const char* domain_items[] = {"All", "Air", "Land", "Sea"};
    int domain_vals[] = {0, 2, 3, 4};
    const char* domain_preview = "All";
    for (int i = 0; i < 4; ++i) {
        if (filter_domain_ == domain_vals[i]) { domain_preview = domain_items[i]; break; }
    }
    if (ImGui::BeginCombo("##domain", domain_preview)) {
        for (int i = 0; i < 4; ++i) {
            const bool sel = (filter_domain_ == domain_vals[i]);
            if (ImGui::Selectable(domain_items[i], sel)) {
                filter_domain_ = domain_vals[i];
                filter_dirty_ = true;
            }
            if (sel) ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
    }

    ImGui::SameLine();
    ImGui::Text("Class:");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(100);
    const char* class_items[] = {"All", "Objective", "Unit", "Vehicle", "Weapon", "Feature"};
    int class_vals[] = {0, 4, 6, 7, 8, 2};
    const char* class_preview = "All";
    for (int i = 0; i < 6; ++i) {
        if (filter_class_ == class_vals[i]) { class_preview = class_items[i]; break; }
    }
    if (ImGui::BeginCombo("##class", class_preview)) {
        for (int i = 0; i < 6; ++i) {
            const bool sel = (filter_class_ == class_vals[i]);
            if (ImGui::Selectable(class_items[i], sel)) {
                filter_class_ = class_vals[i];
                filter_dirty_ = true;
            }
            if (sel) ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
    }

    ImGui::SameLine();
    ImGui::Text("Search:");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(200);
    if (ImGui::InputText("##search", search_buf_, sizeof(search_buf_))) {
        filter_dirty_ = true;
    }

    ImGui::SameLine();
    if (ImGui::Button("Clear")) {
        filter_domain_ = 0;
        filter_class_ = 0;
        search_buf_[0] = '\0';
        filter_dirty_ = true;
    }
}

void ClassTableBrowser::draw_table() {
    if (!class_table_.loaded()) return;

    if (filter_dirty_) {
        rebuild_filtered_entries();
    }

    const int visible_count = static_cast<int>(filtered_entries_.size());
    const int total_count = static_cast<int>(class_table_.size());

    ImGui::Text("Showing %d of %d entries", visible_count, total_count);
    ImGui::Separator();

    const ImGuiTableFlags flags =
        ImGuiTableFlags_ScrollY | ImGuiTableFlags_RowBg |
        ImGuiTableFlags_BordersOuter | ImGuiTableFlags_BordersV |
        ImGuiTableFlags_Resizable | ImGuiTableFlags_Reorderable |
        ImGuiTableFlags_Hideable;

    if (ImGui::BeginTable("class_table", 8, flags, ImVec2(0, 300))) {
        ImGui::TableSetupColumn("ID",         ImGuiTableColumnFlags_DefaultSort | ImGuiTableColumnFlags_WidthFixed, 50.0f);
        ImGui::TableSetupColumn("Domain",     ImGuiTableColumnFlags_WidthFixed, 55.0f);
        ImGui::TableSetupColumn("Class",      ImGuiTableColumnFlags_WidthFixed, 75.0f);
        ImGui::TableSetupColumn("Type",       ImGuiTableColumnFlags_WidthFixed, 40.0f);
        ImGui::TableSetupColumn("Subtype",    ImGuiTableColumnFlags_WidthFixed, 100.0f);
        ImGui::TableSetupColumn("VisType[0]", ImGuiTableColumnFlags_WidthFixed, 70.0f);
        ImGui::TableSetupColumn("DataTable",  ImGuiTableColumnFlags_WidthFixed, 105.0f);
        ImGui::TableSetupColumn("DataPtr",    ImGuiTableColumnFlags_WidthFixed, 60.0f);
        ImGui::TableSetupScrollFreeze(1,1);
        ImGui::TableHeadersRow();

        ImGuiListClipper clipper;
        clipper.Begin(visible_count);

        while (clipper.Step()) {
            for (int row = clipper.DisplayStart; row < clipper.DisplayEnd; ++row) {
                const auto& fe = filtered_entries_[row];
                const uint16_t et = fe.entity_type;
                const auto* entry = fe.entry;

                ImGui::TableNextRow();
                const bool is_selected = (selected_entity_type_ == static_cast<int>(et));

                ImGui::TableSetColumnIndex(0);
                char id_label[32];
                std::snprintf(id_label, sizeof(id_label), "%d", static_cast<int>(et));
                if (ImGui::Selectable(id_label, is_selected,
                                       ImGuiSelectableFlags_SpanAllColumns)) {
                    selected_entity_type_ = static_cast<int>(et);
                }

                ImGui::TableSetColumnIndex(1);
                ImGui::TextUnformatted(domain_name(entry->domain));

                ImGui::TableSetColumnIndex(2);
                ImGui::TextUnformatted(vu_class_name(entry->cls));

                ImGui::TableSetColumnIndex(3);
                if (entry->cls == f4::world_convert::CLASS_OBJECTIVE && entry->type > 0) {
                    ImGui::Text("%d", entry->type);
                } else {
                    ImGui::TextDisabled("-");
                }

                ImGui::TableSetColumnIndex(4);
                if (entry->cls == f4::world_convert::CLASS_UNIT ||
                    entry->cls == f4::world_convert::CLASS_VEHICLE) {
                    ImGui::TextUnformatted(unit_subtype_name(entry->domain, entry->stype));
                } else {
                    ImGui::TextDisabled("-");
                }

                ImGui::TableSetColumnIndex(5);
                if (entry->vis_type[0] > 0) {
                    ImGui::Text("%d", entry->vis_type[0]);
                } else {
                    ImGui::TextDisabled("-");
                }

                ImGui::TableSetColumnIndex(6);
                ImGui::TextUnformatted(ct_data_type_name(entry->data_type));

                ImGui::TableSetColumnIndex(7);
                if (entry->data_type != f4::world_convert::DTYPE_NOTHING) {
                    ImGui::Text("%u", entry->data_ptr_index);
                } else {
                    ImGui::TextDisabled("-");
                }
            }
        }
        clipper.End();

        ImGui::EndTable();
    }
}

void ClassTableBrowser::draw_detail_panel() {
    if (selected_entity_type_ < 0) return;

    const auto* entry = class_table_.lookup(
        static_cast<uint16_t>(selected_entity_type_));
    if (!entry) return;

    ImGui::Separator();
    ImGui::Text("Detail: entity_type %d  (%s / %s)",
                selected_entity_type_,
                domain_name(entry->domain),
                vu_class_name(entry->cls));

    ImGui::Text("VisType:");
    for (int s = 0; s < 7; ++s) {
        if (entry->vis_type[s] > 0) {
            ImGui::SameLine();
            ImGui::Text("[%d]=%d", s, entry->vis_type[s]);
        }
    }

    if (model_db_ && entry->vis_type[0] > 0) {
        const auto* model = model_db_->model(entry->vis_type[0]);
        if (model) {
            ImGui::Text("Model[%d]: r=%.1f  LODs=%d  slots=%d  DOFs=%d  class=%s",
                        entry->vis_type[0],
                        model->radius,
                        model->n_lods,
                        model->n_slots,
                        model->n_dof,
                        std::string(model->visual_class()).c_str());
        }
    }

    // VCD detail.
    if (entry->data_type == f4::world_convert::DTYPE_VEHICLE) {
        const auto* vcd = theater_db_.vehicles.at(entry->data_ptr_index);
        if (vcd) {
            if (ImGui::TreeNode("VehicleClassData (VCD)")) {
                ImGui::Text("Name:       %s", vcd->name.c_str());
                ImGui::Text("NCTR:       %s", vcd->nctr.c_str());
                ImGui::Text("Hit Points: %d", vcd->hit_points);
                ImGui::Text("Max Speed:  %d kph", vcd->max_speed);
                ImGui::Text("Max Wt:     %d lbs", vcd->max_wt);
                ImGui::Text("Empty Wt:   %d lbs", vcd->empty_wt);
                ImGui::Text("Fuel Wt:    %d lbs", vcd->fuel_wt);
                ImGui::Text("RCS Factor: %.3f", vcd->rcs_factor);
                ImGui::Text("Radar Type: %d", vcd->radar_type);
                ImGui::Text("Pilots:     %d", vcd->number_of_pilots);
                if (ImGui::TreeNode("Weapons (16 hardpoints)")) {
                    for (int i = 0; i < 16; ++i) {
                        if (vcd->weapon[i] != 0) {
                            ImGui::Text("  HP[%2d]: weapon=%d  qty=%d",
                                        i, vcd->weapon[i], vcd->weapons[i]);
                        }
                    }
                    ImGui::TreePop();
                }
                if (ImGui::TreeNode("Damage Modifiers")) {
                    for (std::size_t i = 0; i < vcd->damage_mod.size(); ++i) {
                        if (vcd->damage_mod[i] != 0) {
                            ImGui::Text("  [%zu] = %d", i, vcd->damage_mod[i]);
                        }
                    }
                    ImGui::TreePop();
                }
                ImGui::TreePop();
            }
        } else {
            ImGui::TextDisabled("VCD index %u out of range (table has %zu entries)",
                                entry->data_ptr_index, theater_db_.vehicles.size());
        }
    }

    // UCD detail.
    if (entry->data_type == f4::world_convert::DTYPE_UNIT) {
        const auto* ucd = theater_db_.units.at(entry->data_ptr_index);
        if (ucd) {
            if (ImGui::TreeNode("UnitClassData (UCD)")) {
                ImGui::Text("Name:          %s", ucd->name.c_str());
                ImGui::Text("Movement:      %s (%d kph)",
                            movement_type_name(ucd->movement_type),
                            ucd->movement_speed);
                ImGui::Text("Max Range:     %d km", ucd->max_range);
                ImGui::Text("Fuel:          %d lbs", ucd->fuel);
                ImGui::Text("Fuel Rate:     %d lbs/min", ucd->rate);
                ImGui::Text("Role:          %d", ucd->role);
                if (ImGui::TreeNode("Vehicle Groups (16 slots)")) {
                    for (int i = 0; i < 16; ++i) {
                        if (ucd->num_elements[i] > 0) {
                            ImGui::Text("  Group[%2d]: %d vehicles of type %d",
                                        i, ucd->num_elements[i], ucd->vehicle_type[i]);
                        }
                    }
                    ImGui::TreePop();
                }
                if (ImGui::TreeNode("Mission Scores")) {
                    for (int i = 0; i < 16; ++i) {
                        if (ucd->scores[i] != 0) {
                            ImGui::Text("  Role[%2d] = %d", i, ucd->scores[i]);
                        }
                    }
                    ImGui::TreePop();
                }
                ImGui::TreePop();
            }
        } else {
            ImGui::TextDisabled("UCD index %u out of range (table has %zu entries)",
                                entry->data_ptr_index, theater_db_.units.size());
        }
    }

    // OCD detail.
    if (entry->data_type == f4::world_convert::DTYPE_OBJECTIVE) {
        const auto* ocd = theater_db_.objectives.at(entry->data_ptr_index);
        if (ocd) {
            if (ImGui::TreeNode("ObjectiveClassData (OCD)")) {
                ImGui::Text("Name:          %s", ocd->name.c_str());
                ImGui::Text("Features:      %d", ocd->features);
                ImGui::Text("Radar Feature: %d", ocd->radar_feature);
                ImGui::Text("First Feature: %d", ocd->first_feature);
                ImGui::Text("Data Rate:     %d", ocd->data_rate);
                ImGui::Text("Deag Dist:     %d m", ocd->deag_distance);
                ImGui::Text("PtData Index:  %d", ocd->pt_data_index);
                ImGui::Text("Icon Index:    %d", ocd->icon_index);
                if (ImGui::TreeNode("Detection Ranges")) {
                    for (std::size_t i = 0; i < ocd->detection.size(); ++i) {
                        if (ocd->detection[i] != 0) {
                            ImGui::Text("  [%zu] = %d", i, ocd->detection[i]);
                        }
                    }
                    ImGui::TreePop();
                }
                if (ImGui::TreeNode("Damage Modifiers")) {
                    for (std::size_t i = 0; i < ocd->damage_mod.size(); ++i) {
                        if (ocd->damage_mod[i] != 0) {
                            ImGui::Text("  [%zu] = %d", i, ocd->damage_mod[i]);
                        }
                    }
                    ImGui::TreePop();
                }
                ImGui::TreePop();
            }
        } else {
            ImGui::TextDisabled("OCD index %u out of range (table has %zu entries)",
                                entry->data_ptr_index, theater_db_.objectives.size());
        }
    }

    // FCD detail.
    if (entry->data_type == f4::world_convert::DTYPE_FEATURE) {
        const auto* fcd = theater_db_.features.at(entry->data_ptr_index);
        if (fcd) {
            if (ImGui::TreeNode("FeatureClassData (FCD)")) {
                ImGui::Text("Name:          %s", fcd->name.c_str());
                ImGui::Text("Hit Points:    %d", fcd->hit_points);
                ImGui::Text("Repair Time:   %d s", fcd->repair_time);
                ImGui::Text("Height:        %d", fcd->height);
                ImGui::Text("Angle:         %.1f deg", fcd->angle);
                ImGui::Text("Radar Type:    %d", fcd->radar_type);
                ImGui::Text("Priority:      %d", fcd->priority);
                ImGui::TreePop();
            }
        } else {
            ImGui::TextDisabled("FCD index %u out of range (table has %zu entries)",
                                entry->data_ptr_index, theater_db_.features.size());
        }
    }
}

void ClassTableBrowser::draw_export_bar() {
    ImGui::Separator();

    if (ImGui::Button("Export CSV...")) {
        auto path = pick_save_file("Export Class Table as CSV",
                                    "CSV (*.csv)|All files (*.*)",
                                    std::filesystem::path("class_table.csv"));
        if (!path.empty()) {
            export_csv(path);
        }
    }

    ImGui::SameLine();
    if (ImGui::Button("Export JSON...")) {
        auto path = pick_save_file("Export Class Table as JSON",
                                    "JSON (*.json)|All files (*.*)",
                                    std::filesystem::path("class_table.json"));
        if (!path.empty()) {
            export_json(path);
        }
    }

    if (!export_status_.empty()) {
        ImGui::SameLine();
        ImGui::TextDisabled("%s", export_status_.c_str());
    }
}

// ---------------------------------------------------------------------------
// CSV Export
// ---------------------------------------------------------------------------
void ClassTableBrowser::export_csv(const std::filesystem::path& path) {
    std::ofstream f(path);
    if (!f) {
        export_status_ = "Error: cannot write " + path.string();
        return;
    }

    f << "entity_type,domain,class,type,subtype,vis_type_0,vis_type_1,vis_type_2,"
      << "vis_type_3,vis_type_4,vis_type_5,vis_type_6,data_type,data_ptr_index,"
      << "table_name,vcd_name,vcd_hitpoints,vcd_max_speed,vcd_max_wt,"
      << "ucd_name,ucd_movement_type,ucd_movement_speed,ucd_max_range,"
      << "ocd_name,ocd_features,"
      << "fcd_name,fcd_hit_points,fcd_repair_time\n";

    const int n = static_cast<int>(class_table_.size());
    const int base = f4::world_convert::VU_LAST_ENTITY_TYPE;

    int exported = 0;
    for (int i = 0; i < n; ++i) {
        const uint16_t et = static_cast<uint16_t>(base + i);
        const auto* entry = class_table_.lookup(et);
        if (!entry) continue;
        if (!passes_filter(et, *entry)) continue;

        f << et << ","
          << static_cast<int>(entry->domain) << ","
          << static_cast<int>(entry->cls) << ","
          << static_cast<int>(entry->type) << ","
          << static_cast<int>(entry->stype) << ",";
        for (int s = 0; s < 7; ++s) f << entry->vis_type[s] << ",";
        f << static_cast<int>(entry->data_type) << ","
          << entry->data_ptr_index << ","
          << ct_data_type_name(entry->data_type) << ",";

        if (entry->data_type == f4::world_convert::DTYPE_VEHICLE) {
            const auto* vcd = theater_db_.vehicles.at(entry->data_ptr_index);
            if (vcd) {
                f << vcd->name << "," << vcd->hit_points << ","
                  << vcd->max_speed << "," << vcd->max_wt << ",";
            } else { f << ",,,,"; }
        } else { f << ",,,,"; }

        if (entry->data_type == f4::world_convert::DTYPE_UNIT) {
            const auto* ucd = theater_db_.units.at(entry->data_ptr_index);
            if (ucd) {
                f << ucd->name << "," << ucd->movement_type << ","
                  << ucd->movement_speed << "," << ucd->max_range << ",";
            } else { f << ",,,,"; }
        } else { f << ",,,,"; }

        if (entry->data_type == f4::world_convert::DTYPE_OBJECTIVE) {
            const auto* ocd = theater_db_.objectives.at(entry->data_ptr_index);
            if (ocd) {
                f << ocd->name << "," << static_cast<int>(ocd->features) << ",";
            } else { f << ",,"; }
        } else { f << ",,"; }

        if (entry->data_type == f4::world_convert::DTYPE_FEATURE) {
            const auto* fcd = theater_db_.features.at(entry->data_ptr_index);
            if (fcd) {
                f << fcd->name << "," << fcd->hit_points << ","
                  << fcd->repair_time << ",";
            } else { f << ",,,"; }
        } else { f << ",,,"; }

        f << "\n";
        ++exported;
    }

    export_status_ = "Exported " + std::to_string(exported) +
                     " entries to " + path.filename().string();
}

// ---------------------------------------------------------------------------
// JSON Export
// ---------------------------------------------------------------------------
void ClassTableBrowser::export_json(const std::filesystem::path& path) {
    std::ofstream f(path);
    if (!f) {
        export_status_ = "Error: cannot write " + path.string();
        return;
    }

    f << "[\n";

    const int n = static_cast<int>(class_table_.size());
    const int base = f4::world_convert::VU_LAST_ENTITY_TYPE;
    int exported = 0;

    for (int i = 0; i < n; ++i) {
        const uint16_t et = static_cast<uint16_t>(base + i);
        const auto* entry = class_table_.lookup(et);
        if (!entry) continue;
        if (!passes_filter(et, *entry)) continue;

        if (exported > 0) f << ",\n";

        f << "  {\n";
        f << "    \"entity_type\": " << et << ",\n";
        f << "    \"domain\": " << static_cast<int>(entry->domain)
          << ", \"domain_name\": \"" << domain_name(entry->domain) << "\",\n";
        f << "    \"class\": " << static_cast<int>(entry->cls)
          << ", \"class_name\": \"" << vu_class_name(entry->cls) << "\",\n";
        f << "    \"type\": " << static_cast<int>(entry->type) << ",\n";
        f << "    \"subtype\": " << static_cast<int>(entry->stype)
          << ", \"subtype_name\": \"" << unit_subtype_name(entry->domain, entry->stype) << "\",\n";

        f << "    \"vis_type\": [";
        for (int s = 0; s < 7; ++s) {
            if (s > 0) f << ", ";
            f << entry->vis_type[s];
        }
        f << "],\n";

        f << "    \"data_type\": " << static_cast<int>(entry->data_type)
          << ", \"data_type_name\": \"" << ct_data_type_name(entry->data_type) << "\",\n";
        f << "    \"data_ptr_index\": " << entry->data_ptr_index;

        if (entry->data_type == f4::world_convert::DTYPE_VEHICLE) {
            const auto* vcd = theater_db_.vehicles.at(entry->data_ptr_index);
            if (vcd) {
                f << ",\n    \"vehicle\": {\n";
                f << "      \"name\": \"" << vcd->name << "\",\n";
                f << "      \"nctr\": \"" << vcd->nctr << "\",\n";
                f << "      \"hit_points\": " << vcd->hit_points << ",\n";
                f << "      \"max_speed\": " << vcd->max_speed << ",\n";
                f << "      \"max_wt\": " << vcd->max_wt << ",\n";
                f << "      \"empty_wt\": " << vcd->empty_wt << ",\n";
                f << "      \"fuel_wt\": " << vcd->fuel_wt << ",\n";
                f << "      \"rcs_factor\": " << vcd->rcs_factor << ",\n";
                f << "      \"radar_type\": " << vcd->radar_type << ",\n";
                f << "      \"pilots\": " << vcd->number_of_pilots << ",\n";
                f << "      \"weapons\": [";
                for (int w = 0; w < 16; ++w) {
                    if (vcd->weapon[w] != 0) {
                        if (w > 0 && vcd->weapon[w-1] != 0) f << ", ";
                        f << "{\"id\":" << vcd->weapon[w]
                          << ",\"qty\":" << static_cast<int>(vcd->weapons[w]) << "}";
                    }
                }
                f << "]\n";
                f << "    }";
            }
        }

        if (entry->data_type == f4::world_convert::DTYPE_UNIT) {
            const auto* ucd = theater_db_.units.at(entry->data_ptr_index);
            if (ucd) {
                f << ",\n    \"unit\": {\n";
                f << "      \"name\": \"" << ucd->name << "\",\n";
                f << "      \"movement_type\": " << ucd->movement_type
                  << ", \"movement_type_name\": \"" << movement_type_name(ucd->movement_type) << "\",\n";
                f << "      \"movement_speed\": " << ucd->movement_speed << ",\n";
                f << "      \"max_range\": " << ucd->max_range << ",\n";
                f << "      \"fuel\": " << ucd->fuel << ",\n";
                f << "      \"role\": " << static_cast<int>(ucd->role) << ",\n";
                f << "      \"vehicle_groups\": [";
                bool first = true;
                for (int g = 0; g < 16; ++g) {
                    if (ucd->num_elements[g] > 0) {
                        if (!first) f << ", ";
                        f << "{\"count\":" << ucd->num_elements[g]
                          << ",\"vehicle_type\":" << ucd->vehicle_type[g] << "}";
                        first = false;
                    }
                }
                f << "]\n";
                f << "    }";
            }
        }

        if (entry->data_type == f4::world_convert::DTYPE_OBJECTIVE) {
            const auto* ocd = theater_db_.objectives.at(entry->data_ptr_index);
            if (ocd) {
                f << ",\n    \"objective\": {\n";
                f << "      \"name\": \"" << ocd->name << "\",\n";
                f << "      \"features\": " << static_cast<int>(ocd->features) << ",\n";
                f << "      \"radar_feature\": " << static_cast<int>(ocd->radar_feature) << ",\n";
                f << "      \"first_feature\": " << ocd->first_feature << ",\n";
                f << "      \"deag_distance\": " << ocd->deag_distance << "\n";
                f << "    }";
            }
        }

        if (entry->data_type == f4::world_convert::DTYPE_FEATURE) {
            const auto* fcd = theater_db_.features.at(entry->data_ptr_index);
            if (fcd) {
                f << ",\n    \"feature\": {\n";
                f << "      \"name\": \"" << fcd->name << "\",\n";
                f << "      \"hit_points\": " << fcd->hit_points << ",\n";
                f << "      \"repair_time\": " << fcd->repair_time << ",\n";
                f << "      \"radar_type\": " << fcd->radar_type << "\n";
                f << "    }";
            }
        }

        f << "\n  }";
        ++exported;
    }

    f << "\n]\n";
    export_status_ = "Exported " + std::to_string(exported) +
                     " entries to " + path.filename().string();
}

} // namespace f4::viewer
