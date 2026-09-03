// f4-convert/veh_parser.hpp
//
// Parser for the legacy FreeFalcon vehicle class definitions
// (sim/VehDef/Vehicle.lst + sim/VehDef/*.veh).
//
// Ported 1:1 from FreeFalcon's vehdef.cpp:
//   - SimMoverDefinition::ReadSimMoverDefinitionData (vehdef.cpp:29-85):
//     Vehicle.lst = count token, then (type, file) token pairs; the
//     type selects the per-class reader.
//   - SimACDefinition   (vehdef.cpp:96-159):  combat class, airframe
//     index, player sensor count + (type, idx) pairs, AI sensor count +
//     (type, idx) pairs.
//   - SimHeloDefinition (vehdef.cpp:186-218): airframe index, sensor
//     count + pairs.
//   - SimGroundDefinition (vehdef.cpp:220-252): sensor count + pairs.
//   - SimWpnDefinition  (vehdef.cpp:161-184): flags, cd, weight, area,
//     x/y/z ejection, mnemonic, weapon class, domain, weapon type,
//     data idx.
//
// REFERENCE QUIRKS (documented, load-bearing for fidelity):
//   - Sea rows (type 4) read the filename and NEVER open it — ship.veh
//     / sub.veh / torpedo.veh / the typo'd "dpthchrg,veh" are not in
//     SimData.zip and don't need to be. This parser records the row
//     and moves on.
//   - Windows-style mixed-case paths in the list ("Sim\VehDef\f16.veh")
//     are resolved case-insensitively against the directory holding
//     the .lst file (the reference's case-insensitive file open).
//   - Row "-1 unused" is recorded as MoverType::Unused.
//
// The parser is strict about the positional structure (errors) and
// permissive about extra trailing tokens (warnings).

#pragma once

#include "f4/data/vehicle_def_data.hpp"

#include <string>
#include <vector>

namespace f4::convert {

// Result of parsing a Vehicle.lst (+ every .veh it references).
struct VehParseResult {
    f4::data::VehicleDefinitionLibrary library;
    std::vector<std::string> warnings;   // non-fatal (duplicates, etc.)
    std::vector<std::string> errors;     // fatal (structural)
    bool ok = false;
};

/// Parse a Vehicle.lst from disk; every referenced .veh is opened from
/// the same directory (case-insensitive path resolution).
[[nodiscard]] VehParseResult loadVehicleLstFile(const std::string& lstPath);

/// Parse from an in-memory Vehicle.lst string. `vehDir` is the directory
/// the .veh files live in (same directory semantics as the file load);
/// pass "" to forbid file opens (every definition row errors — useful
/// for structural tests).
[[nodiscard]] VehParseResult loadVehicleLstString(
    const std::string& lstContents,
    const std::string& vehDir,
    const std::string& sourceName = "<string>");

/// Parse a single .veh file from disk (type given by the caller — the
/// .lst is what routes types to files in the reference).
[[nodiscard]] VehParseResult loadVehFile(const std::string& vehPath,
                                         f4::data::MoverType type);

/// Parse a single .veh file from an in-memory string.
[[nodiscard]] VehParseResult loadVehString(const std::string& contents,
                                           f4::data::MoverType type,
                                           const std::string& sourceName);

} // namespace f4::convert
