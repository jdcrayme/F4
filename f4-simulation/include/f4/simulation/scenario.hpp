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
#include <f4/entities/types.hpp>   // GroundLayoutList (real-airbase rendering)

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
    /// "auto": take parking_spot + heading from the derived airfield's
    /// synthesized spots (ignored when airbase_source is absent).
    bool parking_auto{false};
    int parking_index{0};               ///< which derived spot (0-based)
    /// Initial true airspeed at spawn (ft/s). Default 0 = parked. Set to a
    /// small positive value (~5 ft/s) for ground spawns to avoid the
    /// first-tick transient where qsom=0 forces the FCS into the ground
    /// guard (FLIGHT_CONTROL_NEXT_STEPS.md §3.3 Phase 0d).
    double initial_vt_fps{0.0};
    /// Spawn in air (true) or on the ground (false). When true, the FM
    /// is initialized with inAir=true and the gear extended; the brain
    /// still starts in Phase::Ground (the host must arrange for the
    /// mission to sequence to the desired phase). Used by the isolated
    /// `landing_only` scenario to skip the taxi/takeoff roll.
    bool spawn_in_air{false};
    /// Team affiliation ("blue"/"red"), written to the entity's TEAM tag
    /// and consumed by IFF (TrackStore), RWR role checks, and missile
    /// team-copying. Default "blue". Added with the combat integration
    /// (COMBAT_CHAIN_PLAN.md M3) — harmless before it: nothing read the
    /// tag for scenario-list aircraft.
    std::string team{"blue"};
};

/// One parking spot on the airfield (derived from the ground layout —
/// real PHD data where available, synthesized at the ramp end of the taxi
/// polyline when the theater DB has no parking lists).
struct ScenarioParkingSpot {
    geo::WorldPosition position;        ///< ENU feet
    double heading_rad{0.0};            ///< facing (compass, ready to taxi out)
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
    bool departure_overridden{false};            ///< JSON carried an explicit value
    std::vector<geo::WorldPosition> taxi_route;  ///< parking -> hold short (last wp IS the hold-short)
    /// Taxi-in route after landing: runway exit -> parking. Optional —
    /// when empty the LandingModule parks on the runway (M3 may require it).
    std::vector<geo::WorldPosition> taxi_in_route;

    /// Runway dimensions from PLT_RUNWAY_DIM (0 = unknown; renderers fall
    /// back to a default width and threshold->end length).
    double runway_length_ft{0.0};
    double runway_width_ft{0.0};

    /// Parking spots (derived; empty for hand-authored scenarios).
    std::vector<ScenarioParkingSpot> parking_spots;
};

/// One waypoint in the scenario's flight plan (air phase). Flown in order
/// by the NavigationModule after departure. The LAST waypoint is the
/// approach entry fix handed to the LandingModule.
struct ScenarioWaypoint {
    std::string name;                   ///< "WP1", "APCH_FIX" (display + trace)
    geo::WorldPosition position;        ///< ENU feet; z = target altitude MSL
    double speed_kts{350.0};            ///< target CAS approaching this waypoint
};

/// Combat configuration (COMBAT_CHAIN_PLAN.md M3 integration).
/// When enabled, spawned scenario-list aircraft carry the combat component
/// set (WeaponStoreComponent + SignatureComponent + RadarSimComponent +
/// RwrComponent + DamageStateComponent — see combat_bridge.hpp) and the
/// Simulation tick drives the sensor/weapon sweeps (sim-time stamping,
/// update_rwr, sweep_spent_missiles). When disabled (the default), the
/// world is exactly what it was before the combat chain existed.
struct CombatConfig {
    bool enabled{false};
    /// Seed for every spawned RadarSimComponent's detection RNG. Same seed
    /// + same scenario => same detection sequence (the reproducibility
    /// discipline the whole sim runs on). Per-aircraft seeds are derived
    /// from this base so radars don't roll identical sequences.
    std::uint32_t radar_rng_seed{0x46344u};
    /// Hit points given to spawned aircraft's DamageStateComponent (the
    /// light-fighter strength the M1 engagement tests calibrated against).
    double fighter_hit_points{25.0};
};

/// One static feature placement on the airfield — a building, runway section,
/// taxiway segment, control tower, hangar, etc. Spawned as an entity with
/// TransformComponent + VisualModelComponent (no flight model, no brain).
///
/// The vis_type_index is the direct index into KoreaObj.HDR's parent table
/// (same keying model as ScenarioAircraft — we don't go through the
/// Falcon4.CT entity_type → vis_type[0] lookup for hand-authored scenarios).
/// A future campaign-derivation phase can add entity_type-based resolution.
struct ScenarioFeature {
    std::string name;                   ///< "Control Tower", "Runway Section 1"
    int         vis_type_index{0};      ///< index into KoreaObj.HDR parent table
    geo::WorldPosition position{};      ///< ENU feet, relative to theater datum
    double      heading_rad{0.0};       ///< facing (rotation about Z-up)
};

/// How the Simulation should spawn aircraft for this scenario.
///   - `ScenarioList`: spawn exactly the aircraft listed in `Scenario::aircraft`
///     (Phase 1 behavior — hand-authored parking spots).
///   - `CampaignFlights`: ignore `Scenario::aircraft` and instead load the
///     referenced `world_json_path`, find every Flight-class unit, and spawn
///     a child aircraft entity per flight at the squadron's airbase parking
///     spot (Phase 2 — campaign-derived roster).
enum class SpawnMode {
    ScenarioList,
    CampaignFlights,
};

