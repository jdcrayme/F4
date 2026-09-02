// f4-world-convert/include/f4/convert/campaign_decoder.hpp
//
// Decodes the .cmp (campaign metadata) sub-file extracted from a .cam
// archive. The .cmp format is:
//
//   [0..3]   int32  reserved_skip   (ignored by FreeFalcon's preloader)
//   [4..7]   int32  datasize        (decompressed size of the payload)
//   [8..]    uint8[] compressed     (LZSS-compressed payload)
//
// After LZSS decompression, the payload is CampaignClass::Decode's flat
// struct sequence (cmpclass.cpp:1312). For gCampDataVersion >= 52:
//
//   CampaignTime CurrentTime        (int32, game ticks)
//   CampaignTime TE_StartTime       (int32)
//   CampaignTime TE_TimeLimit       (int32)
//   long         TE_VictoryPoints   (v > 49)
//   long         TE_type            (v >= 52)
//   long         TE_number_teams
//   long[8]      TE_number_aircraft
//   long[8]      TE_number_f16s
//   long         TE_team
//   long[8]      TE_team_pts
//   long         TE_flags
//   8x { uint8 team_flags; uint8 team_colour; char[20] name; char[200] motto; }
//   CampaignTime lastMajorEvent     (v >= 19)
//   CampaignTime lastResupply, lastRepair, lastReinforcement
//   short        TimeStamp, Group, GroundRatio, AirRatio,
//                AirDefenseRatio, NavalRatio, Brief
//   short        TheaterSizeX, TheaterSizeY
//   uchar        CurrentDay, ActiveTeams, DayZero, EndgameResult,
//                Situation, EnemyAirExp, EnemyADExp, BullseyeName
//   short        BullseyeX, BullseyeY
//   char[40]     TheaterName, Scenario, SaveFile, UIName   (CAMP_NAME_SIZE)
//   VU_ID        PlayerSquadronID
//   short        entries + entries x { uieventnode(20, x86 on-disk) +
//                short len + char[len] text }        — standard event queue
//   short        entries + entries x { ... }         — priority event queue
//   short        CampMapSize + uchar[CampMapSize]    — terrain ownership map
//   short        LastIndexNum, NumAvailSquadrons
//   NumAvailSquadrons x SquadUIInfoClass (68, x86 on-disk)
//   uchar        Tempo                              (v >= 31)
//   long         CreatorIP, CreationTime, CreationRand   (v >= 43)
//
// uieventnode on-disk (Win32 x86, the ABI that wrote these files):
//   short x(2) short y(2) CampaignTime time(4) uchar flags(1) Team team(1)
//   pad(2) eventText ptr(4) next ptr(4)  = 20 bytes. The pointer slots are
//   ignored; only x/y/time/flags/team/text are meaningful.
//
// SquadUIInfoClass on-disk (68 bytes):
//   float x(4) float y(4) VU_ID id(8) short dIndex(2) short nameId(2)
//   short airbaseIcon(2) short squadronPatch(2) uchar specialty(1)
//   uchar currentStrength(1) uchar country(1) char airbaseName[40]
//   = 67, padded to 68.
//
// Parity: TestCamp.cam (v71) decodes the full 22,080-byte payload with
// the cursor landing exactly at the end; save1.cam (v63) likewise.

#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace f4::world_convert {

struct TeamEntry {
    uint8_t flags = 0;
    uint8_t colour = 0;
    std::string name;     // up to 20 chars, null-terminated in the file
    std::string motto;    // up to 200 chars
};

/// One entry in the campaign's standard/priority UI event queue.
struct CampaignEvent {
    int16_t  x = 0;             // event location (grid), if any
    int16_t  y = 0;
    int32_t  time = 0;          // CampaignTime
    uint8_t  flags = 0;
    uint8_t  team = 0;          // team which benefited most
    std::string text;
};

