// f4-world/include/f4/world/detail/world_state.hpp
//
// DETAIL HEADER — WorldState is an implementation detail of the f4-world
// library's JSON loader and the format adapters. It is NOT part of the
// public API. External consumers should use EntityWorld + the bridge
// functions (load, populate_world, etc.) instead.
//
// This header is kept under detail/ so that:
//   - The umbrella f4_world.hpp does not expose WorldState
//   - Internal code (world_state.cpp, world_loader.hpp, file_ops.cpp)
//     can still include it explicitly
//   - Future data sources (BMS, DIS, procedural) never need this struct
//
// Typed WorldState — the consumer-side counterpart to f4-world-convert.
// f4-world-convert turns a binary .cam archive into JSON; f4-world loads
// that JSON into typed structs and can populate an f4-entities EntityWorld.
//
// Current scope: campaign header, team list, objectives (full), units
// (partial — fixed fields only), and a reference to a terrain JSON file
// (loaded separately via f4-terrain). As f4-world-convert learns to decode
// more sub-files (objective deltas, weather, events, flight plans), this
// struct grows to carry them.
//
// SHARED TYPES: ObjectiveLink, GroundLayoutList, GroundLayoutPoint,
// FeatureEntryState, FeatureClassState, WaypointState, PilotState,
// VehicleGroup, and the UnitClass enum are now defined in
// f4-entities/include/f4/entities/types.hpp. This header re-exports them
// via using-declarations so existing code using f4::world::ObjectiveLink
// etc. continues to compile. New code should prefer the f4::entities
// namespace directly.

#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include <f4/entities/types.hpp>
#include <f4/terrain/terrain_data.hpp>

// Forward declaration — the asset-root overload of load_terrain takes
// a const ref. We don't want to leak f4-assets into every consumer of
// f4-world, so we forward-declare here and include the full header in
// the .cpp only. Consumers that call load_terrain_via_assets() must
// include <f4/assets/asset_root.hpp> themselves.
namespace f4::assets { class AssetRoot; }

namespace f4::world {

// ============================================================================
// Re-export shared types from f4::entities into f4::world for backward
// compatibility. Existing code that uses f4::world::ObjectiveLink etc.
// continues to work. New code should use f4::entities::ObjectiveLink.
// ============================================================================
using ObjectiveLink      = f4::entities::ObjectiveLink;
using GroundLayoutList   = f4::entities::GroundLayoutList;
using GroundLayoutPoint  = f4::entities::GroundLayoutPoint;
using FeatureEntryState  = f4::entities::FeatureEntryState;
using FeatureClassState  = f4::entities::FeatureClassState;
using WaypointState      = f4::entities::WaypointState;
using PilotState         = f4::entities::PilotState;
using VehicleGroup       = f4::entities::VehicleGroup;
using UnitClass          = f4::entities::UnitClass;
// unit_class_name() is available as f4::entities::unit_class_name().
// Code that previously used f4::world::unit_class_name should switch
// to f4::entities::unit_class_name (or rely on ADL via unqualified call
// when the UnitClass argument is f4::entities::UnitClass).

struct TeamState {
    int slot = 0;               // 0..7
    uint8_t flags = 0;
    uint8_t colour = 0;
    std::string name;           // e.g. "ROK", "Japan", "PRC"
    std::string motto;

