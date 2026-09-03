// f4-convert/brain_parser.hpp
//
// Parser for the legacy .brn DigitalBrain archetype files
// (sim/acdata/brain/BRAINDAT.brn and GENERIC.BRN).
//
// These files pre-date FreeFalcon's DigiMode enum and have NO reader in
// the reference engine (see f4/data/brain_data.hpp for the fidelity
// note). The format is reconstructed from the shipped files:
//
//   BRAINDAT.brn (named archetypes):
//     <count>
//     "# <ArchetypeName>"
//     rows... each: "# <ModeLabel>" / <enabled int> / <p r a doubles>
//
//   GENERIC.BRN (one bare archetype):
//     rows... (no count, no section name) + optional trailer:
//     "# Max Gs" / <double>
//
// Rows are positional; labels are captured verbatim (blank "# " labels
// stay blank). A section header is distinguished from a mode row by the
// line that FOLLOWS it: a header is followed by another comment line,
// a mode row by its enabled-int line.

#pragma once

#include "f4/data/brain_data.hpp"

#include <string>
#include <vector>

namespace f4::convert {

// Result of parsing a .brn file.
struct BrainParseResult {
    f4::data::BrainData data;
    double max_gs{0.0};                ///< "# Max Gs" trailer when present (GENERIC.BRN)
    std::vector<std::string> warnings;
    std::vector<std::string> errors;
    bool ok = false;
};

/// Parse a .brn file from disk (BRAINDAT.brn or GENERIC.BRN).
[[nodiscard]] BrainParseResult loadBrainFile(const std::string& path);

/// Parse from an in-memory string (tests).
[[nodiscard]] BrainParseResult loadBrainString(
    const std::string& contents, const std::string& sourceName = "<string>");

} // namespace f4::convert
