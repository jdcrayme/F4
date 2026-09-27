// f4-renderer/src/symbols.cpp
//
// The SymbolKind address space: the canonical ObjectiveType → SymbolKind
// and (UnitClass, subtype) → SymbolKind mappers, plus the
// SymbolKind → library-key seam table.
//
// The procedural drawing that once shared this file is GONE — every
// symbol renders from the data-driven SymbolLibrary (symbol_library.cpp:
// f4_symbols.json + symbols/*.svg overrides) via draw_library_symbol();
// RenderEntityIcon falls back to a plain circle when a key is missing.
// See SYMBOL-SVG-1/2 in CHANGELOG.md for the saga.

#include <f4/renderer/symbols.hpp>

namespace f4::renderer {

// ---------------------------------------------------------------------------
// SymbolKind -> SymbolLibraryKey (the data-driven vocabulary's seam)
// ---------------------------------------------------------------------------
// Table order MUST match the SymbolKind enum order (objectives, unit
// frames, composed unit kinds in ground/air/naval order, UnitUnknown) —
// test_symbol_keys.cpp pins every key against the committed
// f4_symbols.json corpus and static_asserts the size below.
namespace {

constexpr SymbolLibraryKey kLibraryKeys[] = {
    // === Objectives ===
    {"obj_airbase", nullptr},       {"obj_airstrip", nullptr},
    {"obj_army_base", nullptr},     {"obj_beach", nullptr},
    {"obj_border", nullptr},        {"obj_bridge", nullptr},
    {"obj_chemical", nullptr},      {"obj_city", nullptr},
    {"obj_com_control", nullptr},   {"obj_depot", nullptr},
    {"obj_factory", nullptr},       {"obj_ford", nullptr},
    {"obj_fortification", nullptr}, {"obj_hill_top", nullptr},
    {"obj_intersection", nullptr},  {"obj_nuclear", nullptr},
    {"obj_pass", nullptr},          {"obj_port", nullptr},
    {"obj_power_plant", nullptr},   {"obj_radar", nullptr},
    {"obj_radio_tower", nullptr},   {"obj_rail_terminal", nullptr},
    {"obj_railroad", nullptr},      {"obj_refinery", nullptr},
    {"obj_road", nullptr},          {"obj_sam_site", nullptr},
    {"obj_town", nullptr},          {"obj_village", nullptr},
    {"obj_harts", nullptr},         {"obj_air_terminal", nullptr},
    {"obj_unknown", nullptr},
    // === Unit frames ===
    {"frame_battalion", nullptr},   {"frame_brigade", nullptr},
    {"frame_squadron", nullptr},    {"frame_task_force", nullptr},
    {"frame_flight", nullptr},      {"frame_package", nullptr},
    // === Ground unit kinds (composed symbol + frame-agnostic glyph) ===
    {"unit_armor", "glyph_armor"},
    {"unit_air_defense", "glyph_air_defense"},
    {"unit_airmobile", "glyph_airmobile"},
    {"unit_armored_cav", "glyph_armored_cav"},
    {"unit_artillery", "glyph_artillery"},
    {"unit_hq", "glyph_hq"},
    {"unit_infantry", "glyph_infantry"},
    {"unit_engineer", "glyph_engineer"},
    {"unit_marine", "glyph_marine"},
    {"unit_mechanized", "glyph_mechanized"},
    {"unit_rocket", "glyph_rocket"},
    {"unit_sa_missile", "glyph_sa_missile"},
    {"unit_supply", "glyph_supply"},
    // === Air unit kinds ===
    {"unit_fighter", "glyph_fighter"},
    {"unit_bomber", "glyph_bomber"},
    {"unit_transport", "glyph_transport"},
    {"unit_helicopter", "glyph_helicopter"},
    // === Naval unit kinds ===
    {"unit_carrier", "glyph_carrier"},
    {"unit_naval_surface", "glyph_naval_surface"},
    // === Fallback: no unit_unknown in the corpus — the circle frame IS
    // the procedural fallback's shape ===
    {"frame_squadron", nullptr},
};

static_assert(sizeof(kLibraryKeys) / sizeof(kLibraryKeys[0]) ==
                  static_cast<size_t>(SymbolKind::SymbolCount),
              "kLibraryKeys must cover every SymbolKind exactly once");

} // namespace

SymbolLibraryKey symbol_key_for_kind(SymbolKind kind) noexcept {
    const auto i = static_cast<std::size_t>(kind);
    if (i >= sizeof(kLibraryKeys) / sizeof(kLibraryKeys[0])) {
        return {"obj_unknown", nullptr};
    }
    return kLibraryKeys[i];
}

// ---------------------------------------------------------------------------
// Mapping tables (pure functions)
// ---------------------------------------------------------------------------

SymbolKind symbol_for_objective_type(uint8_t t) noexcept {
    switch (t) {
        case 1:  return SymbolKind::ObjAirbase;
        case 2:  return SymbolKind::ObjAirstrip;
        case 3:  return SymbolKind::ObjArmyBase;
        case 4:  return SymbolKind::ObjBeach;
        case 5:  return SymbolKind::ObjBorder;
        case 6:  return SymbolKind::ObjBridge;
        case 7:  return SymbolKind::ObjChemical;
        case 8:  return SymbolKind::ObjCity;
        case 9:  return SymbolKind::ObjComControl;
        case 10: return SymbolKind::ObjDepot;
        case 11: return SymbolKind::ObjFactory;
        case 12: return SymbolKind::ObjFord;
        case 13: return SymbolKind::ObjFortification;
        case 14: return SymbolKind::ObjHillTop;
        case 15: return SymbolKind::ObjIntersection;
        case 17: return SymbolKind::ObjNuclear;
        case 18: return SymbolKind::ObjPass;
        case 19: return SymbolKind::ObjPort;
        case 20: return SymbolKind::ObjPowerPlant;
        case 21: return SymbolKind::ObjRadar;
        case 22: return SymbolKind::ObjRadioTower;
        case 23: return SymbolKind::ObjRailTerminal;
        case 24: return SymbolKind::ObjRailroad;
        case 25: return SymbolKind::ObjRefinery;
        case 26: return SymbolKind::ObjRoad;
        case 27: return SymbolKind::ObjSamSite;
        case 28: return SymbolKind::ObjTown;
        case 29: return SymbolKind::ObjVillage;
        case 30: return SymbolKind::ObjHarts;
        case 39: return SymbolKind::ObjAirTerminal;
        default: return SymbolKind::ObjUnknown;
    }
}

SymbolKind symbol_for_unit(f4::entities::UnitClass cls, uint8_t subtype) noexcept {
    switch (cls) {
        case f4::entities::UnitClass::Battalion:
        case f4::entities::UnitClass::Brigade: {
            // Ground unit subtypes — same glyph vocabulary for both
            // battalion (rect frame) and brigade (diamond frame); the
            // composed library symbol carries the frame.
            switch (subtype) {
                case 1:  return SymbolKind::UnitAirDefense;  // STYPE_LAND_AIR_DEFENSE
                case 2:  return SymbolKind::UnitAirmobile;   // STYPE_LAND_AIRMOBILE
                case 3:  return SymbolKind::UnitArmor;       // STYPE_LAND_ARMOR
                case 4:  return SymbolKind::UnitArmoredCav;  // STYPE_LAND_ARMORED_CAV
                case 5:  return SymbolKind::UnitEngineer;    // STYPE_LAND_ENGINEER
                case 6:  return SymbolKind::UnitHQ;          // STYPE_LAND_HQ
                case 7:  return SymbolKind::UnitInfantry;    // STYPE_LAND_INFANTRY
                case 8:  return SymbolKind::UnitMarine;      // STYPE_LAND_MARINE
                case 9:  return SymbolKind::UnitMechanized;  // STYPE_LAND_MECHANIZED
                case 10: return SymbolKind::UnitRocket;      // STYPE_LAND_ROCKET
                case 11: return SymbolKind::UnitArtillery;   // STYPE_LAND_SP_ARTILLERY
                case 12: return SymbolKind::UnitSAMissile;   // STYPE_LAND_SS_MISSILE
                case 13: return SymbolKind::UnitSupply;      // STYPE_LAND_SUPPLY
                case 14: return SymbolKind::UnitArtillery;   // STYPE_LAND_TOWED_ARTILLERY
                default: break;
            }
            return (cls == f4::entities::UnitClass::Battalion)
                       ? SymbolKind::UnitBattalion
                       : SymbolKind::UnitBrigade;
        }
        case f4::entities::UnitClass::Squadron: {
            switch (subtype) {
                case 1:  return SymbolKind::UnitTransport;   // STYPE_AIR_AIR_TRANSPORT
                case 4:  return SymbolKind::UnitHelicopter;  // STYPE_AIR_ATTACK_HELO
                case 6:  return SymbolKind::UnitBomber;      // STYPE_AIR_BOMBER
                case 8:  return SymbolKind::UnitFighter;     // STYPE_AIR_FIGHTER
                case 9:  return SymbolKind::UnitFighter;     // STYPE_AIR_FIGHTER_BOMBER
                case 13: return SymbolKind::UnitTransport;   // STYPE_AIR_TANKER
                case 14: return SymbolKind::UnitHelicopter;  // STYPE_AIR_TRANSPORT_HELO
                default: break;
            }
            return SymbolKind::UnitSquadron;
        }
        case f4::entities::UnitClass::TaskForce: {
            switch (subtype) {
                case 3:  return SymbolKind::UnitCarrier;     // STYPE_SEA_CARRIER
                default: break;
            }
            return SymbolKind::UnitNavalSurface;
        }
        case f4::entities::UnitClass::Flight:   return SymbolKind::UnitFlight;
        case f4::entities::UnitClass::Package:  return SymbolKind::UnitPackage;
        default: return SymbolKind::UnitUnknown;
    }
}

} // namespace f4::renderer
