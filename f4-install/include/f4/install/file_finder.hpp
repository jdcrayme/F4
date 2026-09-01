// f4-install/include/f4/install/file_finder.hpp
//
// Case-insensitive file/subdir finders — the canonical implementation
// for the project.
//
// Stage 2 of the asset pipeline (ASSET_PIPELINE_SPEC.md §12) folds the
// duplicated case-insensitive finders that lived in:
//   - f4-terrain/src/file_util.hpp        (find_file_ci)
//   - f4-world-convert/src/theater_data.cpp (find_theater_file, internal)
//   - f4-models/src/model_database.cpp    (name-variant arrays)
// into this single header. Each of those callers now delegates here.
//
// Why f4-install owns this: f4-install is already the "knows where
// files live in a Falcon install" library. Case-insensitive lookup is
// the core primitive for that job (Falcon installs ship mixed case:
// "FArtILES.PAL", "texture.zip", "THEATER.MAP" — Windows resolves
// case-insensitively but the code must also work on case-sensitive
// filesystems). Centralizing here keeps the install-layout knowledge in
// one place and lets every other library link a single, tested
// implementation.
//
// Zero external dependencies beyond the standard library. f4-install
// stays portable across Windows, macOS, Linux.

#pragma once

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace f4::install {

/// Case-insensitive file lookup inside `dir`. Tries the exact name
/// first (fast path for case-insensitive filesystems), then any entry
/// whose lowercased filename matches. Returns an empty path when not
/// found or `dir` doesn't exist.
///
/// This is the canonical finder — f4-terrain's `find_file_ci` and
/// f4-models' name-variant arrays now delegate here.
[[nodiscard]] std::filesystem::path find_file_ci(
    const std::filesystem::path& dir,
    std::string_view name);

/// Case-insensitive file lookup with name variants. Tries each name in
/// `names` in order; returns the first match. Used by callers that
/// accept multiple filename conventions (e.g. `KoreaObj.HDR` vs
/// `KoreaObj.DXH`, `KoreaObj.Tex` vs `KoreaObj.tex`). The variant order
/// is the caller's preference (the first variant that exists wins).
[[nodiscard]] std::filesystem::path find_file_ci_with_variants(
    const std::filesystem::path& dir,
    const std::vector<std::string>& names);

/// Case-insensitive subdirectory lookup inside `dir`. Returns an empty
/// path when not found. Used by Installation::detect() to locate sim/,
/// terrdata/, campaign/.
[[nodiscard]] std::filesystem::path find_subdir_ci(
    const std::filesystem::path& dir,
    std::string_view name);

/// Case-insensitive search across multiple directories. Tries each
/// directory in `dirs` in order; returns the first match. Used by
/// callers that search a list of candidate directories (e.g.
/// f4-models' `find_koreaobj_files` searches install_root,
/// terrdata/objects, terrdata/korea/objects).
[[nodiscard]] std::filesystem::path find_file_ci_in_dirs(
    const std::vector<std::filesystem::path>& dirs,
    std::string_view name);

/// Case-insensitive search across multiple directories with name
/// variants. Tries each (dir, name) combination in order; returns the
/// first match. Used by f4-models' `find_koreaobj_files` /
/// `find_tex_file`.
[[nodiscard]] std::filesystem::path find_file_ci_in_dirs_with_variants(
    const std::vector<std::filesystem::path>& dirs,
    const std::vector<std::string>& names);

/// Case-insensitive file lookup by stem + extension. Tries
/// `<base_path>.<ext>` first (with on-disk case canonicalization for
/// case-insensitive filesystems), then a case-insensitive scan of the
/// parent directory for any file matching `<stem>.<ext>`. Returns an
/// empty path when not found.
///
/// This is the canonical finder for theater-data files
/// (Falcon4.OCD/.PHD/.PD/...). f4-world-convert's `find_theater_file`
/// now delegates here.
[[nodiscard]] std::filesystem::path find_file_by_extension_ci(
    const std::filesystem::path& base_path,
    std::string_view ext);

} // namespace f4::install
