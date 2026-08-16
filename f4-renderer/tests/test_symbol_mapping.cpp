// f4-renderer/tests/test_symbol_mapping.cpp
//
// Unit tests for symbol mapping functions. These are pure functions
// that map objective types and unit classes to SymbolKind values.
// No Raylib GPU context or ImGui needed.

#include <f4/renderer/symbols.hpp>

#include <gtest/gtest.h>

using namespace f4::renderer;
using UC = f4::entities::UnitClass;

// ── Objective type mapping ────────────────────────────────────────────────────

TEST(SymbolMapping, ObjectiveType_Airbase) {
    EXPECT_EQ(SymbolKind::ObjAirbase, symbol_for_objective_type(1));
}

TEST(SymbolMapping, ObjectiveType_Airstrip) {
    EXPECT_EQ(SymbolKind::ObjAirstrip, symbol_for_objective_type(2));
}

TEST(SymbolMapping, ObjectiveType_ArmyBase) {
    EXPECT_EQ(SymbolKind::ObjArmyBase, symbol_for_objective_type(3));
}

TEST(SymbolMapping, ObjectiveType_Beach) {
    EXPECT_EQ(SymbolKind::ObjBeach, symbol_for_objective_type(4));
}

TEST(SymbolMapping, ObjectiveType_Border) {
    EXPECT_EQ(SymbolKind::ObjBorder, symbol_for_objective_type(5));
}

TEST(SymbolMapping, ObjectiveType_Bridge) {
    EXPECT_EQ(SymbolKind::ObjBridge, symbol_for_objective_type(6));
}

TEST(SymbolMapping, ObjectiveType_Chemical) {
    EXPECT_EQ(SymbolKind::ObjChemical, symbol_for_objective_type(7));
}

TEST(SymbolMapping, ObjectiveType_City) {
    EXPECT_EQ(SymbolKind::ObjCity, symbol_for_objective_type(8));
}

TEST(SymbolMapping, ObjectiveType_ComControl) {
    EXPECT_EQ(SymbolKind::ObjComControl, symbol_for_objective_type(9));
}

TEST(SymbolMapping, ObjectiveType_Depot) {
    EXPECT_EQ(SymbolKind::ObjDepot, symbol_for_objective_type(10));
}

TEST(SymbolMapping, ObjectiveType_Factory) {
    EXPECT_EQ(SymbolKind::ObjFactory, symbol_for_objective_type(11));
}

TEST(SymbolMapping, ObjectiveType_Ford) {
    EXPECT_EQ(SymbolKind::ObjFord, symbol_for_objective_type(12));
}

TEST(SymbolMapping, ObjectiveType_Fortification) {
    EXPECT_EQ(SymbolKind::ObjFortification, symbol_for_objective_type(13));
}

TEST(SymbolMapping, ObjectiveType_HillTop) {
    EXPECT_EQ(SymbolKind::ObjHillTop, symbol_for_objective_type(14));
}

TEST(SymbolMapping, ObjectiveType_Intersection) {
    EXPECT_EQ(SymbolKind::ObjIntersection, symbol_for_objective_type(15));
}

TEST(SymbolMapping, ObjectiveType_Nuclear) {
    EXPECT_EQ(SymbolKind::ObjNuclear, symbol_for_objective_type(17));
}

TEST(SymbolMapping, ObjectiveType_Port) {
    EXPECT_EQ(SymbolKind::ObjPort, symbol_for_objective_type(19));
}

TEST(SymbolMapping, ObjectiveType_PowerPlant) {
    EXPECT_EQ(SymbolKind::ObjPowerPlant, symbol_for_objective_type(20));
}

TEST(SymbolMapping, ObjectiveType_Radar) {
    EXPECT_EQ(SymbolKind::ObjRadar, symbol_for_objective_type(21));
}

TEST(SymbolMapping, ObjectiveType_SamSite) {
    EXPECT_EQ(SymbolKind::ObjSamSite, symbol_for_objective_type(27));
}

TEST(SymbolMapping, ObjectiveType_Town) {
    EXPECT_EQ(SymbolKind::ObjTown, symbol_for_objective_type(28));
}

