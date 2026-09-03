// f4-campaign/tests/test_threat_map.cpp
//
// C3 tranche tests — ThreatMap (the ScoreThreatFast port):
//   * ownership resolution (nearest objective, 0xF beyond range)
//   * the altitude-band score formulas (2-bit counters x band weights)
//   * the viewer bit-half rule (one map serves both belligerents)
//   * RoE: hostile territory is lethal (32000), enemy land adds +10 at
//     low altitude
//   * counter saturation at 3 (the wire's 2-bit packing limit)
//   * the empty world (no AD units — nothing threatens anyone)

#include <f4/campaign/threat_map.hpp>
#include <f4/world/world_adapters.hpp>

#include <gtest/gtest.h>

#include <array>

using namespace f4::campaign;
using f4::entities::UnitClass;
using f4::world::UnitState;

namespace {

// The test world: USA (slot 1) and DPRK (slot 6) at war; a neutral UN
// team (slot 3) nobody fights. Objectives: the USA airbase (100,100),
// the DPRK target (400,400), a neutral objective (300,300). One DPRK
// air-defense battalion sits ON the neutral objective's cell — its cell
// is neutral-owned, so overflight is legal and the band formulas are
// observable directly (a hostile-owned cell would short-circuit to the
// lethal score before the formulas run).
f4::world::WorldState make_world(int extra_ad_on_neutral = 0) {
    using f4::world::ObjectiveState;
    using f4::world::TeamState;

    f4::world::WorldState ws;
    ws.version = 71;

    ws.teams.resize(8);
    ws.teams[1] = TeamState{1, 1, 1, "USA", ""};
    ws.teams[3] = TeamState{3, 0, 3, "UN", ""};
    ws.teams[6] = TeamState{6, 6, 6, "DPRK", ""};
    ws.teams[1].stance = {0, 0, 0, 0, 0, 0, 5, 0};
    ws.teams[6].stance = {0, 5, 0, 0, 0, 0, 0, 0};

    auto obj = [](int16_t x, int16_t y, uint8_t owner, uint32_t vu,
                  uint8_t priority) {
        ObjectiveState o;
        o.x = x;
        o.y = y;
        o.owner = owner;
        o.id_num = vu;
        o.priority = priority;
        o.objective_type = 4;  // airbase-ish; irrelevant to the map
        return o;
    };
    ws.objectives.push_back(obj(100, 100, 1, 4281, 5));   // USA airbase
    ws.objectives.push_back(obj(400, 400, 6, 9001, 7));   // DPRK target
    ws.objectives.push_back(obj(300, 300, 3, 5150, 3));   // neutral

    auto ad = [](int16_t x, int16_t y, uint8_t owner, uint32_t vu) {
        UnitState u;
        u.unit_class = UnitClass::Battalion;
        u.domain = 3;             // DOMAIN_LAND
        u.unit_subtype = 1;       // STYPE_LAND_AIR_DEFENSE
        u.x = x;
        u.y = y;
        u.owner = owner;
        u.id_num = vu;
        u.class_name = "Air Defense";
        // Low-air ring 24 grid units, air ring 42; both bands can hit.
        u.unit_hit_chance = {0, 0, 0, 0, 60, 55, 0, 0};
        u.unit_weapon_range = {0, 0, 0, 0, 24, 42, 0, 0};
        return u;
    };
    ws.units.push_back(ad(300, 300, 6, 7001));
    for (int i = 0; i < extra_ad_on_neutral; ++i) {
        ws.units.push_back(ad(301, 301, 6,
                              static_cast<uint32_t>(7100 + i)));
    }
    return ws;
}

} // namespace

