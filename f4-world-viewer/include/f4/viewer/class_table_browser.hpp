// f4-world-viewer/include/f4/viewer/class_table_browser.hpp
//
// The ImGui panel that renders the Falcon4.ct class table as a browsable,
// filterable, exportable grid. Owns its own window state; the ViewerApp
// creates one ClassTableBrowser and toggles its visibility via the
// Tools menu.
//
// Layout:
//   ┌──────────────────────────────────────────────────────────────────┐
//   │ [Load] [Reload]  path/to/FALCON4.ct (2134 entries)             │
//   │ Filter: [All ▼] Domain:[All ▼] Class:[All ▼]  Search: [____]  │
//   ├──────────────────────────────────────────────────────────────────┤
//   │ ID    Domain  Class     Type  Subtype   Vis  DataTable  DataPtr │
//   │ 100   Air     Vehicle   -     Fighter   142  VCD        45     │
//   │ 101   Land    Unit      -     Armor     -    UCD        12     │
//   │ ...                                                              │
//   ├──────────────────────────────────────────────────────────────────┤
//   │ Detail: F-16C (VCD #45)                                         │
//   │   Hit Points: 150   Max Speed: 800 kph   Max Wt: 50000 lbs    │
//   │   Weapons: AIM-120x4, MK-82x2, ...                             │
//   ├──────────────────────────────────────────────────────────────────┤
//   │ [Export CSV] [Export JSON]                                       │
//   └──────────────────────────────────────────────────────────────────┘

#pragma once

#include <f4/world_convert/class_table.hpp>
#include <f4/world_convert/theater_data.hpp>
#include <f4/models/model_database.hpp>
#include <f4/install/installation.hpp>

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace f4::viewer {

class ClassTableBrowser {
public:
    ClassTableBrowser() = default;

    /// Open the panel (called from Tools > Class Table Browser menu item).
    void open() { open_ = true; }
    void close() { open_ = false; }
    [[nodiscard]] bool is_open() const noexcept { return open_; }

    /// Provide the Installation so we can lazy-load data. Called each
    /// frame from draw() if we don't have it yet. The browser does NOT
    /// take ownership.
    void set_install(const f4::install::Installation* inst) { install_ = inst; }

    /// Provide the already-loaded 3D ModelDatabase for cross-referencing
    /// vis_type -> ModelRecord metadata. May be nullptr.
    void set_model_db(const f4::models::ModelDatabase* db) { model_db_ = db; }

    /// Render the panel. Call every frame inside the ImGui frame.
    void draw();

private:
    bool open_ = false;

    // --- Data sources (set externally or lazy-loaded) ---
    const f4::install::Installation* install_ = nullptr;
    const f4::models::ModelDatabase* model_db_ = nullptr;

    // --- Lazy-loaded data (owned by this panel) ---
    f4::world_convert::ClassTable class_table_;
    f4::world_convert::TheaterObjectDatabase theater_db_;
    bool data_load_attempted_ = false;
    bool data_loaded_ = false;
    std::string load_error_;

    // --- Filter state ---
    int filter_domain_ = 0;   // 0=All, 2=Air, 3=Land, 4=Sea
    int filter_class_ = 0;    // 0=All, 4=Objective, 6=Unit, 7=Vehicle, 8=Weapon, 2=Feature
    char search_buf_[128] = {};
    bool filter_dirty_ = true;   // true when filters changed; triggers rebuild

    // --- Pre-computed filtered entries (avoids IM_ASSERT from
    //     ImGuiListClipper when skipping filtered-out rows) ---
    struct FilteredEntry {
        uint16_t entity_type;
        const f4::world_convert::ClassTableEntry* entry;
    };
    std::vector<FilteredEntry> filtered_entries_;

    // --- Selection state ---
    int selected_entity_type_ = -1;  // -1 = none

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
        const f4::world_convert::ClassTableEntry& entry) const;

    /// Export the currently-filtered entries as CSV.
    void export_csv(const std::filesystem::path& path);

    /// Export the currently-filtered entries as JSON.
    void export_json(const std::filesystem::path& path);

    /// Corrected data_type_name using the verified DataType mapping from
    /// class_table.hpp. The enum_text.hpp version uses an older unverified
    /// mapping that is off-by-one for some values.
    [[nodiscard]] static const char* ct_data_type_name(uint8_t dt) noexcept;
};

} // namespace f4::viewer