TEST(SymbolMapping, ObjectiveType_Village) {
    EXPECT_EQ(SymbolKind::ObjVillage, symbol_for_objective_type(29));
}

TEST(SymbolMapping, ObjectiveType_Unknown_Fallback) {
    // Types not in the mapping table should return ObjUnknown
    EXPECT_EQ(SymbolKind::ObjUnknown, symbol_for_objective_type(0));
    EXPECT_EQ(SymbolKind::ObjUnknown, symbol_for_objective_type(100));
    EXPECT_EQ(SymbolKind::ObjUnknown, symbol_for_objective_type(255));
}

// ── Unit class mapping ────────────────────────────────────────────────────────

TEST(SymbolMapping, UnitClass_Battalion) {
    EXPECT_EQ(SymbolKind::UnitBattalion, symbol_for_unit(UC::Battalion, 0));
}

TEST(SymbolMapping, UnitClass_Brigade) {
    EXPECT_EQ(SymbolKind::UnitBrigade, symbol_for_unit(UC::Brigade, 0));
}

TEST(SymbolMapping, UnitClass_Squadron) {
    EXPECT_EQ(SymbolKind::UnitSquadron, symbol_for_unit(UC::Squadron, 0));
}

TEST(SymbolMapping, UnitClass_TaskForce) {
    // TaskForce with subtype 0 returns UnitNavalSurface (the default
    // for naval/sea units). Only subtype 3 (carrier) gets UnitCarrier.
    EXPECT_EQ(SymbolKind::UnitNavalSurface, symbol_for_unit(UC::TaskForce, 0));
}

TEST(SymbolMapping, UnitClass_TaskForce_Carrier) {
    EXPECT_EQ(SymbolKind::UnitCarrier, symbol_for_unit(UC::TaskForce, 3));
}

TEST(SymbolMapping, UnitClass_Flight) {
    EXPECT_EQ(SymbolKind::UnitFlight, symbol_for_unit(UC::Flight, 0));
}

TEST(SymbolMapping, UnitClass_Package) {
    EXPECT_EQ(SymbolKind::UnitPackage, symbol_for_unit(UC::Package, 0));
}

TEST(SymbolMapping, UnitClass_Unknown) {
    EXPECT_EQ(SymbolKind::UnitUnknown, symbol_for_unit(UC::Unknown, 0));
}

// ── Ground unit subtype mapping (Battalion) ───────────────────────────────────

TEST(SymbolMapping, Battalion_AirDefense) {
    EXPECT_EQ(SymbolKind::UnitAirDefense, symbol_for_unit(UC::Battalion, 1));
}

TEST(SymbolMapping, Battalion_Airmobile) {
    EXPECT_EQ(SymbolKind::UnitAirmobile, symbol_for_unit(UC::Battalion, 2));
}

TEST(SymbolMapping, Battalion_Armor) {
    EXPECT_EQ(SymbolKind::UnitArmor, symbol_for_unit(UC::Battalion, 3));
}

TEST(SymbolMapping, Battalion_ArmoredCav) {
    EXPECT_EQ(SymbolKind::UnitArmoredCav, symbol_for_unit(UC::Battalion, 4));
}

TEST(SymbolMapping, Battalion_Engineer) {
    EXPECT_EQ(SymbolKind::UnitEngineer, symbol_for_unit(UC::Battalion, 5));
}

TEST(SymbolMapping, Battalion_HQ) {
    EXPECT_EQ(SymbolKind::UnitHQ, symbol_for_unit(UC::Battalion, 6));
}

TEST(SymbolMapping, Battalion_Infantry) {
    EXPECT_EQ(SymbolKind::UnitInfantry, symbol_for_unit(UC::Battalion, 7));
}

TEST(SymbolMapping, Battalion_Marine) {
    EXPECT_EQ(SymbolKind::UnitMarine, symbol_for_unit(UC::Battalion, 8));
}

TEST(SymbolMapping, Battalion_Mechanized) {
    EXPECT_EQ(SymbolKind::UnitMechanized, symbol_for_unit(UC::Battalion, 9));
}

