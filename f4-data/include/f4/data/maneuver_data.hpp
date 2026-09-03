// f4-data/maneuver_data.hpp
//
// Data-only representation of the DigitalBrain maneuver tables
// (sim/acdata/brain/mnvrdata.dat). This is the in-memory representation
// that f4-convert produces from the .dat file and that f4-ai (and other
// consumers) read.
//
// Ported 1:1 from FreeFalcon's DigitalBrain statics:
//   - ManeuverChoiceTable  (digi.h:183-191)
//   - ManeuverClassData    (digi.h:193-196)
//   - ACMnverClass         (digi.h:139-151)
//   - ACMnverClassFlags    (digi.h:153-163)
//   - BVRInterceptType     (digi.h:71-96)
//   - WVRMergeManeuverType (digi.h:122-128)
//   - SpikeReactionType    (digi.h:130-137)
//
// FILE-FORMAT TRUTH (FreeFalcon digimain.cpp:811-913 ReadManeuverData):
//   for each OWN class i (9):
//     read one class-flags token as HEX ("0x724")
//     for each OPPOSING class j (9):
//       read numIntercepts, numMerges, numReacts (counts FIRST)
//       then numIntercepts intercept indices, numMerges merge
//       indices, numReacts react indices — every index stored 1-based
//       in the file and converted to 0-based on read
//       (digimain.cpp:894 "atoi(...) - 1").
//
// REFERENCE QUIRK (documented, load-bearing for fidelity): the shipped
// mnvrdata.dat begins with an 'A' byte ("A#   enum ACMnverClass {"),
// but FreeFalcon's reader only parses when the first byte is '#'
// (digimain.cpp:824) — anything else warns ("Bad Maneuver Data File
// Format") and leaves the tables at their zero/-1 initializer. The
// stock file is therefore SILENTLY SKIPPED by the reference engine; the
// data below represents what the original designers authored, and this
// port makes it actually loadable.
//
// Enum indices are kept as plain ints (not enum classes) so unknown/
// extended values survive the round trip without lossy clamping.

#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include <f4/math/constants.hpp>

namespace f4::data {

// ---------------------------------------------------------------------------
// Enum sizes (digi.h NumMnvrClasses / NumInterceptTypes /
// NumWVRMergeMnverTypes / NumSpikeReactionTypes).
// ---------------------------------------------------------------------------
inline constexpr std::size_t kNumMnvrClasses   = 9;
inline constexpr std::size_t kNumInterceptTypes = 26;
inline constexpr std::size_t kNumMergeTypes      = 3;
inline constexpr std::size_t kNumReactTypes      = 4;

/// ACMnverClass index → canonical name (digi.h:139-151; the file's own
/// comment header spells them without the "MnvrClass" prefix).
inline constexpr const char* kMnvrClassNames[kNumMnvrClasses] = {
    "F4", "F5", "F14", "F15", "F16",
    "Mig25", "Mig27", "A10", "Bomber"
};

/// ACMnverClassFlags bit values (digi.h:153-163).
enum class MnvrClassFlags : std::uint32_t {
    CanLevelTurn    = 0x1,
    CanSlice        = 0x2,
    CanUseVertical  = 0x4,
    CanOneCircle    = 0x10,
    CanTwoCircle    = 0x20,
    CanJinkSnake    = 0x100,
    CanJinkLoaded   = 0x200,
    CanJinkUnloaded = 0x400,
};

/// BVRInterceptType index → canonical name (digi.h:71-96, 0-based).
inline constexpr const char* kInterceptTypeNames[kNumInterceptTypes] = {
    "BvrFollowWaypoints", "BvrFlyFormation", "BvrSingleSideOffset",
    "BvrPince", "BvrPursuit", "BvrNoIntercept", "BvrPump", "BvrCrank",
    "BvrCrankRight", "BvrCrankLeft", "BvrNotch", "BvrNotchRight",
    "BvrNotchRightHigh", "BvrNotchLeft", "BvrNotchLeftHigh", "BvrGrind",
    "BvrCrankHi", "BvrCrankLo", "BvrCrankRightHi", "BvrCrankRightLo",
    "BvrCrankLeftHi", "BvrCrankLeftLo"
};

/// WVRMergeManeuverType index → canonical name (digi.h:122-128, 0-based).
inline constexpr const char* kMergeTypeNames[kNumMergeTypes] = {
    "WvrMergeHitAndRun", "WvrMergeLimited", "WvrMergeUnlimited"
};

/// SpikeReactionType index → canonical name (digi.h:130-137, 0-based).
inline constexpr const char* kReactTypeNames[kNumReactTypes] = {
    "SpikeReactNone", "SpikeReactECM", "SpikeReactBeam", "SpikeReactDrag"
};

// ---------------------------------------------------------------------------
// ManeuverChoiceTable — one own-class × opposing-class cell.
// Direct port of digi.h:183-191 (pointers replaced by vectors; counts
// are implicit in the vector sizes).
// ---------------------------------------------------------------------------
struct ManeuverChoice {
    std::vector<int> intercepts;   // BVRInterceptType, 0-based
    std::vector<int> merges;       // WVRMergeManeuverType, 0-based
    std::vector<int> spikeReacts;  // SpikeReactionType, 0-based
};

// ---------------------------------------------------------------------------
// ManeuverData — the whole file: 9 class flags + the 9×9 choice table.
// ---------------------------------------------------------------------------
struct ManeuverData {
    /// Per-class capability flags (raw ACMnverClassFlags bits; e.g. F4
    /// ships 0x724 = JinkUnloaded|JinkLoaded|JinkSnake|TwoCircle|UseVertical).
    std::array<std::uint32_t, kNumMnvrClasses> classFlags{};

    /// table[own][opposing] — digi.h maneuverData[NumMnvrClasses][NumMnvrClasses].
    std::array<std::array<ManeuverChoice, kNumMnvrClasses>, kNumMnvrClasses> table{};

    /// True when flags & flag (class index bounds-checked, false outside).
    [[nodiscard]] bool classCan(std::size_t ownClass,
                                MnvrClassFlags flag) const noexcept;

    /// Access with a bounds guard (returns nullptr outside 0..8).
    [[nodiscard]] const ManeuverChoice* choice(std::size_t own,
                                               std::size_t opposing) const noexcept;

    /// Total number of populated (non-empty) cells — a quick sanity sum.
    [[nodiscard]] std::size_t populatedCells() const noexcept;
};

// ---------------------------------------------------------------------------
// JSON serialization (canonical format; f4-convert delegates here).
// ---------------------------------------------------------------------------
struct ManeuverDataResult {
    ManeuverData data;
    bool ok = false;
    std::vector<std::string> errors;
    std::vector<std::string> warnings;
};

/// Load from a JSON file on disk.
[[nodiscard]] ManeuverDataResult loadManeuverData(const std::string& path);

/// Load from a JSON string.
[[nodiscard]] ManeuverDataResult loadManeuverDataFromString(
    const std::string& json);

/// Serialize to a pretty-printed JSON string.
[[nodiscard]] std::string writeManeuverData(const ManeuverData& md);

/// Write to a JSON file. Returns true on success.
bool writeManeuverDataFile(const ManeuverData& md, const std::string& path);

} // namespace f4::data
