// f4-models-viewer/src/imgui_panels.cpp
//
// ImGui panels for the 3D model viewer:
//   - Menu bar (File, View, Help)
//   - Model browser (scrollable list of all models)
//   - Inspector (selected model metadata)
//   - DOF panel (sliders for each DOF)
//   - Switch panel (combo boxes for each switch)
//   - LOD panel (radio buttons for LOD selection)
//   - Status bar (model count, selected model, triangle count, FPS)

#include "viewer_state.hpp"
#include "imgui_panels.hpp"
#include "file_ops.hpp"

#include <f4/models/model_database.hpp>
#include <f4/models/model_record.hpp>

#include <imgui.h>
#include <rlImGui.h>
#include <raylib.h>

// tinyfiledialogs for native file open dialogs
#include <tinyfiledialogs.h>

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>

namespace f4::models_viewer {

// ── Menu bar ───────────────────────────────────────────────────────────────
static void draw_menu_bar(ViewerApp::Impl& impl) {
    if (ImGui::BeginMainMenuBar()) {
        // ── File ──────────────────────────────────────────────────────
        if (ImGui::BeginMenu("File")) {
            if (ImGui::MenuItem("Open HDR/LOD...")) {
                // Open a native file dialog for the HDR file
                const char* hdr_filter[] = {"*.HDR", "*.hdr"};
                const char* hdr_result = tinyfd_openFileDialog(
                    "Open KoreaObj.HDR", "",
                    1, hdr_filter, "HDR files", 0);
                if (hdr_result) {
                    std::filesystem::path hdr_path(hdr_result);
                    // Derive LOD path from HDR path (same dir, .LOD extension)
                    std::filesystem::path lod_path =
                        hdr_path.parent_path() /
                        hdr_path.stem().replace_extension(".LOD");

                    // Check for .LOD, .lod, .DXL, .dxl
                    if (!std::filesystem::exists(lod_path)) {
                        lod_path = hdr_path.parent_path() /
                                   hdr_path.stem().replace_extension(".lod");
                    }
                    if (!std::filesystem::exists(lod_path)) {
                        lod_path = hdr_path.parent_path() /
                                   hdr_path.stem().replace_extension(".DXL");
                    }
                    if (!std::filesystem::exists(lod_path)) {
                        lod_path = hdr_path.parent_path() /
                                   hdr_path.stem().replace_extension(".dxl");
                    }

                    if (std::filesystem::exists(lod_path)) {
                        impl.load_model_files(hdr_path, lod_path);
                    } else {
                        impl.status_msg = "LOD file not found for: " + hdr_path.string();
                    }
                }
            }

            if (ImGui::MenuItem("Open Install...")) {
                const char* dir_result = tinyfd_selectFolderDialog(
                    "Select Falcon 4.0 Install Directory", "");
                if (dir_result) {
                    std::filesystem::path install_path(dir_result);
                    auto inst = f4::install::Installation::detect(install_path);
                    if (inst.valid()) {
                        impl.install = std::move(inst);
                        impl.load_from_install();
                    } else {
                        impl.status_msg = "Not a valid Falcon install: " +
                                          install_path.string();
                    }
                }
            }

            ImGui::Separator();

            if (ImGui::MenuItem("Exit", "Alt+F4")) {
                impl.should_exit = true;
            }
            ImGui::EndMenu();
        }

        // ── View ──────────────────────────────────────────────────────
        if (ImGui::BeginMenu("View")) {
            ImGui::Checkbox("Wireframe", &impl.show_wireframe);
            ImGui::Checkbox("Grid", &impl.show_grid);
            ImGui::Checkbox("Axes", &impl.show_axes);
            ImGui::Checkbox("Bounding Sphere", &impl.show_bounding_sphere);
            ImGui::Checkbox("AABB", &impl.show_aabb);
            ImGui::EndMenu();
        }

        // ── Help ──────────────────────────────────────────────────────
        if (ImGui::BeginMenu("Help")) {
            if (ImGui::MenuItem("Controls")) {
                // Could open a popup — for now just set status
                impl.status_msg = "L-drag: orbit | R-drag: pan | Scroll: zoom | F: fit | R: reset";
            }
            ImGui::EndMenu();
        }

        ImGui::EndMainMenuBar();
    }
}

// ── Model browser ──────────────────────────────────────────────────────────
static void draw_model_browser(ViewerApp::Impl& impl) {
    ImGui::SetNextWindowSize({350, 500}, ImGuiCond_FirstUseEver);
    if (ImGui::Begin("Model Browser")) {
        if (!impl.doc_loaded) {
            ImGui::TextDisabled("No model database loaded.");
            ImGui::TextDisabled("Use File > Open HDR/LOD to load.");
        } else {
            ImGui::Text("Models: %d", impl.db.n_models());

            // Search filter
            static char filter_buf[128] = {};
            ImGui::InputText("Filter", filter_buf, sizeof(filter_buf));

            const auto& models = impl.db.models();

            // Scrollable list
            if (ImGui::BeginChild("ModelList", {-1, -1}, ImGuiChildFlags_Borders)) {
                ImGuiListClipper clipper;
                clipper.Begin(static_cast<int>(models.size()));
                while (clipper.Step()) {
                    for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i) {
                        const auto& m = models[static_cast<std::size_t>(i)];

                        // Apply filter
                        if (filter_buf[0] != '\0') {
                            char idx_buf[32];
                            std::snprintf(idx_buf, sizeof(idx_buf), "%d", i);
                            // Simple substring match on index or visual class
                            std::string_view vc = m.visual_class();
                            std::string label = idx_buf + std::string(" ") +
                                                std::string(vc);
                            if (label.find(filter_buf) == std::string::npos) {
                                continue;
                            }
                        }

                        ImGui::PushID(i);
                        bool is_selected = (i == impl.selected_parent);
                        char label[256];
                        std::snprintf(label, sizeof(label),
                            "%d | r=%.1f | %s | sl:%d d:%d",
                            i, m.radius,
                            m.visual_class().data(),
                            m.n_slots, m.effective_dofs());

                        if (ImGui::Selectable(label, is_selected)) {
                            impl.selected_parent = i;
                            impl.selected_lod = 0;
                            impl.meshes_dirty = true;
                            impl.model_list_scroll_to = i;

                            // Initialize model state for the new selection
                            impl.model_state = {};
                            for (int d = 0; d < m.effective_dofs(); ++d) {
                                f4::models::DofState ds;
                                ds.dof_number = d;
                                ds.value = 0;
                                ds.min = 0;
                                ds.max = 6.28318530718f;
                                impl.model_state.dofs.push_back(ds);
                            }
                            for (int s = 0; s < m.effective_switches(); ++s) {
                                f4::models::SwitchState ss;
                                ss.switch_number = s;
                                ss.active_child = 0;
                                ss.n_children = 2;
                                impl.model_state.switches.push_back(ss);
                            }

                            impl.fit_to_model();
                        }
                        ImGui::PopID();
                    }
                }
            }
            ImGui::EndChild();

            // Auto-scroll if requested
            if (impl.model_list_scroll_to >= 0) {
                impl.model_list_scroll_to = -1;
            }
        }
    }
    ImGui::End();
}

