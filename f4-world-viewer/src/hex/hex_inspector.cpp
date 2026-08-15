// f4-world-viewer/src/hex/hex_inspector.cpp

#include <f4/viewer/hex_inspector.hpp>
#include <f4/viewer/file_dialog.hpp>

#include "hex_utils.hpp"

#include <imgui.h>
#include <rlImGui.h>

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <string>

namespace f4::viewer {

namespace {

using hex::hex_byte;

/// Format an offset as an 8-digit hex address (e.g. "00001A2B").
std::string offset_hex(std::size_t off) {
    std::ostringstream ss;
    ss << std::hex << std::setfill('0') << std::setw(8) << off;
    return ss.str();
}

/// Color for an annotation category. Returns an ImVec4 suitable for
/// passing to ImGui::PushStyleColor.
ImVec4 color_for_category(const std::string& cat) {
    if (cat == "header")  return ImVec4(0.40f, 0.70f, 1.00f, 1.0f);  // blue
    if (cat == "field")   return ImVec4(0.40f, 1.00f, 0.40f, 1.0f);  // green
    if (cat == "string")  return ImVec4(1.00f, 0.80f, 0.40f, 1.0f);  // orange
    if (cat == "padding") return ImVec4(0.50f, 0.50f, 0.50f, 1.0f);  // gray
    return ImVec4(0.80f, 0.80f, 0.80f, 1.0f);  // unknown — light gray
}

/// Lookup table for the decoder dropdown.
struct DecoderEntry {
    const char* label;
    FileType type;
};
const DecoderEntry kDecoders[] = {
    {"Auto (detect)",            FileType::Unknown},
    {"Campaign Archive (.cam)",  FileType::CamArchive},
    {"Campaign Metadata (.cmp)", FileType::CmpSubfile},
    {"Theater Header (MAP)",     FileType::TheaterMap},
    {"Class Table (.ct)",        FileType::Falcon4Ct},
    {"Generic (magic+strings)",  FileType::Binary},
};
constexpr int kDecoderCount = static_cast<int>(sizeof(kDecoders) / sizeof(kDecoders[0]));

} // namespace

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

void HexInspector::load_file(const std::filesystem::path& path) {
    try {
        model_.load_file(path);
        model_.apply_decoder();
        decoder_index_ = 0;  // auto
        std::snprintf(path_buf_, sizeof(path_buf_), "%s", path.string().c_str());
        scroll_row_ = 0;
        status_msg_ = "Loaded " + path.filename().string() +
                      " (" + std::to_string(model_.size()) + " bytes, " +
                      file_type_name(model_.file_type()) + ")";
        last_error_.clear();
    } catch (const std::exception& e) {
        last_error_ = e.what();
        status_msg_.clear();
    }
}

void HexInspector::set_pending_path(const std::filesystem::path& path) {
    std::snprintf(path_buf_, sizeof(path_buf_), "%s", path.string().c_str());
}

void HexInspector::pick_file_dialog() {
    auto path = pick_open_file("Open File for Hex Inspection",
                                 "All files (*.*)|Campaign (*.cam)|Class Table (FALCON4.ct)|Theater (THEATER.*)",
                                 path_buf_[0] ? std::filesystem::path(path_buf_) : std::filesystem::path{});
    if (!path.empty()) {
        load_file(path);
    }
}

// ---------------------------------------------------------------------------
// Drawing
// ---------------------------------------------------------------------------

void HexInspector::draw() {
    if (!open_) return;

    ImGui::SetNextWindowSize(ImVec2(900, 600), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Hex Inspector", &open_,
                       ImGuiWindowFlags_NoCollapse)) {
        ImGui::End();
        return;
    }

    draw_toolbar();

    if (model_.loaded()) {
        // Splitter-style horizontal layout: annotations panel on left,
        // hex dump on right.
        const float left_width = 280.0f;
        draw_annotations_panel();
        ImGui::SameLine();
        draw_hex_dump();
        draw_selection_bar();
    } else if (!last_error_.empty()) {
        ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "Error: %s",
                            last_error_.c_str());
    } else {
        ImGui::TextDisabled("No file loaded. Click [Open File...] to pick one.");
    }

    ImGui::End();
}

