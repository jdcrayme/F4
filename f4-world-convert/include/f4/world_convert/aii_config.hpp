// f4-world-convert/include/f4/world_convert/aii_config.hpp
//
// AiiConfig — the Falcon4.AII reader (terrdata/ai/Falcon4.AII).
//
// Falcon4.AII is the campaign AI INI (FreeFalcon's aiinput.h parameters
// written out as a Windows INI file). FALCON4_FILE_LAYOUT.md §4 documents
// it as the source of the deaggregation bubble sizes:
//
//   [Sim]
//   SIM_BUBBLE_SIZE    = 2.5    ; air-sim deagg bubble, grid units
//   GROUND_BUBBLE_SIZE = 1.0    ; ground-sim deagg bubble, grid units
//
// The plan doc (NEXT_PHASE_PLAN.md B.0) also names FreeFalcon-source
// spellings for the same settings — MinBubbleSize (air) and
// BubbleRatioToUnitSpan (ground). FreeFalcon builds renamed INI keys
// across versions; this reader accepts BOTH spellings per setting and
// resolves the value with a fixed precedence:
//
//   air bubble    : [Sim] MinBubbleSize, else [Sim] SIM_BUBBLE_SIZE,
//                   else the documented default (2.5 grid units)
//   ground bubble : [Sim] BubbleRatioToUnitSpan, else
//                   [Sim] GROUND_BUBBLE_SIZE, else default (1.0)
//
// When the exact [Sim] section has neither spelling, any other section
// carrying exactly one of the names is consulted (last-resort scan, in a
// deterministic order) — hand-edited AII files are known to drift the
// keys out of their documented section.
//
// Everything else in the file (MIN_TASK_AIR, AIR_PATH_MAX, ATC tuning,
// ...) is kept verbatim in a section→key→value map and reachable through
// lookup() — consumers opt in per key instead of the struct accreting a
// field per tuning parameter.
//
// Parsing contract (Windows INI semantics, matching the house rule of
// failing loudly on malformed input — see class_table.cpp):
//   * Sections: "[name]" — one per line. Names fold to lowercase.
//   * Keys: "key = value" — value runs to the first ';' (inline comment)
//     or end of line, then trims. Keys fold to lowercase (Windows
//     GetPrivateProfileString is case-insensitive; real AII files mix).
//   * Duplicate keys: last value wins.
//   * Comments: ';' anywhere on a line starts a comment. A line that is
//     blank after trimming is skipped.
//   * Anything else (a bare word, a stray bracket, a key with no '=') is
//     a parse error: load() throws with the path and line number.
//   * The two bubble settings must parse as numbers or load() throws
//     (a bubble size of "auto" would silently change deagg geometry —
//     that failure must be loud, not defaulted).
//
// Grid units: one campaign grid unit = 1024 ft (the campaign's grid
// coordinate scale — see f4-simulation's bubble_manager.hpp for the
// ft conversion at the BubbleManager boundary).
//
// Dependencies: f4-io (read_file). C++20.

#pragma once

#include <filesystem>
#include <map>
#include <string>

namespace f4::world_convert {

class AiiConfig {
public:
    /// Documented defaults (FALCON4_FILE_LAYOUT.md §4.1 / bubble_manager.hpp):
    /// SIM_BUBBLE_SIZE 2.5 grid units, GROUND_BUBBLE_SIZE 1.0 grid unit.
    static constexpr double DEFAULT_SIM_BUBBLE_SIZE_GRID = 2.5;
    static constexpr double DEFAULT_GROUND_BUBBLE_SIZE_GRID = 1.0;

    /// Parse an AII file. Throws std::runtime_error (prefix "aii:") on
    /// open failure or a malformed line. Missing bubble keys resolve to
    /// the documented defaults — a file that exists but tunes other
    /// parameters only is valid.
    static AiiConfig load(const std::filesystem::path& path);

    /// Load when the file exists; otherwise return `fallback` (default =
    /// the documented defaults). An EMPTY path counts as absent — the
    /// "no AII configured" case every pre-B.0 caller exercises. A path
    /// that EXISTS but fails to parse still throws (loud, as everywhere).
    static AiiConfig load_if_exists(
        const std::filesystem::path& path,
        const AiiConfig& fallback = documented_defaults());

    /// The documented-defaults config (no file needed) — exposed for
    /// tests and callers that want to mix per-key overrides.
    static AiiConfig documented_defaults();

    /// Air-sim deagg bubble, campaign grid units (default 2.5).
    [[nodiscard]] double sim_bubble_size_grid() const noexcept {
        return sim_bubble_size_grid_;
    }

    /// Ground-sim deagg bubble, campaign grid units (default 1.0).
    [[nodiscard]] double ground_bubble_size_grid() const noexcept {
        return ground_bubble_size_grid_;
    }

    /// Raw value for section/key (both folded case-insensitively).
    /// nullptr when the key is absent. The returned pointer borrows from
    /// this config — valid for the config's lifetime.
    [[nodiscard]] const std::string* lookup(std::string_view section,
                                            std::string_view key) const;

    /// Every parsed section (folded names), for tests and diagnostics.
    [[nodiscard]] const std::map<std::string, std::map<std::string, std::string>>&
    sections() const noexcept { return sections_; }

private:
    /// The two tuned settings. Defaults until a file supplies them.
    double sim_bubble_size_grid_{DEFAULT_SIM_BUBBLE_SIZE_GRID};
    double ground_bubble_size_grid_{DEFAULT_GROUND_BUBBLE_SIZE_GRID};

    /// Folded section name -> folded key name -> raw (trimmed) value.
    std::map<std::string, std::map<std::string, std::string>> sections_;

    /// Resolve one bubble setting through the precedence chain.
    /// Returns the raw string value and whether any spelling was found.
    std::pair<std::string, bool> resolve_bubble_value_(
        std::string_view primary, std::string_view alias) const;
};

} // namespace f4::world_convert
