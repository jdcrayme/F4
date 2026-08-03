// f4-world-convert/include/f4/convert/objective_decoder.hpp
//
// Decodes the .obj sub-file (base objectives) from a .cam archive.
//
// .obj format (from FreeFalcon's SaveBaseObjectives / LoadBaseObjectives,
// objectiv.cpp:2845 and :2955):
//
//   [short]  num_objectives
//   [long]   uncompressed_size     (size of the buffer below, pre-LZSS)
//   [long]   compressed_size       (size of the LZSS payload that follows)
//   [bytes]  lzss_payload          (decompresses to `uncompressed_size` bytes)
//
// The decompressed buffer is a sequence of `num_objectives` records, each:
//   [short]  type        (ObjectiveType: 1=AIRBASE, 2=AIRSTRIP, 3=ARMYBASE,
//                         6=BRIDGE, 8=CITY, 11=FACTORY, 19=PORT, 20=POWERPLANT,
//                         21=RADAR, 24=RAILROAD, ... see classtbl.h:65)
//   [bytes]  CampBaseClass + ObjectiveClass save data
//
// CampBaseClass::Save (campbase.cpp:229) layout:
//   [VU_ID]     id            (8 bytes: creator uint32 + num uint32)
//   [ushort]    entity_type
//   [GridIndex] x             (int16, grid column)
//   [GridIndex] y             (int16, grid row)
//   [float]     z             (altitude, feet)
//   [CampaignTime] spot_time  (int32)
//   [short]     spotted
//   [short]     base_flags
//   [Control]   owner         (uint8: 0=neutral, 1=red/enemy, 2=blue/ally, ...)
//   [short]     camp_id
//
// Then ObjectiveClass::Save (objectiv.cpp:432) appends:
//   [CampaignTime] last_repair   (int32)
//   [ulong]     obj_flags
//   [uchar]     supply
//   [uchar]     fuel
//   [uchar]     losses
//   [uchar]     fstatus_len
//   [bytes]     fstatus[fstatus_len]
//   [uchar]     priority
//   [short]     nameid           (index into the name table)
//   [VU_ID]     parent           (parent objective)
//   [Control]   first_owner      (uint8)
//   [uchar]     links
//   [links * CampObjectiveLinkDataType] link_data
//   [uchar]     has_radar_data
//   [if has_radar_data: RadarRangeClass]
//
// For the first pass we decode the positional and identity fields (type,
// x, y, z, owner, nameid, priority) AND the variable-length link data
// (road/rail network — each link is 8 uchar costs + 8-byte neighbor VU_ID
// = 16 bytes). The fstatus array is parsed to advance the cursor but not
// exposed as a typed field (it's a per-feature status bitmap whose
// semantics depend on the objective type). The optional RadarRangeClass
// (32 bytes) is likewise parsed to advance the cursor but not exposed —
// it's only present for radar-type objectives and will be exposed when
// f4-radar lands.
//
// The DecodedObjectives struct carries bytes_consumed and inner_size for
// cursor-landing verification (parity with DecodedUnits): on a clean
// decode, bytes_consumed == inner_size. If they differ, the cursor
// desynced on some record and the decoder stopped early.

#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace f4::world_convert {

/// FreeFalcon objective type constants (from classtbl.h:65). Only the
/// campaign-objective-relevant subset; others are unused.
enum ObjectiveType : int16_t {
    TYPE_AIRBASE      = 1,
    TYPE_AIRSTRIP     = 2,
    TYPE_ARMYBASE     = 3,
    TYPE_BEACH        = 4,
    TYPE_BORDER       = 5,
    TYPE_BRIDGE       = 6,
    TYPE_CHEMICAL     = 7,
    TYPE_CITY         = 8,
    TYPE_COM_CONTROL  = 9,
    TYPE_DEPOT        = 10,
    TYPE_FACTORY      = 11,
    TYPE_FORD         = 12,
    TYPE_FORTIFICATION= 13,
    TYPE_HILL_TOP     = 14,
    TYPE_INTERSECT    = 15,
    TYPE_NUCLEAR      = 17,
    TYPE_PASS         = 18,
    TYPE_PORT         = 19,
    TYPE_POWERPLANT   = 20,
    TYPE_RADAR        = 21,
    TYPE_RADIO_TOWER  = 22,
    TYPE_RAIL_TERMINAL= 23,
    TYPE_RAILROAD     = 24,
    TYPE_TOWN         = 39,  // see objectiv.cpp:224
};