void HexInspector::draw_toolbar() {
    // Row 1: Open File button + path display + file size + type badge.
    if (ImGui::Button("Open File...")) {
        pick_file_dialog();
    }
    ImGui::SameLine();
    ImGui::PushItemWidth(-300);
    if (ImGui::InputText("##path", path_buf_, sizeof(path_buf_),
                          ImGuiInputTextFlags_EnterReturnsTrue)) {
        // User pressed Enter — load the file.
        if (path_buf_[0] != '\0') {
            load_file(std::filesystem::path(path_buf_));
        }
    }
    ImGui::PopItemWidth();
    ImGui::SameLine();
    if (model_.loaded()) {
        ImGui::TextDisabled("%s  %zu bytes  %s",
                             model_.path().filename().string().c_str(),
                             model_.size(),
                             file_type_name(model_.file_type()));
    }

    // Row 2: Decoder dropdown + Re-decode button + status message.
    ImGui::SetNextItemWidth(220);
    const char* current_label = kDecoders[decoder_index_].label;
    if (ImGui::BeginCombo("##decoder", current_label)) {
        for (int i = 0; i < kDecoderCount; ++i) {
            const bool sel = (i == decoder_index_);
            if (ImGui::Selectable(kDecoders[i].label, sel)) {
                decoder_index_ = i;
                redecode_with_current();
            }
            if (sel) ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
    }
    ImGui::SameLine();
    if (ImGui::Button("Re-decode")) {
        redecode_with_current();
    }
    ImGui::SameLine();
    if (!status_msg_.empty()) {
        ImGui::TextDisabled("%s", status_msg_.c_str());
    }
    if (!last_error_.empty()) {
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "  %s",
                            last_error_.c_str());
    }
}

void HexInspector::draw_annotations_panel() {
    ImGui::BeginChild("annotations", ImVec2(280, 0), true);
    ImGui::Text("Annotations (%zu)", model_.annotations().size());
    ImGui::Separator();

    for (const auto& a : model_.annotations()) {
        // Color the label by category.
        const auto color = color_for_category(a.category);
        ImGui::PushStyleColor(ImGuiCol_Text, color);
        const std::string label = offset_hex(a.range.offset) +
                                   "+" + std::to_string(a.range.length) +
                                   "  " + a.label;
        // Make the row selectable — clicking scrolls the hex view to it.
        const bool sel = (model_.selection().offset == a.range.offset &&
                          model_.selection().length == a.range.length);
        if (ImGui::Selectable(label.c_str(), sel)) {
            model_.set_selection(a.range);
            scroll_row_ = a.range.offset / bytes_per_row_;
        }
        ImGui::PopStyleColor();

        // Indented value + description.
        ImGui::Indent(20.0f);
        ImGui::TextDisabled("%s", a.value.c_str());
        if (ImGui::IsItemHovered() && !a.description.empty()) {
            ImGui::SetTooltip("%s", a.description.c_str());
        }
        ImGui::Unindent(20.0f);
    }

    ImGui::EndChild();
}

