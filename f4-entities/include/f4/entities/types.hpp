// f4-entities/types.hpp
//
// Shared domain types used by both the ECS components (f4-entities) and the
// format-derived world state (f4-world). Moving these here breaks the
// dependency of components on world_state.hpp — components reference domain
// concepts, not format-derived structs.
//
// These types were originally defined in f4/world/world_state.hpp. That header
// now includes this one and re-exports the types via using-declarations so
// existing code continues to compile, but new code should prefer the
// f4::entities namespace.

#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace f4::entities {

// ============================================================================
// Unit classification
// ============================================================================

/// Unit subclass — same enum values as f4::world_convert::UnitClass, but
/// defined here so f4-entities components can reference it without depending
/// on f4-world-convert (the contract between them is JSON, not C++ types).
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

// ============================================================================
// Objective network types
// ============================================================================

/// One link in the road/rail network. Connects this objective to a neighbor.
struct ObjectiveLink {
    uint32_t neighbor_num = 0;      // VU_ID.num of the linked objective
    uint32_t neighbor_creator = 0;  // VU_ID.creator
    bool is_road = false;           // supports wheeled movement
    bool is_rail = false;           // supports rail movement
    /// Per-movement-type traversal cost (FreeFalcon MoveType enum, 8 values).
    /// Index 0=NoMove, 1=Foot, 2=Wheeled, 3=Tracked, 4=LowAir, 5=Air,
    /// 6=Naval, 7=Rail. A value of 255 means "impassable for this mode".
    /// Low cost = fast path; high cost = slow path.
    uint8_t costs[8] = {0};
};

// ============================================================================
// Objective feature / ground layout types
// ============================================================================

/// One feature class (building/structure type) — mirrors the fields we
/// need from FeatureClassData (Falcon4.FCD).
struct FeatureClassState {
    int16_t  index = 0;
    std::string name;           // e.g. "Control Tower", "Runway", "Hangar"
    int16_t  hit_points = 0;
    int16_t  repair_time = 0;   // seconds to repair from destroyed to operational
    uint8_t  priority = 0;      // display priority
    uint16_t flags = 0;         // FEAT_ flags bitmap
    int16_t  radar_type = 0;    // index into Falcon4.RCD (NOT yet parsed)
};

/// One feature placement on an objective — mirrors FeatureEntryData
/// (Falcon4.FED). Combined with the objective's fstatus byte array
/// (2-bit-per-feature damage bitmap), gives the live damage state of
/// every building/runway/feature on every objective.
struct FeatureEntryState {
    int16_t  index = 0;          // entity_type of the feature (class-table index)
    uint16_t flags = 0;
    uint8_t  value = 0;          // % loss in operational status for destruction
    float    offset_x = 0.0f;    // X offset from objective center (feet)
    float    offset_y = 0.0f;
    float    offset_z = 0.0f;
    int16_t  facing = 0;         // facing angle (degrees)
    std::string name;            // e.g. "Control Tower", "Runway 09/27"
    int16_t  hit_points = 0;     // from FCD
    int16_t  repair_time = 0;    // seconds to repair from destroyed to operational
    uint8_t  priority = 0;       // display priority
    uint16_t feat_flags = 0;     // FEAT_ flags bitmap from FCD
    int16_t  radar_type = 0;     // index into Falcon4.RCD (NOT yet parsed)
    uint8_t  damage_state = 0;   // 0..3, 0=intact
};

/// One point in an airbase ground layout (runway/taxiway/parking).
struct GroundLayoutPoint {
    float    x = 0.0f;             // offset from objective center (feet)
    float    y = 0.0f;
    uint8_t  type = 0;             // PtType (1=Runway, 2=Taxiway, 11=SmallPark, ...)
    uint8_t  flags = 0;            // PtDataFlags bitmap
};

/// One list of ground-layout points (e.g. a runway, a taxiway, a parking row).
struct GroundLayoutList {
    uint8_t  type = 0;             // PointListType (1=Runway, 8=RunwayDim, 11=Parking, ...)
    uint8_t  count = 0;            // # of points in this list
    uint8_t  runway_num = 0;       // which runway (0/1/2), if type is runway
    int8_t   ltrt = 0;             // -1=left, +1=right, 0=neither
    float    heading_deg = 0.0f;   // runway heading in degrees (from sin/cos)
    std::vector<GroundLayoutPoint> points;
};

// ============================================================================
// Unit waypoint / pilot / vehicle types
// ============================================================================

/// One waypoint in a unit's flight/ground plan.
struct WaypointState {
    int16_t  x = 0;             // grid column
    int16_t  y = 0;             // grid row
    int16_t  z = 0;             // altitude (feet)
    int32_t  arrive = 0;        // arrival time (campaign seconds)
    uint8_t  action = 0;        // WP_ACTION enum
    uint8_t  route_action = 0;
    uint8_t  formation = 0;
    int16_t  flags = 0;
    uint32_t target_num = 0;          // VU_ID.num of target
    uint32_t target_creator = 0;
    uint8_t  target_building = 0;     // feature index on the target objective
    int32_t  depart = 0;              // departure time (campaign seconds)
};

/// One station in a flight's weapon loadout (decoded from the save's
/// LoadoutStruct: wire weapon id + rounds on the hardpoint). The engine
/// mapping (wire id -> WeaponClassTable handle) happens at the campaign
/// bridge; world/world-convert carry the wire ids verbatim.
struct LoadoutStationState {
    uint16_t weapon_id = 0;     // campaign WeaponDataTable index (0 = empty)
    uint16_t count = 0;         // rounds/bombs on the station
};

/// One pilot in a squadron's roster.
struct PilotState {
    int16_t  pilot_id = 0;
    uint8_t  skill = 0;
    uint8_t  rating = 0;
    uint8_t  status = 0;        // 0=available, 1=dead, 2=on leave, etc.
    uint8_t  aa_kills = 0;      // air-to-air kills
    uint8_t  ag_kills = 0;      // air-to-ground kills
    uint8_t  as_kills = 0;      // air-to-sea kills
    uint8_t  an_kills = 0;      // air-to-naval kills
    int16_t  missions_flown = 0;
};

/// One vehicle group in a unit's composition.
struct VehicleGroup {
    uint8_t  group = 0;           // group index (0-15)
    int16_t  vehicle_type = 0;    // entity_type (UCD stores 0-based CT index; converted on emission)
    int32_t  count = 0;           // nominal vehicle count for this group
    int32_t  live_count = 0;      // live count from roster (0-3)
    std::string vehicle_name;     // from VCD (e.g. "M-1A1", "F-16C")
    std::string vehicle_nctr;     // NCTR classification (radar IFF)
    int16_t  hit_points = 0;      // from VCD
    int16_t  max_speed = 0;       // from VCD (knots for air, kph for ground)
};

} // namespace f4::entities
