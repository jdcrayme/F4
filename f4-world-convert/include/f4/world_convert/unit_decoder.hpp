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
//   [bytes]  CampBaseClass       (25 bytes at v63; 29 at v>=70: + pos_.z_)
//   [bytes]  UnitClass fixed     (40 bytes at v63; 41 at v>=71: current_wp
//                                 becomes a ushort)
//   [wpcnt]  wp_count            (1 byte at v63; ushort at v>=71)
//   [bytes]  WayPointClass × wp_count
//   [bytes]  subclass tail       (Battalion / Brigade / Squadron / TaskForce /
//                                 Flight / Package — see UnitClass enum below)
//
// Per-subclass byte layouts at gCampDataVersion=63 vs 71:
//
//   Battalion    : 41 bytes  (11 GroundUnit + 30 Battalion)      [unchanged]
//   Brigade      : 12 + 8*elements bytes                          [unchanged]
//   Squadron     : 794 bytes at v63 (stores[200]); 814 at 69<=v<72
//                  (stores[220]); stores[600] at v>=72
//   TaskForce    : 2 bytes                                       [unchanged]
//   Flight       : 67 + 32*loadouts at v63; +10 bytes at v>65
//                  (old_mission 1 + mission_context 1 + requester VU_ID 8);
//                  +4 more at v>=72 (refuel uint)
//   Package      : common header (1 + 8*elements + 5*8 + 1), then:
//                  small branch (Final() && !wait_cycles):
//                    requests 2 + responses 2 + mission 2 + context 2
//                    + requesterID 8 + targetID 8 + tot 4 (v>=26)
//                    + action_type 1 (v>=35) + priority 2 (v>=41)
//                  big branch (else):
//                    flights 1 + wait_for 2 + 8×GridIndex 16 + takeoff 4
//                    + tp_time 4 + package_flags 4 + caps 2 + requests 2
//                    + responses 2 + ingress route (uchar count + wps)
//                    + egress route (uchar count + wps)
//                    + MissionRequestClass 76 (v>=35, sizeof incl. packing)
//
// Branch selection for Package is deterministic at decode time:
// FreeFalcon's PackageClass::Save writes the small branch iff
// Final() && !wait_cycles, where Final() = (unit_flags & U_FINAL) and
// U_FINAL = 0x100000 (unit.h:65). unit_flags is already in the record.
//
// Subclass dispatch (v63 heritage): FreeFalcon looks up
// Falcon4ClassTable[type - 100] to pick the constructor. When a class
// table is supplied (ClassTable from Falcon4.ct — same file the game
// loads), dispatch is deterministic: domain/type from classInfo_ →
//   (DOMAIN_AIR,1)=Flight, (2)=Package, (3)=Squadron,
//   (DOMAIN_LAND,1)=Battalion, (2)=Brigade, (DOMAIN_SEA,1)=TaskForce.
// Without a class table we fall back to try-each-tail-and-validate.
//
// Next-record validation (both paths): at the candidate end position the
// next record's [short type] must equal [ushort entity_type] and the
// [uchar owner] must be in 0..7. At v63 owner sits at record+24 (25-byte
// CampBase after the 2-byte type); at v>=70 it moves to record+28 (the
// inserted pos_.z_ float shifts everything after y). The minimum header
// size grows from 25 to 31 bytes accordingly. Valid type range is
// [100 .. 100+ct_entries) when a class table is loaded (entity types
// can exceed 2000 — TestCamp.cam has battalions at type 2022), else
// the legacy heuristic [100..2000].

#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace f4::world_convert {

class ClassTable;

