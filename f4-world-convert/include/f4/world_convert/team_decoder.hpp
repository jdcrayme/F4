// f4-world-convert/include/f4/convert/team_decoder.hpp
//
// Decodes the .tea sub-file (team records) from a .cam archive.
//
// .tea format (from FreeFalcon's LoadTeams / SaveTeams, team.cpp:1666/1720):
//
//   [short]  num_teams
//   then per team, 4 records serialized in sequence:
//     TeamClass::TeamClass(FILE*)          (team.cpp:270, Save at :863)
//     AirTaskingManagerClass(FILE*)        (atm.cpp:238,  Save at :430)
//     GroundTaskingManagerClass(FILE*)     (gtm.cpp:137,  Save at ~171)
//     NavalTaskingManagerClass(FILE*)      (ntm.cpp:65,   Save at ~117)
//
// TeamClass on-disk layout (739 bytes at v63/v71 — no version gates in
// that range; offsets from the record start):
//     0:  VU_ID (num 4 + creator 4)
//     8:  ushort entity_type
//    10:  uchar  who            (Team = uchar — NOT a short!)
//    11:  uchar  cteam
//    12:  short  flags
//    14:  uchar  member[8]      (NUM_COUNS)
//    22:  short  stance[8]      (NUM_TEAMS)
//    38:  short  first_colonel / first_commander / first_wingman / last_wingman
//    46:  uchar  air / air_defense / ground / naval experience
//    50:  short  initiative
//    52:  ushort supply_avail
//    54:  ushort fuel_avail
//    56:  ushort replacements_avail          (v > 53)
//    58:  float  player_rating               (v > 53)
//    62:  int    last_player_mission         (v > 53)
//    66:  TeamStatusType current_stats (16, packed)
//    82:  TeamStatusType start_stats   (16, packed)
//    98:  short  reinforcement
//   100:  VU_ID  bonus_objs[20]              (MAX_BONUSES)
//   260:  int    bonus_time[20]
//   340:  uchar  objtype_priority[36]        (MAX_TGTTYPE)
//   376:  uchar  unittype_priority[20]       (MAX_UNITTYPE)
//   396:  uchar  mission_priority[41]        (AMIS_OTHER)
//   438:  uchar  max_vehicle[4]
//   442:  uchar  team_flag                   (v > 4)
//   443:  uchar  team_color                  (v > 32)
//   444:  uchar  equipment
//   445:  char   name[20]                    (MAX_TEAM_NAME_SIZE)
//   465:  char   motto[200]                  (MAX_MOTTO_SIZE, v > 32)
//   665:  TeamGndActionType ground_action (19, packed, v > 33; 27 at 41<v<50)
//   684:  TeamAirActionType defensive_air_action (28, natural alignment)
//   712:  TeamAirActionType offensive_air_action (28)
//   739:  end
//
// CampManagerClass header (13 bytes — manager.cpp:60):
//     0:  VU_ID (8)
//     8:  ushort entity_type
//    10:  short  manager_flags
//    12:  uchar  owner (Team)
//
// ATM tail after the manager header (atm.cpp:238):
//    flags short (2), average_ca_strength short (2, v>=63),
//    average_ca_missions short (2, v>=28), sample_cycles uchar (1, v>=28),
//    num airbases uchar (1) + num × ATMAirbaseClass { VU_ID(8) +
//    schedule[32] (ATM_MAX_CYCLES) }, cycle uchar (1),
//    nreq short (2) + nreq × MissionRequestClass (76 bytes at v>=35).
//    The request list is the team's pending air tasking requests —
//    mission data we surface as ATMRequestRecord.
//
// GTM tail: flags short (2). NTM tail: flags short (2).

#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace f4::world_convert {

