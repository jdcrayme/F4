// f4-simulation/src/scenario.cpp
//
// Scenario JSON loader. Uses f4::json::Reader (the dependency-free recursive-
// descent parser) to walk the scenario JSON schema.
//
// The schema (see f4-scenario-player/scenarios/takeoff_kunsan.json for an example):
//   {
//     "name": "takeoff_kunsan",
//     "theater": "korea",
//     "terrain_json_path": "korea.terrain.json",
//     "models_hdr_path": "KoreaObj.HDR",
//     "models_lod_path": "KoreaObj.LOD",
//     "models_tex_path": "KoreaObj.TEX",
//     "aircraft": [ { "callsign", "aircraft_config_path", "aircraft_name",
//                     "vis_type_index", "parking_spot": {x,y,z}, "heading_rad",
//                     "initial_fuel_lbs", "team": "blue|red" } ],
//     "airfield": { "active_runway_id", "active_runway_name", "runway_heading_rad",
//                   "threshold_position": {x,y,z}, "runway_end_position": {x,y,z},
//                   "threshold_altitude_ft", "departure_altitude_ft",
//                   "taxi_route": [ {x,y,z}, ... ],
//                   "taxi_in_route": [ {x,y,z}, ... ] (optional) },
//     "waypoints": [ { "name", "position": {x,y,z}, "speed_kts" } ] (optional),
//     "sim_dt": 0.0166667, "total_ticks": 600,
//     "record": true, "record_path": "trace.json"
//   }
//
// All numeric positions are ENU feet (east, north, up) relative to the theater datum.

#include "f4/simulation/scenario.hpp"

#include <f4/json/reader.hpp>
#include <f4/io/read_file.hpp>

#include <stdexcept>
#include <string>

