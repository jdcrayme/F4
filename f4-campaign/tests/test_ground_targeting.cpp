// test_ground_targeting.cpp — G2, the interdiction link's campaign-side
// tasking vocabulary, pinned over the same hand-built rig discipline
// test_ground_war.cpp keeps:
//
//   1. belligerent_pair: the first at-war NAMED pair in slot order
//      (the engine's own rule, extracted); unnamed slots never pair;
//      a war-less world yields none.
//   2. front_columns_from_objectives: the shared FLOT — contested
//      columns between the pair's forward holdings, the midpoint row,
//      sides by centroid (the smaller mean y holds the south).
//   3. rank_battalion_targets: the CAS target list — hostility filter
//      (symmetric War rows), land-domain Battalion class, non-empty
//      roster, ledger-destroyed skipped, front-distance ascending with
//      wire-order ties.

#include <f4/campaign/ground_war.hpp>
#include <f4/campaign/result_ledger.hpp>
#include <f4/world/world_adapters.hpp>

#include <gtest/gtest.h>

#include <memory>
#include <string>

using namespace f4::campaign;
using f4::world::WorldState;
using f4::world::WorldStateAdapters;

namespace {

/// 12 vehicles: six 2-vehicle groups (the wire's 2-bit packing).
constexpr std::uint32_t kRoster12 = 0xAAA;

f4::world::TeamState team(int slot, const std::string& name,
                          std::vector<int16_t> stance) {
    f4::world::TeamState t;
    t.slot = slot;
    t.name = name;
    t.stance = std::move(stance);
    t.tea_loaded = true;
    return t;
}

f4::world::ObjectiveState objective(std::uint32_t vu, int x, int y,
                                    std::uint8_t owner) {
    f4::world::ObjectiveState o;
    o.id_num = vu;
    o.x = static_cast<int16_t>(x);
    o.y = static_cast<int16_t>(y);
    o.owner = owner;
    o.first_owner = owner;
    o.priority = 30;
    return o;
}

f4::world::UnitState battalion(std::uint32_t vu, std::uint8_t owner,
                               int x, int y,
                               std::uint32_t roster = kRoster12) {
    f4::world::UnitState u;
    u.unit_class = f4::entities::UnitClass::Battalion;
    u.domain = 3;   // DOMAIN_LAND
    u.id_num = vu;
    u.owner = owner;
    u.x = static_cast<int16_t>(x);
    u.y = static_cast<int16_t>(y);
    u.roster = roster;
    return u;
}

/// The base rig: ROK 2 (south, y=100) vs DPRK 6 (north, y=140), the
/// front between their holdings; a neutral team 3.
WorldState base_world() {
    WorldState ws;
    ws.version = 71;
    ws.campaign.current_time = 1'000'000;
    ws.campaign.te_number_aircraft = {0, 0, 0, 0, 0, 0, 0, 0};

    std::vector<int16_t> rok{0, 0, 0, 3, 0, 0, 5, 0};
    std::vector<int16_t> dprk{0, 0, 5, 3, 0, 0, 0, 0};
    std::vector<int16_t> neutral{0, 0, 3, 0, 0, 0, 3, 0};
    ws.teams = {team(2, "ROK", rok), team(6, "DPRK", dprk),
                team(3, "Neutral", neutral)};

    // Objectives: ROK holds y=100, DPRK y=140 — the contested columns
    // carry the midpoint 120.
    ws.objectives = {
        objective(100, 10, 100, 2), objective(101, 11, 100, 2),
        objective(102, 10, 140, 6), objective(103, 11, 140, 6),
        objective(104, 50, 100, 2),   // far column: uncontested
    };
    return ws;
}

} // namespace

// ---------------------------------------------------------------------------
// belligerent_pair
// ---------------------------------------------------------------------------

TEST(GroundTargeting, BelligerentPairFirstAtWarNamedSlots) {
    WorldState ws = base_world();
    WorldStateAdapters adapters(ws);
    const auto pair = belligerent_pair(adapters.teams);
    ASSERT_EQ(pair.size(), 2u);
    EXPECT_EQ(pair[0], 2);   // slot order: ROK first
    EXPECT_EQ(pair[1], 6);
}

TEST(GroundTargeting, BelligerentPairSkipsUnnamedAndPeace) {
    WorldState ws = base_world();
    // Unnamed belligerents never pair.
    ws.teams[0].name.clear();
    WorldStateAdapters adapters(ws);
    EXPECT_TRUE(belligerent_pair(adapters.teams).empty());

    // A war-less world yields none.
    WorldState peace = base_world();
    for (auto& t : peace.teams) {
        for (auto& s : t.stance) s = 3;
    }
    WorldStateAdapters peace_adapters(peace);
    EXPECT_TRUE(belligerent_pair(peace_adapters.teams).empty());
}

// ---------------------------------------------------------------------------
// front_columns_from_objectives
// ---------------------------------------------------------------------------

TEST(GroundTargeting, FrontColumnsContestedMidpoints) {
    WorldState ws = base_world();
    WorldStateAdapters adapters(ws);
    const auto view = front_objective_view(adapters.objectives);
    ASSERT_EQ(view.size(), 5u);
    EXPECT_EQ(view[0].x, 10);
    EXPECT_EQ(view[0].owner, 2);

    const auto front =
        front_columns_from_objectives(view, 2, 6);
    // Columns 10..50 are the objectives' x span; every column present.
    ASSERT_EQ(front.size(), 41u);   // x = 10..50
    EXPECT_EQ(front.front().x, 10);
    EXPECT_EQ(front.back().x, 50);

    // Sides by centroid: ROK (mean y 100) south, DPRK (140) north.
    EXPECT_EQ(front[0].south_owner, 2);
    EXPECT_EQ(front[0].north_owner, 6);

    // Column 10 contested: midpoint of (140 + 100)/2 = 120.
    EXPECT_TRUE(front[0].contested);
    EXPECT_EQ(front[0].y, 120);

    // The far column (50) has only a ROK objective in-band: not
    // contested, y invalid.
    const auto& far_col = front[static_cast<std::size_t>(50 - 10)];
    EXPECT_FALSE(far_col.contested);
}