// ── Inspector ──────────────────────────────────────────────────────────────
static void draw_inspector(ViewerApp::Impl& impl) {
    ImGui::SetNextWindowSize({320, 400}, ImGuiCond_FirstUseEver);
    if (ImGui::Begin("Inspector")) {
        if (!impl.doc_loaded || impl.selected_parent < 0) {
            ImGui::TextDisabled("No model selected.");
        } else {
            const auto* rec = impl.db.model(impl.selected_parent);
            if (!rec) {
                ImGui::TextDisabled("Invalid model index.");
            } else {
                ImGui::Text("Model %d", rec->index);
                ImGui::Separator();

                ImGui::Text("Radius: %.2f", rec->radius);
                ImGui::Text("Visual class: %s", rec->visual_class().data());

                // Bounding box
                if (ImGui::TreeNode("Bounding Box")) {
                    ImGui::Text("X: [%.2f, %.2f]", rec->bbox.min_x, rec->bbox.max_x);
                    ImGui::Text("Y: [%.2f, %.2f]", rec->bbox.min_y, rec->bbox.max_y);
                    ImGui::Text("Z: [%.2f, %.2f]", rec->bbox.min_z, rec->bbox.max_z);
                    ImGui::TreePop();
                }

                // Signatures
                ImGui::Text("Radar sig: %.4f", rec->radar_signature);
                ImGui::Text("IR sig: %.4f", rec->ir_signature);

                ImGui::Separator();

                // Structure counts
                ImGui::Text("LODs: %d", static_cast<int>(rec->n_lods));
                ImGui::Text("Slots: %d", static_cast<int>(rec->n_slots));
                ImGui::Text("DOFs: %d", rec->effective_dofs());
                ImGui::Text("Switches: %d", rec->effective_switches());
                ImGui::Text("Tex sets: %d", static_cast<int>(rec->n_texture_sets));
                ImGui::Text("Dynamic coords: %d", static_cast<int>(rec->n_dynamic_coords));

                // LOD list
                if (!rec->lods.empty() && ImGui::TreeNode("LOD List")) {
                    for (std::size_t i = 0; i < rec->lods.size(); ++i) {
                        const auto& lod = rec->lods[i];
                        ImGui::Text("[%zu] %s  range=%.1f  idx=%d",
                                    i,
                                    lod.name.empty() ? "(unnamed)" : lod.name.c_str(),
                                    lod.max_range,
                                    lod.lod_table_idx);
                    }
                    ImGui::TreePop();
                }

                // Slot list
                if (!rec->slots.empty() && ImGui::TreeNode("Slots")) {
                    for (std::size_t i = 0; i < rec->slots.size(); ++i) {
                        const auto& slot = rec->slots[i];
                        ImGui::Text("[%zu] pos=(%.2f, %.2f, %.2f)",
                                    i,
                                    slot.position.x,
                                    slot.position.y,
                                    slot.position.z);
                    }
                    ImGui::TreePop();
                }
            }
        }
    }
    ImGui::End();
}

