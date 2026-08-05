// f4-world-viewer/src/model_snapshot.hpp
//
// PRIVATE HEADER — internal to the f4-world-viewer library. Declares
// the HDR/LOD parsing functions used by the --list-models and
// --parse-model CLI flags.
//
// These functions parse the KoreaObj.HDR and KoreaObj.LOD files from
// a Falcon 4.0 installation to extract 3D model metadata. The output
// is JSON, suitable for the model viewer development workflow.

#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace f4::viewer {

/// Build a JSON string listing all models from KoreaObj.HDR.
/// Returns the JSON text on success, or an empty string on failure
/// (with err_out set if non-null).
[[nodiscard]] std::string build_model_list_json(
    const std::filesystem::path& hdr_path,
    const std::filesystem::path& lod_path,
    std::string* err_out = nullptr);

/// Build a JSON string for a specific parent model from
/// KoreaObj.HDR + KoreaObj.LOD. Reads the HDR to get the parent's
/// LOD entries, then reads each LOD record from the LOD file.
/// Returns the JSON text on success, or empty string on failure.
[[nodiscard]] std::string build_model_json(
    const std::filesystem::path& hdr_path,
    const std::filesystem::path& lod_path,
    int parent_index,
    std::string* err_out = nullptr);

/// Find KoreaObj.HDR and KoreaObj.LOD in the given directory.
/// Searches for common case variants and also probes terrdata/objects/.
/// Returns {hdr_path, lod_path}; either may be empty if not found.
[[nodiscard]] std::pair<std::filesystem::path, std::filesystem::path>
find_koreaobj_files(const std::filesystem::path& install_root);

} // namespace f4::viewer
