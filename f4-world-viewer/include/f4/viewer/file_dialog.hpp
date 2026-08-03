// f4-world-viewer/include/f4/viewer/file_dialog.hpp
//
// Native OS file/folder picker wrappers around tinyfiledialogs.
//
// Replaces the old ImGui text-input modal we shipped originally. The new
// flow uses the OS's native picker (Win32 GetOpenFileName on Windows,
// NSOpenPanel on macOS, zenity/kdialog on Linux), which is what users
// expect from a desktop app — and crucially, supports folder picking
// (the old modal couldn't).
//
// All functions are blocking — they enter a modal loop and return the
// chosen path (or an empty path if the user cancelled). Call them from
// menu-item callbacks; they're not safe to call from inside a tight
// render loop because they stall the window for as long as the picker
// is open. The viewer's main loop tolerates this because Raylib doesn't
// time out on a single stalled frame.
//
// Filters: tinyfiledialogs uses a "|" separated string of "Description
// (*.ext1;*.ext2)|Description2 (*.ext3)" patterns. We accept the same
// format and pass it through verbatim — keeps the wrapper thin.

#pragma once

#include <filesystem>
#include <string>

namespace f4::viewer {

/// Open a native "Open File" dialog. Returns the chosen path, or an
/// empty path if the user cancelled. `filters` uses tinyfiledialogs'
/// format: "JSON files (*.json)|All files (*.*)" — empty string means
/// no filter (show all files). `default_path` may be empty.
[[nodiscard]] std::filesystem::path pick_open_file(
    const std::string& title,
    const std::string& filters = {},
    const std::filesystem::path& default_path = {});

/// Open a native "Save File" dialog. Same filter format. Returns the
/// chosen path or empty on cancel.
[[nodiscard]] std::filesystem::path pick_save_file(
    const std::string& title,
    const std::string& filters = {},
    const std::filesystem::path& default_path = {});

/// Open a native "Select Folder" dialog. The one the old ImGui modal
/// couldn't do — needed for the install-path picker. Returns the chosen
/// folder or empty on cancel.
[[nodiscard]] std::filesystem::path pick_folder(
    const std::string& title,
    const std::filesystem::path& default_path = {});

/// Show a native OK/Cancel message box. `kind` is one of:
///   "info", "warning", "error", "question"
/// Returns true for OK (or Yes on question), false for Cancel/No.
bool show_message_box(const std::string& title,
                      const std::string& message,
                      const std::string& kind = "info");

} // namespace f4::viewer