TEST(ThreatMap, OwnershipResolvesToNearestObjective) {
    const auto ws = make_world();
    f4::world::WorldStateAdapters adapters(ws);
    const ThreatMap map(adapters.objectives, adapters.units,
                        adapters.teams, /*viewer=*/1);

    // On the objectives themselves: exact ownership.
    EXPECT_EQ(map.owner_at(100, 100), 1);
    EXPECT_EQ(map.owner_at(400, 400), 6);
    EXPECT_EQ(map.owner_at(300, 300), 3);
    // Way off the data (far beyond the 80-grid search radius from any
    // cell there): unowned.
    EXPECT_EQ(map.owner_at(0, 0), 0x0F);
}

TEST(ThreatMap, BandFormulasMatchTheReferenceWeights) {
    const auto ws = make_world();
    f4::world::WorldStateAdapters adapters(ws);
    const ThreatMap map(adapters.objectives, adapters.units,
                        adapters.teams, /*viewer=*/1);

    // The DPRK AD sits on the neutral cell (300,300): exactly one unit
    // covers its own cell in both bands (low = high = 1 for USA).
    EXPECT_EQ(map.low_band_density(300, 300, 1), 1);
    EXPECT_EQ(map.high_band_density(300, 300, 1), 1);

    // ScoreThreatFast's formulas: Low = 28*low + 2*high (+10 over enemy
    // land — neutral here, so no bonus); Medium = 10*low + 23*high;
    // High = 30*high; VeryHigh = 15*high. Ground = 0.
    EXPECT_EQ(map.score(300, 300, AltBand::Low, 1), 28 + 2);
    EXPECT_EQ(map.score(300, 300, AltBand::Medium, 1), 10 + 23);
    EXPECT_EQ(map.score(300, 300, AltBand::High, 1), 30);
    EXPECT_EQ(map.score(300, 300, AltBand::VeryHigh, 1), 15);
    EXPECT_EQ(map.score(300, 300, AltBand::Ground, 1), 0);
}

TEST(ThreatMap, ViewerBitHalfRuleOneMapTwoSides) {
    const auto ws = make_world();
    f4::world::WorldStateAdapters adapters(ws);
    const ThreatMap map(adapters.objectives, adapters.units,
                        adapters.teams, /*viewer=*/1);

    // The map is built from USA's perspective: the DPRK AD (which can
    // engage USA) packs the viewer's half (bits 4-7). The viewer reads
    // its own half: threatened. The DPRK team reads the OTHER half
    // (bits 0-3 — units NOT hostile to the viewer): nothing there, so
    // its own defenses do not threaten it.
    EXPECT_GT(map.score(300, 300, AltBand::High, 1), 0);
    EXPECT_EQ(map.score(300, 300, AltBand::High, 6), 0);
    EXPECT_EQ(map.low_band_density(300, 300, 6), 0);
    EXPECT_EQ(map.high_band_density(300, 300, 6), 0);

    // Coverage stats: one battalion painted, and the painted cells
    // count regardless of HALF (the low half serves the enemy's
    // queries — a one-sided count would hide working rings).
    EXPECT_EQ(map.stats().ad_units, 1);
    EXPECT_GT(map.stats().threatened_cells, 0);
}

TEST(ThreatMap, WarTerritoryIsFlyableAndAddsTenAtLowAltitude) {
    const auto ws = make_world();
    f4::world::WorldStateAdapters adapters(ws);
    const ThreatMap map(adapters.objectives, adapters.units,
                        adapters.teams, /*viewer=*/1);

    // The reference's RoEData grants ROE_AIR_OVERFLY in WAR — strike
    // missions reach their targets through enemy land. The DPRK-owned
    // target cell is flyable for USA at every band (no 32000, no
    // short-circuit).
    EXPECT_LT(map.score(400, 400, AltBand::High, 1), 100);
    EXPECT_LT(map.score(400, 400, AltBand::Low, 1), 100);

    // The +10 general threat over WAR territory at LOW altitude: a
    // USA query at a cell covered by the AD AND owned by a team USA is
    // at war with. Rebuild with a big low ring so the DPRK-owned target
    // objective's cell (400,400) is inside the AD's ring: low = 1,
    // high = 1, +10 over war land = 28 + 2 + 10 = 40.
    auto ws2 = make_world();
    ws2.units[0].unit_weapon_range = {0, 0, 0, 0, 150, 150, 0, 0};
    f4::world::WorldStateAdapters ad2(ws2);
    const ThreatMap map2(ad2.objectives, ad2.units, ad2.teams,
                          /*viewer=*/1);
    EXPECT_EQ(map2.score(400, 400, AltBand::Low, 1), 28 + 2 + 10);
    // ...and NOT at high altitude (the +10 is the low-band bonus only).
    EXPECT_EQ(map2.score(400, 400, AltBand::High, 1), 30);
    // Over the NEUTRAL cell (covered, not at war): no +10.
    EXPECT_EQ(map2.score(300, 300, AltBand::Low, 1), 28 + 2);
}

