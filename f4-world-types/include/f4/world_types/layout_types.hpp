// f4-world-types/include/f4/world_types/layout_types.hpp
//
// Ground-layout enum constants (from FreeFalcon's ptdata.h / classtbl.h).
//
// Tranche 0d (NO_BINARY_RUNTIME_PLAN.md): extracted from
// f4-world-convert/theater_data.hpp + objective_decoder.hpp so the runtime
// (f4-simulation::campaign_bridge) can reference TYPE_AIRBASE / PLT_RUNWAY
// / PT_RUNWAY etc. without linking the legacy binary parser library.
//
// These are pure value definitions — no binary parsing, no I/O. Any runtime
// target may include this header freely.

#pragma once

#include <cstdint>

namespace f4::world_types {

// ============================================================================
// Objective type constants (from classtbl.h:65).
// Used by ObjectiveState::objective_type + f4-simulation::campaign_bridge
// to identify airbases/airstrips for ground-layout derivation.
// ============================================================================
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

// ============================================================================
// Point type enum (from ptdata.h:40-60) — used by PtHeaderDataType.type
// and PtDataType.type. Determines the semantic role of a ground-layout
// point (runway threshold, taxiway node, parking spot, etc.).
// ============================================================================
enum PointType : uint8_t {
    PT_NOT_USED          = 0,
    PT_RUNWAY            = 1,   // Runway threshold (takeoff/landing end)
    PT_TAKEOFF           = 2,   // Takeoff position (held short of runway)
    PT_TAXI              = 3,   // Taxiway node
    PT_SAM               = 4,   // SAM site placement
    PT_ARTILLERY         = 5,   // Artillery placement
    PT_AAA               = 6,   // AAA placement
    PT_RADAR             = 7,   // Radar placement
    PT_RUNWAY_DIM        = 8,   // Runway dimensional point (length/width marks)
    PT_SUPPORT           = 9,   // Support vehicle placement
    PT_STATIC_RADAR      = 10,  // Static radar (building-sized)
    PT_SMALL_PARK        = 11,  // Small parking spot (fighters)
    PT_LARGE_PARK        = 12,  // Large parking spot (transports/bombers)
    PT_SMALL_DOCK        = 13,  // Small dock (small boats)
    PT_LARGE_DOCK        = 14,  // Large dock (capital ships)
    PT_TAKE_RUNWAY       = 15,  // Runway access point (taxiway -> runway)
    PT_HELICOPTER        = 16,  // Helicopter pad
    PT_FOLLOW_ME         = 17,  // Follow-me truck rendezvous
    PT_TRACK             = 18,  // Track/path point (ground vehicle routes)
    PT_CRIT_TAXI         = 19,  // Critical taxiway intersection
};

// ============================================================================
// Point list type enum (from ptdata.h:62-78) — used by PtHeaderDataType.type
// to indicate what kind of point list this header begins.
// ============================================================================
enum PointListType : uint8_t {
    PLT_NONE              = 0,
    PLT_RUNWAY            = 1,   // Runway centerline points
    PLT_SAM               = 4,   // SAM placement points
    PLT_ARTILLERY         = 5,   // Artillery placement points
    PLT_AAA               = 6,   // AAA placement points
    PLT_RUNWAY_DIM        = 8,   // Runway dimensional marks
    PLT_STATIC_RADAR      = 10,  // Static radar placement
    PLT_PARK              = 11,  // Parking spots (small + large mixed)
    PLT_RUNWAY_LT         = 12,  // Runway left-side points
    PLT_RUNWAY_RT         = 13,  // Runway right-side points
    PLT_HELICOPTER        = 14,  // Helicopter landing spots
    PLT_FOLLOW_ME         = 15,  // Follow-me truck route
    PLT_DOCK              = 16,  // Docking points
    PLT_TRACK             = 17,  // Ground vehicle track
};

} // namespace f4::world_types
