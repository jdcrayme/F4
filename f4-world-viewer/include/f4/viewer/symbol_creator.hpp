// f4-world-viewer/include/f4/viewer/symbol_creator.hpp
//
// The Symbol Creator panel — an interactive editor for the data-driven
// symbol library (see f4/renderer/symbol_library.hpp).
//
// Layout:
//   ┌────────────────────────────────────────────────────────────────────┐
//   │ [New] [Duplicate] [Delete] | [Load Library...] [Save Library]      │
//   ├──────────────────┬──────────────────────────┬──────────────────────┤
//   │ Library browser  │  Editing canvas          │  Primitives list     │
//   │  • example_sq    │  ┌────────────────────┐  │  Polygons:           │
//   │  • example_tri   │  │   .  .  .  .  .    │  │   [0] filled (4 pts) │
//   │  • example_dia   │  │   .  .  .  .  .    │  │   [1] outline (3 pt) │
//   │  ▶ my_symbol     │  │   .  .  •  .  .    │  │  Polylines:          │
//   │                  │  │   .  .  .  .  .    │  │   [0] width 1.5 (2p) │
//   │                  │  │   .  .  .  .  .    │  │  [+ Polyline]        │
//   │                  │  │   .  .  .  .  .    │  │  [+ Polygon (fill)]  │
//   │                  │  │   .  .  .  .  .    │  │  [+ Polygon (outln)] │
//   │                  │  └────────────────────┘  │  [Delete primitive]  │
//   ├──────────────────┴──────────────────────────┴──────────────────────┤
//   │ Symbol: my_symbol  Display: [My Symbol____]  Category: [custom___] │
//   │ Description: [A user-defined symbol.______________________________] │
//   │ Hints: click empty canvas to add point • drag point to move •     │
//   │        right-click point to delete • Shift+click to start new line │
//   └────────────────────────────────────────────────────────────────────┘
//
// All editor state (library, current selection, view pan/zoom) lives in
// this class. The ViewerApp creates one SymbolCreator and toggles its
// visibility via the Tools menu. The library is initialized to
// make_default_symbol_library() on first open so the user sees a few
// examples to start from.

#pragma once

#include <f4/renderer/symbol_library.hpp>

#include <filesystem>
#include <string>
#include <vector>

namespace f4::viewer {

class SymbolCreator {
public:
    SymbolCreator();

    /// Open the panel (called from Tools > Symbol Creator menu item).
    void open();
    void close();
    [[nodiscard]] bool is_open() const noexcept { return open_; }

    /// Render the panel. Call every frame inside the ImGui frame.
    void draw();

private:
    bool open_ = false;

    /// The library being edited. Initialized to make_default_symbol_library()
    /// on first open so the user has 3 trivial examples to look at.
    f4::renderer::SymbolLibrary library_;
    bool library_initialized_ = false;  // lazy init on first open()

    /// Index into library_.symbols() of the symbol currently being edited.
    /// -1 when no symbol is selected (empty library or none picked).
    int selected_symbol_idx_ = -1;

    /// Which primitive is selected for editing.
    enum class SelectedPrimKind { None, Polyline, Polygon };
    SelectedPrimKind sel_prim_kind_ = SelectedPrimKind::None;
    int sel_prim_idx_ = -1;  // index into the polylines[] or polygons[] vector

    /// Which point in the selected primitive is being dragged.
    /// -1 when no point is selected.
    int sel_point_idx_ = -1;

    /// True while the left mouse button is held down on a point.
    bool dragging_point_ = false;

    /// Editable text buffers for the symbol's metadata fields.
    /// Kept as raw char arrays so ImGui::InputText can write into them
    /// directly. Synced to/from the SymbolDefinition on selection change.
    char key_buf_[128] = {};
    char display_name_buf_[128] = {};
    char category_buf_[64] = {};
    char description_buf_[256] = {};

    /// Canvas view state. The canvas shows the [-1, +1] × [-1, +1] symbol
    /// space. The user can pan (middle-drag) and zoom (wheel) to get a
    /// closer look at small features. The canvas extent in screen pixels
    /// is computed each frame from the available content region.
    float view_pan_x_ = 0.0f;  // screen-space offset (pixels)
    float view_pan_y_ = 0.0f;
    float view_zoom_  = 1.0f;  // multiplier on the base scale

    /// Last path used by Load/Save — pre-filled in the next dialog as
    /// a convenience. Empty until the first successful load/save.
    std::filesystem::path last_library_path_;

    /// Status / error messages shown at the bottom of the panel.
    std::string status_msg_;
    std::string last_error_;

    // --- Internal helpers (defined in symbol_creator.cpp) ---

    /// Initialize library_ to the default examples if not yet done.
    /// Called from draw() when open_ && !library_initialized_.
    void ensure_library_initialized();

    /// Sync the metadata text buffers from the currently-selected symbol.
    /// Called when the selection changes.
    void sync_metadata_buffers();

    /// Sync the metadata text buffers back into the currently-selected
    /// symbol definition. Called each frame the buffers change.
    void apply_metadata_buffers();

    /// Get the currently-selected SymbolDefinition, or nullptr if none.
    f4::renderer::SymbolDefinition* current_symbol();
    const f4::renderer::SymbolDefinition* current_symbol() const;

    /// Get the currently-selected primitive as a mutable point vector +
    /// metadata. Returns nullptr points if no primitive is selected.
    struct PrimRef {
        std::vector<f4::renderer::SymbolPoint>* points = nullptr;
        f4::renderer::SymbolPolyline* polyline = nullptr;  // non-null if polyline
        f4::renderer::SymbolPolygon*  polygon  = nullptr;  // non-null if polygon
    };
    PrimRef current_primitive();

    /// Canvas geometry computed each frame from the available content
    /// region. Stored as raw floats to keep this header free of imgui.
    struct CanvasGeom {
        float top_left_x = 0.0f;  // screen-space x of canvas top-left
        float top_left_y = 0.0f;  // screen-space y of canvas top-left
        float size_px    = 0.0f;  // square canvas edge length in pixels (incl. zoom)
        float half       = 0.0f;  // size_px / 2 (cache)
    };
    CanvasGeom compute_canvas_geom() const;

    /// Draw the library browser (left column).
    void draw_library_panel(float panel_h);

    /// Draw the editing canvas (middle column).
    void draw_canvas_panel(float panel_h);

    /// Draw the primitives list (right column).
    void draw_primitives_panel(float panel_h);

    /// Draw the metadata editor + hints (bottom strip).
    void draw_metadata_panel();

    /// Open a native file picker and load a JSON library.
    void pick_load_library_dialog();

    /// Open a native save-file picker and write the library to JSON.
    void pick_save_library_dialog();
};

} // namespace f4::viewer
