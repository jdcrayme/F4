// f4-convert/dat_parser.hpp
//
// Parser for the legacy FreeFalcon .dat aircraft definition file format.
//
// The .dat format is a flat sequence of whitespace-separated tokens with
// '#' line comments. Some sections (the AuxAeroData block at the bottom) use
// a key/value form ("normSpoolRate 3.0"); the top sections are positional.
//
// The parser is intentionally permissive: unknown keys and unknown sections
// are skipped silently (or recorded as warnings) rather than aborting. Every
// key/value pair from the AuxAeroData section is captured verbatim into
// `cfg.rawAuxAeroData`, guaranteeing no data loss in the .dat -> JSON
// round-trip.
//
// Ported from F4Flight's dat_loader.cpp, which is itself a clean port of
// FreeFalcon's readin.cpp. Behaviour verified against the FF source as the
// baseline truth for functionality.

#pragma once

#include "f4/data/aircraft_config.hpp"

#include <string>
#include <vector>

namespace f4::convert {

// Result of parsing a .dat file.
struct ParseResult {
    f4::data::AircraftConfig config;
    std::vector<std::string> warnings;   // non-fatal issues (skipped sections, etc.)
    std::vector<std::string> errors;     // fatal issues (would not produce a usable config)
    bool ok = false;
};

// Parse a Falcon 4 .dat file from disk. Returns a ParseResult; check
// `result.ok` before using `result.config`.
[[nodiscard]] ParseResult loadFile(const std::string& path);

// Parse a Falcon 4 .dat file from an in-memory string (mainly for tests).
// sourceName is used in error/warning messages (typically the file path).
[[nodiscard]] ParseResult loadString(const std::string& contents, const std::string& sourceName = "<string>");

} // namespace f4::convert
