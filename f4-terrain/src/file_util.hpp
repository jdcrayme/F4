// f4-terrain/src/file_util.hpp
//
// Internal helpers shared by the theater data decoders (post_level,
// far_tile_db, near_tile_db). Not part of the public interface.
//
// Stage 2 (ASSET_PIPELINE_SPEC.md §12): the case-insensitive file finder
// was duplicated across f4-terrain, f4-world-convert, f4-models, and
// f4-install. The canonical implementation now lives in
// f4/install/file_finder.hpp; this header is a thin forwarder so the
// existing f4-terrain call sites (post_level.cpp, far_tile_db.cpp,
// near_tile_db.cpp) keep working without churn. New f4-terrain code
// should include <f4/install/file_finder.hpp> directly.

#pragma once

#include <f4/install/file_finder.hpp>

#include <filesystem>
#include <string>
#include <string_view>

namespace f4::terrain::detail {

/// Case-insensitive file lookup inside `dir`. Delegates to the
/// canonical implementation in f4-install (Stage 2 consolidation).
inline std::filesystem::path find_file_ci(const std::filesystem::path& dir,
                                          std::string_view name) {
    return f4::install::find_file_ci(dir, name);
}

} // namespace f4::terrain::detail
