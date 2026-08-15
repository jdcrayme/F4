// f4-world-viewer/src/class_table_browser.cpp
//
// ClassTableBrowser -- a browsable, filterable, exportable view over the
// Falcon4.ct class table + OCD/UCD/VCD/FCD/WCD/SSD theater object data,
// with 3D model preview via RenderTexture2D.
//
// Follows the HexInspector pattern: self-contained panel with open/close/
// draw, owns its own state, accessed via Tools menu.

#include "viewer_state.hpp"
#include <f4/viewer/class_table_browser.hpp>
#include <f4/viewer/enum_text.hpp>
#include <f4/viewer/file_dialog.hpp>

#include <f4/world_convert/class_table.hpp>      // unit_subtype_name, DOMAIN_*, CLASS_*
#include <f4/world_convert/objective_decoder.hpp> // objective_type_name
#include <f4/world_convert/theater_data.hpp>     // movement_type_name, damage_type_name

#include <f4/models/geometry.hpp>
#include <f4/models/model_database.hpp>

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
// PreviewCache — PImpl holding all Raylib GPU resources owned by
// ClassTableBrowser. Defined here (not in the header) because it contains
// Raylib types that must NOT leak into the public header (raylib is a
// PRIVATE dependency of f4_world_viewer).
//
// Members:
//   mesh_cache     — per-vis_type vector of (::Mesh, tex_id). Each ::Mesh
//                    is built via UploadMesh and must be freed with
//                    UnloadMesh (which needs the full struct, not just
//                    an id, to access vboId/vaoId for GPU cleanup).
//   texture_cache  — per-tex_id (::Texture2D, ::Material). The Material
//                    references the Texture on its diffuse map; both must
//                    be freed (UnloadMaterial then UnloadTexture).
//   lit_shader     — single directional+ambient shader, compiled once.
//   default_mat    — cached Material with a 1x1 white texture on diffuse
//                    (so untextured meshes sample (1,1,1,1) instead of
//                    undefined data, which would trigger the lit shader's
//                    `if (tex.a < 0.5) discard;` and hide the mesh).
//   fallback_white_tex — the 1x1 texture bound to default_mat.
// ---------------------------------------------------------------------------
struct f4::viewer::PreviewCache {
    struct MeshEntry {
        ::Mesh mesh = {};
        int32_t tex_id = -1;  // -1 = untextured, use default_mat
    };
    struct MeshCacheEntry {
        std::vector<MeshEntry> meshes;
        bool built = false;
        bool valid = false;
        std::size_t tri_count = 0;
    };
    std::unordered_map<int, MeshCacheEntry> mesh_cache;

    // f4::renderer::TextureCache replaces the manual texture cache map.
    f4::renderer::TextureCache texture_cache;

    // f4::renderer::LitShader replaces the manual shader + uniform locs.
    f4::renderer::LitShader lit_shader_ensure;

    ::Texture2D fallback_white_tex = {};
    ::Material default_mat = {};
    bool default_mat_built = false;

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

using f4::world_convert::unit_subtype_name;
using f4::world_convert::movement_type_name;
using f4::world_convert::damage_type_name;

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

    // Free cached meshes. Each ::Mesh was allocated with RL_MALLOC for its
    // vertex/normal/texcoord/color/indices arrays and uploaded with
    // UploadMesh; UnloadMesh handles both the GPU VBOs/VAO and the CPU
    // arrays. We pass the full struct so Raylib can access vboId/vaoId.
    for (auto& [vis_idx, entry] : preview_cache_->mesh_cache) {
        for (auto& me : entry.meshes) {
            // me.mesh.vaoId != 0 means it was actually uploaded to GPU.
            // (A default-constructed ::Mesh has vaoId == 0.)
            if (me.mesh.vaoId != 0 || me.mesh.vboId[0] != 0) {
                UnloadMesh(me.mesh);
            }
            me.mesh = {};
        }
    }
    preview_cache_->mesh_cache.clear();

    // Free cached textures via f4::renderer::TextureCache.
    preview_cache_->texture_cache.unload_all();

