// f4-world-viewer/include/f4/viewer/class_table_browser.hpp
//
// The ImGui panel that renders the runtime class table
// (Data/Classes/falcon4.ct.json — the committed JSON; the binary .ct
// decoder is no longer linked, Tranche 0d) as a browsable, filterable,
// exportable grid with 3D model preview (glTF via the shared
// RuntimeModelCache). Owns its own window state; the ViewerApp creates
// one ClassTableBrowser and toggles its visibility via the Tools menu.
//
// Layout:
//   ┌──────────────────────────────────────────────────────────────────┐
//   │ [Reload]  FALCON4.ct (2134 entries)  Theater DB: ...            │
//   │ Domain:[All ▼] Class:[All ▼]  Search: [____]  [Clear]          │
//   ├──────────────────────────────────────────────────────────────────┤
//   │ ID    Domain  Class     Type  Subtype   Vis  DataTable  DataPtr │
//   │ 100   Air     Vehicle   -     Fighter   142  VCD        45     │
//   │ 101   Land    Unit      -     Armor     -    UCD        12     │
//   ├──────────────────────────────────────────────────────────────────┤
//   │ Detail: entity_type 100 (Air / Vehicle)                         │
//   │ VisType: [0]=142 [1]=143  [Model ▼]                             │
//   │ ┌────────────────────┐  VehicleClassData (VCD)                  │
//   │ │  3D model preview  │    Name: F-16C   Hit Points: 150       │
//   │ │  (orbit/zoom)      │    Max Speed: 800 kph                   │
//   │ └────────────────────┘  Weapons: AIM-120x4, ...                │
//   ├──────────────────────────────────────────────────────────────────┤
//   │ [Export CSV] [Export JSON]                                       │
//   └──────────────────────────────────────────────────────────────────┘

#pragma once

#include <f4/world_types/class_table.hpp>

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace f4::renderer {
class RenderResources;   // fwd — the shared GPU resources (models/textures/shader)
}

namespace f4::viewer {

// Forward declaration — the full definition lives in class_table_browser.cpp
// because it contains Raylib types (::Mesh, ::Material, ::Texture2D,
// ::Shader, ::RenderTexture2D) that we don't want to leak into the public
// header (raylib is a PRIVATE dependency of f4_world_viewer per the
// CMakeLists). This is the PImpl idiom.
struct PreviewCache;

class ClassTableBrowser {
public:
    // Constructor and destructor defined out-of-line in class_table_browser.cpp
    // because the destructor (and the implicitly-generated copy-elision cleanup
    // path in the constructor) need the full PreviewCache definition, which
    // lives in the .cpp (PImpl idiom — PreviewCache contains Raylib types that
    // must not leak into this public header).
    ClassTableBrowser();
    ~ClassTableBrowser();

    // Non-copyable (owns GPU resources)
    ClassTableBrowser(const ClassTableBrowser&) = delete;
    ClassTableBrowser& operator=(const ClassTableBrowser&) = delete;

    /// Open the panel (called from Tools > Class Table Browser menu item).
    void open() { open_ = true; }
    void close() { open_ = false; cleanup_preview(); }
    [[nodiscard]] bool is_open() const noexcept { return open_; }

    /// Provide the shared RenderResources (glTF model cache + PNG texture
    /// cache + lit shader + default material). May be nullptr — the 3D
    /// preview degrades to a status message. The browser does NOT take
    /// ownership. Called each frame from the panels pass.
    void set_render_resources(f4::renderer::RenderResources* res) {
        render_resources_ = res;
    }

    /// Render the panel. Call every frame inside the ImGui frame.
    void draw();

private:
    bool open_ = false;

    // --- Data sources (set externally) ---
    f4::renderer::RenderResources* render_resources_ = nullptr;

    // --- Lazy-loaded data (owned by this panel) ---
    // Tranche 0d: the table loads from Data/Classes/falcon4.ct.json via
    // f4-world-types (the binary .ct decoder is no longer linked). The
    // joined OCD/UCD/VCD/FCD/WCD record browsing moved to the converted
    // artifacts (cam2json --theater-data / ct2json outputs).
    f4::world_types::ClassTable class_table_;
    bool data_load_attempted_ = false;
    bool data_loaded_ = false;
    std::string load_error_;

    // --- Filter state ---
    int filter_domain_ = 0;   // 0=All, 2=Air, 3=Land, 4=Sea
    int filter_class_ = 0;    // 0=All, 4=Objective, 6=Unit, 7=Vehicle, 8=Weapon, 2=Feature
    char search_buf_[128] = {};
    bool filter_dirty_ = true;

