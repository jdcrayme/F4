// f4-install/include/f4/install/installation.hpp
//
// Locates and describes a Falcon 4.0 / FreeFalcon installation on disk.
//
// This is the single source of truth for "where do I find <X> in this
// install" — replacing the ad-hoc search heuristics scattered across
// the codebase (e.g. f4-world-convert's find_class_table). The viewer
// and CLI tools call Installation::detect(root) once at startup, then
// use the returned Installation object to resolve every file by name.
//
// CONOPS:
//   1. The user points the viewer at their Falcon 4.0 install directory
//      (the one containing falcon4.exe / Falcon4.ai).
//   2. detect() validates the directory, scans for theaters and campaigns,
//      and locates the well-known files (FALCON4.ct, sim/, etc.).
//   3. The viewer presents a Theater dropdown (from theaters()) and a
//      Campaign dropdown (from campaigns_for(theater_key)).
//   4. On selection, the viewer loads THEATER.* + the chosen .cam via
//      the existing in-process converters — no manual file picking.
//
// The Installation object is intentionally a value type: it's cheap to
// copy, has no open file handles, and can be cached to disk (e.g. in
// ~/.f4-viewer/settings.json) so the user only picks the install path
// once. Re-scan on every launch — campaigns change as the user saves
// new games, and the scan is cheap (~50 ms for a typical install).

#pragma once

#include <f4/install/campaign.hpp>
#include <f4/install/theater.hpp>

#include <filesystem>
#include <string>
#include <vector>

namespace f4::install {

/// Diagnostic info — what detect() probed, what it found, what it didn't.
/// Used by the viewer's Tools > Install Diagnostics panel to show the user
/// exactly where we looked for each file, so they can diagnose "class
/// table not found" or "theater incomplete" errors without guessing.
struct DiagnosticInfo {
    /// One entry per location probed for FALCON4.ct during detect().
    /// Each entry is the full path we checked; the `class_table` field
    /// of Installation tells you which one (if any) succeeded.
    std::vector<std::filesystem::path> class_table_searched;

    /// One entry per subdirectory of <root> probed as a potential
    /// theater (only those containing THEATER.MAP make it into
    /// `theaters()`; this list also includes the dirs we looked at
    /// and rejected, for transparency).
    std::vector<std::filesystem::path> theater_dirs_probed;

    /// Whether the campaign/ directory was found and scanned.
    bool campaign_dir_found = false;

    /// The theater.lst path (if present). Empty if not found.
    std::filesystem::path theater_lst_path;

    /// Whether theater.lst was successfully parsed (false if absent
    /// or unparseable — we fall back to a directory scan).
    bool theater_lst_parsed = false;

    /// Number of keys read from theater.lst (0 if not parsed).
    std::size_t theater_lst_key_count = 0;

    /// Render as a human-readable multi-line string. Used by the
    /// viewer's diagnostics modal and by the --diagnostics CLI flag.
    [[nodiscard]] std::string format() const;
};

/// A discovered Falcon 4.0 / FreeFalcon installation.
class Installation {
public:
    Installation() = default;

    /// Detect a Falcon install at `root`.
    ///
    /// Lenient: never throws for missing optional files. Returns an
    /// Installation with `valid() == false` if the directory doesn't
    /// look like a Falcon install (no FALCON4.ct and no terrdata/).
    /// Callers should check `valid()` before relying on the listings.
    static Installation detect(const std::filesystem::path& root);

    /// True when the directory looks like a Falcon install.
    /// Currently: FALCON4.ct exists at the root OR a terrdata/ subdir
    /// exists. We don't require falcon4.exe so dev/CI trees that only
    /// ship data files (e.g. our test fixtures) still validate.
    [[nodiscard]] bool valid() const noexcept;

    /// The install root the user pointed at. Empty if not detected.
    [[nodiscard]] const std::filesystem::path& root() const noexcept { return root_; }

    /// Path to FALCON4.ct (the class table). Empty if not found.
    /// The viewer and cam2json both need this to resolve objective_type
    /// and unit_subtype from raw entity_type values.
    [[nodiscard]] const std::filesystem::path& class_table() const noexcept { return class_table_; }

