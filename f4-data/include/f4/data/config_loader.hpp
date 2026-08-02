// f4-data/config_loader.hpp
//
// Loads aircraft configuration from JSON files (produced by f4-convert's
// dat2json tool). The JSON format is a direct 1:1 mapping of the
// AircraftConfig struct.
//
// f4-data depends on:
//   - nlohmann/json  (for JSON parsing)
//   - f4-math        (for Table2D table accessors, optional)
//   - f4-units       (NOT yet — typed Quantity accessors will be added when
//                     f4-flight-model lands and we know what units the
//                     consumers actually need)

#pragma once

#include "f4/data/aircraft_config.hpp"

#include <string>
#include <vector>

namespace f4::data {

struct LoadResult {
    AircraftConfig config;
    bool ok = false;
    std::vector<std::string> errors;
};

/// Load an AircraftConfig from a JSON file on disk.
LoadResult loadConfig(const std::string& path);

/// Load an AircraftConfig from a JSON string.
LoadResult loadConfigFromString(const std::string& json);

/// Serialize an AircraftConfig to a JSON string (pretty-printed).
std::string writeConfig(const AircraftConfig& config);

/// Write an AircraftConfig to a JSON file. Returns true on success.
bool writeConfig(const AircraftConfig& config, const std::string& path);

} // namespace f4::data
