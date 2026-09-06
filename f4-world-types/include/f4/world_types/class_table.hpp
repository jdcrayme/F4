// f4-world-types/include/f4/world_types/class_table.hpp
//
// Runtime-safe class table: the JSON loader + lookup methods.
//
// Tranche 0d (NO_BINARY_RUNTIME_PLAN.md): the runtime subset of
// f4-world-convert's ClassTable. The binary FALCON4.ct decoder
// (ClassTable::load) + find_class_table() stay in f4-world-convert
// (importer-only); the runtime loads falcon4.ct.json via load_json /
// load_auto.
//
// Dependencies: f4-json (for the JSON reader), standard library only.

#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace f4::world_types {

/// VU classInfo_ indices (from f4vu.h).
enum : int {
    VU_DOMAIN = 0,
    VU_CLASS  = 1,
    VU_TYPE   = 2,
    VU_STYPE  = 3,
};

/// VU class values (from classtbl.h).
enum : int {
    CLASS_ABSTRACT   = 0,
    CLASS_ANIMAL     = 1,
    CLASS_FEATURE    = 2,
    CLASS_MANAGER    = 3,
    CLASS_OBJECTIVE  = 4,
    CLASS_SFX        = 5,
    CLASS_UNIT       = 6,
    CLASS_VEHICLE    = 7,
    CLASS_WEAPON     = 8,
};

/// VU domain values (from classtbl.h).
enum : int {
    DOMAIN_AIR   = 2,
    DOMAIN_LAND  = 3,
    DOMAIN_SEA   = 4,
};

/// VU_LAST_ENTITY_TYPE — entity_type values start at 100.
constexpr int VU_LAST_ENTITY_TYPE = 100;

/// Falcon4EntityClassType.dataType values (from classtbl.h).
enum DataType : int {
    DTYPE_NOTHING    = 0,
    DTYPE_FEATURE    = 1,    // Falcon4.FCD
    DTYPE_OBJECTIVE  = 3,    // Falcon4.OCD
    DTYPE_UNIT       = 4,    // Falcon4.UCD
    DTYPE_VEHICLE    = 5,    // Falcon4.VCD
    DTYPE_WEAPON     = 6,
    DTYPE_SQUAD_STORES = 7,
};

/// Unit subtypes for DOMAIN_LAND / TYPE_BATTALION (classtbl.h:319-332).
enum LandUnitSubtype : int {
    STYPE_LAND_AIR_DEFENSE     = 1,
    STYPE_LAND_AIRMOBILE       = 2,
    STYPE_LAND_ARMOR           = 3,
    STYPE_LAND_ARMORED_CAV     = 4,
    STYPE_LAND_ENGINEER        = 5,
    STYPE_LAND_HQ              = 6,
    STYPE_LAND_INFANTRY        = 7,
    STYPE_LAND_MARINE          = 8,
    STYPE_LAND_MECHANIZED      = 9,
    STYPE_LAND_ROCKET          = 10,
    STYPE_LAND_SP_ARTILLERY    = 11,
    STYPE_LAND_SS_MISSILE      = 12,
    STYPE_LAND_SUPPLY          = 13,
    STYPE_LAND_TOWED_ARTILLERY = 14,
};

/// Unit subtypes for DOMAIN_AIR / TYPE_SQUADRON (classtbl.h:360-373).
enum AirUnitSubtype : int {
    STYPE_AIR_AIR_TRANSPORT  = 1,
    STYPE_AIR_ASW            = 2,
    STYPE_AIR_ATTACK         = 3,
    STYPE_AIR_ATTACK_HELO    = 4,
    STYPE_AIR_AWACS          = 5,
    STYPE_AIR_BOMBER         = 6,
    STYPE_AIR_ECM            = 7,
    STYPE_AIR_FIGHTER        = 8,
    STYPE_AIR_FIGHTER_BOMBER = 9,
    STYPE_AIR_JSTAR          = 10,
    STYPE_AIR_RECON          = 11,
    STYPE_AIR_RECON_HELO     = 12,
    STYPE_AIR_TANKER         = 13,
    STYPE_AIR_TRANSPORT_HELO = 14,
};

/// Unit subtypes for DOMAIN_SEA / TYPE_TASKFORCE (classtbl.h:411-420).
enum SeaUnitSubtype : int {
    STYPE_SEA_AMPHIBIOUS = 1,
    STYPE_SEA_BATTLESHIP = 2,
    STYPE_SEA_CARRIER    = 3,
    STYPE_SEA_CRUISER    = 4,
    STYPE_SEA_DESTROYER  = 5,
    STYPE_SEA_FRIGATE    = 6,
    STYPE_SEA_PATROL     = 7,
    STYPE_SEA_SEA_SUPPLY = 8,
    STYPE_SEA_SEA_TANKER = 9,
    STYPE_SEA_SEA_TRANSPORT = 10,
};

/// One class table entry — the fields the runtime needs.
struct ClassTableEntry {
    uint8_t domain = 0;      // classInfo_[0]
    uint8_t cls = 0;         // classInfo_[1]
    uint8_t type = 0;        // classInfo_[2] — ObjectiveType for objectives
    uint8_t stype = 0;       // classInfo_[3]

    // Visual model indices (offset 60-73 in the on-disk record).
    // visType[0] is the primary model; visType[1..6] are alternates.
    // A value of 0 means "no model for this slot".
    int16_t vis_type[7] = {0, 0, 0, 0, 0, 0, 0};

    uint8_t  data_type = 0;      // DTYPE_OBJECTIVE / DTYPE_UNIT / ...
    uint32_t data_ptr_index = 0; // index into the corresponding data table
};