/// FreeFalcon unit subclass. Determined by the class table when available
/// (domain/type from classInfo_), otherwise by validating the candidate
/// tail against the next record's header (see file header comment).
enum class UnitClass : uint8_t {
    Unknown    = 0,
    Battalion  = 1,   // GroundUnit + Battalion tail
    Brigade    = 2,   // GroundUnit + Brigade tail (variable: elements)
    Squadron   = 3,   // AirUnit + Squadron tail (794 bytes at v63)
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

/// Which PackageClass::Save branch a package record used. Determined at
/// decode time from (unit_flags & U_FINAL) and wait_cycles — see the
/// file header comment for the exact rule.
enum class PackageBranch : uint8_t {
    None      = 0,    // not a package / undecoded
    Small     = 1,    // Final() && !wait_cycles — requests/responses + compact mis_request
    Big       = 2,    // everything else — routes + full MissionRequestClass
};

/// A single waypoint. WayPointClass layout (campwp.cpp:89):
///   [uchar]  haves          (flags: 0x02 = has target, 0x01 = has depart)
///   [short]  GridX
///   [short]  GridY
///   [short]  GridZ
///   [long]   Arrive
///   [uchar]  Action
///   [uchar]  RouteAction
///   [uchar]  Formation
///   [short]  Flags          (2 bytes at v<72; 32-bit ulong at v>=72)
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
/// pilot_skill_and_rating: low nibble = skill, high nibble = rating.
/// pilot_status: 0=available, 1=dead, 2=on leave, 3=in hospital, etc.
struct PilotRecord {
    int16_t  pilot_id = 0;
    uint8_t  skill = 0;
    uint8_t  rating = 0;
    uint8_t  status = 0;
    uint8_t  aa_kills = 0;
    uint8_t  ag_kills = 0;
    uint8_t  as_kills = 0;
    uint8_t  an_kills = 0;
    int16_t  missions_flown = 0;
};

/// The mission request that drives a package (MissionRequestClass,
/// mission.h:326). The big Package branch bulk-copies all 76 bytes
/// (Win32 x86 sizeof incl. alignment padding); the small branch writes
/// a compact subset. Fields default to 0 and are only populated where
/// the branch provides them.
struct MissionRequestRecord {
    uint32_t requester_id_num = 0;      // requesterID (VU_ID)
    uint32_t requester_id_creator = 0;
    uint32_t target_id_num = 0;         // targetID (VU_ID)
    uint32_t target_id_creator = 0;
    uint32_t secondary_id_num = 0;      // secondaryID (big branch only)
    uint32_t secondary_id_creator = 0;
    uint32_t pak_id_num = 0;            // pakID (big branch only)
    uint32_t pak_id_creator = 0;
    uint8_t  who = 0;                   // requesting team
    uint8_t  vs = 0;                    // opposing team
    int32_t  tot = 0;                   // time over target
    int16_t  tx = 0;                    // target grid x (big branch only)
    int16_t  ty = 0;                    // target grid y (big branch only)
    uint32_t flags = 0;                 // big branch only
    int16_t  caps = 0;                  // requested capabilities (big only)
    int16_t  target_num = 0;            // big branch only
    int16_t  speed = 0;                 // big branch only
    int16_t  match_strength = 0;        // big branch only
    int16_t  priority = 0;
    uint8_t  tot_type = 0;
    uint8_t  action_type = 0;
    uint8_t  mission = 0;               // MissionTypeEnum (AMIS_*)
    uint8_t  aircraft = 0;              // # of aircraft requested (big only)
    uint8_t  context = 0;               // why this was requested
    uint8_t  roe_check = 0;             // big branch only
    uint8_t  delayed = 0;               // big branch only
    uint8_t  start_block = 0;           // big branch only
    uint8_t  final_block = 0;           // big branch only
    uint8_t  slots[4] = {0, 0, 0, 0};   // big branch only
    int8_t   min_to = 0;                // big branch only
    int8_t   max_to = 0;                // big branch only
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
    // stores[] (200/220/600 bytes) and schedule[] (64 bytes) are decoded
    // but not exposed — too large to be useful in the viewer.
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
    uint8_t  old_mission = 0;   // v > 65 only
    uint8_t  priority = 0;
    uint8_t  mission_id = 0;
    uint8_t  eval_flags = 0;
    uint8_t  mission_context = 0;   // v > 65 only
    uint32_t package_creator = 0;       // VU_ID
    uint32_t package_num = 0;
    uint32_t squadron_creator = 0;      // VU_ID
    uint32_t squadron_num = 0;
    uint32_t requester_creator = 0;     // VU_ID — v > 65 only
    uint32_t requester_num = 0;
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

