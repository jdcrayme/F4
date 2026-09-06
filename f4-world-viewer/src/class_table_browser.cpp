// f4-world-viewer/src/class_table_browser.cpp
//
// ClassTableBrowser -- a browsable, filterable, exportable view over the
// runtime class table (Data/Classes/falcon4.ct.json), with 3D model
// preview via RenderTexture2D drawing glTF models from the shared
// RuntimeModelCache.
//
// Tranche 0d: the binary FALCON4.ct decoder and the joined
// OCD/UCD/VCD/FCD/WCD/SSD theater-record browsing are gone from the
// viewer (f4-world-convert is no longer linked). The same joined data is
// available through the converted artifacts (cam2json --theater-data and
// ct2json outputs).
//
// Follows the HexInspector pattern: self-contained panel with open/close/
// draw, owns its own state, accessed via Tools menu.

#include "viewer_state.hpp"
#include <f4/viewer/class_table_browser.hpp>
#include <f4/viewer/enum_text.hpp>
#include <f4/viewer/file_dialog.hpp>

#include <f4/world_types/class_table.hpp>        // unit_subtype_name, DOMAIN_*, CLASS_*
#include <f4/assets/asset_root.hpp>              // Data/ discovery

#include <imgui.h>
#include <rlImGui.h>
#include <raylib.h>
#include <raymath.h>   // MatrixIdentity
#include <rlgl.h>      // rlDisableBackfaceCulling

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <string>

// ---------------------------------------------------------------------------
// PreviewCache — PImpl holding the Raylib GPU state owned by
// ClassTableBrowser. Defined here (not in the header) because it contains
// Raylib types that must NOT leak into the public header (raylib is a
// PRIVATE dependency of f4_world_viewer).
//
// Tranche 0d: the meshes, textures, lit shader, and default material all
// come from the shared f4::renderer::RenderResources (one GPU upload per
// unique model across every view). The only GPU state the browser still
// owns is the preview's display-texture descriptor (see its comment).
// ---------------------------------------------------------------------------
struct f4::viewer::PreviewCache {
    // Persistent Texture2D descriptor for the preview RenderTexture2D's
    // color attachment. rlImGuiImageSize casts the Texture* pointer to
    // ImTextureID and later DEREFERENCES it during ImGui::Render() (which
    // happens after draw_model_preview() returns). A stack temporary would
    // be destroyed before that, leaving a dangling pointer. This member
    // stays alive for the lifetime of PreviewCache, so the pointer is valid
    // when ImGui renders. Updated each frame in draw_model_preview().
    ::Texture2D preview_display_tex = {};
};