/// Human-readable name for an objective type (for visualization labels).
[[nodiscard]] std::string objective_type_name(int16_t type);

/// Movement type indices (from falcent.h MoveType enum). Used to interpret
/// the costs[] array in ObjectiveLink. The cost for a given movement type
/// indicates whether a link supports that movement mode — a low cost means
/// the link is a fast path for that mode, a high/zero cost means it's slow
/// or impassable.
enum MoveType : int {
    NoMove   = 0,
    Foot     = 1,
    Wheeled  = 2,   // roads
    Tracked  = 3,
    LowAir   = 4,
    Air      = 5,
    Naval    = 6,
    Rail     = 7,   // railroads
    MOVEMENT_TYPES = 8,
};

/// One link between two objectives. Part of the road/rail network.
/// Layout: uchar costs[8] + VU_ID(8 bytes) = 16 bytes total.
/// The VU_ID refers to the NEIGHBORING objective (the other end of the link).
struct ObjectiveLink {
    uint8_t costs[MOVEMENT_TYPES] = {0};   // cost to traverse for each MoveType
    uint32_t neighbor_creator = 0;          // VU_ID.creator of the linked objective
    uint32_t neighbor_num = 0;              // VU_ID.num of the linked objective

    /// Is this a road link? (Wheeled movement cost is non-zero and reasonable)
    [[nodiscard]] bool is_road() const noexcept {
        return costs[Wheeled] > 0 && costs[Wheeled] < 250;
    }

    /// Is this a rail link? (Rail movement cost is non-zero)
    [[nodiscard]] bool is_rail() const noexcept {
        return costs[Rail] > 0 && costs[Rail] < 250;
    }
};

struct ObjectiveRecord {
    int16_t  type = 0;          // ObjectiveType
    // CampBaseClass fields:
    uint32_t id_creator = 0;    // VU_ID.creator
    uint32_t id_num = 0;        // VU_ID.num
    uint16_t entity_type = 0;
    int16_t  x = 0;             // grid column (GridIndex)
    int16_t  y = 0;             // grid row (GridIndex)
    float    z = 0.0f;          // altitude (feet)
    int32_t  spot_time = 0;
    int16_t  spotted = 0;
    int16_t  base_flags = 0;
    uint8_t  owner = 0;         // Control: 0=neutral, 1=enemy, 2=friendly...
    int16_t  camp_id = 0;
    // ObjectiveClass fields:
    int32_t  last_repair = 0;
    uint32_t obj_flags = 0;
    uint8_t  supply = 0;
    uint8_t  fuel = 0;
    uint8_t  losses = 0;
    uint8_t  priority = 0;
    int16_t  nameid = 0;
    uint8_t  first_owner = 0;
    uint8_t  links = 0;
    // Decoded link data (road/rail network):
    std::vector<ObjectiveLink> link_data;
    // fstatus and radar data are parsed to advance the cursor but not
    // yet exposed as typed fields (kept verbatim for future passes).
};

struct DecodedObjectives {
    int16_t count = 0;
    std::vector<ObjectiveRecord> objectives;

    /// Number of bytes consumed from the decompressed buffer. On a clean
    /// decode (all `count` records parsed without cursor desync), this
    /// equals inner_size. If less, the decoder stopped early — the
    /// objectives vector contains only the records that decoded cleanly.
    std::size_t bytes_consumed = 0;

    /// Total size of the decompressed buffer (the LZSS payload's
    /// uncompressed size from the sub-file header). bytes_consumed should
    /// equal inner_size on a clean decode.
    std::size_t inner_size = 0;
};

/// Decode the .obj sub-file's raw bytes. Throws on malformed input.
[[nodiscard]] DecodedObjectives decode_obj(const uint8_t* data, std::size_t size);

} // namespace f4::world_convert