    // Package small branch:
    int16_t  requests = 0;          // outstanding vulcan requests
    int16_t  responses = 0;

    // Package big branch:
    uint8_t  flights = 0;           // # of flights in the package
    int16_t  wait_for = 0;
    int16_t  iax = 0, iay = 0;      // ingress x/y
    int16_t  eax = 0, eay = 0;      // egress x/y
    int16_t  bpx = 0, bpy = 0;      // bypass x/y
    int16_t  tpx = 0, tpy = 0;      // target point x/y
    int32_t  takeoff = 0;           // package takeoff time
    int32_t  tp_time = 0;           // target point time
    uint32_t package_flags = 0;
    int16_t  caps = 0;
    std::vector<WaypointRecord> ingress;   // ingress route
    std::vector<WaypointRecord> egress;    // egress route

    // Package mis_request (either branch — see MissionRequestRecord for
    // which fields are populated in each):
    MissionRequestRecord mis_request;
    PackageBranch package_branch = PackageBranch::None;
};

struct UnitRecord {
    int16_t  type = 0;          // share_.entityType_ (100..2000)
    UnitClass unit_class = UnitClass::Unknown;

    // CampBaseClass (25 bytes at v63; 29 at v>=70):
    uint32_t id_creator = 0;
    uint32_t id_num = 0;
    uint16_t entity_type = 0;
    int16_t  x = 0;
    int16_t  y = 0;
    float    z = 0.0f;          // 0 at v<70 (not in stream)
    int32_t  spot_time = 0;
    int16_t  spotted = 0;
    int16_t  base_flags = 0;
    uint8_t  owner = 0;
    int16_t  camp_id = 0;

    // UnitClass fixed fields (40 bytes at v63; 41 at v>=71):
    int32_t  last_check = 0;
    uint32_t roster = 0;
    uint32_t unit_flags = 0;    // bit 0x100000 = U_FINAL (see PackageBranch)
    int16_t  dest_x = 0;
    int16_t  dest_y = 0;
    uint32_t target_id_creator = 0;   // VU_ID
    uint32_t target_id_num = 0;
    uint32_t cargo_id_creator = 0;    // VU_ID
    uint32_t cargo_id_num = 0;
    uint8_t  moved = 0;
    uint8_t  losses = 0;
    uint8_t  tactic = 0;
    uint16_t current_wp = 0;    // 1 byte at v63; ushort at v>=71
    int16_t  name_id = 0;
    int16_t  reinforcement = 0;

    // Waypoints (variable):
    uint16_t wp_count = 0;      // 1 byte at v63; ushort at v>=71
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

/// Decode options: the campaign data version (from the .ver sub-file)
/// selects the on-disk layout, and an optional class table enables
/// deterministic subclass dispatch (the same Falcon4.ct the game loads).
struct UnitDecodeOptions {
    int camp_version = 63;                   // gCampDataVersion
    const ClassTable* class_table = nullptr; // optional, from FALCON4.ct
};

/// Decode the .uni sub-file. Throws on malformed input. Decodes ALL records
/// for the supplied camp_version (63 and 71 are the layouts in active use;
/// intermediate versions 64-70 follow the same rules via the gates below).
/// Subclass is identified via the class table when provided (domain/type
/// from classInfo_), else by trying each candidate tail layout and
/// validating the next record's header.
[[nodiscard]] DecodedUnits decode_uni(const uint8_t* data, std::size_t size,
                                      const UnitDecodeOptions& opts = {});

} // namespace f4::world_convert