namespace f4::viewer {

using f4::world_types::unit_subtype_name;

// ---------------------------------------------------------------------------
// Corrected data_type_name -- uses the verified DataType enum from
// class_table.hpp, NOT the stale mapping in enum_text.hpp.
// ---------------------------------------------------------------------------
const char* ClassTableBrowser::ct_data_type_name(uint8_t dt) noexcept {
    using namespace f4::world_types;
    switch (static_cast<DataType>(dt)) {
        case DTYPE_NOTHING:       return "Nothing";
        case DTYPE_FEATURE:       return "Feature(FCD)";
        case DTYPE_OBJECTIVE:     return "Objective(OCD)";
        case DTYPE_UNIT:          return "Unit(UCD)";
        case DTYPE_VEHICLE:       return "Vehicle(VCD)";
        case DTYPE_WEAPON:        return "Weapon(WCD)";
        case DTYPE_SQUAD_STORES:  return "SquadStores(SSD)";
        default:                  return "Unknown";
    }
}

// ---------------------------------------------------------------------------
// Constructor — defined out-of-line because the implicit cleanup path
// for the unique_ptr<PreviewCache> member needs the full PreviewCache type
// (PImpl idiom). preview_cache_ is left null here; it's lazily allocated
// on first GPU use in ensure_lit_shader()/ensure_default_material()/
// build_preview_meshes(), which are all called from inside the ImGui frame
// (GL context is available by then).
// ---------------------------------------------------------------------------
ClassTableBrowser::ClassTableBrowser() = default;

// ---------------------------------------------------------------------------
// Destructor — clean up GPU resources.
//
// Guard with IsWindowReady() because the dtor runs when ViewerApp's impl_
// unique_ptr is destroyed, which happens AFTER ~ViewerApp's body — and
// ~ViewerApp's body may have already called CloseWindow() (if run() ever
// returned). Calling UnloadMesh / UnloadTexture / UnloadShader after the
// GL context is gone crashes on some drivers.
//
// If the user closes the browser via the X button (close()), that happens
// inside the ImGui frame, so IsWindowReady() is true and cleanup runs
// normally.
// ---------------------------------------------------------------------------
ClassTableBrowser::~ClassTableBrowser() {
    if (IsWindowReady()) {
        cleanup_preview();
    }
}

void ClassTableBrowser::cleanup_preview() {
    // Free the RenderTexture2D (color attachment + GL framebuffer).
    // UnloadRenderTexture only needs rt.id + rt.texture.id, so the raw
    // ids stored in the header are sufficient.
    if (preview_rt_valid_) {
        RenderTexture2D rt = {};
        rt.id = preview_rt_id_;
        rt.texture.id = preview_tex_id_;
        UnloadRenderTexture(rt);
        preview_rt_valid_ = false;
        preview_rt_id_ = 0;
        preview_tex_id_ = 0;
    }

    // Free all PreviewCache GPU resources. We must access the full
    // Raylib structs (::Mesh, ::Material, ::Shader, ::Texture2D) because
    // their Unload functions need internal fields (vboId arrays, maps
    // pointers, locs pointers) that aren't captured by a bare id.
    if (!preview_cache_) {
        // Nothing was ever built (browser was never opened, or no model
        // was ever previewed). Still reset the selection tracking.
        last_previewed_vis_type_ = -1;
        return;
    }

    // Tranche 0d: the meshes, PNG textures, lit shader, and default
    // material live in the shared f4::renderer::RenderResources — owned
    // and cleaned up by the viewer app, nothing to free here anymore.

    // Reset selection tracking so the next preview refits the camera.
    last_previewed_vis_type_ = -1;
}

// ---------------------------------------------------------------------------
// Data loading
// ---------------------------------------------------------------------------
void ClassTableBrowser::ensure_data_loaded() {
    if (data_load_attempted_) return;
    data_load_attempted_ = true;

    // Tranche 0d: load the committed JSON class table — the binary .ct
    // decoder is no longer linked. Data/ is discovered from the working
    // directory (AssetRoot::discover) or the source tree (F4_SOURCE_DIR).
    std::filesystem::path data_dir;
    if (auto root = f4::assets::AssetRoot::discover()) {
        data_dir = root->data_dir();
    }
    if (data_dir.empty() || !std::filesystem::exists(data_dir)) {
#ifdef F4_SOURCE_DIR
        data_dir = std::filesystem::path(F4_SOURCE_DIR) / "Data";
#endif
    }
    const auto ct_path = data_dir / "Classes" / "falcon4.ct.json";
    if (!std::filesystem::exists(ct_path)) {
        load_error_ = "Data/Classes/falcon4.ct.json not found (the binary "
                      "FALCON4.ct is no longer loaded in-app — run ct2json "
                      "or scripts/export-game-data).";
        return;
    }
    try {
        class_table_.load_auto(ct_path.string());
    } catch (const std::exception& e) {
        load_error_ = std::string("Error loading class table: ") + e.what();
        return;
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
    const f4::world_types::ClassTableEntry& entry) const
{
    if (filter_domain_ != 0 && entry.domain != filter_domain_) return false;
    if (filter_class_ != 0 && entry.cls != filter_class_) return false;

    if (search_buf_[0] != '\0') {
        std::string search_lower(search_buf_);
        std::transform(search_lower.begin(), search_lower.end(),
                       search_lower.begin(), ::tolower);

        // Match entity type ID
        std::string id_str = std::to_string(entity_type);
        std::transform(id_str.begin(), id_str.end(), id_str.begin(), ::tolower);
        if (id_str.find(search_lower) != std::string::npos) return true;

        // Match subtype name
        const char* sub = unit_subtype_name(entry.domain, entry.stype);
        if (sub) {
            std::string sub_lower(sub);
            std::transform(sub_lower.begin(), sub_lower.end(),
                           sub_lower.begin(), ::tolower);
            if (sub_lower.find(search_lower) != std::string::npos) return true;
        }

        // Match class name
        const char* cls = vu_class_name(entry.cls);
        if (cls) {
            std::string cls_lower(cls);
            std::transform(cls_lower.begin(), cls_lower.end(),
                           cls_lower.begin(), ::tolower);
            if (cls_lower.find(search_lower) != std::string::npos) return true;
        }

        // Match data table name
        const char* dt_name = ct_data_type_name(entry.data_type);
        if (dt_name) {
            std::string dt_lower(dt_name);
            std::transform(dt_lower.begin(), dt_lower.end(),
                           dt_lower.begin(), ::tolower);
            if (dt_lower.find(search_lower) != std::string::npos) return true;
        }

        return false;
    }

    return true;
}

// ---------------------------------------------------------------------------
// Pre-compute filtered entries whenever filters change.
// ---------------------------------------------------------------------------
void ClassTableBrowser::rebuild_filtered_entries() {
    filtered_entries_.clear();
    if (!class_table_.loaded()) return;

    const int n = static_cast<int>(class_table_.size());
    const int base = f4::world_types::VU_LAST_ENTITY_TYPE;

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

    ImGui::SetNextWindowSize(ImVec2(1100, 780), ImGuiCond_FirstUseEver);
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
        ImGui::TextDisabled("No data loaded. Data/Classes/falcon4.ct.json not found.");
    }

    ImGui::End();
}

void ClassTableBrowser::draw_toolbar() {
    if (data_loaded_) {
        ImGui::Text("falcon4.ct.json: %zu entries", class_table_.size());
    } else if (!load_error_.empty()) {
        ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "%s",
                            load_error_.c_str());
    }

