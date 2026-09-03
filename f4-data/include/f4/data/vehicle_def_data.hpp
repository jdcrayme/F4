// f4-data/vehicle_def_data.hpp
//
// Data-only representation of the SimMover vehicle class definitions
// (sim/VehDef/Vehicle.lst + sim/VehDef/*.veh).
//
// This is the class table that tells the engine WHAT each VU vehicle
// class is and — the part the AI actually consumes — WHICH SENSORS it
// mounts. Ported 1:1 from FreeFalcon's
//   - SimMoverDefinition::ReadSimMoverDefinitionData (vehdef.cpp:29-85):
//     Vehicle.lst = count, then (type, file) rows; the type selects the
//     per-class reader.
//   - SimACDefinition   (vehdef.cpp:96-159)  — aircraft: combat class,
//     airframe index, player sensor loadout, AI sensor loadout.
//   - SimHeloDefinition  (vehdef.cpp:186-218) — helo: airframe index +
//     one sensor loadout.
//   - SimGroundDefinition(vehdef.cpp:220-252) — ground: one sensor
//     loadout.
//   - SimWpnDefinition   (vehdef.cpp:161-184) — weapon: drag/weight/
//     area, ejection velocities, mnemonic, class/domain/type, data idx.
//
// MoverType values (mvrdef.h:7-18): 0 Aircraft, 1 Ground, 2 Helicopter,
// 3 Weapon, 4 Sea. Vehicle.lst also uses -1 for "unused" rows.
//
// REFERENCE QUIRKS (documented, load-bearing for fidelity):
//   - Sea rows (type 4) read the filename and NEVER open it
//     (vehdef.cpp:74-78: GetNext() then a bare SimMoverDefinition) —
//     ship.veh / sub.veh / torpedo.veh / "dpthchrg,veh" (a typo'd comma
//     in the shipped file) are not in SimData.zip and do not need to be.
//   - Paths in Vehicle.lst are Windows-style ("Sim\VehDef\f16.veh")
//     with mixed case; the reference opens them case-insensitively.
//     This parser resolves them case-insensitively against the
//     directory that holds Vehicle.lst.
//   - The shipped list contains duplicate stems (f16.veh is listed at
//     rows 2 and 42) — every row stays an entry; find() returns the
//     first match.
//
// SensorType values (the f16.veh file's own comment header; vehdef.cpp
// consumes them as raw ints):
//   0 IRST, 1 Radar, 2 RWR, 3 Visual, 4 HTS, 5 TargetingPod,
//   6 RadarHoming.
//
// Enum indices are kept as plain ints (not enum classes) so unknown/
// extended values survive the round trip without lossy clamping.

#pragma once

#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace f4::data {

// ---------------------------------------------------------------------------
// Enums + canonical names (all raw ints in the file; named here for
// humans and tests).
// ---------------------------------------------------------------------------

/// mvrdef.h MoverType + the list's -1 "unused" rows.
enum class MoverType {
    Unused    = -1,
    Aircraft  = 0,
    Ground    = 1,
    Helicopter = 2,
    Weapon    = 3,
    Sea       = 4,
};

/// MoverType index → name ("-1" rows are the list's own "unused" filler).
inline constexpr const char* kMoverTypeNames[] = {
    "Aircraft", "Ground", "Helicopter", "Weapon", "Sea"
};

/// SensorType count (f16.veh comment: "enum SensorType {... NumSensorTypes}").
inline constexpr std::size_t kNumSensorTypes = 7;

/// SensorType index → canonical name.
inline constexpr const char* kSensorTypeNames[kNumSensorTypes] = {
    "IRST", "Radar", "RWR", "Visual", "HTS", "TargetingPod", "RadarHoming"
};

/// CombatClass (acdef.h:23-34) — what the aircraft class fights like.
inline constexpr const char* kCombatClassNames[] = {
    "F4", "F5", "F14", "F15", "F16", "Mig25", "Mig27", "A10", "Bomber"
};
inline constexpr std::size_t kNumCombatClasses = 9;

/// WeaponClass (hardpnt.h:27-41).
inline constexpr const char* kWeaponClassNames[] = {
    "wcAimWpn", "wcRocketWpn", "wcBombWpn", "wcGunWpn", "wcECM", "wcTank",
    "wcAgmWpn", "wcHARMWpn", "wcSamWpn", "wcGbuWpn", "wcCamera", "wcNoWpn"
};
inline constexpr std::size_t kNumWeaponClasses = 12;

/// WeaponType (hardpnt.h:9-21).
inline constexpr const char* kWeaponTypeName[] = {
    "wtGuns", "wtAim9", "wtAim120", "wtAgm88", "wtAgm65", "wtMk82",
    "wtMk84", "wtGBU", "wtSAM", "wtLAU", "wtFixed", "wtNone", "wtGPS"
};
inline constexpr std::size_t kNumWeaponTypes = 13;

/// WeaponDomain (hardpnt.h:42-47) — bitmask (wdAir|wdGround = wdBoth).
enum WeaponDomain : int {
    wdNoDomain = 0,
    wdAir      = 0x1,
    wdGround   = 0x2,
    wdBoth     = 0x3,
};

// ---------------------------------------------------------------------------
// Sensor slots — (SensorType, dataset index) pairs (vehdef.cpp reads them
// as int pairs; f16.veh's comment documents the enum).
// ---------------------------------------------------------------------------
struct SensorSlot {
    int type = 0;    // SensorType index (see kSensorTypeNames)
    int index = 0;   // dataset index into that sensor's data table

    [[nodiscard]] bool operator==(const SensorSlot&) const = default;
};

