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
// Current scope: the campaign header + team list. As f4-world-convert
// learns to decode objectives (.obj) and units (.uni), this struct grows
// to carry ObjectiveState and UnitState vectors, and populate_entities()
// will spawn entities for each of them.

#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

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

struct WorldState {
    int version = 0;                        // gCampDataVersion (from .ver)
    CampaignState campaign;
    std::vector<TeamState> teams;           // 8 slots

    /// Load from a world JSON file (produced by f4-world-convert's cam2json).
    /// Throws on I/O or parse error.
    void load(const std::filesystem::path& json_path);

    /// Load from an in-memory JSON string (for testing).
    void load_from_string(const std::string& json);
};

} // namespace f4::world