    ImGui::SameLine(ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x - 70);
    if (ImGui::Button("Reload")) {
        data_load_attempted_ = false;
        data_loaded_ = false;
        load_error_.clear();
        class_table_ = f4::world_types::ClassTable{};
        selected_entity_type_ = -1;
        filtered_entries_.clear();
        filter_dirty_ = true;
        cleanup_preview();
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

    if (ImGui::BeginTable("class_table", 8, flags, ImVec2(0, 280))) {
        ImGui::TableSetupColumn("ID",         ImGuiTableColumnFlags_DefaultSort | ImGuiTableColumnFlags_WidthFixed, 50.0f);
        ImGui::TableSetupColumn("Domain",     ImGuiTableColumnFlags_WidthFixed, 55.0f);
        ImGui::TableSetupColumn("Class",      ImGuiTableColumnFlags_WidthFixed, 75.0f);
        ImGui::TableSetupColumn("Type",       ImGuiTableColumnFlags_WidthFixed, 40.0f);
        ImGui::TableSetupColumn("Subtype",    ImGuiTableColumnFlags_WidthFixed, 100.0f);
        ImGui::TableSetupColumn("VisType[0]", ImGuiTableColumnFlags_WidthFixed, 70.0f);
        ImGui::TableSetupColumn("DataTable",  ImGuiTableColumnFlags_WidthFixed, 115.0f);
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
                    selected_vis_slot_ = 0;  // reset to primary model
                }

                ImGui::TableSetColumnIndex(1);
                ImGui::TextUnformatted(domain_name(entry->domain));

                ImGui::TableSetColumnIndex(2);
                ImGui::TextUnformatted(vu_class_name(entry->cls));

                ImGui::TableSetColumnIndex(3);
                if (entry->cls == f4::world_types::CLASS_OBJECTIVE && entry->type > 0) {
                    ImGui::Text("%d", entry->type);
                } else {
                    ImGui::TextDisabled("-");
                }

                ImGui::TableSetColumnIndex(4);
                if (entry->cls == f4::world_types::CLASS_UNIT ||
                    entry->cls == f4::world_types::CLASS_VEHICLE) {
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
                if (entry->data_type != f4::world_types::DTYPE_NOTHING) {
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

// ---------------------------------------------------------------------------
// 3D Model Preview
//
// Pipeline (mirrors f4-models-viewer/src/scene.cpp + ViewerApp::Impl's
// ground_layout_3d.cpp mesh cache):
//   1. ensure_lit_shader()      — compile the directional+ambient shader
//   2. ensure_default_material()— 1x1 white tex + default material
//   3. build_preview_meshes()   — extract geometry, build Raylib ::Mesh,
//                                  upload to GPU, cache by vis_type
//   4. upload_preview_textures()- decode TEX blobs, upload as Texture2D
//   5. fit_camera_to_model()    — point camera at bbox center, set distance
//   6. BeginTextureMode + BeginMode3D + DrawMesh per mesh + EndMode3D +
//      EndTextureMode, then rlImGuiImageSize to display.
//
// Coordinate conversion: FreeFalcon BSP vertices are LH Y-up
//   +X=right, +Y=up, +Z=forward
// Raylib is RH Y-up:
//   +X=right, +Y=up, +Z=toward viewer
// Conversion: (x, y, z) -> (x, -z, y)
// (NOTE: the previous implementation used (x, y, -z) which matched the
//  outdated comment in f4-models-viewer/src/viewer_state.hpp but NOT the
//  actual working code. Models rendered sideways / upside-down.)
// ---------------------------------------------------------------------------

// f4::renderer provides model_vertex_to_raylib and resolve_vertex_color.
// No local duplicates needed.

// Lit shader is now provided by f4::renderer::LitShader.
// No local shader source strings needed.

void ClassTableBrowser::ensure_preview_target(int w, int h) {
    if (preview_rt_valid_ && preview_rt_w_ == w && preview_rt_h_ == h) return;
    cleanup_preview();
    RenderTexture2D rt = LoadRenderTexture(w, h);
    preview_rt_id_ = rt.id;
    preview_tex_id_ = rt.texture.id;
    preview_rt_w_ = w;
    preview_rt_h_ = h;
    preview_rt_valid_ = true;
}

void ClassTableBrowser::build_preview_meshes(int16_t vis_type_idx) {
    if (vis_type_idx <= 0) return;
    if (!render_resources_) return;
    // The shared RuntimeModelCache handles load + mesh build + PNG
    // texture upload (lazy, idempotent, marked built even on failure).
    render_resources_->build_mesh_for_model(vis_type_idx);
}

void ClassTableBrowser::fit_camera_to_model(int16_t vis_type_idx) {
    if (!render_resources_ || vis_type_idx <= 0) return;

    // Compute the bbox by scanning the loaded LOD-0 mesh vertices (they
    // are already in Raylib RH Y-up feet — the glTF loader baked the
    // transform, so no model-space conversion is needed).
    const auto* model = render_resources_->model_cache.lookup(vis_type_idx);
    if (!model || model->lod0_meshes.empty()) return;

    float min_x = 0, min_y = 0, min_z = 0, max_x = 0, max_y = 0, max_z = 0;
    bool any = false;
    for (const auto& me : model->lod0_meshes) {
        if (!me.mesh.vertices || me.mesh.vertexCount <= 0) continue;
        for (int v = 0; v < me.mesh.vertexCount; ++v) {
            const float x = me.mesh.vertices[v * 3 + 0];
            const float y = me.mesh.vertices[v * 3 + 1];
            const float z = me.mesh.vertices[v * 3 + 2];
            if (!any) {
                min_x = max_x = x; min_y = max_y = y; min_z = max_z = z;
                any = true;
            } else {
                min_x = std::min(min_x, x); max_x = std::max(max_x, x);
                min_y = std::min(min_y, y); max_y = std::max(max_y, y);
                min_z = std::min(min_z, z); max_z = std::max(max_z, z);
            }
        }
    }
    if (!any) return;

    cam_target_x_ = (min_x + max_x) * 0.5f;
    cam_target_y_ = (min_y + max_y) * 0.5f;
    cam_target_z_ = (min_z + max_z) * 0.5f;

    const float dx = max_x - min_x, dy = max_y - min_y, dz = max_z - min_z;
    const float radius = 0.5f * std::sqrt(dx * dx + dy * dy + dz * dz);

    // Distance = radius * 2.5 so the model fills a reasonable portion of
    // the view at fovy=45. Degenerate (radius=0) models get a sane default.
    cam_distance_ = radius * 2.5f;
    if (cam_distance_ < 1.0f) cam_distance_ = 50.0f;

    // Reset orbit angles so the user sees the model head-on.
    // The 30° pitch is important: with the lit shader's lightDir pointing
    // DOWN (sun above), top-facing inward normals are unlit (NdotL=0,
    // ambient only). A steeper pitch shows more of the side surfaces,
    // which catch the directional light and make the model clearly
    // visible. With the previous 20° pitch, the camera saw mostly the
    // dark top face and the model appeared nearly black.
    cam_azimuth_ = 0.785398f;   // 45°
    cam_elevation_ = 0.523599f; // 30°
}

void ClassTableBrowser::draw_model_preview(int16_t vis_type_idx) {
    last_preview_drew_meshes_ = false;
    last_preview_status_.clear();

    if (!render_resources_) {
        ImGui::TextDisabled("No render resources (glTF models unavailable).");
        last_preview_status_ = "no render_resources";
        return;
    }
    if (vis_type_idx <= 0) {
        ImGui::TextDisabled("No vis_type selected.");
        last_preview_status_ = "no vis_type";
        return;
    }

    // Build (or reuse) the glTF model through the shared cache.
    build_preview_meshes(vis_type_idx);
    const auto* model = render_resources_->model_cache.lookup(vis_type_idx);
    if (!model || model->lod0_meshes.empty()) {
        ImGui::TextDisabled("Model[%d] has no glTF export in Data/Models/koreaobj",
                            vis_type_idx);
        last_preview_status_ = "no geometry";
        return;
    }

    // Refit camera when the user picks a different visType.
    if (last_previewed_vis_type_ != vis_type_idx) {
        fit_camera_to_model(vis_type_idx);
        last_previewed_vis_type_ = vis_type_idx;
    }

    // Ensure the RenderTexture2D exists before drawing.
    const int preview_w = 512;
    const int preview_h = 512;
    ensure_preview_target(preview_w, preview_h);

    // Shared GPU setup (lit shader, default material — the model meshes
    // and PNG textures are already in the shared caches).
    render_resources_->ensure_default_material();

    // Build camera from orbit params + model bbox center.
    const float az = cam_azimuth_;
    const float el = cam_elevation_;
    const float dist = cam_distance_;
    const Vector3 target = { cam_target_x_, cam_target_y_, cam_target_z_ };
    const Vector3 cam_pos = {
        target.x + dist * std::cos(el) * std::sin(az),
        target.y + dist * std::sin(el),
        target.z + dist * std::cos(el) * std::cos(az)
    };

    Camera3D camera = {};
    camera.position = cam_pos;
    camera.target = target;
    camera.up = { 0, -1, 0 };
    camera.fovy = 45.0f;
    camera.projection = CAMERA_PERSPECTIVE;

    // Reconstruct RenderTexture2D for BeginTextureMode.
    //
    // CRITICAL: BeginTextureMode() calls rlViewport(0, 0, target.texture.width,
    // target.texture.height). If width/height are 0 (as they would be if we
    // only populated rt.id and rt.texture.id), the GL viewport becomes 0x0
    // and every draw call inside BeginMode3D is clipped out — the framebuffer
    // ends up with only the ClearBackground color (which reads as 'black' to
    // the user). Populate the full texture descriptor so the viewport is set
    // to the actual render-target size.
    RenderTexture2D rt = {};
    rt.id = preview_rt_id_;
    rt.texture.id      = preview_tex_id_;
    rt.texture.width   = preview_rt_w_;
    rt.texture.height  = preview_rt_h_;
    rt.texture.mipmaps = 1;
    rt.texture.format  = PIXELFORMAT_UNCOMPRESSED_R8G8B8A8;

    auto& lit_shader = render_resources_->lit_shader;
    const Material* default_mat = render_resources_->default_material_valid()
        ? &render_resources_->default_material() : nullptr;

    BeginTextureMode(rt);
        ClearBackground({ 30, 30, 38, 255 });
        BeginMode3D(camera);
            // Push shader uniforms via the shared f4::renderer::LitShader.
            if (lit_shader.is_loaded()) {
                lit_shader.set_lighting(
                    { 0.65f, -1.0f, 0.35f },     // light_dir
                    { 255, 250, 235, 255 },         // light_color
                    1.0f,                            // intensity
                    { 80, 80, 90, 255 });            // ambient
            }

            // Draw each mesh with its material (textured or default).
            // Draw opaque meshes first, then alpha ones — belt-and-suspenders
            // ordering for the chroma-key alpha in the PNG textures.
            const auto& meshes = model->lod0_meshes;
            std::vector<std::size_t> opaque_order, alpha_order;
            opaque_order.reserve(meshes.size());
            alpha_order.reserve(meshes.size());
            for (std::size_t i = 0; i < meshes.size(); ++i) {
                const auto& me = meshes[i];
                bool has_alpha = false;
                if (me.tex_id >= 0) {
                    auto* ce = render_resources_->texture_cache.lookup(me.tex_id);
                    if (ce && ce->uploaded) {
                        has_alpha = ce->has_alpha;
                    }
                }
                if (has_alpha) alpha_order.push_back(i);
                else           opaque_order.push_back(i);
            }

            auto draw_one = [&](std::size_t idx) {
                const auto& me = meshes[idx];
                if (me.mesh.vaoId == 0 && me.mesh.vboId[0] == 0) return;
                if (me.mesh.triangleCount <= 0) return;

                const Material* matToUse = default_mat;
                if (me.tex_id >= 0) {
                    auto* ce = render_resources_->texture_cache.lookup(me.tex_id);
                    if (ce && ce->uploaded) {
                        matToUse = &ce->material;
                    }
                    // else: texture not yet uploaded — fall through to
                    // default material (mesh renders with vertex colors).
                }
                if (!matToUse) return;
                DrawMesh(me.mesh, *matToUse, MatrixIdentity());
            };

            BeginBlendMode(BLEND_ALPHA);
            rlDisableBackfaceCulling();
            std::size_t drawn = 0;
            for (auto idx : opaque_order) { draw_one(idx); ++drawn; }
            for (auto idx : alpha_order)  { draw_one(idx); ++drawn; }
            rlEnableBackfaceCulling();
            EndBlendMode();

            last_preview_drew_meshes_ = (drawn > 0);
        EndMode3D();
    EndTextureMode();

    // Display the rendered texture via rlImGui.
    //
    // CRITICAL: rlImGuiImageSize (rlImGui.cpp:517) casts the Texture*
    // POINTER to ImTextureID — NOT texture->id. The ImGui render callback
    // later casts ImTextureID back to Texture* and reads ->id to bind the
    // GL texture. This means the Texture struct MUST outlive the call to
    // rlImGuiImageSize — it must still be valid when ImGui::Render() runs
    // at the end of the frame. The persistent PreviewCache member below
    // guarantees that (a stack temporary would dangle).
    if (!preview_cache_) preview_cache_ = std::make_unique<PreviewCache>();
    preview_cache_->preview_display_tex = { preview_tex_id_,
                                              preview_rt_w_, preview_rt_h_, 1,
                                              PIXELFORMAT_UNCOMPRESSED_R8G8B8A8 };
    rlImGuiImageSize(&preview_cache_->preview_display_tex, preview_w, preview_h);

    // Status line below the preview.
    {
        std::size_t n_meshes = meshes.size();
        std::size_t n_tris = 0;
        int n_textured = 0;
        for (const auto& me : meshes) {
            n_tris += static_cast<std::size_t>(me.mesh.triangleCount);
            if (me.tex_id >= 0) ++n_textured;
        }
        ImGui::TextDisabled("%zu meshes | %zu tris | %d textured | glTF LOD 0",
                             n_meshes, n_tris, n_textured);
    }

    // Orbit camera controls via drag on the image.
    if (ImGui::IsItemActive()) {
        ImVec2 delta = ImGui::GetIO().MouseDelta;
        cam_azimuth_ -= delta.x * 0.01f;
        cam_elevation_ += delta.y * 0.01f;
        cam_elevation_ = std::clamp(cam_elevation_, -1.5f, 1.5f);
    }
    // Zoom via scroll on the image.
    if (ImGui::IsItemHovered()) {
        float wheel = ImGui::GetIO().MouseWheel;
        if (wheel != 0) {
            cam_distance_ *= (1.0f - wheel * 0.1f);
            cam_distance_ = std::clamp(cam_distance_, 0.1f, 5000.0f);
        }
    }
    // Reset-camera button (refit to current model).
    ImGui::SameLine();
    if (ImGui::SmallButton("Fit")) {
        fit_camera_to_model(vis_type_idx);
    }
}

// ---------------------------------------------------------------------------
// Detail Panel
// ---------------------------------------------------------------------------
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

    // VisType selection — a proper dropdown (replaces the previous row of
    // tiny unlabeled buttons that appeared as "little blue rectangles").
    //
    // We list every non-zero visType slot for this entry. The user picks
    // one; the 3D preview on the left updates immediately. We also show
    // a compact inline summary so the user can see all available visTypes
    // at a glance without opening the dropdown.
    ImGui::Text("VisType:");
    ImGui::SameLine();

    // Build the list of available (slot, vis_type) pairs.
    // selected_vis_slot_ may point at a slot that has vis_type==0 (e.g.
    // after switching entries); if so, auto-advance to the first non-zero
    // slot so the preview always shows something.
    int first_nonzero_slot = -1;
    for (int s = 0; s < 7; ++s) {
        if (entry->vis_type[s] > 0) {
            if (first_nonzero_slot < 0) first_nonzero_slot = s;
        }
    }
    if (first_nonzero_slot < 0) {
        // No visType at all — nothing to preview.
        ImGui::TextDisabled("(none — this entity has no 3D model)");
        ImGui::NewLine();
    } else {
        // Clamp selected_vis_slot_ to a valid non-zero slot.
        if (selected_vis_slot_ < 0 || selected_vis_slot_ >= 7 ||
            entry->vis_type[selected_vis_slot_] <= 0) {
            selected_vis_slot_ = first_nonzero_slot;
        }

        // Dropdown preview label: "[slot] vis_type_id"
        char preview_label[64];
        std::snprintf(preview_label, sizeof(preview_label), "[%d] %d",
                      selected_vis_slot_,
                      entry->vis_type[selected_vis_slot_]);

        ImGui::SetNextItemWidth(160);
        if (ImGui::BeginCombo("##vistype_combo", preview_label)) {
            for (int s = 0; s < 7; ++s) {
                if (entry->vis_type[s] <= 0) continue;
                char item_label[64];
                std::snprintf(item_label, sizeof(item_label), "[%d] %d",
                              s, entry->vis_type[s]);
                const bool is_sel = (selected_vis_slot_ == s);
                if (ImGui::Selectable(item_label, is_sel)) {
                    selected_vis_slot_ = s;
                }
                if (is_sel) ImGui::SetItemDefaultFocus();
            }
            ImGui::EndCombo();
        }

        // Compact inline summary of all visType slots (read-only).
        ImGui::SameLine();
        ImGui::TextDisabled("(");
        for (int s = 0; s < 7; ++s) {
            if (entry->vis_type[s] > 0) {
                ImGui::SameLine(0, 0);
                if (s == selected_vis_slot_) {
                    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.4f, 0.8f, 1.0f, 1.0f));
                    ImGui::Text(" %d", entry->vis_type[s]);
                    ImGui::PopStyleColor();
                } else {
                    ImGui::TextDisabled(" %d", entry->vis_type[s]);
                }
            }
        }
        ImGui::SameLine(0, 0);
        ImGui::TextDisabled(" )");
        ImGui::NewLine();
    }

    // Two-column layout: 3D preview on left, data detail on right
    float panel_width = ImGui::GetContentRegionAvail().x;
    float preview_width = 320.0f;
    float detail_width = panel_width - preview_width - 20.0f;
    if (detail_width < 200.0f) detail_width = 200.0f;

    // Left column: 3D model preview
    int16_t active_vis = (selected_vis_slot_ >= 0 && selected_vis_slot_ < 7)
                            ? entry->vis_type[selected_vis_slot_] : 0;
    if (active_vis > 0) {
        ImGui::BeginGroup();
        ImGui::Text("Model[%d] (glTF)", active_vis);
        // Always call draw_model_preview — it handles the "no render
        // resources" and "no glTF export" cases gracefully with a status
        // message, and it keeps the preview pane at a stable size so the
        // layout doesn't jump when the user switches entries.
        draw_model_preview(active_vis);
        ImGui::EndGroup();
        ImGui::SameLine();
    }

    // Right column: data detail
    ImGui::BeginGroup();

    // Tranche 0d: the joined OCD/UCD/VCD/FCD/WCD/SSD record browsing is
    // gone from the viewer (f4-world-convert — the binary parser library
    // — is no longer linked). The same joined data is available through
    // the converted artifacts: Data/World/korea.world.json
    // (cam2json --theater-data) and Data/Classes/falcon4.ct.json (ct2json).
    ImGui::Text("Data table: %s", ct_data_type_name(entry->data_type));
    if (entry->data_type != f4::world_types::DTYPE_NOTHING) {
        ImGui::Text("Data ptr index: %u", entry->data_ptr_index);
    }
    ImGui::Text("Type: %d   Subtype: %s", static_cast<int>(entry->type),
                unit_subtype_name(entry->domain, entry->stype));
    ImGui::Separator();
    ImGui::TextDisabled("Class-table record detail (OCD/UCD/VCD/FCD/WCD joins)\n"
                        "moved to the converted JSON artifacts — see\n"
                        "Data/World/korea.world.json + ct2json output.");

    ImGui::EndGroup();
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
      << "vis_type_3,vis_type_4,vis_type_5,vis_type_6,data_type,data_ptr_index\n";

    const int n = static_cast<int>(class_table_.size());
    const int base = f4::world_types::VU_LAST_ENTITY_TYPE;

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
          << entry->data_ptr_index << "\n";
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
    const int base = f4::world_types::VU_LAST_ENTITY_TYPE;
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

        f << "\n  }";
        ++exported;
    }

    f << "\n]\n";
    export_status_ = "Exported " + std::to_string(exported) +
                     " entries to " + path.filename().string();
}

} // namespace f4::viewer