/// One pending mission request in a team's Air Tasking Manager
/// (MissionRequestClass as stored in the ATM's requestList; the first
/// 28 meaningful bytes of the 76-byte on-disk struct — see
/// unit_decoder.hpp's MissionRequestRecord for the full layout).
struct ATMRequestRecord {
    uint32_t requester_id_num = 0;
    uint32_t requester_id_creator = 0;
    uint32_t target_id_num = 0;
    uint32_t target_id_creator = 0;
    uint32_t secondary_id_num = 0;
    uint32_t secondary_id_creator = 0;
    uint32_t pak_id_num = 0;
    uint32_t pak_id_creator = 0;
    uint8_t  who = 0;
    uint8_t  vs = 0;
    int32_t  tot = 0;
    int16_t  tx = 0;
    int16_t  ty = 0;
    uint32_t flags = 0;
    int16_t  caps = 0;
    int16_t  target_num = 0;
    int16_t  speed = 0;
    int16_t  match_strength = 0;
    int16_t  priority = 0;
    uint8_t  tot_type = 0;
    uint8_t  action_type = 0;
    uint8_t  mission = 0;
    uint8_t  aircraft = 0;
    uint8_t  context = 0;
    uint8_t  roe_check = 0;
    uint8_t  delayed = 0;
    uint8_t  start_block = 0;
    uint8_t  final_block = 0;
    uint8_t  slots[4] = {0, 0, 0, 0};
    int8_t   min_to = 0;
    int8_t   max_to = 0;
};

/// One airbase entry in a team's ATM (ATMAirbaseClass, atm.cpp:124):
/// the airbase's VU_ID plus its 32-cycle sortie schedule bitmask.
struct ATMAirbaseRecord {
    uint32_t id_num = 0;
    uint32_t id_creator = 0;
    uint8_t  schedule[32] = {0};   // ATM_MAX_CYCLES
};

/// The Air Tasking Manager state (atm.cpp:238).
struct ATMRecord {
    uint32_t id_num = 0;
    uint32_t id_creator = 0;
    uint16_t entity_type = 0;
    int16_t  manager_flags = 0;
    uint8_t  owner = 0;
    int16_t  flags = 0;
    int16_t  average_ca_strength = 0;
    int16_t  average_ca_missions = 0;
    uint8_t  sample_cycles = 0;
    std::vector<ATMAirbaseRecord> airbases;
    uint8_t  cycle = 0;
    std::vector<ATMRequestRecord> requests;
};