    // --- Enrichment from .tea (TeamRecord) ---
    // These fields are emitted by f4-world-convert's world_json when a
    // .tea sub-file was successfully decoded. They're optional — fresh
    // campaign saves without .tea data will leave them at defaults.
    // `tea_loaded` is true when the .tea block was found and parsed for
    // this team slot; the viewer uses it to decide whether to render
    // the enrichment panel.
    bool     tea_loaded = false;
    int16_t  cteam = 0;          // current team (may differ from slot during realignment)
    int16_t  team_flags = 0;     // TeamRecord.flags (renamed to disambiguate from .cmp flags)
    std::vector<uint8_t>  member;  // NUM_COUNS country memberships (0/1 per slot)
    std::vector<int16_t>  stance;  // NUM_TEAMS stance values toward each other team
    int16_t  first_colonel = 0;     // pilot slot indices into the global pilot pool
    int16_t  first_commander = 0;
    int16_t  first_wingman = 0;
    int16_t  last_wingman = 0;
    uint8_t  air_experience = 0;          // 0..100
    uint8_t  air_defense_experience = 0;
    uint8_t  ground_experience = 0;
    uint8_t  naval_experience = 0;
};

struct CampaignState {
    int32_t current_time = 0;
    int32_t te_start_time = 0;
    int32_t te_time_limit = 0;
    int32_t te_victory_points = 0;
    int32_t te_type = 0;
    int32_t te_number_teams = 0;
    int32_t te_team = 0;
    int32_t te_flags = 0;
    std::vector<int32_t> te_number_aircraft;   // 8
    std::vector<int32_t> te_team_pts;           // 8
    // Bullseye (B.3 QC tranche): the campaign's shared reference point.
    // Grid coordinates; bullseye_name is the display name byte (0=none).
    int32_t bullseye_x = 0;
    int32_t bullseye_y = 0;
    int32_t bullseye_name = 0;
};

/// A campaign objective (airbase, bridge, city, port, ...). Mirrors the
/// fields decoded by f4-world-convert's objective_decoder.
struct ObjectiveState {
    int16_t  type = 0;          // entity_type (class table index, 100+)
    uint8_t  objective_type = 0; // ObjectiveType enum (1-39), 0 if unknown
    uint32_t id_creator = 0;
    uint32_t id_num = 0;
    uint16_t entity_type = 0;
    int16_t  x = 0;             // grid column (GridIndex)
    int16_t  y = 0;             // grid row
    float    z = 0.0f;          // altitude (feet)
    uint8_t  owner = 0;
    int16_t  camp_id = 0;
    uint8_t  priority = 0;
    int16_t  nameid = 0;
    // Logistics & state (decoded by f4-world-convert, now exposed):
    uint32_t obj_flags = 0;
    uint8_t  supply = 0;
    uint8_t  fuel = 0;
    uint8_t  losses = 0;
    int32_t  last_repair = 0;
    uint8_t  first_owner = 0;
    uint32_t parent_id = 0;     // VU_ID.num of parent objective (0 if none)
    // Per-feature damage bitmap (packed 2 bits per feature). Raw bytes —
    // interpreting requires the Features count from Falcon4.OCD.
    std::vector<uint8_t> fstatus;
    // Radar detection arcs (only populated when has_radar == true).
    bool     has_radar = false;
    float    detect_ratio[8] = {0.0f};
    // Phase 3: real radar range from Falcon4.RCD. Previously the viewer
    // used a fabricated 32-grid-unit constant. Populated when theater_db
    // is loaded and the OCD → FED → FCD → RCD chain resolves.
    float    radar_range_km = 0.0f;   // 0 = unknown / use fallback
    std::string radar_name;           // e.g. "APG-68", "Pat Hand"
    int16_t  radar_type_idx = -1;     // index into Falcon4.RCD
    std::vector<ObjectiveLink> links;   // road/rail network connections

    // --- Theater static-data enrichment (from Falcon4.OCD/PHD/PD) ---
    // Populated when the world JSON was built with a loaded TheaterObjectDatabase.
    // Empty/default when no static data was available.
    std::string class_name;           // e.g. "02_20 Airbase 2", "Highway Strip NS"
    uint8_t  features_count = 0;      // # of features (buildings, runways, etc.)
    uint8_t  radar_feature = 0;       // which feature provides radar (255=none)
    uint8_t  deag_distance = 0;       // distance at which to deaggregate
    uint16_t pt_data_index = 0;       // index into PHD table
    // OCD.Detection[8] — electronic detection ranges per movement type
    // (Foot/Wheeled/Tracked/LowAir/Air/Naval/Rail/...). Phase 1 fix A.7:
    // previously decoded by theater_data.cpp but never emitted.
    std::array<uint8_t, 8> objective_detection{};
    std::vector<GroundLayoutList> ground_layout;  // runway/taxiway/parking lists
    // Per-objective feature placements (from Falcon4.FED + FCD). Combined
    // with the fstatus bitmap above, gives the live damage state of every
    // building/runway/feature on this objective.
    std::vector<FeatureEntryState> features;
};

/// A campaign unit (flight, battalion, squadron, ship). Mirrors the fields
/// decoded by f4-world-convert's unit_decoder. Includes the subclass-specific
/// tail fields that are most useful for visualization and AI consumption.
///
/// \note INTERNAL — DO NOT consume directly from outside f4-world.
/// This struct is a tagged-union mirror of the binary campaign format and
/// is retained as an implementation detail of the WorldStateAdapter
/// (the bridge's concrete IUnitCoreSource implementation). New consumers
/// should read from `EntityWorld` components (`UnitCoreComponent`,
/// `GroundTacticalComponent`, `SquadronComponent`, `FlightPlanComponent`,
/// `PackageSupportComponent`) populated by `populate_world()`, or implement
/// `IUnitCoreSource` directly to plug in an alternative data source. See
/// ECS_DECOUPLING_PLAN.md §6 and test_interface_bridge.cpp for the
/// contract test.
struct UnitState {
    int16_t  type = 0;             // share_.entityType_ (100..2000)
    UnitClass unit_class = UnitClass::Unknown;
    uint8_t  unit_subtype = 0;     // STYPE_UNIT_* (armor/infantry/fighter/bomber/...)
    uint8_t  domain = 0;           // VU_DOMAIN (2=air, 3=land, 4=sea) — needed to interpret unit_subtype