// ---------------------------------------------------------------------------
// Per-type class definitions — one variant per reader in vehdef.cpp.
// ---------------------------------------------------------------------------

/// SimACDefinition (vehdef.cpp:96-159) — aircraft classes.
struct AircraftVehicleDef {
    int combat_class = 0;    // CombatClass (acdef.h), e.g. F-16 ships 4
    int airframe_index = 0;  // index into the ACTYPES.LST airframe table
    std::vector<SensorSlot> player_sensors;  // loadout when player-flown
    std::vector<SensorSlot> ai_sensors;      // loadout when AI-flown

    [[nodiscard]] bool operator==(const AircraftVehicleDef&) const = default;
};

/// SimHeloDefinition (vehdef.cpp:186-218).
struct HeloVehicleDef {
    int airframe_index = 0;
    std::vector<SensorSlot> sensors;

    [[nodiscard]] bool operator==(const HeloVehicleDef&) const = default;
};

/// SimGroundDefinition (vehdef.cpp:220-252).
struct GroundVehicleDef {
    std::vector<SensorSlot> sensors;

    [[nodiscard]] bool operator==(const GroundVehicleDef&) const = default;
};

/// SimWpnDefinition (vehdef.cpp:161-184) — the physical card for weapon
/// classes (drag coefficient, weight, reference area, ejection kick,
/// SMS mnemonic, and the class/domain/type triple the sim routes on).
struct WeaponVehicleDef {
    int flags = 0;
    double cd = 0.0;            // drag coefficient
    double weight = 0.0;        // pounds
    double area = 0.0;          // reference area, ft^2
    double x_ejection = 0.0;    // ejection velocity, ft/s
    double y_ejection = 0.0;
    double z_ejection = 0.0;
    std::string mnemonic;       // 8-char SMS display name
    int weapon_class = 0;       // WeaponClass (hardpnt.h)
    int domain = 0;             // WeaponDomain bitmask
    int weapon_type = 0;        // WeaponType (hardpnt.h)
    int data_idx = 0;           // index into the MISDATA missile datasets

    [[nodiscard]] bool operator==(const WeaponVehicleDef&) const = default;
};

// ---------------------------------------------------------------------------
// VehicleEntry — one Vehicle.lst row (+ the parsed definition when the
// type has one).
// ---------------------------------------------------------------------------
struct VehicleEntry {
    MoverType type = MoverType::Sea;
    std::string file;   // path exactly as listed ("Sim\VehDef\f16.veh")
    std::string name;   // lowercase stem for lookup ("f16")

    /// Only Aircraft/Ground/Helo/Weapon rows open a .veh file (Sea rows
    /// never do — vehdef.cpp:74-78).
    std::variant<AircraftVehicleDef, HeloVehicleDef, GroundVehicleDef,
                 WeaponVehicleDef>
        def{GroundVehicleDef{}};

    [[nodiscard]] bool has_definition() const noexcept {
        return type == MoverType::Aircraft || type == MoverType::Ground ||
               type == MoverType::Helicopter || type == MoverType::Weapon;
    }

    /// Typed accessors (nullptr when the row is not that type).
    [[nodiscard]] const AircraftVehicleDef* aircraft() const noexcept {
        return type == MoverType::Aircraft
                   ? &std::get<AircraftVehicleDef>(def)
                   : nullptr;
    }
    [[nodiscard]] const HeloVehicleDef* helo() const noexcept {
        return type == MoverType::Helicopter
                   ? &std::get<HeloVehicleDef>(def)
                   : nullptr;
    }
    [[nodiscard]] const GroundVehicleDef* ground() const noexcept {
        return type == MoverType::Ground
                   ? &std::get<GroundVehicleDef>(def)
                   : nullptr;
    }
    [[nodiscard]] const WeaponVehicleDef* weapon() const noexcept {
        return type == MoverType::Weapon
                   ? &std::get<WeaponVehicleDef>(def)
                   : nullptr;
    }
};

// ---------------------------------------------------------------------------
// The library — all rows in file (VU class index) order.
// ---------------------------------------------------------------------------
struct VehicleDefinitionLibrary {
    std::vector<VehicleEntry> entries;

    /// Case-insensitive stem lookup ("f16", "SA6", "Aim120"); first row
    /// with that stem wins (the shipped list lists f16 twice — rows 2
    /// and 42 — with identical content).
    [[nodiscard]] const VehicleEntry* find(std::string_view name) const noexcept;

    /// Row counts by type (index by MoverType value 0..4).
    [[nodiscard]] std::size_t count_of_type(MoverType type) const noexcept;
};

// ---------------------------------------------------------------------------
// JSON serialization (canonical format; f4-convert delegates here).
// ---------------------------------------------------------------------------
struct VehicleDefLibraryResult {
    VehicleDefinitionLibrary library;
    bool ok = false;
    std::vector<std::string> errors;
    std::vector<std::string> warnings;
};

/// Load from a JSON file on disk.
[[nodiscard]] VehicleDefLibraryResult loadVehicleDefinitionLibrary(
    const std::string& path);

/// Load from a JSON string.
[[nodiscard]] VehicleDefLibraryResult loadVehicleDefinitionLibraryFromString(
    const std::string& json);

/// Serialize to a pretty-printed JSON string.
[[nodiscard]] std::string writeVehicleDefinitionLibrary(
    const VehicleDefinitionLibrary& lib);

/// Write to a JSON file. Returns true on success.
bool writeVehicleDefinitionLibraryFile(const VehicleDefinitionLibrary& lib,
                                       const std::string& path);

} // namespace f4::data
