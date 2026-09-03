// f4-convert/formation_parser.hpp
//
// Parser for the legacy FreeFalcon formation geometry file
// (sim/acdata/formdata/formdat.fil).
//
// Ported 1:1 from FreeFalcon's ACFormationData::ACFormationData
// (formdata.cpp:12-115) using SimlibFileClass::GetNext() token semantics:
//   numFormations
//   for each formation:
//     num4Slots num2Slots formNum <name-token>
//     num4Slots slot triples: relAz_deg relEl_deg range_NM
//     if num2Slots: one more triple (the dedicated 2-ship slot);
//     else the 2-ship slot defaults to slot[0] (formdata.cpp:85-91)
//
// Units are kept verbatim (degrees / NM) — the reference's read-time
// conversions (DTR, NM_TO_FT) live in f4::data::FormationSlot accessors.

#pragma once

#include "f4/data/formation_data.hpp"

#include <string>
#include <vector>

namespace f4::convert {

// Result of parsing a formdat.fil file.
struct FormationParseResult {
    f4::data::FormationLibrary data;
    std::vector<std::string> warnings;
    std::vector<std::string> errors;
    bool ok = false;
};

/// Parse a formdat.fil file from disk.
[[nodiscard]] FormationParseResult loadFormFile(const std::string& path);

/// Parse from an in-memory string (tests).
[[nodiscard]] FormationParseResult loadFormString(
    const std::string& contents, const std::string& sourceName = "<string>");

} // namespace f4::convert
