// f4-world-convert/include/f4/convert/campaign_decoder.hpp
//
// Decodes the .cmp (campaign metadata) sub-file extracted from a .cam
// archive. The .cmp format is:
//
//   [0..3]   int32  reserved_skip   (ignored by FreeFalcon's preloader)
//   [4..7]   int32  datasize        (decompressed size of the payload)
//   [8..]    uint8[] compressed     (LZSS-compressed payload)
//
// After LZSS decompression, the payload is a flat struct sequence. This
// decoder parses the fields documented in CampaignClass::Decode()
// (cmpclass.cpp:1276). For gCampDataVersion >= 52 (our fixture is 63):
//
//   CampaignTime CurrentTime        (int32, game ticks)
//   CampaignTime TE_StartTime       (int32)
//   CampaignTime TE_TimeLimit       (int32)
//   long         TE_VictoryPoints
//   long         TE_type
//   long         TE_number_teams
//   long[8]      TE_number_aircraft
//   long[8]      TE_number_f16s
//   long         TE_team
//   long[8]      TE_team_pts
//   long         TE_flags
//   then 8x { uint8 team_flags; uint8 team_colour;
//             char[20] team_name; char[200] team_motto; }
//
// Only the fields through the team-name/motto block are decoded here; the
// remaining payload (objective/unit references, etc.) is preserved as raw
// bytes for future decoders. This is the rosetta methodology: decode what
// we understand now, keep the rest verbatim.

#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace f4::convert {

struct TeamEntry {
    uint8_t flags = 0;
    uint8_t colour = 0;
    std::string name;     // up to 20 chars, null-terminated in the file
    std::string motto;    // up to 200 chars
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
    std::vector<uint8_t> remaining_payload;     // unparsed bytes after the team block
};

/// Decode a .cmp sub-file's raw bytes into a CampaignHeader.
/// Throws on malformed input or LZSS error.
[[nodiscard]] CampaignHeader decode_cmp(const uint8_t* data, std::size_t size);

/// Read the .ver sub-file (a text decimal version number, e.g. "63").
[[nodiscard]] int read_version(const uint8_t* data, std::size_t size);

} // namespace f4::convert
