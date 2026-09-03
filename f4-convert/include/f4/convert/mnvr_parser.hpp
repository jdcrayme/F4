// f4-convert/mnvr_parser.hpp
//
// Parser for the legacy FreeFalcon mnvrdata.dat AI maneuver tables
// (sim/acdata/brain/mnvrdata.dat).
//
// Ported 1:1 from FreeFalcon's DigitalBrain::ReadManeuverData
// (digimain.cpp:811-913), including its token semantics:
//   - '#' comment lines are skipped (SimlibFileClass::GetNext, file.cpp:411-441)
//   - class flags are parsed as HEX ("0x724")
//   - per (own, opposing) class pair: three counts FIRST, then the
//     intercept indices, merge indices, react indices — every index
//     1-based in the file, stored 0-based after the reference's
//     "atoi(...) - 1" (digimain.cpp:894)
//
// THE 'A' QUIRK (documented in f4/data/maneuver_data.hpp): the shipped
// file begins with an 'A' byte before the first '#' comment. FreeFalcon's
// reader consumes one byte and only parses when it is '#' — the shipped
// file is silently skipped by the reference engine. This parser accepts
// a leading single-letter file-type marker ('A' or 'B') and warns, so
// the authored data actually loads.
//
// The parser is permissive about unknown trailing tokens (warnings) and
// strict about the table structure (errors).

#pragma once

#include "f4/data/maneuver_data.hpp"

#include <string>
#include <vector>

namespace f4::convert {

// Result of parsing a mnvrdata.dat file.
struct MnvrParseResult {
    f4::data::ManeuverData data;
    std::vector<std::string> warnings;   // non-fatal (skipped markers, etc.)
    std::vector<std::string> errors;     // fatal (structural)
    bool ok = false;
};

/// Parse a mnvrdata.dat file from disk.
[[nodiscard]] MnvrParseResult loadMnvFile(const std::string& path);

/// Parse from an in-memory string (tests).
[[nodiscard]] MnvrParseResult loadMnvString(const std::string& contents,
                                            const std::string& sourceName = "<string>");

} // namespace f4::convert