void HexInspector::draw_hex_dump() {
    ImGui::BeginChild("hexdump", ImVec2(0, -40), true);

    if (!model_.loaded()) {
        ImGui::EndChild();
        return;
    }

    // Compute font metrics for the hex dump layout.
    const ImVec2 char_size = ImGui::CalcTextSize("00");
    const float char_w = char_size.x;
    const float char_h = char_size.y;
    const float line_h = char_h + 2.0f;

    // Layout: [8-digit offset] [16 bytes × 3 chars + spacing] [16 ASCII chars]
    const float offset_w = 8 * char_w + 8;
    const float hex_byte_w = 2 * char_w + 2;     // "XX "
    const float hex_section_w = bytes_per_row_ * hex_byte_w + 8;
    const float ascii_section_w = bytes_per_row_ * char_w + 8;

    const std::size_t total_rows = (model_.size() + bytes_per_row_ - 1) / bytes_per_row_;
    const float content_h = total_rows * line_h;

    // Use ImGui's clipper to only render visible rows (handles huge files).
    ImGuiListClipper clipper;
    clipper.Begin(static_cast<int>(total_rows), line_h);
    while (clipper.Step()) {
        for (int row_idx = clipper.DisplayStart; row_idx < clipper.DisplayEnd; ++row_idx) {
            const std::size_t row = static_cast<std::size_t>(row_idx);
            const std::size_t off = row * bytes_per_row_;
            const std::size_t row_len = std::min<std::size_t>(
                bytes_per_row_, model_.size() - off);

            // Offset column.
            ImGui::TextDisabled("%s", offset_hex(off).c_str());
            ImGui::SameLine(offset_w);

            // Hex bytes column.
            for (std::size_t i = 0; i < bytes_per_row_; ++i) {
                if (i >= row_len) {
                    // Pad with spaces for alignment.
                    ImGui::SameLine(offset_w + i * hex_byte_w);
                    ImGui::TextDisabled("   ");
                    continue;
                }
                const std::size_t byte_off = off + i;
                const uint8_t b = model_.byte_at(byte_off);

                // Color the byte if it falls within an annotation.
                const Annotation* ann = model_.annotation_at(byte_off);
                const bool in_selection = model_.selection().contains(byte_off);

                if (in_selection) {
                    ImGui::PushStyleColor(ImGuiCol_Text,
                        ImVec4(1.0f, 1.0f, 0.4f, 1.0f));  // yellow on selection
                } else if (ann) {
                    ImGui::PushStyleColor(ImGuiCol_Text,
                        color_for_category(ann->category));
                }

                ImGui::SameLine(offset_w + i * hex_byte_w);
                // Use Selectable for click+drag interaction.
                const std::string byte_label = hex_byte(b);
                char id[32];
                std::snprintf(id, sizeof(id), "##b%zx", byte_off);
                if (ImGui::Selectable(id, false, 0, ImVec2(hex_byte_w, char_h))) {
                    // Single click — start a selection.
                    model_.set_selection({byte_off, 1});
                    dragging_ = true;
                    drag_start_byte_ = byte_off;
                    drag_end_byte_ = byte_off;
                }
                // Render the byte text on top of the Selectable (the
                // Selectable's label is empty; we drew the hex into the
                // same cell with a Text call before it).
                ImGui::SameLine(offset_w + i * hex_byte_w);
                ImGui::Text("%s", byte_label.c_str());

                if (in_selection || ann) {
                    ImGui::PopStyleColor();
                }
            }

            // ASCII column.
            ImGui::SameLine(offset_w + hex_section_w);
            std::string ascii_row;
            ascii_row.reserve(bytes_per_row_);
            for (std::size_t i = 0; i < row_len; ++i) {
                const uint8_t b = model_.byte_at(off + i);
                ascii_row.push_back((b >= 0x20 && b < 0x7F) ? static_cast<char>(b) : '.');
            }
            ImGui::Text("%s", ascii_row.c_str());
        }
    }
    clipper.End();

    // Handle drag-to-select: while dragging, update the selection to
    // span from drag_start_byte_ to the byte under the cursor.
    if (dragging_) {
        if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
            dragging_ = false;
        }
        // Note: full drag-to-select across multiple bytes requires
        // hit-testing the mouse position against the byte grid. For
        // simplicity in v1, we only support single-click selection
        // (one byte at a time). Multi-byte drag selection is a future
        // enhancement — the data model already supports ByteRange
        // of any length.
    }

    ImGui::EndChild();
}