    /// Path to the sim/ directory (aircraft .dat files). Empty if absent.
    /// Future: f4-convert's dat2json will use this for batch conversion.
    [[nodiscard]] const std::filesystem::path& aircraft_dir() const noexcept { return aircraft_dir_; }

    /// Path to the campaign/ directory (where .cam saves live).
    /// Empty if absent (some installs may not have any saved campaigns yet).
    [[nodiscard]] const std::filesystem::path& campaign_dir() const noexcept { return campaign_dir_; }

    /// Path to the terrdata/ directory (theater data).
    /// Empty if absent (an install without terrdata/ is not usable for
    /// world visualization, but might still be useful for aircraft work).
    [[nodiscard]] const std::filesystem::path& terrdata_dir() const noexcept { return terrdata_dir_; }

    /// All discovered theaters, in display order (theater.lst order first,
    /// then any additional theaters alphabetically).
    [[nodiscard]] const std::vector<Theater>& theaters() const noexcept { return theaters_; }

    /// Find a theater by key (case-insensitive). Returns nullptr if absent.
    [[nodiscard]] const Theater* find_theater(const std::string& key) const noexcept;

    /// All discovered campaigns across all theaters. Sorted by
    /// (theater_key, stem). Use campaigns_for() to filter by theater.
    [[nodiscard]] const std::vector<Campaign>& campaigns() const noexcept { return campaigns_; }

    /// Campaigns whose theater_key matches `key` (case-insensitive), plus
    /// any campaigns with empty theater_key (flat layout — assumed to
    /// belong to whatever theater the user selected). Empty if no match.
    [[nodiscard]] std::vector<Campaign> campaigns_for(const std::string& theater_key) const;

    /// Diagnostic info collected during detect(). Shows where we looked
    /// for FALCON4.ct, which theater dirs we probed, whether theater.lst
    /// was found and parsed. Used by the viewer's Tools > Install
    /// Diagnostics panel.
    [[nodiscard]] const DiagnosticInfo& diagnostics() const noexcept { return diagnostics_; }

    /// Install-aware class-table resolver.
    ///
    /// Used to back f4::world_convert::find_class_table(). Search order:
    ///   1. Same directory as `reference_file` (the game ships FALCON4.ct
    ///      next to .cam saves).
    ///   2. Up one or two directories from `reference_file`.
    ///   3. The install's class_table() path (set during detect()).
    ///   4. CWD-relative well-known paths (covers running from build dir
    ///      or source-tree fixtures, when no install is configured).
    ///
    /// `reference_file` may be empty — only steps 3 and 4 are performed.
    /// Returns an empty path if not found.
    [[nodiscard]] std::filesystem::path find_class_table(
        const std::filesystem::path& reference_file = {}) const;

    /// Resolve a path relative to the install root.
    /// Returns an empty path if the install is not valid.
    [[nodiscard]] std::filesystem::path resolve(const std::string& relative) const;

private:
    std::filesystem::path root_;
    std::filesystem::path class_table_;
    std::filesystem::path aircraft_dir_;
    std::filesystem::path campaign_dir_;
    std::filesystem::path terrdata_dir_;
    std::vector<Theater> theaters_;
    std::vector<Campaign> campaigns_;
    DiagnosticInfo diagnostics_;
};

/// Quick one-shot: detect the install at `root` and locate FALCON4.ct
/// in one call. Equivalent to Installation::detect(root).find_class_table()
/// but more convenient for callers that don't need the full Installation.
[[nodiscard]] std::filesystem::path find_class_table_in_install(
    const std::filesystem::path& root,
    const std::filesystem::path& reference_file = {});

/// CWD-relative fallback search for FALCON4.ct, used when no install is
/// configured (e.g. CI running against bundled fixtures). Mirrors the
/// behavior of the pre-f4-install find_class_table() helper so existing
/// callers can adopt the new API without behavior change.
[[nodiscard]] std::filesystem::path find_class_table_cwd_fallback();

} // namespace f4::install
