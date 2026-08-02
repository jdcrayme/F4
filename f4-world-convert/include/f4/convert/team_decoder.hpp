// f4-world-convert/include/f4/convert/team_decoder.hpp
//
// Decodes the .tea sub-file (team records) from a .cam archive.
//
// .tea format (from FreeFalcon's LoadTeams / SaveTeams, team.cpp:1636/1689):
//
//   [short]  num_teams
//   then per team, 4 records serialized in sequence:
//     TeamClass::TeamClass(FILE*)     (team.cpp:263)
//     AirTaskingManagerClass(FILE*)
//     GroundTaskingManagerClass(FILE*)
//     NavalTaskingManagerClass(FILE*)
//
// For the first pass we decode only the TeamClass identity fields (who,
// cteam, flags, member countries, stance array, experience values). The
// ATM/GTM/NTM tasking managers are large and mission-system-specific;
// they're skipped (cursor advanced past) for now. This gives us the team
// roster with the same names the .cmp decoder already produced, but with
// the richer per-team state (stance toward other teams, experience levels,
// country membership).

#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace f4::convert {

struct TeamRecord {
    uint32_t id_creator = 0;
    uint32_t id_num = 0;
    uint16_t entity_type = 0;
    int16_t  who = 0;            // team index (0..7)
    int16_t  cteam = 0;          // current team (may differ during realignment)
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
};

struct DecodedTeams {
    int16_t count = 0;
    std::vector<TeamRecord> teams;
};

/// Decode the .tea sub-file. Throws on malformed input. The ATM/GTM/NTM
/// records that follow each TeamClass are NOT decoded here (they're large
/// and tasking-system-specific); only the TeamClass identity block is read.
/// The .cmp decoder already provides team names; this adds team state.
[[nodiscard]] DecodedTeams decode_tea(const uint8_t* data, std::size_t size);

} // namespace f4::convert
