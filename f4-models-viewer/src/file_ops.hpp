// f4-models-viewer/src/file_ops.hpp
//
// File operations — loading model database and finding KoreaObj files.

#pragma once

#include <filesystem>
#include <utility>

namespace f4::models_viewer {

// Functions are defined as ViewerApp::Impl member functions in file_ops.cpp.
// Declared in viewer_state.hpp:
//   void Impl::load_model_files(hdr_path, lod_path);
//   void Impl::load_from_install();

/// Find KoreaObj.HDR and KoreaObj.LOD in common locations within an
/// install root. Returns (hdr_path, lod_path); both empty if not found.
std::pair<std::filesystem::path, std::filesystem::path>
find_koreaobj_files(const std::filesystem::path& install_root);

} // namespace f4::models_viewer
