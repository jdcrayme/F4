// f4-install/include/f4/install/theater.hpp
//
// One theater entry within a Falcon 4.0 / FreeFalcon installation.
//
// A "theater" is a self-contained piece of simulated geography: a terrain
// (elevation grid, palette, overlays), a coast/road/rail network, an airbase
// list, and the per-theater configuration that binds them together. Falcon
// 4.0 shipped one theater (Korea); FreeFalcon and Allied Force added many
// (Balkans, Iceland, Vietnam, ...). Multi-theater installs share one binary
// tree but keep per-theater data under terrdata/<key>/.
//
// On-disk layout (canonical Falcon 4.0 / FreeFalcon):
//
//   <install>/terrdata/
//     theater.lst          (text list of theater directory names; may be absent)
//     korea/
//       THEATER.MAP        (header + palette — required)
//       THEATER.MEA        (elevation grid — required)
//       THEATER.O2         (secondary overlay — optional)
//       THEATER.L0..L5     (per-LOD post data — optional, large)
//       theater.ini        (display name + config — optional)
//       ... (texture, object, weather data, ...)
//     balkans/
//       ...
//
// Detection is intentionally tolerant: we treat any subdirectory of
// terrdata/ that contains a THEATER.MAP as a theater, whether or not it
// appears in theater.lst. The theater.lst file (when present) provides
// preferred ordering and display names, but is not required for a theater
// to be discovered.

#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace f4::install {

/// One theater entry discovered under <install>/terrdata/.
///
/// A Theater is "complete" (terrain loadable by f4-terrain) when both
/// theater_map and theater_mea paths are non-empty. Incomplete theaters
/// are still listed (so the viewer can warn rather than silently skip),
/// but `complete()` lets callers filter them out.
struct Theater {
    /// Lowercase directory name under terrdata/ — e.g. "korea", "balkans".
    /// Used as the stable key for cross-referencing with campaigns.
    std::string key;

    /// Human-readable name from theater.ini [Theater].Title, or `key`
    /// with the first letter capitalized if no .ini was found.
    std::string display_name;

    /// Absolute path to the theater directory (e.g. .../terrdata/korea).
    std::filesystem::path dir;

    /// Absolute path to THEATER.MAP. Empty if not present.
    std::filesystem::path theater_map;

    /// Absolute path to THEATER.MEA. Empty if not present.
    std::filesystem::path theater_mea;

    /// Absolute path to THEATER.O2. Empty if not present.
    std::filesystem::path theater_o2;

    /// Absolute path to theater.ini. Empty if not present.
    std::filesystem::path theater_ini;

    /// All THEATER.* files actually present in the directory (full paths).
    /// Captures L0..L5 and any other variants without enumerating them
    /// in the struct — keeps the struct stable as new files are added by
    /// future Falcon variants.
    std::vector<std::filesystem::path> theater_files;

    /// True when both THEATER.MAP and THEATER.MEA are present — the
    /// minimum required for f4-terrain to load the terrain.
    [[nodiscard]] bool complete() const noexcept {
        return !theater_map.empty() && !theater_mea.empty();
    }
};

/// Parse FreeFalcon's theater.lst file format.
///
/// The format is plain text: one theater directory name per line, with
/// blank lines and lines starting with `#` or `//` treated as comments.
/// Surrounding whitespace and quotes are stripped. This is the same
/// format FreeFalcon's C_TheaterList::LoadTheaterList() reads.
///
/// Returns the list of theater keys in their listed order. Lines that
/// fail to parse (empty after stripping) are skipped silently — the
/// caller can fall back to a directory scan if the list is empty.
[[nodiscard]] std::vector<std::string> parse_theater_lst(const std::filesystem::path& lst_path);

/// Parse theater.lst from an in-memory string (for testing).
[[nodiscard]] std::vector<std::string> parse_theater_lst_string(const std::string& content);

/// Scan a terrdata/ directory for theaters.
///
/// Walks `terrdata_dir` and returns one Theater per subdirectory that
/// contains a THEATER.MAP file. Files are populated by case-insensitive
/// match (Falcon ships uppercase; some community installs mix case).
///
/// If `preferred_order` is non-empty, theaters are returned with the
/// listed keys first (in their listed order), then any additional
/// discovered theaters in alphabetical order. This lets theater.lst
/// drive the UI ordering without hiding theaters missing from the list.
[[nodiscard]] std::vector<Theater> scan_theaters(const std::filesystem::path& terrdata_dir,
                                                  const std::vector<std::string>& preferred_order = {});

/// Parse a single theater.ini for its [Theater].Title field.
/// Returns the title, or empty string if not found / file missing.
[[nodiscard]] std::string read_theater_title(const std::filesystem::path& ini_path);

} // namespace f4::install
