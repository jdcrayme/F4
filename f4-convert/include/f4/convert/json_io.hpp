// f4-convert/json_io.hpp
//
// JSON serialization for AircraftConfig, using nlohmann/json.
//
// The JSON format is a direct 1:1 mapping of the AircraftConfig struct.
// Arrays of doubles are written as JSON arrays of numbers. The
// rawAuxAeroData map is written as a JSON object (key -> string), preserving
// the verbatim .dat capture for no-loss round-trip fidelity.
//
// Replaces F4Flight's hand-rolled JSON writer/reader with the standard
// nlohmann/json library, eliminating an entire class of escaping / parsing
// bugs.

#pragma once

#include "f4/data/aircraft_config.hpp"

#include <string>
#include <vector>

namespace f4::convert {

struct IoResult {
    bool ok = false;
    std::vector<std::string> errors;
    std::vector<std::string> warnings;
};

// Serialize an AircraftConfig to a JSON string. Pretty-printed (indented).
std::string writeJson(const f4::data::AircraftConfig& cfg);

// Write an AircraftConfig to a JSON file. Returns true on success.
bool writeJsonFile(const f4::data::AircraftConfig& cfg, const std::string& path);

// Parse a JSON string into an AircraftConfig. Returns IoResult; on success
// `result.ok` is true and `cfg` is populated.
IoResult readJson(const std::string& json, f4::data::AircraftConfig& cfg);

// Read a JSON file into an AircraftConfig.
IoResult readJsonFile(const std::string& path, f4::data::AircraftConfig& cfg);

// Compare two AircraftConfigs field-by-field with floating-point tolerance.
// Returns a list of human-readable diff lines (empty if equivalent).
// Used by the round-trip test harness and the json_diff CLI tool.
std::vector<std::string> diffConfigs(const f4::data::AircraftConfig& a,
                                      const f4::data::AircraftConfig& b,
                                      double tolerance = 1e-12);

} // namespace f4::convert
