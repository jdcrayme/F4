// f4-world-convert/include/f4/convert/unit_decoder.hpp
//
// Decodes the .uni sub-file (units: flights, battalions, squadrons, ships)
// from a .cam archive.
//
// .uni format (from FreeFalcon's SaveUnits / LoadUnits / DecodeUnitData,
// unit.cpp:5285/5303/6024):
//
//   [long]   outer_size     (written by SaveUnits; consumed by LoadUnits)
//   [short]  count          (number of unit records)
//   [long]   inner_size     (uncompressed size of the LZSS payload)
//   [bytes]  lzss_payload   (decompresses to inner_size bytes)
//
// The decompressed buffer is a sequence of `count` records, each:
//   [short]  type                (share_.entityType_, e.g. 170 for battalion)
//   [bytes]  CampBaseClass       (25 bytes at v63: no pos_.z_)
//   [bytes]  UnitClass fixed     (40 bytes at v63: current_wp is 1 byte)
//   [uchar]  wp_count            (1 byte at v63; was ushort at v>=71)
//   [bytes]  WayPointClass × wp_count
//   [bytes]  subclass tail       (Battalion / Brigade / Squadron / TaskForce /
//                                 Flight / Package — see UnitClass enum below)
//
// At gCampDataVersion=63 (our fixture), the per-subclass byte layouts are:
//
//   Battalion    : 41 bytes  (11 GroundUnit + 30 Battalion)
//   Brigade      : 12 + 8*elements bytes  (11 GroundUnit + 1 + 8*elements)
//   Squadron     : 796 bytes (4+1+200+480+64+8+8+16+2*5+1*3)
//   TaskForce    : 2 bytes
//   Flight       : 67 + 32*loadouts bytes (skips 4 fields at v<65)
//   Package      : variable (small branch: ~71 bytes; big branch: variable)
//
// Subclass dispatch: FreeFalcon looks up Falcon4ClassTable[type - 100] to
// pick the constructor. The class table isn't shipped with the source, so
// we use a try-each-tail-and-validate approach: for each candidate tail,
// check that the NEXT record's [short type] equals [ushort entity_type]
// at offset+10 AND [uchar owner] at offset+24 is in 0..7. This is robust
// to any class-table configuration.

#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace f4::world_convert {

/// FreeFalcon unit subclass. Determined by validating the candidate tail
/// against the next record's header (see file header comment).
enum class UnitClass : uint8_t {
    Unknown    = 0,
    Battalion  = 1,   // GroundUnit + Battalion tail
    Brigade    = 2,   // GroundUnit + Brigade tail (variable: elements)
    Squadron   = 3,   // AirUnit + Squadron tail (796 bytes at v63)
    TaskForce  = 4,   // UnitClass + TaskForce tail (2 bytes)
    Flight     = 5,   // AirUnit + Flight tail (variable: loadouts)
    Package    = 6,   // AirUnit + Package tail (variable: elements + branch)
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

/// A single waypoint. WayPointClass layout at v63 (campwp.cpp:89):
///   [uchar]  haves          (flags: 0x02 = has target, 0x01 = has depart)
///   [short]  GridX
///   [short]  GridY
///   [short]  GridZ
///   [long]   Arrive
///   [uchar]  Action
///   [uchar]  RouteAction
///   [uchar]  Formation
///   [short]  Flags          (2 bytes at v<72; was ulong at v>=72)
///   [if haves & 0x02: VU_ID target_id (8) + uchar target_building (1)]
///   [if haves & 0x01: long depart (4)]
/// Total: 16 bytes (no flags) ... 29 bytes (both flags)
struct WaypointRecord {
    uint8_t  haves = 0;
    int16_t  grid_x = 0;
    int16_t  grid_y = 0;
    int16_t  grid_z = 0;
    int32_t  arrive = 0;
    uint8_t  action = 0;
    uint8_t  route_action = 0;
    uint8_t  formation = 0;
    int16_t  flags = 0;
    // Optional tail (only present if haves & 0x02 / 0x01):
    uint32_t target_id_creator = 0;
    uint32_t target_id_num = 0;
    uint8_t  target_building = 0;
    int32_t  depart = 0;
};

/// One pilot in a squadron's roster. PilotClass is 10 bytes (pilot.h:32):
///   short pilot_id(2) + uchar pilot_skill_and_rating(1) + uchar pilot_status(1)
///   + uchar aa_kills(1) + uchar ag_kills(1) + uchar as_kills(1) + uchar an_kills(1)
///   + short missions_flown(2)
/// pilot_skill_and_rating: low byte = skill (0-100), high byte = rating.
/// pilot_status: 0=available, 1=dead, 2=on leave, 3=in hospital, etc.
struct PilotRecord {
    int16_t  pilot_id = 0;
    uint8_t  skill = 0;         // low byte of pilot_skill_and_rating
    uint8_t  rating = 0;        // high byte of pilot_skill_and_rating
    uint8_t  status = 0;
    uint8_t  aa_kills = 0;
    uint8_t  ag_kills = 0;
    uint8_t  as_kills = 0;
    uint8_t  an_kills = 0;
    int16_t  missions_flown = 0;
};

/// Subclass-specific tail fields. Only the fields relevant to the
/// decoded UnitClass are populated; others stay at default values.
struct UnitSubclassData {
    // Battalion / Brigade (GroundUnit common):
    uint8_t  orders = 0;        // GroundUnit.orders (uchar)
    int16_t  division = 0;      // GroundUnit.division (short)
    uint32_t aobj_creator = 0;  // GroundUnit.aobj (VU_ID)
    uint32_t aobj_num = 0;

