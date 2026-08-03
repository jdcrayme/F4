// f4-world-viewer/include/f4/viewer/hex_inspector.hpp
//
// The ImGui panel that renders a HexModel. Owns its own model + window
// state; the ViewerApp creates one HexInspector and toggles its
// visibility via the Tools menu.
//
// Layout:
//   ┌──────────────────────────────────────────────────────────────┐
//   │ [Open File...] [path/to/file.cam   197340 bytes   .cam]      │
//   │ [decoder: Campaign Archive (.cam) ▼]   [Re-decode]            │
//   ├──────────────────────────────────────────────────────────────┤
//   │ Annotations panel (left, 280px)  │  Hex dump + ASCII (right)  │
//   │  • magic          0x444CFFAE     │  00000000  AE FF 4C 44 ... │
//   │  • manifest_off   197328         │  00000010  ...              │
//   │  • subfile:save1.cmp  4420 bytes │  ...                         │
//   │  ...                             │  (click+drag to select)     │
//   ├──────────────────────────────────────────────────────────────┤
//   │ Selection: [0..16]  16 bytes                                    │
//   │ [Copy as Hex] [Copy as C array] [Copy as Python] [Save As...] │
//   └──────────────────────────────────────────────────────────────┘
//
// All the byte-iteration logic is in here (the view layer); all the
// decoding logic is in decoders.cpp (the model layer). The view knows
// nothing about .cam or THEATER.MAP formats — it just renders bytes
// + annotations + handles selection.

#pragma once

#include <f4/viewer/hex_model.hpp>

#include <filesystem>
#include <string>

namespace f4::viewer {

class HexInspector {
public:
    HexInspector() = default;

    /// Open the panel (called from Tools > Hex Inspector menu item).
    void open() { open_ = true; }
    void close() { open_ = false; }
    [[nodiscard]] bool is_open() const noexcept { return open_; }

    /// Load a file into the inspector. Called from the Open File...
    /// button or from the Tools menu when a path is pre-supplied.
    /// Throws on I/O error.
    void load_file(const std::filesystem::path& path);

    /// Pre-fill the path text input with `path` (without loading).
    /// Useful when the user picks a file from the install tree browser
    /// (future feature) and wants to confirm before loading.
    void set_pending_path(const std::filesystem::path& path);

    /// Render the panel. Call every frame inside the ImGui frame.
    void draw();

private:
    bool open_ = false;
    HexModel model_;

    // Path input field state.
    char path_buf_[1024] = {};

    // View state.
    int bytes_per_row_ = 16;
    std::size_t scroll_row_ = 0;          // first visible row
    int visible_rows_ = 32;                // updated each frame from window height

    // Selection state — managed by the model but the panel drives the
    // click+drag interaction. We track the in-progress drag separately
    // from the committed selection so the user can see what they're
    // highlighting before releasing the mouse.
    bool dragging_ = false;
    std::size_t drag_start_byte_ = 0;
    std::size_t drag_end_byte_ = 0;

    // Decoder dropdown state — the current selection. When the user
    // changes this, we call model_.apply_decoder(new_type) on the next
    // draw.
    int decoder_index_ = 0;  // 0 = auto, 1..N = specific types

    // Status messages (shown at the bottom of the panel).
    std::string status_msg_;
    std::string last_error_;

    // --- Internal helpers ---

    void draw_toolbar();
    void draw_annotations_panel();
    void draw_hex_dump();
    void draw_selection_bar();
    void redecode_with_current();

    /// Open a native file picker and load the chosen file.
    void pick_file_dialog();

    /// Copy the current selection to the clipboard in the given format.
    void copy_selection_as_hex();
    void copy_selection_as_c_array();
    void copy_selection_as_python();

    /// Open a native save dialog and write the current selection to the
    /// chosen file.
    void save_selection_as();

    /// Compute the byte offset at the given ImGui mouse position within
    /// the hex dump. Returns false if the position is not over a byte.
    bool byte_at_position(float mouse_x, float mouse_y, std::size_t* out) const;
};

} // namespace f4::viewer
