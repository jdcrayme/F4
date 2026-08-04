// f4-world/include/f4/world/world_state.hpp
//
// Typed WorldState — the consumer-side counterpart to f4-world-convert.
// f4-world-convert turns a binary .cam archive into JSON; f4-world loads
// that JSON into typed structs and can populate an f4-entities EntityWorld.
//
// This is the keystone from ARCHITECTURE PROPOSAL §18.5: once f4-world
// exists, the entity system is populated from REAL data (a real Korea
// theater with real teams, real airbases, real squadrons), and the AI
// always runs against ground truth. The injection-harness trap (§18.6)
// is structurally retired.
//
// Current scope: campaign header, team list, objectives (full), units
// (partial — fixed fields only), and a reference to a terrain JSON file
// (loaded separately via f4-terrain). As f4-world-convert learns to decode
// more sub-files (objective deltas, weather, events, flight plans), this
// struct grows to carry them.

#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include <f4/terrain/terrain_data.hpp>

namespace f4::world {

struct TeamState {
    int slot = 0;               // 0..7
    uint8_t flags = 0;
    uint8_t colour = 0;
    std::string name;           // e.g. "ROK", "Japan", "PRC"
    std::string motto;
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
};

/// One link in the road/rail network. Connects this objective to a neighbor.
struct ObjectiveLink {
    uint32_t neighbor_num = 0;      // VU_ID.num of the linked objective
    uint32_t neighbor_creator = 0;  // VU_ID.creator
    bool is_road = false;           // supports wheeled movement
    bool is_rail = false;           // supports rail movement
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
    std::vector<ObjectiveLink> links;   // road/rail network connections
};

/// Unit subclass — same enum as f4::world_convert::UnitClass, duplicated
/// here so f4-world doesn't need to depend on f4-world-convert (the contract
/// between them is JSON, not the C++ type).
enum class UnitClass : uint8_t {
    Unknown    = 0,
    Battalion  = 1,
    Brigade    = 2,
    Squadron   = 3,
    TaskForce  = 4,
    Flight     = 5,
    Package    = 6,
};

[[nodiscard]] inline const char* unit_class_name(UnitClass c) noexcept {
    switch (c) {
        case UnitClass::Battalion:  return "battalion";
        case UnitClass::Brigade:    return "brigade";
        case UnitClass::Squadron:   return "squadron";
        case UnitClass::TaskForce:  return "taskforce";
        case UnitClass::Flight:     return "flight";
        case UnitClass::Package:    return "package";
        case UnitClass::Unknown:    break;
    }
    return "unknown";
}

/// One pilot in a squadron's roster.
struct PilotState {
    int16_t  pilot_id = 0;
    uint8_t  skill = 0;
    uint8_t  rating = 0;
    uint8_t  status = 0;        // 0=available, 1=dead, 2=on leave, etc.
    uint8_t  aa_kills = 0;
    uint8_t  ag_kills = 0;
    int16_t  missions_flown = 0;
};

/// One waypoint in a unit's flight/ground plan. Mirrors WaypointRecord
/// in f4-world-convert (campwp.cpp:89 at v63).
struct WaypointState {
    int16_t  x = 0;             // grid column
    int16_t  y = 0;             // grid row
    int16_t  z = 0;             // altitude (feet)
    int32_t  arrive = 0;        // arrival time (campaign seconds)
    uint8_t  action = 0;        // WP_ACTION enum
    uint8_t  route_action = 0;
    uint8_t  formation = 0;
    int16_t  flags = 0;
    // Optional (present only when the original `haves` flag was set):
    uint32_t target_num = 0;          // VU_ID.num of target
    uint32_t target_creator = 0;
    uint8_t  target_building = 0;     // feature index on the target objective
    int32_t  depart = 0;              // departure time (campaign seconds)
};

/// A campaign unit (flight, battalion, squadron, ship). Mirrors the fields
/// decoded by f4-world-convert's unit_decoder. Includes the subclass-specific
/// tail fields that are most useful for visualization and AI consumption.
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
    void load_terrain(const std::filesystem::path& base_dir = "");

private:
    // Directory of the world JSON file (set by load()). Used by
    // load_terrain() to resolve a relative terrain_file path.
    std::string world_json_dir_;
};

} // namespace f4::world