    // Battalion:
    int32_t  last_move = 0;
    int32_t  last_combat = 0;
    uint32_t parent_id_creator = 0;  // VU_ID
    uint32_t parent_id_num = 0;
    uint32_t last_obj_creator = 0;   // VU_ID
    uint32_t last_obj_num = 0;
    uint8_t  supply = 0;        // Percentage
    uint8_t  fatigue = 0;
    uint8_t  morale = 0;
    uint8_t  heading = 0;
    uint8_t  final_heading = 0;
    uint8_t  position = 0;

    // Brigade:
    uint8_t  elements = 0;
    std::vector<uint32_t> element_ids;  // creator/num pairs flattened

    // Squadron:
    int32_t  fuel = 0;
    uint8_t  specialty = 0;
    // stores[] (200 bytes) and schedule[] (64 bytes) are decoded but not
    // exposed — too large to be useful in the viewer.
    std::vector<PilotRecord> pilots;   // 48 pilots per squadron (PILOTS_PER_SQUADRON)
    uint32_t airbase_id_creator = 0;
    uint32_t airbase_id_num = 0;
    uint32_t hot_spot_creator = 0;
    uint32_t hot_spot_num = 0;
    int16_t  aa_kills = 0;
    int16_t  ag_kills = 0;
    int16_t  as_kills = 0;
    int16_t  an_kills = 0;
    int16_t  missions_flown = 0;
    int16_t  mission_score = 0;
    uint8_t  total_losses = 0;
    uint8_t  pilot_losses = 0;
    uint8_t  squadron_patch = 0;

    // TaskForce:
    // (orders + supply already covered above)

    // Flight:
    float    altitude = 0.0f;   // pos_.z_
    int32_t  fuel_burnt = 0;
    int32_t  time_on_target = 0;
    int32_t  mission_over_time = 0;
    int16_t  mission_target = 0;
    uint8_t  loadouts = 0;      // count of loadout[] entries
    uint8_t  mission = 0;
    uint8_t  priority = 0;
    uint8_t  mission_id = 0;
    uint8_t  eval_flags = 0;
    uint32_t package_creator = 0;       // VU_ID
    uint32_t package_num = 0;
    uint32_t squadron_creator = 0;      // VU_ID
    uint32_t squadron_num = 0;
    uint8_t  callsign_id = 0;
    uint8_t  callsign_num = 0;

    // Package:
    uint8_t  wait_cycles = 0;
    uint32_t interceptor_creator = 0;
    uint32_t interceptor_num = 0;
    uint32_t awacs_creator = 0;
    uint32_t awacs_num = 0;
    uint32_t jstar_creator = 0;
    uint32_t jstar_num = 0;
    uint32_t ecm_creator = 0;
    uint32_t ecm_num = 0;
    uint32_t tanker_creator = 0;
    uint32_t tanker_num = 0;
};

struct UnitRecord {
    int16_t  type = 0;          // share_.entityType_ (100..2000)
    UnitClass unit_class = UnitClass::Unknown;

    // CampBaseClass (25 bytes at v63):
    uint32_t id_creator = 0;
    uint32_t id_num = 0;
    uint16_t entity_type = 0;
    int16_t  x = 0;
    int16_t  y = 0;
    float    z = 0.0f;          // always 0 at v63 (skipped)
    int32_t  spot_time = 0;
    int16_t  spotted = 0;
    int16_t  base_flags = 0;
    uint8_t  owner = 0;
    int16_t  camp_id = 0;

    // UnitClass fixed fields (40 bytes at v63):
    int32_t  last_check = 0;
    uint32_t roster = 0;
    uint32_t unit_flags = 0;
    int16_t  dest_x = 0;
    int16_t  dest_y = 0;
    uint32_t target_id_creator = 0;   // VU_ID
    uint32_t target_id_num = 0;
    uint32_t cargo_id_creator = 0;    // VU_ID
    uint32_t cargo_id_num = 0;
    uint8_t  moved = 0;
    uint8_t  losses = 0;
    uint8_t  tactic = 0;
    uint8_t  current_wp = 0;   // 1 byte at v63 (was ushort at v>=71)
    int16_t  name_id = 0;
    int16_t  reinforcement = 0;

    // Waypoints (variable):
    uint8_t  wp_count = 0;
    std::vector<WaypointRecord> waypoints;

    // Subclass-specific tail:
    UnitSubclassData subclass;
};

struct DecodedUnits {
    int16_t count = 0;          // claimed count from the .uni header
    std::vector<UnitRecord> units;

    /// Cursor position when decoding stopped (in bytes from the start of
    /// the LZSS-decompressed buffer). Useful for diagnostics: should equal
    /// inner_size when all records decode cleanly.
    std::size_t bytes_consumed = 0;

    /// Total bytes in the LZSS-decompressed buffer. Should equal
    /// bytes_consumed on a clean decode.
    std::size_t inner_size = 0;
};

/// Decode the .uni sub-file. Throws on malformed input. Decodes ALL records
/// at gCampDataVersion=63 (the fixture version): CampBaseClass + UnitClass
/// fixed + waypoints + subclass-specific tail. Subclass is identified by
/// trying each candidate tail layout and validating the next record's
/// header (type == entity_type at offset+10, owner in 0..7).
///
/// At v63, all 683 records in save1.cam decode cleanly with the cursor
/// landing exactly at the buffer end.
[[nodiscard]] DecodedUnits decode_uni(const uint8_t* data, std::size_t size);

} // namespace f4::world_convert
