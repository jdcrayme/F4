// f4-data/brain_data.hpp
//
// Data-only representation of the DigitalBrain archetype mode tables
// (sim/acdata/brain/BRAINDAT.brn and GENERIC.BRN). This is the in-memory
// representation that f4-convert produces from the .brn files and that
// f4-ai (and other consumers) read.
//
// FILE FORMAT (verified against the shipped 1997-era files):
//   numBrainTypes                     (e.g. 8)
//   for each brain type:
//     "# <TypeName>"                 (comment: Generic, SEAD, Strike,
//                                     Intercepter, Air CAP, Air Sweep,
//                                     Escort, Waypointer)
//     for each mode row (positional, 25 rows in BRAINDAT.brn):
//       "# <ModeLabel>"              (comment; may be blank or a tag
//                                     like "Defensive Modes - This is a
//                                     tag" — labels are advisory only)
//       enabled                      (int: 1 = mode available)
//       priority range angle         (three doubles)
//
// The rows are POSITIONAL in the file; the comment labels document (but
// do not drive) the layout. The label ordering in the shipped files does
// NOT match FreeFalcon's DigiMode enum (digi.h:222-250) — the files are
// older than the enum. Consumers therefore look rows up BY LABEL, which
// f4::data::BrainArchetype::find_mode() does with tolerant matching.
//
// REFERENCE TRUTH (fidelity note, load-bearing): no reader for .brn
// files exists anywhere in FreeFalcon — BRAINDAT.brn / GENERIC.BRN /
// BRAINDAT.FIL are vestigial 1997 design data that the reference engine
// carries on disk but never loads. The live mode-priority system in
// FreeFalcon is the DigiMode enum ordering plus the special cases in
// DigitalBrain::AddMode (dlogic.cpp:800-832). This port treats the .brn
// data as the original design intent: per-archetype mode availability
// plus engagement entry criteria (priority, range ft, angle deg) —
// e.g. GunsEngageMode {1.0, 6000 ft, 45 deg}, WVREngageMode
// {3.0, 50000 ft, 180 deg}. The values are consumed as data, with the
// reference's enum-ordering ladder kept in the BrainComponent code.
//
// Units are verbatim file units: range in FEET, angle in DEGREES,
// priority lower = more urgent (matches the enum ordering's direction:
// defensive/safety modes carry small values, mission modes large ones).

#pragma once

#include <optional>
#include <string>
#include <vector>

namespace f4::data {

// ---------------------------------------------------------------------------
// The load-bearing mode labels used by consumers (the intersection of
// the .brn label set and the BrainComponent's rung set). Matching against
// file labels is tolerant: case-insensitive, spaces/underscores ignored.
// ---------------------------------------------------------------------------
enum class BrainModeKey {
    GroundAvoid,
    CollisionAvoid,
    GunsJink,
    MissileDefeat,
    Defensive,       // the "Defensive Modes - This is a tag" marker row
    FollowOrders,
    Landing,         // only in BRAINDAT.FIL's variant spelling
    Accelerate,
    Merge,
    MissileEngage,
    GunsEngage,
    Roop,
    OverB,
    Overshoot,       // GENERIC.BRN spelling
    WVREngage,
    BVREngage,
    RunAway,
    Loiter,
    Separate,
    RTB,
    Wingy,
    Bugout,
    Waypoint,
    GroundMnvr,
    LastValid,       // the trailing sentinel row (data is filler: 123/456/78.9)
};

/// One positional mode row: label (verbatim comment text, may be empty),
/// availability flag, and the (priority, range ft, angle deg) triple.
struct BrainModeRow {
    std::string label;         ///< verbatim comment label ("" when the file had none)
    int         enabled{0};    ///< 1 = the mode is available to this archetype
    double      priority{0.0}; ///< design priority (lower = more urgent)
    double      range_ft{0.0}; ///< engagement entry range (ft; 0 = n/a)
    double      angle_deg{0.0};///< entry half-angle (deg; 0 = n/a)

    /// File-order index of this row within its archetype (bookkeeping
    /// for diagnostics + round-trip; NOT a mode identity).
    std::size_t row{0};
};

// ---------------------------------------------------------------------------
// BrainArchetype — one named brain type (e.g. "Generic", "SEAD").
// ---------------------------------------------------------------------------
struct BrainArchetype {
    std::string                 name;   ///< verbatim section name
    std::vector<BrainModeRow>   modes;  ///< positional rows, in file order

    /// Tolerant label lookup (case-insensitive, space/underscore-free
    /// comparison). Returns nullptr when the archetype has no row that
    /// matches the key.
    [[nodiscard]] const BrainModeRow* find_mode(BrainModeKey key) const noexcept;

    /// find_mode + enabled check (a missing row counts as disabled).
    [[nodiscard]] bool mode_enabled(BrainModeKey key) const noexcept;
};

// ---------------------------------------------------------------------------
// BrainData — the whole file.
// ---------------------------------------------------------------------------
struct BrainData {
    std::vector<BrainArchetype> archetypes;

    /// Archetype lookup by name (case-insensitive; nullptr when absent).
    [[nodiscard]] const BrainArchetype* find_archetype(
        const std::string& name) const noexcept;

    /// "Generic" — the fallback archetype consumers use when nothing
    /// else is specified (mirrors FreeFalcon's default brain behavior).
    [[nodiscard]] const BrainArchetype* generic() const noexcept;
};

// ---------------------------------------------------------------------------
// JSON serialization (canonical format; f4-convert delegates here).
// ---------------------------------------------------------------------------
struct BrainDataResult {
    BrainData data;
    bool ok = false;
    std::vector<std::string> errors;
    std::vector<std::string> warnings;
};

/// Load from a JSON file on disk.
[[nodiscard]] BrainDataResult loadBrainData(const std::string& path);

/// Load from a JSON string.
[[nodiscard]] BrainDataResult loadBrainDataFromString(const std::string& json);

/// Serialize to a pretty-printed JSON string.
[[nodiscard]] std::string writeBrainData(const BrainData& bd);

/// Write to a JSON file. Returns true on success.
bool writeBrainDataFile(const BrainData& bd, const std::string& path);

} // namespace f4::data
