// f4-simulation/include/f4/simulation/scenario.hpp
//
// Scenario — a JSON-described initial state for a simulation run.
//
// A scenario specifies:
//   - Which aircraft to spawn (callsign, config path, visType, parking spot)
//   - Which airfield is active (runway, threshold, taxi route)
//   - Simulation parameters (dt, total ticks, recording)
//   - Asset paths (terrain, models, aircraft config) — resolved relative to
//     the scenario file's parent directory.
//
// The scenario JSON is hand-authored for the first cut. A future extension
// (see taxi plan §7.1) will derive scenarios from campaign data + the
// airbase's GroundLayoutComponent.
//
// Dependencies: f4-geo (WorldPosition), f4-json (Reader), stdlib.
// C++20.

#pragma once

#include <f4/geo/position.hpp>

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace f4::simulation {

/// One aircraft in the scenario.
struct ScenarioAircraft {
    std::string callsign;               ///< "EAGLE1"
    std::string aircraft_config_path;   ///< "f16.json" (relative to scenario dir)
    std::string aircraft_name;          ///< "F-16C_50" (display name)
    int         vis_type_index{0};      ///< 1052 (F-16's index into KoreaObj.HDR parent table)
    geo::WorldPosition parking_spot{};  ///< ENU feet, relative to theater datum
    double      heading_rad{0.0};       ///< initial magnetic heading
    double      initial_fuel_lbs{0.0};
};

/// The active airfield for the scenario.
struct ScenarioAirfield {
    int active_runway_id{36};           ///< runway number (36, 18, etc.)
    std::string active_runway_name;     ///< "Rwy 36L"
    double runway_heading_rad{0.0};     ///< magnetic heading, radians
    geo::WorldPosition threshold_position{};     ///< runway threshold (ENU feet)
    geo::WorldPosition runway_end_position{};    ///< far end of runway (ENU feet)
    double threshold_altitude_ft{0.0};           ///< threshold elevation MSL
    double departure_altitude_ft{2500.0};        ///< climb-to altitude before "Done"
    std::vector<geo::WorldPosition> taxi_route;  ///< parking -> hold short -> threshold
};

/// A complete scenario.
struct Scenario {
    std::string name;                   ///< "takeoff_kunsan"
    std::string theater;                ///< "korea"

    // Asset paths (relative to the scenario file's parent directory).
    std::filesystem::path terrain_json_path;
    std::filesystem::path models_hdr_path;   ///< KoreaObj.HDR
    std::filesystem::path models_lod_path;   ///< KoreaObj.LOD
    std::filesystem::path models_tex_path;   ///< KoreaObj.TEX

    std::vector<ScenarioAircraft> aircraft;
    ScenarioAirfield airfield;

    double sim_dt{1.0 / 60.0};          ///< tick duration (seconds)
    int    total_ticks{600};            ///< 60 * 600 = 10 min default
    bool   record{true};                ///< write trace.json on exit
    std::filesystem::path record_path;  ///< "trace.json"
};

/// Load a scenario from a JSON file. Resolves asset paths relative to the
/// scenario file's parent directory. Throws std::runtime_error on parse
/// failure, missing required fields, or invalid values (e.g. taxi route
/// with < 2 waypoints, no aircraft, etc.).
Scenario load_scenario(const std::filesystem::path& json_path);

/// Load a scenario from an in-memory JSON string. Asset paths are returned
/// as-is (NOT resolved against a scenario directory). Used for testing.
Scenario load_scenario_from_string(const std::string& json);

} // namespace f4::simulation
