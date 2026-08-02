// f4-world-convert/include/f4/convert/unit_decoder.hpp
//
// Decodes the .uni sub-file (units: flights, battalions, squadrons, ships)
// from a .cam archive.
//
// .uni format (from FreeFalcon's SaveUnits / LoadUnits / DecodeUnitData,
// unit.cpp:5280/5302/6024):
//
//   [long]   outer_size     (written by SaveUnits; consumed by LoadUnits)
//   [short]  count          (number of unit records)
//   [long]   inner_size     (uncompressed size of the LZSS payload)
//   [bytes]  lzss_payload   (decompresses to inner_size bytes)
//
// The decompressed buffer is a sequence of `count` records, each:
//   [short]  type            (unit class: flight, battalion, squadron, ...)
//   [bytes]  CampBaseClass + UnitClass save data
//
// CampBaseClass is the same as for objectives (id, x, y, z, owner, ...).
// UnitClass::Save (unit.cpp:5770ish) appends: last_check, roster, unit_flags,
// dest_x, dest_y, target_id, cargo_id, moved, losses, tactic, current_wp,
// name_id, ... (variable-length depending on unit type).
//
// For the first pass we decode the CampBaseClass positional/identity fields
// (enough to place units on the map) and the first few fixed UnitClass
// fields. Variable-length trailing data is parsed conservatively.

#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace f4::convert {

struct UnitRecord {
    int16_t  type = 0;
    // CampBaseClass:
    uint32_t id_creator = 0;
    uint32_t id_num = 0;
    uint16_t entity_type = 0;
    int16_t  x = 0;
    int16_t  y = 0;
    float    z = 0.0f;
    int32_t  spot_time = 0;
    int16_t  spotted = 0;
    int16_t  base_flags = 0;
    uint8_t  owner = 0;
    int16_t  camp_id = 0;
    // UnitClass (first fixed fields):
    int32_t  last_check = 0;
    uint32_t roster = 0;
    uint32_t unit_flags = 0;
    int16_t  dest_x = 0;
    int16_t  dest_y = 0;
    uint8_t  moved = 0;
    uint8_t  losses = 0;
    uint8_t  tactic = 0;
    uint16_t current_wp = 0;
    int16_t  name_id = 0;
};

struct DecodedUnits {
    int16_t count = 0;
    std::vector<UnitRecord> units;
};

/// Decode the .uni sub-file. Throws on malformed input. Decodes the
/// CampBaseClass + fixed UnitClass fields per unit; variable-length trailing
/// unit-type-specific data is not yet parsed (cursor advances are
/// conservative — may under-count if a unit type has extra fields we don't
/// skip). This is acceptable for the first pass: positions and owners are
/// the visualization-critical fields.
[[nodiscard]] DecodedUnits decode_uni(const uint8_t* data, std::size_t size);

} // namespace f4::convert