TEST(SymbolMapping, Battalion_Rocket) {
    EXPECT_EQ(SymbolKind::UnitRocket, symbol_for_unit(UC::Battalion, 10));
}

TEST(SymbolMapping, Battalion_SPArtillery) {
    EXPECT_EQ(SymbolKind::UnitArtillery, symbol_for_unit(UC::Battalion, 11));
}

TEST(SymbolMapping, Battalion_SAMissile) {
    EXPECT_EQ(SymbolKind::UnitSAMissile, symbol_for_unit(UC::Battalion, 12));
}

TEST(SymbolMapping, Battalion_Supply) {
    EXPECT_EQ(SymbolKind::UnitSupply, symbol_for_unit(UC::Battalion, 13));
}

TEST(SymbolMapping, Battalion_TowedArtillery) {
    EXPECT_EQ(SymbolKind::UnitArtillery, symbol_for_unit(UC::Battalion, 14));
}

// ── Ground unit subtype mapping (Brigade) — same glyphs, diamond frame ─────────

TEST(SymbolMapping, Brigade_AirDefense) {
    EXPECT_EQ(SymbolKind::UnitAirDefense, symbol_for_unit(UC::Brigade, 1));
}

TEST(SymbolMapping, Brigade_Mechanized) {
    EXPECT_EQ(SymbolKind::UnitMechanized, symbol_for_unit(UC::Brigade, 9));
}

TEST(SymbolMapping, Brigade_UnknownSubtype) {
    // Subtype 99 is not a valid land subtype — should fall back to bare brigade
    EXPECT_EQ(SymbolKind::UnitBrigade, symbol_for_unit(UC::Brigade, 99));
}

// ── SymbolKind enum properties ────────────────────────────────────────────────

TEST(SymbolKindEnum, CountIsPositive) {
    EXPECT_GT(static_cast<int>(SymbolKind::SymbolCount), 0);
}

TEST(SymbolKindEnum, AllObjectiveKindsAreBeforeUnits) {
    // Objective symbols should have lower values than unit symbols
    EXPECT_LT(static_cast<int>(SymbolKind::ObjUnknown),
              static_cast<int>(SymbolKind::UnitBattalion));
}

TEST(SymbolKindEnum, EachObjectiveKindIsDistinct) {
    // Verify no two objective types map to the same symbol
    const uint8_t known_types[] = {1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,17,19,20,21,27,28,29};
    std::vector<SymbolKind> seen;
    for (auto t : known_types) {
        auto kind = symbol_for_objective_type(t);
        // Each should be in the objective range
        EXPECT_LT(static_cast<int>(kind),
                  static_cast<int>(SymbolKind::UnitBattalion))
            << "Objective type " << (int)t << " mapped outside objective range";
        seen.push_back(kind);
    }
    // Check uniqueness (all distinct)
    for (size_t i = 0; i < seen.size(); ++i) {
        for (size_t j = i + 1; j < seen.size(); ++j) {
            EXPECT_NE(seen[i], seen[j])
                << "Objective types " << (int)known_types[i]
                << " and " << (int)known_types[j]
                << " map to the same SymbolKind";
        }
    }
}

// ── Determinism ───────────────────────────────────────────────────────────────

TEST(SymbolMapping, ObjectiveType_Deterministic) {
    // Same input should always produce same output
    for (int i = 0; i < 40; ++i) {
        auto a = symbol_for_objective_type(static_cast<uint8_t>(i));
        auto b = symbol_for_objective_type(static_cast<uint8_t>(i));
        EXPECT_EQ(a, b);
    }
}

TEST(SymbolMapping, UnitClass_Deterministic) {
    for (int cls = 0; cls <= 6; ++cls) {
        for (int sub = 0; sub <= 14; ++sub) {
            auto a = symbol_for_unit(static_cast<UC>(cls), static_cast<uint8_t>(sub));
            auto b = symbol_for_unit(static_cast<UC>(cls), static_cast<uint8_t>(sub));
            EXPECT_EQ(a, b);
        }
    }
}