    // CampBaseClass:
    uint32_t id_creator = 0;
    uint32_t id_num = 0;
    uint16_t entity_type = 0;
    int16_t  x = 0;                // grid column (GridIndex)
    int16_t  y = 0;                // grid row
    float    z = 0.0f;             // altitude (feet) — always 0 at v63
    uint8_t  owner = 0;            // Control: 0=neutral, 1=enemy, 2=friendly...
    int16_t  camp_id = 0;

    // UnitClass fixed:
    int16_t  dest_x = 0;
    int16_t  dest_y = 0;
    int16_t  name_id = 0;
    int16_t  reinforcement = 0;
    uint8_t  wp_count = 0;
    uint8_t  losses = 0;

    /// Per-group vehicle count, packed 2 bits/group × 16 groups (32 bits).
    /// GetNumVehicles(vg) = (roster >> (vg*2)) & 0x03 — max 3 vehicles per
    /// group, 16 groups, 48 vehicles max per battalion. Combined with the
    /// UnitClassDataType table (not yet parsed), this gives the live
    /// vehicle composition of the unit.
    uint32_t roster = 0;

    /// Waypoint list (flight plan / ground movement plan). Empty when
    /// wp_count == 0. Drawn on the canvas as a polyline.
    std::vector<WaypointState> waypoints;

    // Subclass-specific (only populated for the matching unit_class):
    uint8_t  supply = 0;           // Battalion / TaskForce / Brigade
    uint8_t  morale = 0;           // Battalion
    uint8_t  fatigue = 0;          // Battalion
    uint8_t  elements = 0;         // Brigade / Package
    int32_t  fuel = 0;             // Squadron

    // Hierarchy (Battalion/Brigade):
    uint32_t parent_id = 0;                    // Battalion → parent Brigade VU_ID.num
    std::vector<uint32_t> element_ids;         // Brigade → child Battalion VU_IDs

    // Battalion tactical state:
    int32_t  last_move = 0;        // CampaignTime of last move
    int32_t  last_combat = 0;      // CampaignTime of last combat
    uint8_t  heading = 0;          // current heading (0-255, *1.4 deg)
    uint8_t  final_heading = 0;    // commanded heading
    uint8_t  position = 0;         // formation position slot

    // Squadron:
    uint32_t airbase_id = 0;       // VU_ID.num of home airbase objective
    uint8_t  specialty = 0;
    int16_t  aa_kills = 0;
    int16_t  ag_kills = 0;
    int16_t  as_kills = 0;
    int16_t  an_kills = 0;
    int16_t  missions_flown = 0;
    int16_t  mission_score = 0;
    uint8_t  total_losses = 0;
    uint8_t  pilot_losses = 0;
    uint8_t  squadron_patch = 0;

    // Squadron pilot roster:
    std::vector<PilotState> pilots;

    // Flight (AirUnit subclass — Phase 1 fix: was decoded but never emitted):
    // A Flight is a single aircraft mission element (one package contains
    // multiple flights). These fields describe the mission state.
    float    flight_altitude = 0.0f;          // pos_.z_ (feet)
    int32_t  fuel_burnt = 0;                  // fuel consumed so far (lbs)
    int32_t  time_on_target = 0;              // CampaignTime
    int32_t  mission_over_time = 0;           // CampaignTime
    int16_t  mission_target = 0;              // target ID slot
    uint8_t  loadouts = 0;                    // # of loadout entries (one per aircraft)
    uint8_t  mission = 0;                     // MissionType enum (BARCAP, INTERCEPT, ...)
    uint8_t  flight_priority = 0;             // mission priority
    uint8_t  mission_id = 0;                  // mission instance ID
    uint8_t  eval_flags = 0;                  // post-mission evaluation state
    uint32_t package_id = 0;                  // VU_ID.num of parent Package
    uint32_t squadron_id = 0;                 // VU_ID.num of owning Squadron
    uint8_t  callsign_id = 0;                 // callsign pool index
    uint8_t  callsign_num = 0;                // callsign slot within pool