void HexInspector::draw_selection_bar() {
    const auto sel = model_.selection();
    if (sel.empty()) {
        ImGui::TextDisabled("Selection: (none — click a byte in the hex dump)");
        return;
    }

    ImGui::Text("Selection: [%zu..%zu]  %zu bytes",
                 sel.offset, sel.end() - 1, sel.length);

    ImGui::SameLine();
    if (ImGui::Button("Copy as Hex")) copy_selection_as_hex();
    ImGui::SameLine();
    if (ImGui::Button("Copy as C array")) copy_selection_as_c_array();
    ImGui::SameLine();
    if (ImGui::Button("Copy as Python")) copy_selection_as_python();
    ImGui::SameLine();
    if (ImGui::Button("Save As...")) save_selection_as();
    ImGui::SameLine();
    if (ImGui::Button("Clear")) model_.clear_selection();
}

void HexInspector::redecode_with_current() {
    if (!model_.loaded()) return;
    const FileType t = kDecoders[decoder_index_].type;
    if (t == FileType::Unknown) {
        model_.apply_decoder();  // auto
    } else {
        model_.apply_decoder(t);
    }
}

// ---------------------------------------------------------------------------
// Clipboard + file export
// ---------------------------------------------------------------------------

void HexInspector::copy_selection_as_hex() {
    const auto sel = model_.selection();
    if (sel.empty()) return;
    const auto bytes = model_.slice(sel.offset, sel.length);
    std::string s;
    s.reserve(bytes.size() * 3);
    for (size_t i = 0; i < bytes.size(); ++i) {
        if (i > 0) s += ' ';
        s += hex_byte(bytes[i]);
    }
    ImGui::SetClipboardText(s.c_str());
    status_msg_ = "Copied " + std::to_string(bytes.size()) + " bytes as hex";
}

void HexInspector::copy_selection_as_c_array() {
    const auto sel = model_.selection();
    if (sel.empty()) return;
    const auto bytes = model_.slice(sel.offset, sel.length);
    std::ostringstream ss;
    ss << "// " << bytes.size() << " bytes from " << model_.path().filename().string()
       << " @ offset " << sel.offset << "\n";
    ss << "static const unsigned char data[" << bytes.size() << "] = {\n  ";
    for (size_t i = 0; i < bytes.size(); ++i) {
        ss << "0x" << std::hex << std::setfill('0') << std::setw(2)
           << static_cast<int>(bytes[i]);
        if (i + 1 < bytes.size()) ss << ",";
        if ((i + 1) % 12 == 0) ss << "\n  ";
        else ss << " ";
    }
    ss << "\n};\n";
    ImGui::SetClipboardText(ss.str().c_str());
    status_msg_ = "Copied " + std::to_string(bytes.size()) + " bytes as C array";
}

void HexInspector::copy_selection_as_python() {
    const auto sel = model_.selection();
    if (sel.empty()) return;
    const auto bytes = model_.slice(sel.offset, sel.length);
    std::ostringstream ss;
    ss << "# " << bytes.size() << " bytes from "
       << model_.path().filename().string() << " @ offset " << sel.offset << "\n";
    ss << "data = bytes.fromhex(\"";
    for (size_t i = 0; i < bytes.size(); ++i) {
        ss << std::hex << std::setfill('0') << std::setw(2)
           << static_cast<int>(bytes[i]);
    }
    ss << "\")\n";
    ImGui::SetClipboardText(ss.str().c_str());
    status_msg_ = "Copied " + std::to_string(bytes.size()) + " bytes as Python bytes";
}

void HexInspector::save_selection_as() {
    const auto sel = model_.selection();
    if (sel.empty()) return;

    auto path = pick_save_file("Save Selection As",
                                 "Binary (*.bin)|All files (*.*)",
                                 model_.path().parent_path() / "selection.bin");
    if (path.empty()) return;

    const auto bytes = model_.slice(sel.offset, sel.length);
    std::ofstream f(path, std::ios::binary);
    if (!f) {
        last_error_ = "cannot write " + path.string();
        return;
    }
    f.write(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    if (!f) {
        last_error_ = "write failed for " + path.string();
        return;
    }
    status_msg_ = "Saved " + std::to_string(bytes.size()) + " bytes to " + path.string();
}

} // namespace f4::viewer