/// Parsed class table. Maps entity_type (100+index) to ClassTableEntry.
///
/// Runtime-safe: loads JSON only (falcon4.ct.json produced by ct2json).
/// The binary FALCON4.ct decoder lives in f4-world-convert (importer-only).
class ClassTable {
public:
    /// Load from a falcon4.ct.json file (produced by ct2json). Throws on
    /// I/O or parse error. Tranche 0a.1 (NO_BINARY_RUNTIME_PLAN.md).
    void load_json(const std::filesystem::path& json_path);

    /// Format-aware load: dispatches on the path extension. .json →
    /// load_json; .ct (or anything else) → throws (the binary decoder is
    /// not linked into the runtime; use f4-world-convert or ct2json first).
    /// Tranche 0a.3 (NO_BINARY_RUNTIME_PLAN.md).
    void load_auto(const std::filesystem::path& path);

    /// Look up an entity_type value. Returns nullptr if out of range.
    [[nodiscard]] const ClassTableEntry* lookup(uint16_t entity_type) const noexcept;

    /// Resolve entity_type → ObjectiveType (1-39). Returns 0 if not found.
    [[nodiscard]] uint8_t objective_type_for(uint16_t entity_type) const noexcept;

    /// Resolve entity_type → unit subtype. Returns 0 if not found.
    [[nodiscard]] uint8_t unit_subtype_for(uint16_t entity_type) const noexcept;

    /// Resolve entity_type → data table index. Returns false if not found
    /// or dataType == DTYPE_NOTHING.
    [[nodiscard]] bool data_ptr_for(uint16_t entity_type,
                                    uint8_t& out_data_type,
                                    uint32_t& out_data_ptr_index) const noexcept;

    /// Resolve entity_type → visual model index for a given visType slot.
    /// slot=0 is the primary model; slot=1..6 are alternates. Returns 0
    /// if not found or the slot has no model.
    [[nodiscard]] int16_t vis_type_for(uint16_t entity_type, int slot = 0) const noexcept;

    /// Number of entries loaded.
    [[nodiscard]] std::size_t size() const noexcept { return entries_.size(); }

    /// Whether the table has been loaded.
    [[nodiscard]] bool loaded() const noexcept { return !entries_.empty(); }

private:
    std::vector<ClassTableEntry> entries_;
};

/// Human-readable name for a unit subtype. Looks up the appropriate enum
/// table (LandUnitSubtype / AirUnitSubtype / SeaUnitSubtype) based on
/// domain. Returns "Unknown" for unrecognized values.
[[nodiscard]] inline const char* unit_subtype_name(uint8_t domain, uint8_t stype);

} // namespace f4::world_types

// ── Inline implementations ─────────────────────────────────────────────────

namespace f4::world_types {

inline const char* unit_subtype_name(uint8_t domain, uint8_t stype) {
    switch (domain) {
        case DOMAIN_LAND:
            switch (stype) {
                case STYPE_LAND_AIR_DEFENSE:     return "Air Defense";
                case STYPE_LAND_AIRMOBILE:       return "Airmobile";
                case STYPE_LAND_ARMOR:           return "Armor";
                case STYPE_LAND_ARMORED_CAV:     return "Armored Cav";
                case STYPE_LAND_ENGINEER:        return "Engineer";
                case STYPE_LAND_HQ:              return "HQ";
                case STYPE_LAND_INFANTRY:        return "Infantry";
                case STYPE_LAND_MARINE:          return "Marine";
                case STYPE_LAND_MECHANIZED:      return "Mechanized";
                case STYPE_LAND_ROCKET:          return "Rocket";
                case STYPE_LAND_SP_ARTILLERY:    return "SP Artillery";
                case STYPE_LAND_SS_MISSILE:      return "SS Missile";
                case STYPE_LAND_SUPPLY:          return "Supply";
                case STYPE_LAND_TOWED_ARTILLERY: return "Towed Artillery";
                default:                          break;
            }
            break;
        case DOMAIN_AIR:
            switch (stype) {
                case STYPE_AIR_AIR_TRANSPORT:    return "Air Transport";
                case STYPE_AIR_ASW:              return "ASW";
                case STYPE_AIR_ATTACK:           return "Attack";
                case STYPE_AIR_ATTACK_HELO:      return "Attack Helo";
                case STYPE_AIR_AWACS:            return "AWACS";
                case STYPE_AIR_BOMBER:           return "Bomber";
                case STYPE_AIR_ECM:              return "ECM";
                case STYPE_AIR_FIGHTER:          return "Fighter";
                case STYPE_AIR_FIGHTER_BOMBER:   return "Fighter Bomber";
                case STYPE_AIR_JSTAR:            return "JSTAR";
                case STYPE_AIR_RECON:            return "Recon";
                case STYPE_AIR_RECON_HELO:       return "Recon Helo";
                case STYPE_AIR_TANKER:           return "Tanker";
                case STYPE_AIR_TRANSPORT_HELO:   return "Transport Helo";
                default:                          break;
            }
            break;
        case DOMAIN_SEA:
            switch (stype) {
                case STYPE_SEA_AMPHIBIOUS:   return "Amphibious";
                case STYPE_SEA_BATTLESHIP:   return "Battleship";
                case STYPE_SEA_CARRIER:      return "Carrier";
                case STYPE_SEA_CRUISER:      return "Cruiser";
                case STYPE_SEA_DESTROYER:    return "Destroyer";
                case STYPE_SEA_FRIGATE:      return "Frigate";
                case STYPE_SEA_PATROL:       return "Patrol";
                case STYPE_SEA_SEA_SUPPLY:   return "Sea Supply";
                case STYPE_SEA_SEA_TANKER:   return "Sea Tanker";
                case STYPE_SEA_SEA_TRANSPORT:return "Sea Transport";
                default:                     break;
            }
            break;
        default: break;
    }
    return "Unknown";
}

} // namespace f4::world_types
