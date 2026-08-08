// f4-world-convert/include/f4/convert/class_table.hpp
//
// Parser for FreeFalcon's Falcon4.ct class table file.
//
// The class table maps entity_type (ushort, stored in .cam files as
// share_.entityType_) to the VuEntityType struct, which contains:
//   classInfo_[0] = VU_DOMAIN (0=air, 3=land, 4=sea, ...)
//   classInfo_[1] = VU_CLASS  (4=objective, 6=unit, 7=vehicle, ...)
//   classInfo_[2] = VU_TYPE   (for objectives: ObjectiveType enum 1-39)
//   classInfo_[3] = VU_STYPE  (subtype)
//
// Without this table, we can't map entity_type (100-2134) to ObjectiveType
// (1-39), which means we can't pick the right icon for each objective.
//
// File layout (Falcon4.ct):
//   [short NumEntities]
//   [NumEntities × 81-byte entries]
//
// Each entry is Falcon4EntityClassType on disk (81 bytes, natural alignment
// for the embedded VuEntityType + packed trailing fields):
//   offset  0: ushort id_
//   offset  2: ushort collisionType_
//   offset  4: float  collisionRadius_
//   offset  8: uchar  classInfo_[8]  ← the data we need
//   offset 16: uint   updateRate_
//   offset 20: uint   updateTolerance_
//   offset 24: float  bubbleRange_
//   offset 28: float  fineUpdateForceRange_
//   offset 32: float  fineUpdateMultiplier_
//   offset 36: uint   damageSeed_
//   offset 40: int    hitpoints_
//   offset 44: ushort majorRevisionNumber_
//   offset 46: ushort minorRevisionNumber_
//   offset 48: ushort createPriority_
//   offset 50: uchar  managementDomain_
//   offset 51: uchar  transferable_
//   offset 52: uchar  private_
//   offset 53: uchar  tangible_
//   offset 54: uchar  collidable_
//   offset 55: uchar  global_
//   offset 56: uchar  persistent_
//   offset 57: [3 bytes padding to align to 4]
//   offset 60: short  visType[7]       (14 bytes)
//   offset 74: short  vehicleDataIndex  (2 bytes)
//   offset 76: uchar  dataType          (1 byte)
//   offset 77: uint   dataPtr           (4 bytes, 32-bit on disk)
//   total = 81 bytes

#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace f4::world_convert {

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

/// Falcon4EntityClassType.dataType values (from classtbl.h). Tells us which
/// data table to index into via dataPtr. The on-disk values were verified
/// against real Falcon4.ct data (Phase 1 fix): the previously-documented
/// values were off by one or more, which silently broke the UCD enrichment
/// path in world_json.cpp (units always failed the `dt == DTYPE_UNIT`
/// check, so no unit ever received class_name / vehicle_groups / scores).
///
/// Verified mapping (entity class → on-disk dataType byte):
///   CLASS_FEATURE  (2) → dataType 1 → FCD table
///   CLASS_OBJECTIVE(4) → dataType 3 → OCD table
///   CLASS_UNIT     (6) → dataType 4 → UCD table
///   CLASS_VEHICLE  (7) → dataType 5 → VCD table
enum DataType : int {
    DTYPE_NOTHING    = 0,
    DTYPE_FEATURE    = 1,    // Falcon4.FCD
    DTYPE_OBJECTIVE  = 3,    // Falcon4.OCD
    DTYPE_UNIT       = 4,    // Falcon4.UCD
    DTYPE_VEHICLE    = 5,    // Falcon4.VCD
    DTYPE_WEAPON     = 6,    // (orientation uncertain — not used by F4 yet)
    DTYPE_SQUAD_STORES = 7,  // (orientation uncertain — not used by F4 yet)
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

/// One class table entry. We expose the fields the rest of the pipeline
/// needs: the classInfo_ tuple (domain/cls/type/stype), the trailing
/// dataType + dataPtr pair, AND the visType[7] array (offset 60 in the
/// on-disk record). visType[0] is the primary 3D model index used by
/// VisualModelComponent to resolve the entity's renderable via
/// f4::models::ModelDatabase; visType[1..6] are alternate LODs / damaged
/// states / wreckage models. The remaining CT fields (collisionType,
/// updateRate, hitpoints, etc.) are not exposed — they're for physics/
/// networking layers we haven't ported yet.
struct ClassTableEntry {
    uint8_t domain = 0;      // classInfo_[0]
    uint8_t cls = 0;         // classInfo_[1]
    uint8_t type = 0;        // classInfo_[2] — ObjectiveType for objectives
    uint8_t stype = 0;       // classInfo_[3]

    // Visual model indices (offset 60-73 in the on-disk record).
    // visType[0] is the primary model; visType[1..6] are alternates
    // (damaged states, LODs, etc. — exact semantics vary by entity class).
    // A value of 0 means "no model for this slot".
    // Used by f4-simulation::Simulation to resolve VisualModelComponent's
    // ModelRecord via ModelDatabase::model(vis_type[0]).
    int16_t vis_type[7] = {0, 0, 0, 0, 0, 0, 0};

