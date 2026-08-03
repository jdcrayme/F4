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
    std::vector<ObjectiveLink> links;   // road/rail network connections
};

/// Unit subclass — same enum as f4::convert::UnitClass, duplicated here so
/// f4-world doesn't need to depend on f4-world-convert (the contract between
/// them is JSON, not the C++ type).
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
    uint8_t  status = 0;        // 0=available, 1=dead, 2=on leave, etc.
    uint8_t  aa_kills = 0;
    uint8_t  ag_kills = 0;
    int16_t  missions_flown = 0;
};

/// A campaign unit (flight, battalion, squadron, ship). Mirrors the fields
/// decoded by f4-world-convert's unit_decoder. Includes the subclass-specific
/// tail fields that are most useful for visualization and AI consumption.
struct UnitState {
    int16_t  type = 0;             // share_.entityType_ (100..2000)
    UnitClass unit_class = UnitClass::Unknown;
    uint8_t  unit_subtype = 0;     // STYPE_UNIT_* (armor/infantry/fighter/bomber/...)

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

    // Subclass-specific (only populated for the matching unit_class):
    uint8_t  supply = 0;           // Battalion / TaskForce / Brigade
    uint8_t  morale = 0;           // Battalion
    uint8_t  fatigue = 0;          // Battalion
    uint8_t  elements = 0;         // Brigade / Package
    int32_t  fuel = 0;             // Squadron

    // Hierarchy (Battalion/Brigade):
    uint32_t parent_id = 0;                    // Battalion → parent Brigade VU_ID.num
    std::vector<uint32_t> element_ids;         // Brigade → child Battalion VU_IDs

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