    // Free the default material + fallback white texture + lit shader.
    // These are built once per browser-open cycle; freeing them here lets
    // us re-build cleanly if the user closes and re-opens the browser.
    if (preview_cache_->default_mat_built) {
        // Detach the fallback texture so UnloadMaterial doesn't free it
        // (we own it separately).
        preview_cache_->default_mat.maps[MATERIAL_MAP_DIFFUSE].texture = {};
        UnloadMaterial(preview_cache_->default_mat);
        preview_cache_->default_mat = {};
        preview_cache_->default_mat_built = false;
    }
    if (preview_cache_->fallback_white_tex.id != 0) {
        UnloadTexture(preview_cache_->fallback_white_tex);
        preview_cache_->fallback_white_tex = {};
    }
    // LitShader handles its own cleanup via RAII destructor.
    // No need to manually UnloadShader.

    // Reset selection tracking so the next preview refits the camera.
    last_previewed_vis_type_ = -1;
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

    // Load theater object database (OCD/UCD/VCD/FCD/WCD/SSD/etc.)
    // Same two search locations as install_flow.cpp:
    //   1. install-level terrdata/objects
    //   2. per-theater <theater.dir>/objects
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

        // Match name from the corresponding data table
        using namespace f4::world_convert;
        if (entry.data_type == DTYPE_VEHICLE) {
            const auto* vcd = theater_db_.vehicles.at(entry.data_ptr_index);
            if (vcd && !vcd->name.empty()) {
                std::string name_lower(vcd->name);
                std::transform(name_lower.begin(), name_lower.end(),
                               name_lower.begin(), ::tolower);
                if (name_lower.find(search_lower) != std::string::npos) return true;
            }
        }
        if (entry.data_type == DTYPE_OBJECTIVE) {
            const auto* ocd = theater_db_.objectives.at(entry.data_ptr_index);
            if (ocd && !ocd->name.empty()) {
                std::string name_lower(ocd->name);
                std::transform(name_lower.begin(), name_lower.end(),
                               name_lower.begin(), ::tolower);
                if (name_lower.find(search_lower) != std::string::npos) return true;
            }
        }
        if (entry.data_type == DTYPE_UNIT) {
            const auto* ucd = theater_db_.units.at(entry.data_ptr_index);
            if (ucd && !ucd->name.empty()) {
                std::string name_lower(ucd->name);
                std::transform(name_lower.begin(), name_lower.end(),
                               name_lower.begin(), ::tolower);
                if (name_lower.find(search_lower) != std::string::npos) return true;
            }
        }
        if (entry.data_type == DTYPE_FEATURE) {
            const auto* fcd = theater_db_.features.at(entry.data_ptr_index);
            if (fcd && !fcd->name.empty()) {
                std::string name_lower(fcd->name);
                std::transform(name_lower.begin(), name_lower.end(),
                               name_lower.begin(), ::tolower);
                if (name_lower.find(search_lower) != std::string::npos) return true;
            }
        }
        if (entry.data_type == DTYPE_WEAPON) {
            const auto* wcd = theater_db_.weapons.at(entry.data_ptr_index);
            if (wcd && !wcd->name.empty()) {
                std::string name_lower(wcd->name);
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
        ImGui::TextDisabled("No data loaded. Set an install path and re-open.");
    }

    ImGui::End();
}

void ClassTableBrowser::draw_toolbar() {
    if (data_loaded_) {
        ImGui::Text("FALCON4.ct: %zu entries", class_table_.size());
        if (theater_db_.loaded()) {
            ImGui::SameLine();
            ImGui::TextDisabled("  Theater: OCD=%zu UCD=%zu VCD=%zu FCD=%zu WCD=%zu SSD=%zu",
                                theater_db_.objectives.size(),
                                theater_db_.units.size(),
                                theater_db_.vehicles.size(),
                                theater_db_.features.size(),
                                theater_db_.weapons.size(),
                                theater_db_.squad_stores.size());
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

bool ClassTableBrowser::ensure_lit_shader() {
    // Lazy-allocate the PreviewCache on first GPU use. We can't allocate
    // it in the constructor because the GL context doesn't exist yet
    // (ViewerApp constructs ClassTableBrowser before run() → InitWindow).
    if (!preview_cache_) preview_cache_ = std::make_unique<PreviewCache>();
    // Use f4::renderer::LitShader instead of manual shader compilation.
    return preview_cache_->lit_shader_ensure.ensure();
}

bool ClassTableBrowser::ensure_default_material() {
    if (!preview_cache_) preview_cache_ = std::make_unique<PreviewCache>();
    if (preview_cache_->default_mat_built) {
        return preview_cache_->default_mat.maps != nullptr;
    }

    // 1) 1x1 opaque-white fallback texture. Required so the lit shader's
    //    `if (tex.a < 0.5) discard;` doesn't kill every fragment of every
    //    untextured mesh (tex_id < 0). Without this, untextured meshes
    //    sample undefined data from the default sampler, which on most
    //    drivers returns (0,0,0,0) → fully transparent → discarded.
    if (preview_cache_->fallback_white_tex.id == 0) {
        Image img = {};
        img.data = RL_MALLOC(4);
        if (!img.data) return false;
        unsigned char* px = static_cast<unsigned char*>(img.data);
        px[0] = 255; px[1] = 255; px[2] = 255; px[3] = 255;
        img.width = 1;
        img.height = 1;
        img.mipmaps = 1;
        img.format = PIXELFORMAT_UNCOMPRESSED_R8G8B8A8;
        preview_cache_->fallback_white_tex = LoadTextureFromImage(img);
        UnloadImage(img);
        if (preview_cache_->fallback_white_tex.id == 0) return false;
    }

    // 2) Default material with white texture on diffuse map + lit shader.
    preview_cache_->default_mat = LoadMaterialDefault();
    preview_cache_->default_mat.maps[MATERIAL_MAP_DIFFUSE].texture =
        preview_cache_->fallback_white_tex;
    preview_cache_->default_mat.maps[MATERIAL_MAP_DIFFUSE].color = WHITE;
    if (preview_cache_->lit_shader_ensure.is_loaded()) {
        preview_cache_->default_mat.shader = preview_cache_->lit_shader_ensure.shader();
    }
    preview_cache_->default_mat_built = true;
    return true;
}

void ClassTableBrowser::build_preview_meshes(int16_t vis_type_idx) {
    if (vis_type_idx <= 0) return;
    if (!preview_cache_) preview_cache_ = std::make_unique<PreviewCache>();

    auto it = preview_cache_->mesh_cache.find(vis_type_idx);
    if (it != preview_cache_->mesh_cache.end() && it->second.built) return;

    if (!model_db_) {
        if (it != preview_cache_->mesh_cache.end()) it->second.built = true;
        else preview_cache_->mesh_cache[vis_type_idx].built = true;
        return;
    }

    const auto* rec = model_db_->model(vis_type_idx);
    if (!rec || rec->lods.empty()) {
        if (it != preview_cache_->mesh_cache.end()) it->second.built = true;
        else preview_cache_->mesh_cache[vis_type_idx].built = true;
        return;
    }

    // Lock to LOD 0 (highest detail) for the preview.
    const int lod = 0;
    auto err = model_db_->parse_lod(vis_type_idx, lod);
    if (!err.empty()) {
        if (it != preview_cache_->mesh_cache.end()) it->second.built = true;
        else preview_cache_->mesh_cache[vis_type_idx].built = true;
        return;
    }

    f4::models::ModelState default_state;
    default_state.texture_set = 0;
    default_state.n_texture_sets = std::max(1, static_cast<int>(rec->n_texture_sets));

    auto geom = model_db_->extract_model_geometry(vis_type_idx, lod, default_state);
    if (geom.meshes.empty()) {
        preview_cache_->mesh_cache[vis_type_idx].built = true;
        return;
    }

    const auto& cb = model_db_->color_bank();

    // Use f4::renderer to build Raylib meshes + mesh entries.
    auto raylib_meshes = f4::renderer::build_raylib_meshes(
        geom, cb, f4::renderer::model_vertex_to_raylib);
    // Filter to triangles only (skip lines/points for preview).
    std::vector<::Mesh> tri_meshes;
    std::vector<int> tri_tex_ids;
    for (std::size_t i = 0; i < geom.meshes.size(); ++i) {
        if (geom.meshes[i].kind == f4::models::PrimitiveKind::Triangles &&
            !geom.meshes[i].triangles.empty() && !geom.meshes[i].vertices.empty()) {
            tri_meshes.push_back(std::move(raylib_meshes[i]));
            tri_tex_ids.push_back(geom.meshes[i].tex_id);
        }
    }

    PreviewCache::MeshCacheEntry entry;
    entry.meshes.reserve(tri_meshes.size());
    std::size_t total_tris = 0;

    for (std::size_t i = 0; i < tri_meshes.size(); ++i) {
        PreviewCache::MeshEntry me;
        me.mesh = tri_meshes[i];
        me.tex_id = tri_tex_ids[i];
        entry.meshes.push_back(std::move(me));
        total_tris += static_cast<std::size_t>(tri_meshes[i].triangleCount);
    }

    entry.built = true;
    entry.valid = !entry.meshes.empty();
    entry.tri_count = total_tris;
    preview_cache_->mesh_cache[vis_type_idx] = std::move(entry);

    // Upload textures via f4::renderer::TextureCache.
    std::vector<int> tex_ids;
    for (const auto& me : entry.meshes) {
        if (me.tex_id >= 0) tex_ids.push_back(me.tex_id);
    }
    if (!tex_ids.empty() && model_db_) {
        preview_cache_->texture_cache.upload(*model_db_, tex_ids);
    }
}

// upload_preview_textures() is no longer needed — textures are uploaded
// via f4::renderer::TextureCache in build_preview_meshes().

void ClassTableBrowser::fit_camera_to_model(int16_t vis_type_idx) {
    if (!model_db_ || vis_type_idx <= 0) return;
    const auto* rec = model_db_->model(vis_type_idx);
    if (!rec) return;

    // Bbox center is in LH Y-up model space; convert to Raylib RH Y-up.
    const auto center_f3 = f4::renderer::model_vertex_to_raylib(
        rec->bbox.center_x(), rec->bbox.center_y(), rec->bbox.center_z());
    const Vector3 center = {center_f3.x, center_f3.y, center_f3.z};
    cam_target_x_ = center.x;
    cam_target_y_ = center.y;
    cam_target_z_ = center.z;

    // Distance = radius * 2.5 so the model fills a reasonable portion of
    // the view at fovy=45. Degenerate (radius=0) models get a sane default.
    cam_distance_ = rec->radius * 2.5f;
    if (cam_distance_ < 1.0f) cam_distance_ = 50.0f;

    // Reset orbit angles so the user sees the model head-on.
    // Values match the f4-models-viewer's defaults (cam_yaw=45°,
    // cam_pitch=30°) — see f4-models-viewer/src/viewer_state.hpp:61-62.
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

    if (!model_db_) {
        ImGui::TextDisabled("Model database not loaded (set install path).");
        last_preview_status_ = "no model_db";
        return;
    }
    if (vis_type_idx <= 0) {
        ImGui::TextDisabled("No vis_type selected.");
        last_preview_status_ = "no vis_type";
        return;
    }

    const auto* model = model_db_->model(vis_type_idx);
    if (!model) {
        ImGui::TextDisabled("Model[%d] not found in database", vis_type_idx);
        last_preview_status_ = "model not found";
        return;
    }

    // Refit camera when the user picks a different visType.
    if (last_previewed_vis_type_ != vis_type_idx) {
        fit_camera_to_model(vis_type_idx);
        last_previewed_vis_type_ = vis_type_idx;
    }

    // Ensure the RenderTexture2D exists BEFORE building any GPU resources,
    // because ensure_preview_target() may call cleanup_preview() (if the
    // target size changed) which would clear the mesh/shader/material
    // caches. By doing this first, we guarantee that any cleanup happens
    // before we build, not after.
    const int preview_w = 512;
    const int preview_h = 512;
    ensure_preview_target(preview_w, preview_h);

    // Lazy GPU setup (shader, default material, mesh upload, textures).
    ensure_lit_shader();
    ensure_default_material();
    build_preview_meshes(vis_type_idx);

    const auto cache_it = preview_cache_->mesh_cache.find(vis_type_idx);
    if (cache_it == preview_cache_->mesh_cache.end() || !cache_it->second.valid) {
        ImGui::TextDisabled("Model[%d] has no renderable geometry", vis_type_idx);
        last_preview_status_ = "no geometry";
        return;
    }

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

    BeginTextureMode(rt);
        ClearBackground({ 30, 30, 38, 255 });
        BeginMode3D(camera);
            // Bounding sphere hint (wireframe) so the user can see the
            // model's extent even if the mesh itself is sparse.
            //DrawSphereWires(target, model->radius, 12, 12, { 80, 80, 100, 255 });

            // Push shader uniforms via f4::renderer::LitShader.
            if (preview_cache_->lit_shader_ensure.is_loaded()) {
                preview_cache_->lit_shader_ensure.set_lighting(
                    { 0.65f, -1.0f, 0.35f },     // light_dir
                    { 255, 250, 235, 255 },         // light_color
                    1.0f,                            // intensity
                    { 80, 80, 90, 255 });            // ambient
            }

            // Draw each cached mesh with its material (textured or default).
            // We use the full ::Mesh and ::Material structs from the cache
            // (not reconstructed-from-id stand-ins) because DrawMesh reads
            // vaoId/vboId from the Mesh and maps/shader from the Material.
            //
            // Draw opaque meshes first, then alpha ones. FreeFalcon's .TEX
            // chroma-key textures have alpha=0 on transparent pixels; the
            // lit shader's `discard` handles them, but drawing opaque first
            // is a belt-and-suspenders safety net for any unlit fallback.
            std::vector<std::size_t> opaque_order, alpha_order;
            opaque_order.reserve(cache_it->second.meshes.size());
            alpha_order.reserve(cache_it->second.meshes.size());
            for (std::size_t i = 0; i < cache_it->second.meshes.size(); ++i) {
                const auto& me = cache_it->second.meshes[i];
                bool has_alpha = false;
                if (me.tex_id >= 0) {
                    auto* ce = preview_cache_->texture_cache.lookup(me.tex_id);
                    if (ce && ce->uploaded) {
                        has_alpha = ce->has_alpha;
                    }
                }
                if (has_alpha) alpha_order.push_back(i);
                else           opaque_order.push_back(i);
            }

            auto draw_one = [&](std::size_t idx) {
                const auto& me = cache_it->second.meshes[idx];
                if (me.mesh.vaoId == 0 && me.mesh.vboId[0] == 0) return;
                if (me.mesh.triangleCount <= 0) return;

                const Material* matToUse = &preview_cache_->default_mat;
                if (me.tex_id >= 0) {
                    auto* ce = preview_cache_->texture_cache.lookup(me.tex_id);
                    if (ce && ce->uploaded) {
                        matToUse = &ce->material;
                    }
                    // else: texture not yet uploaded — fall through to
                    // default material (mesh renders with vertex colors).
                }
                DrawMesh(me.mesh, *matToUse, MatrixIdentity());
            };

            BeginBlendMode(BLEND_ALPHA);
            std::size_t drawn = 0;
            for (auto idx : opaque_order) { draw_one(idx); ++drawn; }
            for (auto idx : alpha_order)  { draw_one(idx); ++drawn; }
            EndBlendMode();

            last_preview_drew_meshes_ = (drawn > 0);
        EndMode3D();
    EndTextureMode();

    // Display the rendered texture via rlImGui.
    //
    // CRITICAL: rlImGuiImageSize (rlImGui.cpp:517) casts the Texture*
    // POINTER to ImTextureID — NOT texture->id. The ImGui render callback
    // (rlImGui.cpp:177) later casts ImTextureID back to Texture* and reads
    // ->id to bind the GL texture. This means the Texture struct MUST
    // outlive the call to rlImGuiImageSize — it must still be valid when
    // ImGui::Render() runs at the end of the frame.
    //
    // A stack temporary (as the previous code used) gets destroyed when
    // draw_model_preview() returns, leaving a dangling pointer. ImGui then
    // dereferences the dangling pointer, reads garbage for texture->id
    // (likely 0), and renders a solid black rectangle — which is exactly
    // the symptom the user reported (changing ClearBackground to red had
    // no effect; the rect stayed solid black).
    //
    // Fix: store the Texture2D in the persistent PreviewCache and pass a
    // pointer to that. The PreviewCache lives for the lifetime of the
    // browser panel, so the pointer is valid when ImGui renders.
    if (!preview_cache_) preview_cache_ = std::make_unique<PreviewCache>();
    preview_cache_->preview_display_tex = { preview_tex_id_,
                                              preview_rt_w_, preview_rt_h_, 1,
                                              PIXELFORMAT_UNCOMPRESSED_R8G8B8A8 };
    rlImGuiImageSize(&preview_cache_->preview_display_tex, preview_w, preview_h);

    // Status line below the preview.
    {
        std::size_t n_meshes = cache_it->second.meshes.size();
        std::size_t n_tris   = cache_it->second.tri_count;
        int n_textured = 0;
        for (const auto& me : cache_it->second.meshes) if (me.tex_id >= 0) ++n_textured;
        ImGui::TextDisabled("%zu meshes | %zu tris | %d textured | r=%.1f | LODs=%d",
                             n_meshes, n_tris, n_textured,
                             model->radius, model->n_lods);
    }

    // Orbit camera controls via drag on the image.
    if (ImGui::IsItemActive()) {
        ImVec2 delta = ImGui::GetIO().MouseDelta;
        cam_azimuth_ += delta.x * 0.01f;
        cam_elevation_ -= delta.y * 0.01f;
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
        const auto* model = model_db_ ? model_db_->model(active_vis) : nullptr;
        if (model) {
            ImGui::Text("Model[%d]: r=%.1f  LODs=%d  slots=%d  class=%s",
                        active_vis,
                        model->radius,
                        model->n_lods,
                        model->n_slots,
                        std::string(model->visual_class()).c_str());
        } else if (model_db_) {
            ImGui::TextDisabled("Model[%d] not found in database", active_vis);
        } else {
            ImGui::TextDisabled("Model database not loaded");
        }
        // Always call draw_model_preview — it handles the "no model_db"
        // and "model not found" cases gracefully with a status message,
        // and it keeps the preview pane at a stable size so the layout
        // doesn't jump when the user switches entries.
        draw_model_preview(active_vis);
        ImGui::EndGroup();
        ImGui::SameLine();
    }

    // Right column: data table detail
    ImGui::BeginGroup();

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
                            // Try to resolve weapon name from WCD
                            const auto* wcd = theater_db_.weapons.at(
                                static_cast<std::size_t>(vcd->weapon[i]));
                            if (wcd && !wcd->name.empty()) {
                                ImGui::Text("  HP[%2d]: %s  qty=%d",
                                            i, wcd->name.c_str(), vcd->weapons[i]);
                            } else {
                                ImGui::Text("  HP[%2d]: weapon=%d  qty=%d",
                                            i, vcd->weapon[i], vcd->weapons[i]);
                            }
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
                ImGui::Text("Special Index: %d", ucd->special_index);
                if (ImGui::TreeNode("Vehicle Groups (16 slots)")) {
                    for (int i = 0; i < 16; ++i) {
                        if (ucd->num_elements[i] > 0) {
                            // vehicle_type in UCD is a 0-based class table
                            // entries[] index, NOT an entity_type. Convert to
                            // entity_type by adding VU_LAST_ENTITY_TYPE (100)
                            // before resolving through the class table.
                            const int16_t vt = ucd->vehicle_type[i];
                            const uint16_t vt_et = static_cast<uint16_t>(
                                vt + f4::world_convert::VU_LAST_ENTITY_TYPE);
                            const char* vname = nullptr;
                            if (class_table_.loaded() && theater_db_.vehicles.loaded()) {
                                uint8_t vcd_dt = 0;
                                uint32_t vcd_idx = 0;
                                if (class_table_.data_ptr_for(
                                        vt_et, vcd_dt, vcd_idx)
                                    && vcd_dt == f4::world_convert::DTYPE_VEHICLE) {
                                    const auto* vcd = theater_db_.vehicles.at(
                                        static_cast<std::size_t>(vcd_idx));
                                    if (vcd) vname = vcd->name.c_str();
                                }
                            }
                            if (vname) {
                                ImGui::Text("  Group[%2d]: %d vehicles of type %d (%s)",
                                            i, ucd->num_elements[i], vt_et, vname);
                            } else {
                                ImGui::Text("  Group[%2d]: %d vehicles of type %d",
                                            i, ucd->num_elements[i], vt_et);
                            }
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
                // Show squadron stores if special_index is valid
                if (ucd->special_index >= 0 && theater_db_.squad_stores.loaded()) {
                    const auto* ssd = theater_db_.squad_stores.at(
                        static_cast<std::size_t>(ucd->special_index));
                    if (ssd) {
                        if (ImGui::TreeNode("Squadron Stores (SSD)")) {
                            ImGui::Text("Infinite AG:  %d", ssd->infinite_ag);
                            ImGui::Text("Infinite AA:  %d", ssd->infinite_aa);
                            ImGui::Text("Infinite Gun: %d", ssd->infinite_gun);
                            int non_zero = 0;
                            for (int w = 0; w < f4::world_convert::TD_MAXIMUM_WEAPTYPES; ++w) {
                                if (ssd->stores[w] > 0) ++non_zero;
                            }
                            ImGui::Text("Weapons in stock: %d types", non_zero);
                            if (ImGui::TreeNode("Stock detail")) {
                                for (int w = 0; w < f4::world_convert::TD_MAXIMUM_WEAPTYPES; ++w) {
                                    if (ssd->stores[w] > 0) {
                                        const auto* wcd = theater_db_.weapons.at(
                                            static_cast<std::size_t>(w));
                                        if (wcd && !wcd->name.empty()) {
                                            ImGui::Text("  [%3d] %s: %d",
                                                        w, wcd->name.c_str(), ssd->stores[w]);
                                        } else {
                                            ImGui::Text("  [%3d] weapon %d: %d",
                                                        w, w, ssd->stores[w]);
                                        }
                                    }
                                }
                                ImGui::TreePop();
                            }
                            ImGui::TreePop();
                        }
                    }
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

    // WCD detail.
    if (entry->data_type == f4::world_convert::DTYPE_WEAPON) {
        const auto* wcd = theater_db_.weapons.at(entry->data_ptr_index);
        if (wcd) {
            if (ImGui::TreeNode("WeaponClassData (WCD)")) {
                ImGui::Text("Name:          %s", wcd->name.c_str());
                ImGui::Text("Strength:      %u", wcd->strength);
                ImGui::Text("Damage Type:   %s (%d)",
                            damage_type_name(wcd->damage_type), wcd->damage_type);
                ImGui::Text("Range:         %d km", wcd->range_km);
                ImGui::Text("Weight:        %u lbs", wcd->weight);
                ImGui::Text("Blast Radius:  %u ft", wcd->blast_radius);
                ImGui::Text("Fire Rate:     %d", wcd->fire_rate);
                ImGui::Text("Rarity:        %d%%", wcd->rarity);
                ImGui::Text("Guidance:      0x%04X", wcd->guidance_flags);
                ImGui::Text("Max Alt:       %d kft", static_cast<int>(wcd->max_alt));
                ImGui::Text("Radar Type:    %d", wcd->radar_type);
                ImGui::Text("SimWeap Index: %d", wcd->simweap_index);
                ImGui::Text("SimData Index: %d", wcd->sim_data_idx);
                ImGui::Text("Flags:         0x%04X", wcd->flags);
                if (ImGui::TreeNode("Hit Chance (per movement type)")) {
                    for (std::size_t i = 0; i < wcd->hit_chance.size(); ++i) {
                        if (wcd->hit_chance[i] != 0) {
                            ImGui::Text("  %s: %d%%",
                                        movement_type_name(static_cast<int32_t>(i)),
                                        wcd->hit_chance[i]);
                        }
                    }
                    ImGui::TreePop();
                }
                ImGui::TreePop();
            }
        } else {
            ImGui::TextDisabled("WCD index %u out of range (table has %zu entries)",
                                entry->data_ptr_index, theater_db_.weapons.size());
        }
    }

    // SSD detail (rare — usually accessed via UCD::special_index,
    // but the class table can point directly too).
    if (entry->data_type == f4::world_convert::DTYPE_SQUAD_STORES) {
        const auto* ssd = theater_db_.squad_stores.at(entry->data_ptr_index);
        if (ssd) {
            if (ImGui::TreeNode("SquadronStoresData (SSD)")) {
                ImGui::Text("Infinite AG:  %d", ssd->infinite_ag);
                ImGui::Text("Infinite AA:  %d", ssd->infinite_aa);
                ImGui::Text("Infinite Gun: %d", ssd->infinite_gun);
                int non_zero = 0;
                for (int w = 0; w < f4::world_convert::TD_MAXIMUM_WEAPTYPES; ++w) {
                    if (ssd->stores[w] > 0) ++non_zero;
                }
                ImGui::Text("Weapons in stock: %d types", non_zero);
                if (ImGui::TreeNode("Stock detail")) {
                    for (int w = 0; w < f4::world_convert::TD_MAXIMUM_WEAPTYPES; ++w) {
                        if (ssd->stores[w] > 0) {
                            const auto* wcd = theater_db_.weapons.at(
                                static_cast<std::size_t>(w));
                            if (wcd && !wcd->name.empty()) {
                                ImGui::Text("  [%3d] %s: %d",
                                            w, wcd->name.c_str(), ssd->stores[w]);
                            } else {
                                ImGui::Text("  [%3d] weapon %d: %d",
                                            w, w, ssd->stores[w]);
                            }
                        }
                    }
                    ImGui::TreePop();
                }
                ImGui::TreePop();
            }
        } else {
            ImGui::TextDisabled("SSD index %u out of range (table has %zu entries)",
                                entry->data_ptr_index, theater_db_.squad_stores.size());
        }
    }

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
      << "vis_type_3,vis_type_4,vis_type_5,vis_type_6,data_type,data_ptr_index,"
      << "table_name,vcd_name,vcd_hitpoints,vcd_max_speed,vcd_max_wt,"
      << "ucd_name,ucd_movement_type,ucd_movement_speed,ucd_max_range,"
      << "ocd_name,ocd_features,"
      << "fcd_name,fcd_hit_points,fcd_repair_time,"
      << "wcd_name,wcd_strength,wcd_damage_type,wcd_range_km,wcd_weight\n";

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

        using namespace f4::world_convert;

        // VCD
        if (entry->data_type == DTYPE_VEHICLE) {
            const auto* vcd = theater_db_.vehicles.at(entry->data_ptr_index);
            if (vcd) {
                f << vcd->name << "," << vcd->hit_points << ","
                  << vcd->max_speed << "," << vcd->max_wt << ",";
            } else { f << ",,,,"; }
        } else { f << ",,,,"; }

        // UCD
        if (entry->data_type == DTYPE_UNIT) {
            const auto* ucd = theater_db_.units.at(entry->data_ptr_index);
            if (ucd) {
                f << ucd->name << "," << ucd->movement_type << ","
                  << ucd->movement_speed << "," << ucd->max_range << ",";
            } else { f << ",,,,"; }
        } else { f << ",,,,"; }

        // OCD
        if (entry->data_type == DTYPE_OBJECTIVE) {
            const auto* ocd = theater_db_.objectives.at(entry->data_ptr_index);
            if (ocd) {
                f << ocd->name << "," << static_cast<int>(ocd->features) << ",";
            } else { f << ",,"; }
        } else { f << ",,"; }

        // FCD
        if (entry->data_type == DTYPE_FEATURE) {
            const auto* fcd = theater_db_.features.at(entry->data_ptr_index);
            if (fcd) {
                f << fcd->name << "," << fcd->hit_points << ","
                  << fcd->repair_time << ",";
            } else { f << ",,,"; }
        } else { f << ",,,"; }

        // WCD
        if (entry->data_type == DTYPE_WEAPON) {
            const auto* wcd = theater_db_.weapons.at(entry->data_ptr_index);
            if (wcd) {
                f << wcd->name << "," << wcd->strength << ","
                  << wcd->damage_type << "," << wcd->range_km << ","
                  << wcd->weight << ",";
            } else { f << ",,,,,"; }
        } else { f << ",,,,,"; }

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

        using namespace f4::world_convert;

        if (entry->data_type == DTYPE_VEHICLE) {
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

        if (entry->data_type == DTYPE_UNIT) {
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

        if (entry->data_type == DTYPE_OBJECTIVE) {
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

        if (entry->data_type == DTYPE_FEATURE) {
            const auto* fcd2 = theater_db_.features.at(entry->data_ptr_index);
            if (fcd2) {
                f << ",\n    \"feature\": {\n";
                f << "      \"name\": \"" << fcd2->name << "\",\n";
                f << "      \"hit_points\": " << fcd2->hit_points << ",\n";
                f << "      \"repair_time\": " << fcd2->repair_time << ",\n";
                f << "      \"radar_type\": " << fcd2->radar_type << "\n";
                f << "    }";
            }
        }

        if (entry->data_type == DTYPE_WEAPON) {
            const auto* wcd = theater_db_.weapons.at(entry->data_ptr_index);
            if (wcd) {
                f << ",\n    \"weapon\": {\n";
                f << "      \"name\": \"" << wcd->name << "\",\n";
                f << "      \"strength\": " << wcd->strength << ",\n";
                f << "      \"damage_type\": " << wcd->damage_type
                  << ", \"damage_type_name\": \"" << damage_type_name(wcd->damage_type) << "\",\n";
                f << "      \"range_km\": " << wcd->range_km << ",\n";
                f << "      \"weight\": " << wcd->weight << ",\n";
                f << "      \"blast_radius\": " << wcd->blast_radius << ",\n";
                f << "      \"fire_rate\": " << static_cast<int>(wcd->fire_rate) << ",\n";
                f << "      \"rarity\": " << static_cast<int>(wcd->rarity) << ",\n";
                f << "      \"guidance_flags\": " << wcd->guidance_flags << "\n";
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