/// Where a scenario's airfield comes from: a real campaign world JSON.
/// The Simulation loads the referenced objective's ground layout at
/// initialize() and DERIVES the airfield (runway, taxi routes, parking)
/// from it, overriding any hand-authored airfield block.
struct ScenarioAirbaseSource {
    std::filesystem::path world_json_path;  ///< world JSON (cam2json output)
    /// FALCON4.ct — resolves FeatureEntryState.index -> KoreaObj vis_type
    /// for real 3D building models at the airbase. Optional: without it
    /// the layout geometry renders but no feature models spawn.
    std::filesystem::path class_table_path;
    /// Objective selection: grid coordinates (exact match preferred) or a
    /// name substring fallback ("Kunsan").
    int grid_x{-1};
    int grid_y{-1};
    std::string name;                       ///< substring match fallback
    int active_heading_deg{20};             ///< desired runway direction (020)
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

    /// Optional: the theater directory (e.g. <install>/terrdata/korea)
    /// with raw binary terrain + texture data (terrain/THEATER.L*,
    /// texture/TEXTURE.BIN + texture.zip). When present, viewers load
    /// the post levels + tile databases and render TEXTURED terrain;
    /// when absent they fall back to the terrain JSON (vertex colors).
    /// Empty string in authored scenarios = not configured.
    std::filesystem::path theater_dir;

    // Phase 2: campaign-derivation inputs. Used only when spawn_mode ==
    // CampaignFlights. The world_json_path points at a world JSON produced
    // by f4-world-convert's cam2json (it carries objectives + units + the
    // airbase ground layout). The class_table_path points at Falcon4.CT
    // (needed to resolve entity_type → vis_type[0] for the spawned aircraft).
    std::filesystem::path world_json_path;
    std::filesystem::path class_table_path;

    SpawnMode spawn_mode{SpawnMode::ScenarioList};

    std::vector<ScenarioAircraft> aircraft;
    ScenarioAirfield airfield;

    /// Real-airbase source (optional). When set, Simulation::initialize
    /// derives scenario_.airfield from the referenced objective's ground
    /// layout (see campaign_bridge) and populates layout_lists/layout_center
    /// below for the renderer.
    ScenarioAirbaseSource airbase_source;
    bool has_airbase_source{false};

    /// The real ground layout (objective-local lists) when derived from
    /// airbase_source — for the renderer (shared f4-renderer builder).
    /// layout_center is the objective's ENU position; list points are
    /// feet offsets from it.
    std::vector<f4::entities::GroundLayoutList> layout_lists;
    geo::WorldPosition layout_center{};

    /// Air-phase flight plan (optional). Empty = no route; the mission ends
    /// when TakeoffModule completes. When present, the NavigationModule
    /// flies the waypoints in order and hands off to the LandingModule at
    /// the last one (the approach entry fix).
    std::vector<ScenarioWaypoint> waypoints;

    /// Waypoint frame: false (default) = ENU absolute. true = runway frame:
    /// x = right of the runway heading, y = downrange from the threshold
    /// (feet); z stays MSL. Rotated into ENU by derive_real_airbase().
    /// Lets one template scenario fly any runway direction.
    bool waypoints_runway_frame{false};

    /// Approach style flown after the last waypoint: "straight_in"
    /// (default — the entry fix sits on the extended centerline and the
    /// module intercepts the final course directly) or "pattern" (full
    /// left-hand traffic pattern: downwind, base, final).
    std::string approach_mode{"straight_in"};
    [[nodiscard]] bool approach_is_pattern() const noexcept {
        return approach_mode == "pattern";
    }

    /// Skip the takeoff/navigation phases and start the brain in Approach.
    /// Used by the isolated `landing_only` scenario which spawns the aircraft
    /// airborne on final. See FLIGHT_CONTROL_NEXT_STEPS.md §3.2.
    /// When true, the first aircraft's spawn_in_air flag should also be true
    /// (the brain will hand off to the LandingModule immediately on the first
    /// tick; the FM needs to be airborne to fly).
    bool start_in_approach{false};

    /// NAV-D1: when true, spawn airborne and hand the route directly to the
    /// NavigationModule (skips taxi/takeoff). Used by the LNAV diagnostic
    /// scenarios (course_intercept, standard_rate_turn). Requires the first
    /// aircraft's spawn_in_air to be true and a non-empty waypoint route.
    bool start_enroute{false};

    /// Static feature placements on the airfield — buildings, runway sections,
    /// taxiways, towers, hangars. Spawned as TransformComponent +
    /// VisualModelComponent entities (no FM, no brain). The renderer iterates
    /// all VisualModelComponent-bearing entities, so these get drawn alongside
    /// the aircraft without any renderer-side special-casing.
    std::vector<ScenarioFeature> airfield_features;

    double sim_dt{1.0 / 60.0};          ///< tick duration (seconds)
    int    total_ticks{600};            ///< 60 * 600 = 10 min default
    bool   record{true};                ///< write trace.json on exit
    std::filesystem::path record_path;  ///< "trace.json"
    /// Optional per-tick FCS/AI/EOM CSV trace path (for control-loop diagnosis).
    /// When non-empty, Simulation writes a CSV with one row per tick per
    /// aircraft containing AI commands, FCS intermediates, body rates,
    /// kinematics, and navigation intent. See f4/recorder/fcs_trace.hpp.
    std::filesystem::path fcs_trace_path;

    /// Combat configuration (M3 integration). Default: disabled.
    CombatConfig combat;
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
