// f4-convert/signature_parser.hpp
//
// Parser for the legacy FreeFalcon signature grids in SimData.zip's
// sim/SIGDATA tree:
//   - SIGDATA.LST            — one count + one signature stem per entry
//   - RCSDAT/<stem>.RCS      — radar cross section grid (m^2)
//   - IR/<stem>.IR0/.IR1/.IR2 — IR signature ratio grids
//   - VISUAL/<stem>.VIS      — visual size factor grid
//
// Grid text format (all '#' comments; the GENERIC.RCS file documents
// each section inline):
//     # Num Azimuth Breakpoints
//     3
//     # Num Elevation Breakpoints
//     3
//     # Azimuth Breakpoints
//     -180.0 0.0 180.0
//     # Elevation, then the data
//     -90.0  10.0 10.0 10.0
//       0.0  10.0 10.0 10.0
//      90.0  10.0 10.0 10.0
//
// FIDELITY NOTE: these grids have no readers in the FreeFalcon tree
// (the runtime consumes precompiled binary campdata); VisualClass::
// GetSignature (visual.cpp:79-99) documents the intended consumption —
// Math.TwodInterp over (azimuth, elevation) — which f4-data's
// SignatureGrid::value_at implements. Same design-data situation as
// BRAINDAT.brn: this port makes the authored grids actually loadable.

#pragma once

#include "f4/data/signature_data.hpp"

#include <string>
#include <vector>

namespace f4::convert {

// Result of parsing one signature grid text file.
struct SigGridParseResult {
    f4::data::SignatureGrid grid;
    std::vector<std::string> warnings;
    std::vector<std::string> errors;
    bool ok = false;
};

// Result of parsing the whole SIGDATA directory (SIGDATA.LST + every
// grid family for every stem).
struct SigParseResult {
    f4::data::SignatureDataLibrary library;
    std::vector<std::string> warnings;
    std::vector<std::string> errors;
    bool ok = false;
};

/// Parse one grid text file from disk.
[[nodiscard]] SigGridParseResult loadSigGridFile(const std::string& path);

/// Parse one grid text file from an in-memory string (tests).
[[nodiscard]] SigGridParseResult loadSigGridString(
    const std::string& contents, const std::string& sourceName = "<string>");

/// Parse a SIGDATA directory: <dir>/SIGDATA.LST + RCSDAT/, IR/,
/// VISUAL/ subdirectories.
[[nodiscard]] SigParseResult loadSignatureDataDir(const std::string& dir);

} // namespace f4::convert