    // Trailing fields (offset 76-80 in the on-disk Falcon4EntityClassType).
    // dataType tells us which data table to index into; dataPtr is the
    // 32-bit (on disk) index into that table. On 64-bit hosts we read it
    // as uint32_t (the on-disk value is always 32-bit per FF's design).
    uint8_t  data_type = 0;      // DTYPE_OBJECTIVE / DTYPE_UNIT / DTYPE_VEHICLE / ...
    uint32_t data_ptr_index = 0; // index into the corresponding data table
};

/// Human-readable name for a unit subtype. Looks up the appropriate enum
/// table (LandUnitSubtype / AirUnitSubtype / SeaUnitSubtype) based on
/// domain. Returns "Unknown" for unrecognized values. Used by the viewer
/// to show e.g. "Armor" / "Fighter" / "Carrier" next to the unit icon.
[[nodiscard]] inline const char* unit_subtype_name(uint8_t domain, uint8_t stype) {
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
                case STYPE_AIR_FIGHTER_BOMBER:   return "Fighter-Bomber";
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
                case STYPE_SEA_AMPHIBIOUS:       return "Amphibious";
                case STYPE_SEA_BATTLESHIP:       return "Battleship";
                case STYPE_SEA_CARRIER:          return "Carrier";
                case STYPE_SEA_CRUISER:          return "Cruiser";
                case STYPE_SEA_DESTROYER:        return "Destroyer";
                case STYPE_SEA_FRIGATE:          return "Frigate";
                case STYPE_SEA_PATROL:           return "Patrol";
                case STYPE_SEA_SEA_SUPPLY:       return "Sea Supply";
                case STYPE_SEA_SEA_TANKER:       return "Sea Tanker";
                case STYPE_SEA_SEA_TRANSPORT:    return "Sea Transport";
                default:                          break;
            }
            break;
        default:
            break;
    }
    return "Unknown";
}

/// Parsed Falcon4.ct class table. Maps entity_type (100+index) to
/// ClassTableEntry.
class ClassTable {
public:
    /// Load from a Falcon4.ct file. Throws on I/O or parse error.
    void load(const std::filesystem::path& ct_path);

    /// Look up an entity_type value. Returns nullptr if out of range.
    [[nodiscard]] const ClassTableEntry* lookup(uint16_t entity_type) const noexcept;

    /// Resolve entity_type → ObjectiveType (1-39). Returns 0 if the
    /// entity_type is not an objective or not found.
    [[nodiscard]] uint8_t objective_type_for(uint16_t entity_type) const noexcept;

    /// Resolve entity_type → unit subtype (STYPE_UNIT_*). Returns 0 if the
    /// entity_type is not a unit or not found. The subtype distinguishes
    /// battalion types (armor/infantry/artillery/supply/engineer/...) and
    /// squadron types (fighter/bomber/transport/awacs/...).
    [[nodiscard]] uint8_t unit_subtype_for(uint16_t entity_type) const noexcept;

    /// Resolve entity_type → data table index. The data_type field tells
    /// you which theater-data table to look up (objective/unit/vehicle/
    /// feature); data_ptr_index is the 0-based index into that table.
    /// Returns false if the entity_type is not found or has no data table
    /// entry (dataType == DTYPE_NOTHING).
    [[nodiscard]] bool data_ptr_for(uint16_t entity_type,
                                    uint8_t& out_data_type,
                                    uint32_t& out_data_ptr_index) const noexcept;

    /// Resolve entity_type → visual model index for a given visType slot.
    /// slot=0 is the primary model (the one VisualModelComponent should
    /// resolve); slot=1..6 are alternates (damaged / LOD / wreckage).
    /// Returns 0 if the entity_type is not found or the slot has no model
    /// (visType[slot] == 0 means "no model"). The caller should treat 0
    /// as "no renderable" and skip drawing.
    ///
    /// Note: entity_type ≠ vis_type! For example, entity_type 1052 is the
    /// F-16C Block 50 *squadron* (CLASS_UNIT) and points at model 1050
    /// (probably a squadron-level icon). The F-16 *aircraft* model itself
    /// has KoreaObj.HDR index 1052, and is referenced by VEHICLE-class
    /// entity_types like 273/276/285/719. The scenario JSON carries the
    /// vis_type_index (model index) directly — it does NOT go through the
    /// CT lookup. The CT lookup is for spawning aircraft from campaign
    /// data, where we know the entity_type but not the model index.
    [[nodiscard]] int16_t vis_type_for(uint16_t entity_type, int slot = 0) const noexcept;

    /// Number of entries loaded.
    [[nodiscard]] std::size_t size() const noexcept { return entries_.size(); }

    /// Whether the table has been loaded.
    [[nodiscard]] bool loaded() const noexcept { return !entries_.empty(); }

private:
    std::vector<ClassTableEntry> entries_;
};

/// Search for FALCON4.ct (the class table) in well-known locations.
/// Used by the cam2json CLI and the f4-world-viewer's in-process .cam
/// import path so both resolve objective_type without requiring the user
/// to point at the file manually.
///
/// Search order:
///   1. Same directory as `reference_file` (typically the .cam file —
///      the game ships FALCON4.ct next to its saves).
///   2. ../FALCON4.ct, ../../FALCON4.ct (if reference_file is nested).
///   3. CWD-relative: ./FALCON4.ct, ./assets/FALCON4.ct, ./temp/FALCON4.ct,
///      f4-world-convert/tests/fixtures/FALCON4.ct (+ ../ variants).
///
/// `reference_file` may be empty — only the CWD-relative search is performed.
/// Returns an empty path if not found.
[[nodiscard]] std::filesystem::path find_class_table(const std::filesystem::path& reference_file);

} // namespace f4::world_convert