    // --- Pre-computed filtered entries ---
    struct FilteredEntry {
        uint16_t entity_type;
        const f4::world_types::ClassTableEntry* entry;
    };
    std::vector<FilteredEntry> filtered_entries_;

    // --- Selection state ---
    int selected_entity_type_ = -1;  // -1 = none
    int selected_vis_slot_ = 0;       // which visType[0..6] to preview

    // --- 3D model preview state ---
    // RenderTexture2D stored as raw GPU texture ids + dimensions
    // (can't put Raylib types in header without pulling in raylib.h).
    // UnloadRenderTexture only needs rt.id + rt.texture.id, so raw ids
    // are sufficient here (unlike Mesh/Material/Shader which need their
    // full structs for cleanup — those live in PreviewCache).
    unsigned int preview_rt_id_ = 0;       // RenderTexture2D.id
    unsigned int preview_tex_id_ = 0;      // RenderTexture2D.texture.id (color attachment)
    int preview_rt_w_ = 0;
    int preview_rt_h_ = 0;
    bool preview_rt_valid_ = false;

    // PreviewCache (PImpl) — holds all Raylib GPU resources that need
    // full structs for cleanup: the mesh cache (per vis_type), texture
    // cache (per tex_id), lit shader, default material, and fallback
    // white texture. Defined in class_table_browser.cpp.
    // Lazily allocated on first use (can't be allocated at construction
    // time because the GL context doesn't exist yet — ViewerApp creates
    // ClassTableBrowser before run() calls InitWindow).
    std::unique_ptr<PreviewCache> preview_cache_;

    // Orbit camera (stored in model-space units; cam_target is the
    // model's bbox center so the camera actually orbits the model,
    // not the world origin).
    // Defaults match f4-models-viewer (cam_yaw=45°, cam_pitch=30°) —
    // see class_table_browser.cpp's fit_camera_to_model() for why the
    // 30° pitch matters (side surfaces catch the directional light;
    // top surfaces are dark due to inward-pointing BSP normals).
    float cam_azimuth_ = 0.785398f;   // 45° in radians
    float cam_elevation_ = 0.523599f; // 30° in radians
    float cam_distance_ = 100.0f;    // distance from cam_target
    float cam_target_x_ = 0.0f;      // model bbox center (RH Y-up)
    float cam_target_y_ = 0.0f;
    float cam_target_z_ = 0.0f;
    // When the user picks a new visType, we refit the camera once.
    int last_previewed_vis_type_ = -1;
    // Diagnostic: did the last draw_model_preview() actually draw anything?
    bool last_preview_drew_meshes_ = false;
    std::string last_preview_status_;

    // --- Export state ---
    char export_path_buf_[1024] = {};
    std::string export_status_;

    // --- Internal helpers ---
    void ensure_data_loaded();
    void draw_toolbar();
    void draw_filter_bar();
    void draw_table();
    void draw_detail_panel();
    void draw_export_bar();
    void rebuild_filtered_entries();

    /// Check if an entry matches the current filters.
    [[nodiscard]] bool passes_filter(
        uint16_t entity_type,
        const f4::world_types::ClassTableEntry& entry) const;

    /// Export the currently-filtered entries as CSV.
    void export_csv(const std::filesystem::path& path);

    /// Export the currently-filtered entries as JSON.
    void export_json(const std::filesystem::path& path);

    /// Corrected data_type_name using the verified DataType mapping from
    /// class_table.hpp.
    [[nodiscard]] static const char* ct_data_type_name(uint8_t dt) noexcept;

    /// Draw the 3D model preview for a given vis_type index.
    void draw_model_preview(int16_t vis_type_idx);

    /// Ensure the RenderTexture2D for the preview exists.
    void ensure_preview_target(int w, int h);

    /// Build (or skip if cached) the glTF model for one vis_type through
    /// the shared RuntimeModelCache. Requires the GL context. No-op when
    /// render_resources_ is null or vis_type_idx <= 0.
    void build_preview_meshes(int16_t vis_type_idx);

    /// Fit the orbit camera to the model's bounding box so the model
    /// fills a reasonable portion of the preview viewport. Called once
    /// per new vis_type selection.
    void fit_camera_to_model(int16_t vis_type_idx);

    /// Clean up GPU resources on close/reload.
    void cleanup_preview();
};

} // namespace f4::viewer
