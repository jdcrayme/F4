// f4-world-types/include/f4/world_types/campaign_names.hpp
//
// Runtime-safe human-readable names for campaign enum values.
//
// Tranche 0d: the f4-world-viewer draws labels from these helpers but
// must not link f4-world-convert (the binary parser library) — so the
// pure enum→string mappings live here, next to the runtime class table.
// The importer-side copies in f4-world-convert (objective_decoder.hpp,
// theater_data.hpp) keep their signatures for the converter CLIs; the
// two sets must stay textually identical.
//
// Signatures mirror the f4-world-convert originals exactly, so viewer
// call sites switch by changing the namespace qualifier only.

#pragma once

#include <cstdint>
#include <string>

namespace f4::world_types {

/// FreeFalcon objective type constants (from classtbl.h:65). The
/// campaign-objective-relevant subset (mirrors f4-world-convert's
/// ObjectiveType).
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

/// Human-readable name for a theater point type (PtData entries).
[[nodiscard]] const char* point_type_name(uint8_t pt) noexcept;

/// Human-readable name for a theater point-list type.
[[nodiscard]] const char* point_list_type_name(uint8_t plt) noexcept;

/// Human-readable name for a movement type index (falcent.h MoveType).
[[nodiscard]] const char* movement_type_name(int32_t mt) noexcept;

/// Human-readable name for a damage type index.
[[nodiscard]] const char* damage_type_name(int32_t dt) noexcept;

} // namespace f4::world_types