/// One preload-squadron record (SquadUIInfoClass — the squadron list the
/// UI shows before the full campaign loads).
struct SquadronUIInfo {
    float    x = 0.0f;          // sim coordinates of the squadron's airbase
    float    y = 0.0f;
    uint32_t id_num = 0;        // VU_ID
    uint32_t id_creator = 0;
    int16_t  d_index = 0;       // description index
    int16_t  name_id = 0;       // UI name/patch id
    int16_t  airbase_icon = 0;
    int16_t  squadron_patch = 0;
    uint8_t  specialty = 0;
    uint8_t  current_strength = 0;   // active aircraft count
    uint8_t  country = 0;
    std::string airbase_name;   // char[40]
};

struct CampaignHeader {
    int32_t reserved_skip = 0;
    int32_t decompressed_size = 0;
    // Decoded fields (from the decompressed payload):
    int32_t current_time = 0;
    int32_t te_start_time = 0;
    int32_t te_time_limit = 0;
    int32_t te_victory_points = 0;
    int32_t te_type = 0;
    int32_t te_number_teams = 0;
    std::vector<int32_t> te_number_aircraft;   // 8 entries
    std::vector<int32_t> te_number_f16s;        // 8 entries
    int32_t te_team = 0;
    std::vector<int32_t> te_team_pts;           // 8 entries
    int32_t te_flags = 0;
    std::vector<TeamEntry> teams;               // 8 entries (name/motto per slot)

    // v >= 19 block (all v63/v71 files):
    int32_t  last_major_event = 0;
    int32_t  last_resupply = 0;
    int32_t  last_repair = 0;
    int32_t  last_reinforcement = 0;
    int16_t  time_stamp = 0;
    int16_t  group = 0;
    int16_t  ground_ratio = 0;
    int16_t  air_ratio = 0;
    int16_t  air_defense_ratio = 0;
    int16_t  naval_ratio = 0;
    int16_t  brief = 0;
    int16_t  theater_size_x = 0;     // grid extents (e.g. 128 for korea)
    int16_t  theater_size_y = 0;
    uint8_t  current_day = 0;
    uint8_t  active_teams = 0;
    uint8_t  day_zero = 0;
    uint8_t  endgame_result = 0;
    uint8_t  situation = 0;
    uint8_t  enemy_air_exp = 0;
    uint8_t  enemy_ad_exp = 0;
    uint8_t  bullseye_name = 0;
    int16_t  bullseye_x = 0;
    int16_t  bullseye_y = 0;
    std::string theater_name;        // char[CAMP_NAME_SIZE=40]
    std::string scenario;
    std::string save_file;
    std::string ui_name;
    uint32_t player_squadron_num = 0;       // VU_ID
    uint32_t player_squadron_creator = 0;

    // Event queues (standard + priority):
    std::vector<CampaignEvent> standard_events;
    std::vector<CampaignEvent> priority_events;

    // Terrain ownership map (CampMapSize bytes; 2 bits per cell, packed).
    int16_t  camp_map_size = 0;
    std::vector<uint8_t> camp_map;

    // Squadron preload list:
    int16_t  last_index_num = 0;
    int16_t  num_avail_squadrons = 0;
    std::vector<SquadronUIInfo> squadrons;

    uint8_t  tempo = 0;                     // v >= 31
    int32_t  creator_ip = 0;                // v >= 43
    int32_t  creation_time = 0;
    int32_t  creation_rand = 0;

    /// Bytes of the decompressed payload consumed by the decoder. Equals
    /// decompressed_size on a clean decode.
    std::size_t bytes_consumed = 0;

    /// Unparsed bytes after the last known field (should be empty for
    /// v63/v71 files; kept for forward compatibility).
    std::vector<uint8_t> remaining_payload;
};

/// Decode a .cmp sub-file's raw bytes into a CampaignHeader.
/// Throws on malformed input or LZSS error.
[[nodiscard]] CampaignHeader decode_cmp(const uint8_t* data, std::size_t size,
                                        int camp_version = 63);

/// Read the .ver sub-file (a text decimal version number, e.g. "63").
[[nodiscard]] int read_version(const uint8_t* data, std::size_t size);

} // namespace f4::world_convert