    // Package (AirUnit subclass — Phase 1 fix: was decoded but never emitted):
    // A Package groups multiple Flights into a coordinated strike/mission.
    uint8_t  wait_cycles = 0;                 // ATO wait cycles remaining
    uint32_t interceptor_id = 0;              // VU_ID.num of interceptor flight
    uint32_t awacs_id = 0;                    // VU_ID.num of AWACS flight
    uint32_t jstar_id = 0;                    // VU_ID.num of JSTARS flight
    uint32_t ecm_id = 0;                      // VU_ID.num of ECM flight
    uint32_t tanker_id = 0;                   // VU_ID.num of tanker flight

    // Package element flights + the ATM mission request that produced the
    // package (B.3 tranche — TestCamp.cam v71 carries both). element_ids is
    // shared with the Brigade field above (one wire vector, two meanings by
    // unit class); the request fields mirror the mis_request JSON block.
    bool     request_present = false;
    uint8_t  request_mission = 0;             // MissionType enum
    int32_t  request_tot = 0;                 // CampaignTime (absolute)
    uint8_t  request_priority = 0;
    uint16_t request_action_type = 0;
    uint32_t request_target_num = 0;          // VU_ID.num of requested target
    uint32_t request_target_creator = 0;      // VU_ID.creator
    uint32_t request_requester_num = 0;       // VU_ID.num of requester

    // --- Theater static-data enrichment (from Falcon4.UCD/VCD) ---
    // Populated when the world JSON was built with a loaded TheaterObjectDatabase.
    // Empty/default when no static data was available.
    std::string class_name;           // e.g. "Patrol", "Supply", "Armor Battalion"
    int32_t  movement_type = 0;       // MoveType (1=Foot, 2=Wheeled, 5=Air, 6=Naval, ...)
    std::string movement_type_name;   // human-readable
    int16_t  movement_speed = 0;      // kph or knots
    int16_t  max_range = 0;           // km
    std::vector<VehicleGroup> vehicle_groups;  // per-group vehicle composition

    // --- Unit class scores (Phase 1 fix — A.8: UnitClassData.scores[16]) ---
    // Per-mission-role scoring (16 uchar values from Falcon4.UCD.Scores).
    // Higher score = better suited for that mission role. Populated when
    // theater_db is loaded.
    std::array<uint8_t, 16> unit_class_scores{};
};

struct WorldState {
    int version = 0;                        // gCampDataVersion (from .ver)
    std::string theater;                    // theater name (e.g. "korea")
    std::string terrain_file;               // path to terrain JSON (relative)

    CampaignState campaign;
    std::vector<TeamState> teams;           // up to 8 slots
    std::vector<ObjectiveState> objectives;
    std::vector<UnitState> units;

    /// Optional loaded terrain. Populated by load_terrain(). When null,
    /// the consumer should fall back to a land-mask or skip terrain tiles.
    f4::terrain::TerrainData terrain;
    bool terrain_loaded = false;

    /// Load from a world JSON file (produced by f4-world-convert's cam2json).
    /// Throws on I/O or parse error. Does NOT load the referenced terrain
    /// file — call load_terrain() separately with terrain_file as a hint.
    void load(const std::filesystem::path& json_path);

    /// Load from an in-memory JSON string (for testing).
    void load_from_string(const std::string& json);

    /// Load the terrain JSON referenced by `terrain_file`. If `base_dir`
    /// is non-empty, the terrain_file path is resolved relative to it
    /// (typically the directory containing the world JSON). Throws on
    /// I/O or parse error. Sets terrain_loaded = true on success.
    ///
    /// If `terrain_file` starts with `@asset:` (the asset-pipeline form
    /// from ASSET_PIPELINE_SPEC.md §4), the loader requires the manifest
    /// to resolve the reference — call `load_terrain_via_assets()`
    /// instead, OR call this with an empty base_dir AND a world_json_dir_
    /// that contains a `Data/` tree (the legacy bare-filename form
    /// remains the default).
    void load_terrain(const std::filesystem::path& base_dir = "");

    /// Asset-pipeline overload: resolve `terrain_file` through the
    /// AssetRoot's manifest when it's in the `@asset:theater:<id>` form.
    /// Throws if `terrain_file` is NOT in the asset-ref form (call the
    /// no-arg form for legacy bare filenames). Throws if the asset is
    /// not in the manifest. Sets terrain_loaded = true on success.
    void load_terrain_via_assets(const f4::assets::AssetRoot& root);

private:
    // Directory of the world JSON file (set by load()). Used by
    // load_terrain() to resolve a relative terrain_file path.
    std::string world_json_dir_;
};

} // namespace f4::world