/// TeamClass as stored in .tea (739 bytes at v63/v71 — full layout in
/// the file header comment).
struct TeamRecord {
    uint32_t id_num = 0;
    uint32_t id_creator = 0;
    uint16_t entity_type = 0;
    uint8_t  who = 0;            // team index (0..7)
    uint8_t  cteam = 0;          // current team (may differ during realignment)
    int16_t  flags = 0;
    std::vector<uint8_t> member;    // NUM_COUNS country memberships
    std::vector<int16_t> stance;    // NUM_TEAMS stances toward other teams
    int16_t  first_colonel = 0;
    int16_t  first_commander = 0;
    int16_t  first_wingman = 0;
    int16_t  last_wingman = 0;
    uint8_t  air_experience = 0;
    uint8_t  air_defense_experience = 0;
    uint8_t  ground_experience = 0;
    uint8_t  naval_experience = 0;
    int16_t  initiative = 0;
    uint16_t supply_avail = 0;
    uint16_t fuel_avail = 0;
    uint16_t replacements_avail = 0;
    float    player_rating = 0.0f;
    int32_t  last_player_mission = 0;
    // TeamStatusType (packed, 16 bytes each)
    uint16_t current_air_defense_vehs = 0;
    uint16_t current_aircraft = 0;
    uint16_t current_ground_vehs = 0;
    uint16_t current_ships = 0;
    uint16_t current_supply = 0;
    uint16_t current_fuel = 0;
    uint16_t current_airbases = 0;
    uint8_t  current_supply_level = 0;
    uint8_t  current_fuel_level = 0;
    uint16_t start_air_defense_vehs = 0;
    uint16_t start_aircraft = 0;
    uint16_t start_ground_vehs = 0;
    uint16_t start_ships = 0;
    uint16_t start_supply = 0;
    uint16_t start_fuel = 0;
    uint16_t start_airbases = 0;
    uint8_t  start_supply_level = 0;
    uint8_t  start_fuel_level = 0;
    int16_t  reinforcement = 0;
    std::vector<uint32_t> bonus_obj_nums;    // MAX_BONUSES VU_ID nums
    std::vector<int32_t>  bonus_times;       // MAX_BONUSES
    std::vector<uint8_t>  objtype_priority;  // 36
    std::vector<uint8_t>  unittype_priority; // 20
    std::vector<uint8_t>  mission_priority;  // AMIS_OTHER (42)
    uint8_t  max_vehicle[4] = {0, 0, 0, 0};
    uint8_t  team_flag = 0;
    uint8_t  team_color = 0;
    uint8_t  equipment = 0;
    std::string name;                        // MAX_TEAM_NAME_SIZE (20)
    std::string motto;                       // MAX_MOTTO_SIZE (200)
    // TeamGndActionType (packed, 19)
    int32_t  gnd_action_time = 0;
    int32_t  gnd_action_timeout = 0;
    uint32_t gnd_action_obj_num = 0;
    uint32_t gnd_action_obj_creator = 0;
    uint8_t  gnd_action_type = 0;
    uint8_t  gnd_action_tempo = 0;
    uint8_t  gnd_action_points = 0;
    // TeamAirActionType (natural alignment, 28) × 2
    int32_t  def_air_start_time = 0;
    int32_t  def_air_stop_time = 0;
    uint32_t def_air_obj_num = 0;
    uint32_t def_air_obj_creator = 0;
    uint32_t def_air_last_obj_num = 0;
    uint32_t def_air_last_obj_creator = 0;
    uint8_t  def_air_action_type = 0;
    int32_t  off_air_start_time = 0;
    int32_t  off_air_stop_time = 0;
    uint32_t off_air_obj_num = 0;
    uint32_t off_air_obj_creator = 0;
    uint32_t off_air_last_obj_num = 0;
    uint32_t off_air_last_obj_creator = 0;
    uint8_t  off_air_action_type = 0;

    /// The team's Air Tasking Manager (decoded from the ATM record that
    /// follows this TeamClass in the .tea stream).
    ATMRecord atm;

    /// Raw bytes of the GTM (Ground Tasking Manager) record — 15 bytes
    /// (13-byte manager header + 2-byte flags). Captured verbatim by the
    /// decoder so the .tea encoder can reproduce them byte-faithfully
    /// (the GTM's VU_ID/entity_type/owner are meaningful to FreeFalcon's
    /// LoadTeams, not just structural padding). Empty when the team was
    /// constructed by a caller (the encoder writes zeros).
    std::vector<uint8_t> gtm_raw;

    /// Raw bytes of the NTM (Naval Tasking Manager) record — 15 bytes,
    /// same layout as gtm_raw. Captured verbatim for the encoder.
    std::vector<uint8_t> ntm_raw;
};

struct DecodedTeams {
    int16_t count = 0;
    std::vector<TeamRecord> teams;

    /// Bytes consumed from the .tea stream. Equals `size` on a clean
    /// decode of all teams and their managers.
    std::size_t bytes_consumed = 0;
};

/// Decode the .tea sub-file. Throws on malformed input. Decodes the full
/// TeamClass records (roster, stance, stats, priorities, ground/air
/// actions) AND each team's AirTaskingManager — including its pending
/// mission request list (the ATO worklist). GTM/NTM are consumed
/// structurally (manager header + flags) but not exposed.
[[nodiscard]] DecodedTeams decode_tea(const uint8_t* data, std::size_t size,
                                      int camp_version = 63);

} // namespace f4::world_convert