// ── DOF panel ──────────────────────────────────────────────────────────────
static void draw_dof_panel(ViewerApp::Impl& impl) {
    ImGui::SetNextWindowSize({300, 200}, ImGuiCond_FirstUseEver);
    if (ImGui::Begin("DOFs")) {
        if (impl.model_state.dofs.empty()) {
            ImGui::TextDisabled("No DOFs for this model.");
        } else {
            bool changed = false;
            for (auto& dof : impl.model_state.dofs) {
                ImGui::PushID(dof.dof_number);
                char label[64];
                std::snprintf(label, sizeof(label), "DOF %d", dof.dof_number);
                if (ImGui::SliderFloat(label, &dof.value, dof.min, dof.max)) {
                    changed = true;
                }
                ImGui::PopID();
            }
            if (changed) {
                impl.meshes_dirty = true;
            }
        }
    }
    ImGui::End();
}

// ── Switch panel ───────────────────────────────────────────────────────────
static void draw_switch_panel(ViewerApp::Impl& impl) {
    ImGui::SetNextWindowSize({300, 200}, ImGuiCond_FirstUseEver);
    if (ImGui::Begin("Switches")) {
        if (impl.model_state.switches.empty()) {
            ImGui::TextDisabled("No switches for this model.");
        } else {
            bool changed = false;
            for (auto& sw : impl.model_state.switches) {
                ImGui::PushID(sw.switch_number);
                char label[64];
                std::snprintf(label, sizeof(label), "Switch %d", sw.switch_number);

                // Combo box for child selection
                char current[32];
                std::snprintf(current, sizeof(current), "Child %d", sw.active_child);

                if (ImGui::BeginCombo(label, current)) {
                    for (int c = 0; c < sw.n_children; ++c) {
                        char child_label[32];
                        std::snprintf(child_label, sizeof(child_label), "Child %d", c);
                        bool is_selected = (sw.active_child == c);
                        if (ImGui::Selectable(child_label, is_selected)) {
                            sw.active_child = c;
                            changed = true;
                        }
                        if (is_selected) {
                            ImGui::SetItemDefaultFocus();
                        }
                    }
                    ImGui::EndCombo();
                }
                ImGui::PopID();
            }
            if (changed) {
                impl.meshes_dirty = true;
            }
        }
    }
    ImGui::End();
}

