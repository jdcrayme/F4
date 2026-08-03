// f4-world-viewer/src/diagnostics.hpp
//
// PRIVATE HEADER — internal to the f4-world-viewer library. Declares the
// two free-function diagnostic-report builders used by install_flow.cpp
// (which calls them from ViewerApp::set_install_path, ::open_install_diagnostics,
// and the campaign-load error modal) and defined in diagnostics.cpp.
//
// These are free functions (not ViewerApp members) because they take only
// a const Installation& (+ optional context strings) and produce a
// std::string report. They have no Impl access, no side effects, and no
// raylib/imgui deps — pure text assembly. That makes them trivially
// testable in isolation if we ever want to add unit tests for the report
// formatting.

#pragma once

#include <f4/install/installation.hpp>

#include <string>

namespace f4::viewer {

/// Build a comprehensive diagnostic report for the given install.
/// Used by Tools > Install Diagnostics and by the --diagnostics CLI flag.
/// Walks every theater, every campaign, every FALCON4.ct search path, and
/// emits a single human-readable string with sections separated by '---'
/// banners. The text is intentionally plain (no color codes, no markup)
/// so it renders correctly in ImGui::InputTextMultiline read-only view
/// AND copies cleanly to the clipboard for sharing.
[[nodiscard]] std::string build_install_diagnostics(
    const f4::install::Installation& inst);

/// Build a detailed error report for a failed campaign load. Used by
/// the campaign-load error modal to show the user the full context
/// (theater info, .cam file info, class table info) alongside the
/// exception message — so they can diagnose the failure without
/// having to open the diagnostics panel separately.
[[nodiscard]] std::string build_campaign_load_error(
    const f4::install::Installation& inst,
    const std::string& theater_key,
    const std::string& campaign_stem,
    const std::string& exception_msg);

} // namespace f4::viewer