namespace f4::simulation {

namespace {

// Read a JSON object of the form { "x": ..., "y": ..., "z": ... } into a
// geo::WorldPosition. The reader must be positioned at the opening '{'.
geo::WorldPosition read_world_position(f4::json::Reader& r) {
    geo::WorldPosition p{};
    r.expect('{');
    bool first = true;
    while (!r.consume('}')) {
        if (!first) r.expect(',');
        first = false;
        const auto key = r.read_string();
        r.expect(':');
        const double v = r.read_number();
        if (key == "x" || key == "east_ft")      p.x = v;
        else if (key == "y" || key == "north_ft") p.y = v;
        else if (key == "z" || key == "alt_msl_ft" || key == "alt_ft") p.z = v;
        // silently ignore unknown keys (forward-compat)
    }
    return p;
}

// Skip a value we don't recognize (forward-compat for unknown scenario keys).
void skip_unknown(f4::json::Reader& r) {
    r.skip_value();
}

ScenarioAircraft read_aircraft(f4::json::Reader& r) {
    ScenarioAircraft a{};
    r.expect('{');
    bool first = true;
    while (!r.consume('}')) {
        if (!first) r.expect(',');
        first = false;
        const auto key = r.read_string();
        r.expect(':');
        if (key == "callsign")               a.callsign = r.read_string();
        else if (key == "aircraft_config_path") a.aircraft_config_path = r.read_string();
        else if (key == "aircraft_name")     a.aircraft_name = r.read_string();
        else if (key == "vis_type_index")    a.vis_type_index = static_cast<int>(r.read_int());
        else if (key == "parking_spot")      a.parking_spot = read_world_position(r);
        else if (key == "heading_rad")       a.heading_rad = r.read_number();
        else if (key == "initial_fuel_lbs")  a.initial_fuel_lbs = r.read_number();
        else if (key == "parking") {         // "auto": derived spot at sim init
            const auto mode = r.read_string();
            if (mode == "auto") a.parking_auto = true;
            else throw std::runtime_error("scenario: aircraft '" + a.callsign +
                "' has unknown parking mode '" + mode + "'");
        }
        else if (key == "parking_index")     a.parking_index = static_cast<int>(r.read_int());
        else if (key == "initial_vt_fps")   a.initial_vt_fps = r.read_number();
        else if (key == "spawn_in_air")     a.spawn_in_air = r.read_bool();
        else if (key == "team") {
            a.team = r.read_string();
            if (a.team != "blue" && a.team != "red")
                throw std::runtime_error("scenario: aircraft '" + a.callsign +
                    "' has unknown team '" + a.team + "' (blue|red)");
        }
        else if (key == "hold_fire")       a.hold_fire = r.read_bool();
        else if (key == "lead_callsign")    a.lead_callsign = r.read_string();
        else                                 skip_unknown(r);
    }
    return a;
}

ScenarioFeature read_feature(f4::json::Reader& r) {
    ScenarioFeature f{};
    r.expect('{');
    bool first = true;
    while (!r.consume('}')) {
        if (!first) r.expect(',');
        first = false;
        const auto key = r.read_string();
        r.expect(':');
        if (key == "name")             f.name = r.read_string();
        else if (key == "vis_type_index") f.vis_type_index = static_cast<int>(r.read_int());
        else if (key == "position")    f.position = read_world_position(r);
        else if (key == "heading_rad") f.heading_rad = r.read_number();
        else                           skip_unknown(r);
    }
    return f;
}

ScenarioWaypoint read_waypoint(f4::json::Reader& r) {
    ScenarioWaypoint w{};
    r.expect('{');
    bool first = true;
    while (!r.consume('}')) {
        if (!first) r.expect(',');
        first = false;
        const auto key = r.read_string();
        r.expect(':');
        if (key == "name")            w.name = r.read_string();
        else if (key == "position")   w.position = read_world_position(r);
        else if (key == "speed_kts")  w.speed_kts = r.read_number();
        else                          skip_unknown(r);
    }
    return w;
}

ScenarioAirfield read_airfield(f4::json::Reader& r) {
    ScenarioAirfield af{};
    r.expect('{');
    bool first = true;
    while (!r.consume('}')) {
        if (!first) r.expect(',');
        first = false;
        const auto key = r.read_string();
        r.expect(':');
        if (key == "active_runway_id")         af.active_runway_id = static_cast<int>(r.read_int());
        else if (key == "active_runway_name")  af.active_runway_name = r.read_string();
        else if (key == "runway_heading_rad")  af.runway_heading_rad = r.read_number();
        else if (key == "threshold_position")  af.threshold_position = read_world_position(r);
        else if (key == "runway_end_position") af.runway_end_position = read_world_position(r);
        else if (key == "threshold_altitude_ft")   af.threshold_altitude_ft = r.read_number();
        else if (key == "departure_altitude_ft") {
            af.departure_altitude_ft = r.read_number();
            af.departure_overridden = true;
        }
        else if (key == "taxi_route" || key == "taxi_in_route") {
            auto& route = (key == "taxi_route") ? af.taxi_route : af.taxi_in_route;
            r.expect('[');
            bool arr_first = true;
            while (!r.consume(']')) {
                if (!arr_first) r.expect(',');
                arr_first = false;
                route.push_back(read_world_position(r));
            }
        } else {
            skip_unknown(r);
        }
    }
    return af;
}

Scenario parse_scenario(f4::json::Reader& r) {
    Scenario s{};
    r.expect('{');
    bool first = true;
    while (!r.consume('}')) {
        if (!first) r.expect(',');
        first = false;
        const auto key = r.read_string();
        r.expect(':');
        if (key == "name")                s.name = r.read_string();
        else if (key == "theater")        s.theater = r.read_string();
        else if (key == "terrain_json_path") s.terrain_json_path = r.read_string();
        else if (key == "theater_dir")       s.theater_dir = r.read_string();
        else if (key == "models_hdr_path")   s.models_hdr_path = r.read_string();
        else if (key == "models_lod_path")   s.models_lod_path = r.read_string();
        else if (key == "models_tex_path")   s.models_tex_path = r.read_string();
        else if (key == "world_json_path")   s.world_json_path = r.read_string();
        else if (key == "class_table_path")  s.class_table_path = r.read_string();
        else if (key == "spawn_mode") {
            const auto mode = r.read_string();
            if (mode == "scenario_list")         s.spawn_mode = SpawnMode::ScenarioList;
            else if (mode == "campaign_flights") s.spawn_mode = SpawnMode::CampaignFlights;
            else throw std::runtime_error("scenario: unknown spawn_mode '" + mode + "'");
        }
        else if (key == "sim_dt")          s.sim_dt = r.read_number();
        else if (key == "total_ticks")     s.total_ticks = static_cast<int>(r.read_int());
        else if (key == "record")          s.record = r.read_bool();
        else if (key == "record_path")     s.record_path = r.read_string();
        else if (key == "fcs_trace_path")  s.fcs_trace_path = r.read_string();
        else if (key == "start_in_approach") s.start_in_approach = r.read_bool();
        else if (key == "start_enroute")     s.start_enroute = r.read_bool();
        else if (key == "aircraft") {
            r.expect('[');
            bool arr_first = true;
            while (!r.consume(']')) {
                if (!arr_first) r.expect(',');
                arr_first = false;
                s.aircraft.push_back(read_aircraft(r));
            }
        } else if (key == "airfield") {
            s.airfield = read_airfield(r);
        } else if (key == "airbase_source") {
            s.has_airbase_source = true;
            r.expect('{');
            bool firstk = true;
            while (!r.consume('}')) {
                if (!firstk) r.expect(',');
                firstk = false;
                const auto k = r.read_string();
                r.expect(':');
                if (k == "world_json")          s.airbase_source.world_json_path = r.read_string();
                else if (k == "class_table")     s.airbase_source.class_table_path = r.read_string();
                else if (k == "grid_x")         s.airbase_source.grid_x = static_cast<int>(r.read_int());
                else if (k == "grid_y")         s.airbase_source.grid_y = static_cast<int>(r.read_int());
                else if (k == "name")           s.airbase_source.name = r.read_string();
                else if (k == "active_heading_deg")
                    s.airbase_source.active_heading_deg = static_cast<int>(r.read_int());
                else                            skip_unknown(r);
            }
        } else if (key == "waypoints") {
            r.expect('[');
            bool arr_first = true;
            while (!r.consume(']')) {
                if (!arr_first) r.expect(',');
                arr_first = false;
                s.waypoints.push_back(read_waypoint(r));
            }
        } else if (key == "waypoints_frame") {
            const auto f = r.read_string();
            if (f == "runway")       s.waypoints_runway_frame = true;
            else if (f == "enu")     s.waypoints_runway_frame = false;
            else throw std::runtime_error("scenario: unknown waypoints_frame '" + f + "'");
        } else if (key == "approach") {
            const auto f = r.read_string();
            if (f == "pattern")          s.approach_mode = "pattern";
            else if (f == "straight_in") s.approach_mode = "straight_in";
            else throw std::runtime_error("scenario: unknown approach '" + f + "'");
        } else if (key == "combat") {
            r.expect('{');
            bool cfirst = true;
            while (!r.consume('}')) {
                if (!cfirst) r.expect(',');
                cfirst = false;
                const auto k = r.read_string();
                r.expect(':');
                if (k == "enabled")            s.combat.enabled = r.read_bool();
                else if (k == "radar_rng_seed") s.combat.radar_rng_seed =
                    static_cast<std::uint32_t>(r.read_int());
                else if (k == "fighter_hit_points") s.combat.fighter_hit_points = r.read_number();
                else if (k == "bvr_hold")      s.combat.bvr_hold = r.read_bool();
                else if (k == "missiles_hold") s.combat.missiles_hold = r.read_bool();
                else if (k == "guns_hold")     s.combat.guns_hold = r.read_bool();
                else                            skip_unknown(r);
            }
        } else if (key == "fuel") {
            r.expect('{');
            bool ffirst = true;
            while (!r.consume('}')) {
                if (!ffirst) r.expect(',');
                ffirst = false;
                const auto k = r.read_string();
                r.expect(':');
                if (k == "joker_lbs")      s.fuel.joker_lbs = r.read_number();
                else if (k == "bingo_lbs") s.fuel.bingo_lbs = r.read_number();
                else                       skip_unknown(r);
            }
        } else if (key == "airfield_features") {
            r.expect('[');
            bool arr_first = true;
            while (!r.consume(']')) {
                if (!arr_first) r.expect(',');
                arr_first = false;
                s.airfield_features.push_back(read_feature(r));
            }
        } else {
            skip_unknown(r);
        }
    }
    return s;
}

void validate(const Scenario& s) {
    if (s.name.empty())
        throw std::runtime_error("scenario: missing required field 'name'");
    if (s.sim_dt <= 0.0)
        throw std::runtime_error("scenario: sim_dt must be positive");
    if (s.total_ticks <= 0)
        throw std::runtime_error("scenario: total_ticks must be positive");

    // Validate airfield_features (optional but if present must be well-formed).
    for (const auto& f : s.airfield_features) {
        if (f.vis_type_index <= 0)
            throw std::runtime_error("scenario: feature '" + f.name +
                "' has invalid vis_type_index (must be > 0)");
    }

    if (s.has_airbase_source && s.airbase_source.world_json_path.empty()) {
        throw std::runtime_error("scenario: airbase_source requires 'world_json'");
    }

    // Validate the flight plan (optional, but if present must be flyable).
    for (std::size_t i = 0; i < s.waypoints.size(); ++i) {
        const auto& w = s.waypoints[i];
        if (w.speed_kts <= 0.0)
            throw std::runtime_error("scenario: waypoint[" + std::to_string(i) +
                "] ('" + w.name + "') has invalid speed_kts (must be > 0)");
    }

    // Fuel policy: negative thresholds make no sense, and a joker BELOW
    // the bingo would flip both flags on the same pound — loud, early.
    if (s.fuel.joker_lbs < 0.0 || s.fuel.bingo_lbs < 0.0)
        throw std::runtime_error("scenario: fuel thresholds must be >= 0");
    if (s.fuel.joker_lbs > 0.0 && s.fuel.bingo_lbs > 0.0 &&
        s.fuel.joker_lbs < s.fuel.bingo_lbs)
        throw std::runtime_error(
            "scenario: fuel joker_lbs must be >= bingo_lbs (joker fires first)");

    // Spawn-mode-specific validation.
    if (s.spawn_mode == SpawnMode::ScenarioList) {
        if (s.aircraft.empty())
            throw std::runtime_error("scenario: no aircraft defined (need at least one)");
        for (const auto& a : s.aircraft) {
            if (a.callsign.empty())
                throw std::runtime_error("scenario: aircraft missing 'callsign'");
            if (a.aircraft_config_path.empty())
                throw std::runtime_error("scenario: aircraft '" + a.callsign + "' missing 'aircraft_config_path'");
            if (a.vis_type_index <= 0)
                throw std::runtime_error("scenario: aircraft '" + a.callsign + "' has invalid vis_type_index");
        }
        // taxi_route minimum applies only to hand-authored airfields —
        // when airbase_source is set the route is derived at sim init.
        if (s.airfield.taxi_route.size() < 2 && !s.has_airbase_source)
            throw std::runtime_error("scenario: taxi_route must have at least 2 waypoints (start + threshold)");
    } else if (s.spawn_mode == SpawnMode::CampaignFlights) {
        if (s.world_json_path.empty())
            throw std::runtime_error("scenario: spawn_mode=campaign_flights requires 'world_json_path'");
        if (s.class_table_path.empty())
            throw std::runtime_error("scenario: spawn_mode=campaign_flights requires 'class_table_path'");
        if (s.aircraft.empty() || s.aircraft.front().aircraft_config_path.empty())
            throw std::runtime_error(
                "scenario: spawn_mode=campaign_flights requires aircraft[0].aircraft_config_path "
                "(used as the shared config for all spawned aircraft)");
    }
}

// Resolve a relative path against the scenario file's parent directory.
// Absolute paths are returned unchanged. Strings starting with
// `@asset:` are returned unchanged — they're asset-pipeline references
// (Stage 2, ASSET_PIPELINE_SPEC.md §4) that the consumer resolves
// through an AssetRoot at runtime, not relative paths.
std::filesystem::path resolve(const std::filesystem::path& base_dir,
                              const std::filesystem::path& p) {
    if (p.empty()) return p;
    const std::string s = p.string();
    if (s.size() >= 7 && s.substr(0, 7) == "@asset:") {
        return p;  // asset reference — preserved for runtime resolution
    }
    return p.is_absolute() ? p : (base_dir / p);
}

} // namespace

Scenario load_scenario_from_string(const std::string& json) {
    f4::json::Reader r(json);
    Scenario s = parse_scenario(r);
    validate(s);
    return s;
}

Scenario load_scenario(const std::filesystem::path& json_path) {
    // Read the file. f4::io::read_file throws on I/O error with a clear message.
    const auto bytes = f4::io::read_file(json_path, "scenario");
    std::string json(bytes.begin(), bytes.end());

    Scenario s = load_scenario_from_string(json);

    // Resolve asset paths relative to the scenario file's parent directory.
    const auto base_dir = json_path.parent_path();
    s.terrain_json_path = resolve(base_dir, s.terrain_json_path);
    s.theater_dir       = resolve(base_dir, s.theater_dir);
    s.models_hdr_path   = resolve(base_dir, s.models_hdr_path);
    s.models_lod_path   = resolve(base_dir, s.models_lod_path);
    s.models_tex_path   = resolve(base_dir, s.models_tex_path);
    s.world_json_path   = resolve(base_dir, s.world_json_path);
    s.airbase_source.world_json_path = resolve(base_dir, s.airbase_source.world_json_path);
    s.airbase_source.class_table_path = resolve(base_dir, s.airbase_source.class_table_path);
    s.class_table_path  = resolve(base_dir, s.class_table_path);
    s.record_path       = resolve(base_dir, s.record_path);
    s.fcs_trace_path    = resolve(base_dir, s.fcs_trace_path);

    // Resolve aircraft config paths too.
    for (auto& a : s.aircraft) {
        a.aircraft_config_path = resolve(base_dir, a.aircraft_config_path).string();
    }

    return s;
}

} // namespace f4::simulation