// ── LOD panel ──────────────────────────────────────────────────────────────
static void draw_lod_panel(ViewerApp::Impl& impl) {
    ImGui::SetNextWindowSize({200, 150}, ImGuiCond_FirstUseEver);
    if (ImGui::Begin("LOD Selection")) {
        if (!impl.doc_loaded || impl.selected_parent < 0) {
            ImGui::TextDisabled("No model selected.");
        } else {
            const auto* rec = impl.db.model(impl.selected_parent);
            if (!rec || rec->lods.empty()) {
                ImGui::TextDisabled("No LODs available.");
            } else {
                bool changed = false;
                for (std::size_t i = 0; i < rec->lods.size(); ++i) {
                    ImGui::PushID(static_cast<int>(i));
                    char label[64];
                    if (rec->lods[i].name.empty()) {
                        std::snprintf(label, sizeof(label), "LOD %zu", i);
                    } else {
                        std::snprintf(label, sizeof(label), "LOD %zu: %s",
                                      i, rec->lods[i].name.c_str());
                    }
                    if (ImGui::RadioButton(label,
                            impl.selected_lod == static_cast<int>(i))) {
                        impl.selected_lod = static_cast<int>(i);
                        changed = true;
                    }
                    ImGui::PopID();
                }
                if (changed) {
                    impl.meshes_dirty = true;
                    impl.status_msg = "LOD " + std::to_string(impl.selected_lod);
                }
            }
        }
    }
    ImGui::End();
}

// ── Status bar ─────────────────────────────────────────────────────────────
static void draw_status_bar(ViewerApp::Impl& impl) {
    const float height = ImGui::GetFrameHeight();
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos({viewport->WorkPos.x, viewport->WorkPos.y + viewport->WorkSize.y - height});
    ImGui::SetNextWindowSize({viewport->WorkSize.x, height});
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);

    ImGuiWindowFlags flags = ImGuiWindowFlags_NoScrollbar |
                             ImGuiWindowFlags_NoSavedSettings |
                             ImGuiWindowFlags_NoTitleBar |
                             ImGuiWindowFlags_NoResize |
                             ImGuiWindowFlags_NoMove |
                             ImGuiWindowFlags_NoBringToFrontOnFocus;

    if (ImGui::Begin("##StatusBar", nullptr, flags)) {
        // Model count
        if (impl.doc_loaded) {
            ImGui::Text("Models: %d", impl.db.n_models());
            ImGui::SameLine();
            ImGui::Text("| Selected: %d", impl.selected_parent);
            ImGui::SameLine();
            ImGui::Text("| Tris: %zu", impl.total_tri_count);
        } else {
            ImGui::Text("No model loaded");
        }

        ImGui::SameLine(ImGui::GetContentRegionAvail().x - 80);
        ImGui::Text("FPS: %d", GetFPS());

        // Status message (shown briefly)
        if (!impl.status_msg.empty()) {
            ImGui::SameLine(ImGui::GetContentRegionAvail().x / 2);
            ImGui::TextColored({1, 1, 0.6f, 1}, "%s", impl.status_msg.c_str());
        }
    }
    ImGui::End();
    ImGui::PopStyleVar();
}

// ── draw_imgui (top-level) ─────────────────────────────────────────────────
void ViewerApp::Impl::draw_imgui() {
    rlImGuiBegin();
    draw_menu_bar(*this);
    draw_model_browser(*this);
    draw_inspector(*this);
    draw_dof_panel(*this);
    draw_switch_panel(*this);
    draw_lod_panel(*this);
    draw_status_bar(*this);
    rlImGuiEnd();
}

} // namespace f4::models_viewer