TEST(GroundTargeting, FrontColumnsDegenerateInputs) {
    const std::vector<FrontObjectiveView> empty;
    EXPECT_TRUE(front_columns_from_objectives(empty, 1, 2).empty());
    // Same side twice: no pair.
    const std::vector<FrontObjectiveView> one{{10, 100, 2}};
    EXPECT_TRUE(front_columns_from_objectives(one, 2, 2).empty());
}

// ---------------------------------------------------------------------------
// rank_battalion_targets
// ---------------------------------------------------------------------------

TEST(GroundTargeting, RankTargetsFrontDistanceHostilityFilters) {
    WorldState ws = base_world();
    // Battalions: a DPRK battalion AT the front (10, 120), one 5 back
    // (10, 125), one far (10, 135); a ROK battalion (own side — never
    // a target for ROK CAS); a neutral team-3 battalion at the front;
    // a spent (roster 0) DPRK battalion; a non-land "battalion".
    ws.units = {
        battalion(200, 6, 10, 120),
        battalion(201, 6, 10, 125),
        battalion(202, 6, 10, 135),
        battalion(203, 2, 10, 120),          // own side
        battalion(204, 3, 10, 120),          // neutral
        battalion(205, 6, 10, 120, 0),       // spent
    };
    auto naval = battalion(206, 6, 10, 120);
    naval.domain = 4;   // air domain — not land
    ws.units.push_back(naval);

    WorldStateAdapters adapters(ws);
    const auto view = front_objective_view(adapters.objectives);
    const auto front = front_columns_from_objectives(view, 2, 6);

    const auto ranked = rank_battalion_targets(
        adapters.units, adapters.teams, front, /*team=*/2, nullptr);

    // Only the three live DPRK land battalions, nearest-first.
    ASSERT_EQ(ranked.size(), 3u);
    EXPECT_EQ(ranked[0], 200u);
    EXPECT_EQ(ranked[1], 201u);
    EXPECT_EQ(ranked[2], 202u);
}

TEST(GroundTargeting, RankTargetsWireOrderTies) {
    WorldState ws = base_world();
    // Two battalions at the SAME distance from their nearest contested
    // column (both dy = 10): the wire index breaks the tie.
    ws.units = {
        battalion(300, 6, 10, 130),
        battalion(299, 6, 11, 130),   // same squared distance, later wire
    };
    WorldStateAdapters adapters(ws);
    const auto view = front_objective_view(adapters.objectives);
    const auto front = front_columns_from_objectives(view, 2, 6);
    const auto ranked = rank_battalion_targets(
        adapters.units, adapters.teams, front, 2, nullptr);
    ASSERT_EQ(ranked.size(), 2u);
    EXPECT_EQ(ranked[0], 300u);   // wire order breaks the distance tie
    EXPECT_EQ(ranked[1], 299u);
}

TEST(GroundTargeting, RankTargetsLedgerDestroyedSkipped) {
    WorldState ws = base_world();
    ws.units = {
        battalion(400, 6, 10, 120),
        battalion(401, 6, 10, 121),
    };
    WorldStateAdapters adapters(ws);

    // A ledger marking 400 destroyed (the G1 sync shape).
    CampaignResultLedger ledger(adapters.campaign, adapters.teams,
                                adapters.units);
    GroundUnitLedger dead;
    dead.vu = 400;
    dead.owner = 6;
    dead.strength = 0;
    dead.destroyed = true;
    ledger.sync_ground_unit(dead);

    const auto view = front_objective_view(adapters.objectives);
    const auto front = front_columns_from_objectives(view, 2, 6);
    const auto ranked = rank_battalion_targets(
        adapters.units, adapters.teams, front, 2, &ledger);
    ASSERT_EQ(ranked.size(), 1u);
    EXPECT_EQ(ranked[0], 401u);
}

TEST(GroundTargeting, RankTargetsNoContestedFront) {
    // No contested columns: every candidate ties at max, wire order
    // decides (the honest degenerate case).
    WorldState ws;
    ws.version = 71;
    std::vector<int16_t> rok{0, 0, 0, 3, 0, 0, 5, 0};
    std::vector<int16_t> dprk{0, 0, 5, 3, 0, 0, 0, 0};
    ws.teams = {team(2, "ROK", rok), team(6, "DPRK", dprk)};
    // Only ROK objectives — no contested column anywhere.
    ws.objectives = {objective(100, 10, 100, 2)};
    ws.units = {battalion(500, 6, 10, 120), battalion(501, 6, 10, 130)};

    WorldStateAdapters adapters(ws);
    const auto view = front_objective_view(adapters.objectives);
    const auto front = front_columns_from_objectives(view, 2, 6);
    const auto ranked = rank_battalion_targets(
        adapters.units, adapters.teams, front, 2, nullptr);
    ASSERT_EQ(ranked.size(), 2u);
    EXPECT_EQ(ranked[0], 500u);   // wire order
    EXPECT_EQ(ranked[1], 501u);
}
