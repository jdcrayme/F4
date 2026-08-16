// f4-models-viewer/src/imgui_panels.cpp
//
// ImGui panels for the 3D model viewer:
//   - Menu bar (File, View, Help)
//   - Model browser (scrollable list of all models)
//   - Inspector (selected model metadata)
//   - DOF panel (sliders for each DOF)
//   - Switch panel (combo boxes for each switch)
//   - LOD panel (radio buttons for LOD selection)
//   - Texture info panel (texture bank stats, per-mesh texture IDs)
//   - Status bar (model count, selected model, triangle count, FPS)

#include "viewer_state.hpp"

#include <f4/models/model_database.hpp>
#include <f4/models/model_record.hpp>
#include <f4/models/texture.hpp>

#include <imgui.h>
#include <rlImGui.h>
#include <raylib.h>

// tinyfiledialogs for native file open dialogs
#include <tinyfiledialogs.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

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

            if (ImGui::MenuItem("Load TEX...")) {
                const char* tex_filter[] = {"*.Tex", "*.tex", "*.TEX"};
                const char* tex_result = tinyfd_openFileDialog(
                    "Open KoreaObj.Tex", "",
                    3, tex_filter, "TEX files", 0);
                if (tex_result) {
                    std::filesystem::path tex_path(tex_result);
                    std::string err = impl.db.load_tex(tex_path);
                    if (err.empty()) {
                        impl.status_msg = "Loaded TEX: " +
                                         std::to_string(impl.db.tex_entries().size()) +
                                         " textures from " + tex_path.filename().string();
                        // Force mesh rebuild to pick up textures
                        impl.meshes_dirty = true;
                    } else {
                        impl.status_msg = "TEX load error: " + err;
                    }
                }
            }

            ImGui::Separator();

            if (ImGui::MenuItem("Save Screenshot...", "F2")) {
                const char* png_filter[] = {"*.png"};
                const char* png_result = tinyfd_saveFileDialog(
                    "Save Screenshot", "f4_model_viewer.png",
                    1, png_filter, "PNG images");
                if (png_result) {
                    // Schedule a screenshot 0.5s in the future (gives the
                    // dialog time to close and the next frame to render).
                    impl.screenshot_pending = true;
                    impl.screenshot_at = GetTime() + 0.5;
                    impl.screenshot_path = std::filesystem::path(png_result);
                    impl.status_msg = std::string("Screenshot queued: ") + png_result;
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
            ImGui::Separator();
            ImGui::Checkbox("Lighting", &impl.lighting_enabled);
            ImGui::Checkbox("Light Gizmo", &impl.show_light_gizmo);
            ImGui::Checkbox("Stats Overlay", &impl.show_stats_overlay);
            ImGui::EndMenu();
        }

        // ── Help ──────────────────────────────────────────────────────
        if (ImGui::BeginMenu("Help")) {
            if (ImGui::MenuItem("Controls")) {
                impl.status_msg = "L-drag: orbit | R-drag: pan | Scroll: zoom | F: fit | R: reset | F2: screenshot";
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
                            impl.select_parent_internal(i);
                        }
                        ImGui::PopID();
                    }
                }
            }
            ImGui::EndChild();
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
    ImGui::SetNextWindowSize({320, 240}, ImGuiCond_FirstUseEver);
    if (ImGui::Begin("DOFs")) {
        if (impl.model_state.dofs.empty()) {
            ImGui::TextDisabled("No DOFs for this model.");
        } else {
            // Header row: master controls
            if (ImGui::Button("Reset All")) {
                impl.reset_animations();
            }
            ImGui::SameLine();
            if (ImGui::Button(impl.animation_paused ? "Play" : "Pause")) {
                impl.animation_paused = !impl.animation_paused;
            }
            ImGui::SameLine();
            ImGui::TextDisabled("(%d DOFs)", static_cast<int>(impl.model_state.dofs.size()));

            ImGui::Separator();

            // Sliders
            bool changed = false;
            for (auto& dof : impl.model_state.dofs) {
                ImGui::PushID(dof.dof_number);

                // Find the matching animation track (if any) so we can
                // show an Auto checkbox on the same row as the slider.
                ViewerApp::Impl::AnimationTrack* track = nullptr;
                for (auto& t : impl.animations) {
                    if (t.dof_number == dof.dof_number) { track = &t; break; }
                }

                char label[64];
                std::snprintf(label, sizeof(label), "DOF %d", dof.dof_number);
                if (ImGui::SliderFloat(label, &dof.value, dof.min, dof.max)) {
                    changed = true;
                    // If the user drags a DOF manually, disable its
                    // animation track to avoid fighting.
                    if (track) track->enabled = false;
                }

                // Auto checkbox on the same row, right-aligned
                if (track) {
                    ImGui::SameLine();
                    char cb_label[64];
                    std::snprintf(cb_label, sizeof(cb_label), "Auto##%d", dof.dof_number);
                    if (ImGui::Checkbox(cb_label, &track->enabled)) {
                        // Enabling auto resets phase so it starts cleanly
                        if (track->enabled) track->phase = 0.0f;
                    }
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

// ── Animation panel ────────────────────────────────────────────────────────
// Per-track speed / mode controls. Each DOF with an animation track gets
// a row: [enabled] [speed slider] [mode dropdown] [phase reset]
static void draw_animation_panel(ViewerApp::Impl& impl) {
    ImGui::SetNextWindowSize({340, 220}, ImGuiCond_FirstUseEver);
    if (ImGui::Begin("Animation")) {
        if (impl.animations.empty()) {
            ImGui::TextDisabled("No DOFs (select a model first).");
        } else {
            // Global controls
            if (ImGui::Button(impl.animation_paused ? "Resume All##anim" : "Pause All##anim")) {
                impl.animation_paused = !impl.animation_paused;
            }
            ImGui::SameLine();
            if (ImGui::Button("Reset Phases")) {
                for (auto& t : impl.animations) t.phase = 0.0f;
            }
            ImGui::SameLine();
            ImGui::TextDisabled("(space toggles)");

            ImGui::Separator();

            // Per-track controls
            bool changed = false;
            for (auto& t : impl.animations) {
                ImGui::PushID(t.dof_number);

                // Find the matching DofState for live value display
                const f4::models::DofState* ds = nullptr;
                for (const auto& d : impl.model_state.dofs) {
                    if (d.dof_number == t.dof_number) { ds = &d; break; }
                }

                char header[64];
                std::snprintf(header, sizeof(header),
                              "DOF %d%s", t.dof_number,
                              t.enabled ? " (running)" : "");
                if (ImGui::Checkbox(header, &t.enabled)) {
                    if (t.enabled) t.phase = 0.0f;
                    changed = true;
                }

                if (t.enabled) {
                    ImGui::Indent(16.0f);
                    ImGui::SetNextItemWidth(180);
                    if (ImGui::SliderFloat("Speed (Hz)", &t.speed, 0.0f, 30.0f, "%.2f")) {
                        changed = true;
                    }
                    ImGui::SameLine();
                    if (ImGui::Button("Reset##phase")) {
                        t.phase = 0.0f;
                    }

                    ImGui::SetNextItemWidth(180);
                    const char* mode = t.wrap_2pi ? "Wrap 0..2pi" : "Ping-pong min..max";
                    if (ImGui::BeginCombo("Mode##anim", mode)) {
                        if (ImGui::Selectable("Wrap 0..2pi", t.wrap_2pi)) {
                            t.wrap_2pi = true; changed = true;
                        }
                        if (ImGui::Selectable("Ping-pong min..max", !t.wrap_2pi)) {
                            t.wrap_2pi = false; changed = true;
                        }
                        ImGui::EndCombo();
                    }

                    if (ds) {
                        ImGui::TextDisabled("value=%.3f  range=[%.2f, %.2f]",
                                            ds->value, ds->min, ds->max);
                    }
                    ImGui::Unindent(16.0f);
                }

                ImGui::PopID();
                ImGui::Separator();
            }
            if (changed) impl.meshes_dirty = true;
        }
    }
    ImGui::End();
}

// ── Lighting panel ─────────────────────────────────────────────────────────
static void draw_lighting_panel(ViewerApp::Impl& impl) {
    ImGui::SetNextWindowSize({300, 280}, ImGuiCond_FirstUseEver);
    if (ImGui::Begin("Lighting")) {
        ImGui::Checkbox("Enable directional light", &impl.lighting_enabled);
        ImGui::Checkbox("Show light gizmo", &impl.show_light_gizmo);
        ImGui::Separator();

        // Direction as separate sliders — easier to drag than a 3-float widget
        float dir[3] = { impl.light_direction.x,
                         impl.light_direction.y,
                         impl.light_direction.z };
        ImGui::Text("Direction (world space)");
        ImGui::SetNextItemWidth(-1);
        if (ImGui::SliderFloat3("##dir", dir, -1.0f, 1.0f, "%.2f")) {
            impl.light_direction = { dir[0], dir[1], dir[2] };
        }
        ImGui::SameLine();
        if (ImGui::Button("Normalize")) {
            const float l = std::sqrt(dir[0]*dir[0] + dir[1]*dir[1] + dir[2]*dir[2]);
            if (l > 0.0001f) {
                impl.light_direction = { dir[0]/l, dir[1]/l, dir[2]/l };
            }
        }

        // Quick presets
        ImGui::Text("Presets:");
        ImGui::SameLine();
        if (ImGui::Button("Overhead")) {
            impl.light_direction = { 0.0f, -1.0f, 0.0f };
        }
        ImGui::SameLine();
        if (ImGui::Button("Sunset")) {
            impl.light_direction = { 0.7f, -0.3f, 0.7f };
        }
        ImGui::SameLine();
        if (ImGui::Button("Front-right")) {
            impl.light_direction = { 0.65f, -1.0f, 0.35f };
        }

        ImGui::Separator();
        ImGui::SetNextItemWidth(180);
        ImGui::SliderFloat("Intensity", &impl.light_intensity, 0.0f, 3.0f, "%.2f");

        // Color editors
        float amb[4] = { impl.ambient_color.r / 255.0f,
                         impl.ambient_color.g / 255.0f,
                         impl.ambient_color.b / 255.0f,
                         impl.ambient_color.a / 255.0f };
        float light[4] = { impl.light_color.r / 255.0f,
                           impl.light_color.g / 255.0f,
                           impl.light_color.b / 255.0f,
                           impl.light_color.a / 255.0f };
        if (ImGui::ColorEdit4("Ambient", amb)) {
            impl.ambient_color = {
                static_cast<unsigned char>(amb[0] * 255.0f),
                static_cast<unsigned char>(amb[1] * 255.0f),
                static_cast<unsigned char>(amb[2] * 255.0f),
                static_cast<unsigned char>(amb[3] * 255.0f)
            };
        }
        if (ImGui::ColorEdit4("Light", light)) {
            impl.light_color = {
                static_cast<unsigned char>(light[0] * 255.0f),
                static_cast<unsigned char>(light[1] * 255.0f),
                static_cast<unsigned char>(light[2] * 255.0f),
                static_cast<unsigned char>(light[3] * 255.0f)
            };
        }
    }
    ImGui::End();
}

// ── Materials panel (ColorBank viewer) ─────────────────────────────────────
// Renders the parsed ColorBank as a grid of swatches. Clicking a swatch
// copies its RGBA value to the status bar — useful for identifying which
// index a particular vertex color resolves to.
static void draw_materials_panel(ViewerApp::Impl& impl) {
    ImGui::SetNextWindowSize({400, 320}, ImGuiCond_FirstUseEver);
    if (ImGui::Begin("Materials (ColorBank)")) {
        const auto& cb = impl.db.color_bank();
        if (cb.empty()) {
            ImGui::TextDisabled("No ColorBank loaded.");
            ImGui::TextDisabled("Load an HDR file to populate the ColorBank.");
        } else {
            ImGui::Text("ColorBank: %zu entries (%d darkened)",
                        cb.size(), cb.n_darkened);
            ImGui::Separator();

            // Render swatches as individual ColorButtons. Simpler and more
            // portable than uploading a strip texture, and works regardless
            // of GL context state.
            const int swatch_px = 16;
            constexpr int cols = 32;
            const int total = static_cast<int>(cb.size());
            const int rows = (total + cols - 1) / cols;

            ImGui::Text("Grid: %d cols x %d rows", cols, rows);
            ImGui::Separator();

            // Draw swatch grid using individual color buttons (more portable
            // than ImageButton and works even if the texture upload failed).
            for (int i = 0; i < total; ++i) {
                if (i % cols != 0) ImGui::SameLine();

                const uint32_t rgba = cb.rgba_at(i);
                const unsigned char r = (rgba >> 24) & 0xFF;
                const unsigned char g = (rgba >> 16) & 0xFF;
                const unsigned char b = (rgba >> 8) & 0xFF;
                const unsigned char a = rgba & 0xFF;

                ImGui::PushID(i);
                const ImVec4 col(r / 255.0f, g / 255.0f, b / 255.0f, a / 255.0f);
                char btn_label[32];
                std::snprintf(btn_label, sizeof(btn_label), "%d", i);
                if (ImGui::ColorButton(btn_label, col,
                                        ImGuiColorEditFlags_NoTooltip |
                                        ImGuiColorEditFlags_AlphaPreview,
                                        ImVec2(swatch_px, swatch_px))) {
                    char msg[128];
                    std::snprintf(msg, sizeof(msg),
                        "ColorBank[%d] = R%d G%d B%d A%d", i, r, g, b, a);
                    impl.status_msg = msg;
                }
                if (ImGui::IsItemHovered()) {
                    ImGui::SetTooltip("[%d] R%d G%d B%d A%d", i, r, g, b, a);
                }
                ImGui::PopID();
            }

            ImGui::Separator();
            ImGui::TextDisabled("Click a swatch to copy its index/value to the status bar.");
        }
    }
    ImGui::End();
}

// ── Texture Thumbnails panel ────────────────────────────────────────────────
// Shows a scrollable grid of decoded RGBA thumbnails. Uses the same
// texture_cache that the canvas populates lazily — to view a thumbnail,
// the user must first select a model whose meshes reference it (which
// triggers fetch_texture() in scene.cpp's rebuild_meshes()).
static void draw_texture_thumbnails_panel(ViewerApp::Impl& impl) {
    ImGui::SetNextWindowSize({420, 360}, ImGuiCond_FirstUseEver);
    if (ImGui::Begin("Texture Thumbnails")) {
        const auto& tex_entries = impl.db.tex_entries();
        if (tex_entries.empty()) {
            ImGui::TextDisabled("No TEX bank loaded.");
            ImGui::TextDisabled("Use File > Load TEX... or load HDR/LOD/TEX trio.");
        } else {
            ImGui::Text("Textures: %zu  |  Cached: %zu",
                        tex_entries.size(), impl.texture_cache.map().size());
            ImGui::Separator();

            const int thumb_px = 64;
            const int avail = static_cast<int>(ImGui::GetContentRegionAvail().x);
            const int cols = std::max(1, avail / (thumb_px + 8));

            int shown = 0;
            for (std::size_t i = 0; i < tex_entries.size(); ++i) {
                const auto& entry = tex_entries[i];
                auto* ce = impl.texture_cache.lookup(static_cast<int>(i));
                if (!ce || !ce->uploaded) {
                    // Skip textures that aren't decoded yet. The user can
                    // populate the cache by selecting models that use them.
                    continue;
                }

                if (shown % cols != 0) ImGui::SameLine();
                ++shown;

                ImGui::PushID(static_cast<int>(i));

                // Render the cached Texture2D via rlImGuiImage
                const Texture2D& tex = ce->texture;
                rlImGuiImageButtonSize("##tex", &tex,
                                        Vector2(thumb_px, thumb_px));

                if (ImGui::IsItemHovered()) {
                    ImGui::SetTooltip(
                        "tex %zu\ndim=%u\npal=%d\nsize=%u bytes\nalpha=%s",
                        i, entry.dimension, entry.palette_id, entry.file_size,
                        ce->has_alpha ? "yes" : "no");
                }
                ImGui::PopID();
            }

            if (shown == 0) {
                ImGui::TextDisabled("No textures decoded yet.");
                ImGui::TextDisabled("Select a model whose meshes reference textures");
                ImGui::TextDisabled("to populate this cache.");
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
            // Help text explaining the options
            ImGui::TextDisabled("Tip: 'None' hides all children. 'Show All' renders every child at once.");
            ImGui::Separator();

            bool changed = false;
            for (auto& sw : impl.model_state.switches) {
                ImGui::PushID(sw.switch_number);
                char label[64];
                std::snprintf(label, sizeof(label), "Switch %d (%d children)",
                              sw.switch_number, sw.n_children);

                // Combo box for child selection.
                // active_child sentinel values:
                //   -2 = "None"     — hide all children
                //   -1 = "Show All" — render every child simultaneously
                //   0..n-1 = render only this child
                char current[48];
                if (sw.active_child == -2) {
                    std::snprintf(current, sizeof(current), "None");
                } else if (sw.active_child == -1) {
                    std::snprintf(current, sizeof(current), "Show All");
                } else {
                    std::snprintf(current, sizeof(current), "Child %d", sw.active_child);
                }

                if (ImGui::BeginCombo(label, current)) {
                    // "None" option — hide all children. Useful for
                    // inspecting the rest of the model without the
                    // switch's parts occluding it.
                    {
                        bool is_selected = (sw.active_child == -2);
                        if (ImGui::Selectable("None", is_selected)) {
                            sw.active_child = -2;
                            changed = true;
                        }
                        if (is_selected) {
                            ImGui::SetItemDefaultFocus();
                        }
                    }
                    // "Show All" option — renders every child simultaneously.
                    // Useful for switches that act as bitmasks (e.g. pylon
                    // loadout selectors where each child is one store).
                    {
                        bool is_selected = (sw.active_child == -1);
                        if (ImGui::Selectable("Show All", is_selected)) {
                            sw.active_child = -1;
                            changed = true;
                        }
                        if (is_selected) {
                            ImGui::SetItemDefaultFocus();
                        }
                    }
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

// ── Texture info panel ──────────────────────────────────────────────────────
static void draw_texture_panel(ViewerApp::Impl& impl) {
    ImGui::SetNextWindowSize({320, 300}, ImGuiCond_FirstUseEver);
    if (ImGui::Begin("Textures")) {
        const auto& tex_entries = impl.db.tex_entries();
        const auto& palettes = impl.db.palettes();

        if (tex_entries.empty()) {
            ImGui::TextDisabled("No texture bank loaded.");
            ImGui::TextDisabled("Use File > Load TEX to load KoreaObj.Tex.");
        } else {
            ImGui::Text("Textures: %d", static_cast<int>(tex_entries.size()));
            ImGui::Text("Palettes: %d", static_cast<int>(palettes.size()));
            ImGui::Text("Cached: %d", static_cast<int>(impl.texture_cache.map().size()));

            // Texture set selector
            const auto* rec = impl.db.model(impl.selected_parent);
            if (rec && rec->n_texture_sets > 1) {
                ImGui::Separator();
                ImGui::Text("Texture Sets: %d", rec->n_texture_sets);
                const char* set_names[] = {"Summer", "Winter", "Desert", "Set 3", "Set 4"};
                for (int i = 0; i < rec->n_texture_sets && i < 5; ++i) {
                    if (ImGui::RadioButton(set_names[i],
                            impl.selected_texture_set == i)) {
                        impl.selected_texture_set = i;
                        // Texture set change requires mesh rebuild since
                        // tex_id mapping changes per set.
                        impl.meshes_dirty = true;
                    }
                }
            }

            // Per-mesh texture IDs for current model
            if (!impl.mesh_entries.empty()) {
                ImGui::Separator();
                if (ImGui::TreeNode("Mesh Textures")) {
                    for (std::size_t i = 0; i < impl.mesh_entries.size(); ++i) {
                        const auto& me = impl.mesh_entries[i];
                        int tex_id = me.tex_id;
                        if (tex_id >= 0) {
                            // Show texture info
                            const auto& entry = tex_entries[static_cast<std::size_t>(tex_id)];
                            ImGui::Text("[%zu] tex=%d  dim=%u  pal=%d  size=%u",
                                        i, tex_id, entry.dimension,
                                        entry.palette_id, entry.file_size);

                            // Show if texture is cached/decoded
                            auto* ce2 = impl.texture_cache.lookup(tex_id);
                            if (ce2 && ce2->uploaded) {
                                ImGui::SameLine();
                                ImGui::TextColored({0.5f, 1.0f, 0.5f, 1.0f}, "OK");
                            } else {
                                ImGui::SameLine();
                                ImGui::TextColored({1.0f, 0.5f, 0.5f, 1.0f}, "?");
                            }
                        } else {
                            ImGui::Text("[%zu] (untextured)", i);
                        }
                    }
                    ImGui::TreePop();
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
    draw_texture_panel(*this);
    draw_animation_panel(*this);
    draw_lighting_panel(*this);
    draw_materials_panel(*this);
    draw_texture_thumbnails_panel(*this);
    draw_status_bar(*this);
    rlImGuiEnd();
}

} // namespace f4::models_viewer
