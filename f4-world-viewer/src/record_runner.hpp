// f4-world-viewer/src/record_runner.hpp
//
// PRIVATE HEADER — internal to the f4-world-viewer library.
//
// MISSION-QC-RECORD: the Mission QC window's Record button runs the
// sibling campaign_qc recorder headlessly. The spawn lives in its own
// translation unit because it needs <windows.h> — and windows.h cannot
// coexist with raylib.h in one TU on MSVC (raylib's CloseWindow /
// ShowCursor collide with winuser.h's extern "C" declarations, C2733).
// Everything here is raylib-free on purpose; see mission_qc_view.cpp.

#pragma once

#include <filesystem>
#include <string>

namespace f4::viewer {

/// Run one recorder invocation to completion (blocking — call it from a
/// worker thread) with no console-window flash:
///   "<tool>" --scenario "<scenario>" --out-dir "<out_dir>"
/// Returns the process exit code (the recorder's QC gate), or -1 when
/// the process could not be started (with a Win32 error note appended
/// to *diag when diag is non-null). The child's stdout/stderr are
/// captured to <out_dir>.log (next to the out-dir) when that file can
/// be opened — the recorder's gate messages land there even though the
/// parent is a windowless GUI process.
int run_recorder(const std::filesystem::path& tool,
                 const std::filesystem::path& scenario,
                 const std::filesystem::path& out_dir,
                 std::string* diag = nullptr);

} // namespace f4::viewer
