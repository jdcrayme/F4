// f4-world-types/src/campaign_names.cpp
//
// Runtime-safe campaign enum→name mappings (see campaign_names.hpp).
// Textually mirrors the f4-world-convert implementations
// (objective_decoder.cpp / theater_data.cpp) — keep both in sync.

#include <f4/world_types/campaign_names.hpp>

namespace f4::world_types {

std::string objective_type_name(int16_t type) {
    switch (type) {
        case TYPE_AIRBASE:       return "Airbase";
        case TYPE_AIRSTRIP:      return "Airstrip";
        case TYPE_ARMYBASE:      return "Army Base";
        case TYPE_BEACH:         return "Beach";
        case TYPE_BORDER:        return "Border";
        case TYPE_BRIDGE:        return "Bridge";
        case TYPE_CHEMICAL:      return "Chemical";
        case TYPE_CITY:          return "City";
        case TYPE_COM_CONTROL:   return "Com Control";
        case TYPE_DEPOT:         return "Depot";
        case TYPE_FACTORY:       return "Factory";
        case TYPE_FORD:          return "Ford";
        case TYPE_FORTIFICATION: return "Fortification";
        case TYPE_HILL_TOP:      return "Hill Top";
        case TYPE_INTERSECT:     return "Intersection";
        case TYPE_NUCLEAR:       return "Nuclear Plant";
        case TYPE_PASS:          return "Pass";
        case TYPE_PORT:          return "Port";
        case TYPE_POWERPLANT:    return "Power Plant";
        case TYPE_RADAR:         return "Radar";
        case TYPE_RADIO_TOWER:   return "Radio Tower";
        case TYPE_RAIL_TERMINAL: return "Rail Terminal";
        case TYPE_RAILROAD:      return "Railroad";
        case TYPE_TOWN:          return "Town";
        default:                 return "Objective#" + std::to_string(type);
    }
}

const char* point_type_name(uint8_t pt) noexcept {
    switch (pt) {
        case 1:  return "Runway";        // PT_RUNWAY
        case 2:  return "Takeoff";       // PT_TAKEOFF
        case 3:  return "Taxi";          // PT_TAXI
        case 4:  return "SAM";           // PT_SAM
        case 5:  return "Artillery";     // PT_ARTILLERY
        case 6:  return "AAA";           // PT_AAA
        case 7:  return "Radar";         // PT_RADAR
        case 8:  return "Runway Dim";    // PT_RUNWAY_DIM
        case 9:  return "Support";       // PT_SUPPORT
        case 10: return "Static Radar";  // PT_STATIC_RADAR
        case 11: return "Small Park";    // PT_SMALL_PARK
        case 12: return "Large Park";    // PT_LARGE_PARK
        case 13: return "Small Dock";    // PT_SMALL_DOCK
        case 14: return "Large Dock";    // PT_LARGE_DOCK
        case 15: return "Take Runway";   // PT_TAKE_RUNWAY
        case 16: return "Helicopter";    // PT_HELICOPTER
        case 17: return "Follow Me";     // PT_FOLLOW_ME
        case 18: return "Track";         // PT_TRACK
        case 19: return "Crit Taxi";     // PT_CRIT_TAXI
        default: return "Unknown";
    }
}

const char* point_list_type_name(uint8_t plt) noexcept {
    switch (plt) {
        case 1:  return "Runway";        // PLT_RUNWAY
        case 4:  return "SAM";           // PLT_SAM
        case 5:  return "Artillery";     // PLT_ARTILLERY
        case 6:  return "AAA";           // PLT_AAA
        case 8:  return "Runway Dim";    // PLT_RUNWAY_DIM
        case 10: return "Static Radar";  // PLT_STATIC_RADAR
        case 11: return "Parking";       // PLT_PARK
        case 12: return "Runway Left";   // PLT_RUNWAY_LT
        case 13: return "Runway Right";  // PLT_RUNWAY_RT
        case 14: return "Helicopter";    // PLT_HELICOPTER
        case 15: return "Follow Me";     // PLT_FOLLOW_ME
        case 16: return "Dock";          // PLT_DOCK
        case 17: return "Track";         // PLT_TRACK
        default: return "Unknown";
    }
}

const char* movement_type_name(int32_t mt) noexcept {
    switch (mt) {
        case 0: return "NoMove";
        case 1: return "Foot";
        case 2: return "Wheeled";
        case 3: return "Tracked";
        case 4: return "LowAir";
        case 5: return "Air";
        case 6: return "Naval";
        case 7: return "Rail";
        default: return "Unknown";
    }
}

const char* damage_type_name(int32_t dt) noexcept {
    switch (dt) {
        case 0: return "None";
        case 1: return "Penetration";
        case 2: return "HE";
        case 3: return "Heave";
        case 4: return "Incendiary";
        case 5: return "Proximity";
        case 6: return "Kinetic";
        case 7: return "Hydrostatic";
        case 8: return "Chemical";
        case 9: return "Nuclear";
        default: return "Unknown";
    }
}

} // namespace f4::world_types
