// f4-convert/sensor_parser.hpp
//
// Parser for the legacy FreeFalcon sensor authoring text files in
// SimData.zip's sim/SENSDATA tree:
//   - IRST/IRST.LST  + *.irs  (IRST seekers)
//   - RWR/RWR.LST    + *.rwr  (RWR receivers)
//   - VISUAL/VISUAL.LST + *.vss (visual sensors)
//
// FIDELITY NOTE: these text files have no readers in the FreeFalcon
// tree (the runtime freads precompiled .ICD/.VSD/.RWD binaries); the
// text files are the 1998 authoring source, the same design-data
// situation as BRAINDAT.brn. Formats per file: '#' line comments and
// five whitespace-separated values whose names come from the files'
// own comments (see f4/data/sensor_data.hpp for the full contract and
// the compiled-struct mapping).
//
// The .LST files are one count token followed by one file name per
// entry; names resolve case-insensitively in the same directory (the
// shipped lists are lowercase, the files are mixed case).

#pragma once

#include "f4/data/sensor_data.hpp"

#include <string>
#include <vector>

namespace f4::convert {

// Results — one per sensor family.
struct IrstParseResult {
    f4::data::IrstSensorData data;
    std::vector<std::string> warnings;
    std::vector<std::string> errors;
    bool ok = false;
};

struct RwrParseResult {
    f4::data::RwrSensorData data;
    std::vector<std::string> warnings;
    std::vector<std::string> errors;
    bool ok = false;
};

struct VisualParseResult {
    f4::data::VisualSensorDataLibrary data;
    std::vector<std::string> warnings;
    std::vector<std::string> errors;
    bool ok = false;
};

/// Parse one .LST + its files from disk (dir = the .lst's directory).
[[nodiscard]] IrstParseResult loadIrstListFile(const std::string& lstPath);
[[nodiscard]] RwrParseResult loadRwrListFile(const std::string& lstPath);
[[nodiscard]] VisualParseResult loadVisualListFile(const std::string& lstPath);

/// Parse from an in-memory .LST string; names resolve case-insensitively
/// in `sensorDir` (pass "" to forbid file opens).
[[nodiscard]] IrstParseResult loadIrstListString(
    const std::string& lstContents, const std::string& sensorDir,
    const std::string& sourceName = "<string>");
[[nodiscard]] RwrParseResult loadRwrListString(
    const std::string& lstContents, const std::string& sensorDir,
    const std::string& sourceName = "<string>");
[[nodiscard]] VisualParseResult loadVisualListString(
    const std::string& lstContents, const std::string& sensorDir,
    const std::string& sourceName = "<string>");

/// Parse a single sensor file from a string (tests / direct use).
[[nodiscard]] IrstParseResult loadIrstString(const std::string& contents,
                                             const std::string& sourceName);
[[nodiscard]] RwrParseResult loadRwrString(const std::string& contents,
                                           const std::string& sourceName);
[[nodiscard]] VisualParseResult loadVisualString(const std::string& contents,
                                                 const std::string& sourceName);

} // namespace f4::convert