TEST(ThreatMap, CountersSaturateAtThree) {
    // Four AD battalions on the same cell: the 2-bit counters cap at 3.
    const auto ws = make_world(3);
    f4::world::WorldStateAdapters adapters(ws);
    const ThreatMap map(adapters.objectives, adapters.units,
                        adapters.teams, /*viewer=*/1);

    EXPECT_EQ(map.low_band_density(300, 300, 1), 3);
    EXPECT_EQ(map.high_band_density(300, 300, 1), 3);
    // Low band: 3*28 + 3*2 = 90 (the reference's "roughly % chance to
    // be hit" ceiling).
    EXPECT_EQ(map.score(300, 300, AltBand::Low, 1), 90);
}

TEST(ThreatMap, NoAirDefenseNothingThreatens) {
    auto ws = make_world();
    ws.units.clear();
    f4::world::WorldStateAdapters adapters(ws);
    const ThreatMap map(adapters.objectives, adapters.units,
                        adapters.teams, /*viewer=*/1);

    EXPECT_EQ(map.score(300, 300, AltBand::Low, 1), 0);
    EXPECT_EQ(map.score(300, 300, AltBand::High, 1), 0);
    EXPECT_EQ(map.stats().ad_units, 0);
    // No density and no war-territory bonus where nobody is at war:
    // the neutral cell scores zero for everyone.
    EXPECT_EQ(map.score(300, 300, AltBand::Low, 3), 0);
}

TEST(ThreatMap, NonAirDefenseUnitsPaintNothing) {
    // An armor battalion with the same range fields: not in the
    // AirDefenseList — paints nothing.
    auto ws = make_world();
    ws.units[0].unit_subtype = 3;  // STYPE_LAND_ARMOR
    f4::world::WorldStateAdapters adapters(ws);
    const ThreatMap map(adapters.objectives, adapters.units,
                        adapters.teams, /*viewer=*/1);
    EXPECT_EQ(map.stats().ad_units, 0);
    EXPECT_EQ(map.stats().threatened_cells, 0);
    EXPECT_EQ(map.score(300, 300, AltBand::High, 1), 0);
}

TEST(ThreatMap, AltBandFromFeetBoundaries) {
    // The reference's MinAltAtLevel/MaxAltAtLevel bands (feet).
    EXPECT_EQ(alt_band_from_feet(0), AltBand::Ground);
    EXPECT_EQ(alt_band_from_feet(99), AltBand::Ground);
    EXPECT_EQ(alt_band_from_feet(100), AltBand::Low);
    EXPECT_EQ(alt_band_from_feet(4999), AltBand::Low);
    EXPECT_EQ(alt_band_from_feet(5000), AltBand::Medium);
    EXPECT_EQ(alt_band_from_feet(19999), AltBand::Medium);
    EXPECT_EQ(alt_band_from_feet(20000), AltBand::High);
    EXPECT_EQ(alt_band_from_feet(39999), AltBand::High);
    EXPECT_EQ(alt_band_from_feet(40000), AltBand::VeryHigh);
    EXPECT_EQ(alt_band_from_feet(30000), AltBand::High);
}
