// test_ground_targeting.cpp — G2, the interdiction link's campaign-side
// tasking vocabulary, pinned over the same hand-built rig discipline
// test_ground_war.cpp keeps:
//
//   1. belligerent_pair: the first at-war NAMED pair in slot order
//      (the engine's own rule, extracted); unnamed slots never pair;
//      a war-less world yields none.
//   2. front_columns_from_battalions: the shared FLOT — the line
//      between the closest opposing BATTALIONS where they are in
//      contact (the contact rule), smoothed within runs; sides by
//      battalion centroid (the smaller mean y holds the south).
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
// front_columns_from_battalions — the contact front
// ---------------------------------------------------------------------------

TEST(GroundTargeting, FrontColumnsMidpointBetweenClosestPair) {
    // One battalion per side, 10 rows apart (inside the contact
    // range): the column they share is THE front, the line at the
    // midpoint. The battalions' x extent is the front's span — one
    // battalion pair, one column.
    WorldState ws = base_world();
    ws.units = {
        battalion(200, 2, 10, 130),   // ROK
        battalion(201, 6, 10, 140),   // DPRK
    };
    WorldStateAdapters adapters(ws);
    const auto view = front_unit_view(adapters.units);
    ASSERT_EQ(view.size(), 2u);
    EXPECT_EQ(view[0].owner, 2);

    const auto front = front_columns_from_battalions(view, 2, 6);
    ASSERT_EQ(front.size(), 1u);   // the battalions' x extent: column 10
    EXPECT_EQ(front[0].x, 10);
    // Sides by centroid: ROK (mean y 130) south, DPRK (140) north.
    EXPECT_EQ(front[0].south_owner, 2);
    EXPECT_EQ(front[0].north_owner, 6);
    EXPECT_TRUE(front[0].contested);
    EXPECT_EQ(front[0].y, 135);   // (130 + 140) / 2
}

TEST(GroundTargeting, FrontColumnsContactGateKeepsScoutsOffTheLine) {
    // The same pair 40 rows apart: dispositions, not a front — no
    // column is contested. (The old extremes-based rule drew a line
    // between ANY opposing pair, which is what scattered the front
    // across the theater.)
    WorldState ws = base_world();
    ws.units = {
        battalion(200, 2, 10, 100),   // ROK
        battalion(201, 6, 10, 140),   // DPRK — 40 rows north
    };
    WorldStateAdapters adapters(ws);
    const auto front = front_columns_from_battalions(
        front_unit_view(adapters.units), 2, 6);
    ASSERT_EQ(front.size(), 1u);
    EXPECT_FALSE(front[0].contested);
}

TEST(GroundTargeting, FrontColumnsSmoothingWithinRuns) {
    // A five-column run whose head column's closest pair is WIDER
    // (the DPRK flanker at (10,110) is the only north-side battalion
    // in column 10's band): raw midpoints [105, 102, 102, 102, 102].
    // The ±3 moving mean trims the head spike — every column smooths
    // to 102. (Within the ±3 band the closest pair may be a diagonal;
    // the exact raw rows are the algorithm's own business — the run
    // smoothing is what this pin watches.)
    std::vector<FrontUnitView> view = {
        {11, 100, 2},                              // ROK
        {10, 110, 6}, {14, 105, 6},                // DPRK
    };
    const auto front = front_columns_from_battalions(view, 2, 6);
    // The battalions' x extent: columns 10..14, one contiguous run.
    ASSERT_EQ(front.size(), 5u);
    std::vector<std::int32_t> rows;
    for (const auto& col : front) {
        EXPECT_TRUE(col.contested) << "column " << col.x;
        rows.push_back(col.y);
    }
    // The head's raw 105 smoothed away by its in-run window.
    EXPECT_EQ(rows, (std::vector<std::int32_t>{102, 102, 102, 102, 102}));
}

TEST(GroundTargeting, FrontColumnsDegenerateInputs) {
    const std::vector<FrontUnitView> empty;
    EXPECT_TRUE(front_columns_from_battalions(empty, 1, 2).empty());
    // Same side twice: no pair.
    const std::vector<FrontUnitView> one{{10, 100, 2}};
    EXPECT_TRUE(front_columns_from_battalions(one, 2, 2).empty());
}

TEST(GroundTargeting, FrontColumnsLedgerDestroyedSkipped) {
    // A destroyed battalion is not a front: the ledger's sync shape
    // drops the only DPRK battalion and the contact evaporates.
    WorldState ws = base_world();
    ws.units = {
        battalion(200, 2, 10, 130),
        battalion(201, 6, 10, 140),
    };
    WorldStateAdapters adapters(ws);
    CampaignResultLedger ledger(adapters.campaign, adapters.teams,
                                adapters.units);
    GroundUnitLedger rec;
    rec.vu = 201;
    rec.owner = 6;
    rec.strength = 0;
    rec.destroyed = true;
    ledger.sync_ground_unit(rec);
    const auto front = front_columns_from_battalions(
        front_unit_view(adapters.units, &ledger), 2, 6);
    ASSERT_EQ(front.size(), 1u);
    EXPECT_FALSE(front[0].contested);
}

// ---------------------------------------------------------------------------
// stamp_front_defended — the consolidation/display gate (the front
// itself no longer reads objectives at all)
// ---------------------------------------------------------------------------

TEST(GroundTargeting, StampSkipsWrongOwnerDestroyedAndNonBattalions) {
    // A ROK battalion at the DPRK line defends ROK rows only; a
    // destroyed DPRK battalion and a DPRK Brigade (non-Battalion
    // class) defend nothing even standing on the objective's cell.
    WorldState ws = base_world();
    ws.units.push_back(battalion(200, 2, 11, 100));
    auto dead = battalion(201, 6, 10, 140);
    ws.units.push_back(dead);
    auto brigade = battalion(202, 6, 10, 140);
    brigade.unit_class = f4::entities::UnitClass::Brigade;
    ws.units.push_back(brigade);
    WorldStateAdapters adapters(ws);
    CampaignResultLedger ledger(adapters.campaign, adapters.teams,
                                adapters.units);
    GroundUnitLedger rec;
    rec.vu = 201;
    rec.owner = 6;
    rec.strength = 0;
    rec.destroyed = true;
    ledger.sync_ground_unit(rec);
    std::vector<FrontObjectiveView> view;
    for (const auto& o : ws.objectives) {
        view.push_back(FrontObjectiveView{o.x, o.y, o.owner});
    }
    stamp_front_defended(view, adapters.units, &ledger);
    for (const auto& v : view) {
        if (v.owner == 2 && v.x != 50) {
            EXPECT_TRUE(v.defended) << "owner " << +v.owner;
        } else {
            EXPECT_FALSE(v.defended) << "owner " << +v.owner;
        }
    }
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
    // A hand-built front (the rank rule only reads the columns): a
    // contested run at x 7..13, row 120.
    std::vector<FrontColumn> front;
    for (int x = 7; x <= 13; ++x) {
        FrontColumn c;
        c.x = x;
        c.y = 120;
        c.south_owner = 2;
        c.north_owner = 6;
        c.contested = true;
        front.push_back(c);
    }

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
    // A hand-built front (the rank rule only reads the columns): a
    // contested run at x 7..13, row 120.
    std::vector<FrontColumn> front;
    for (int x = 7; x <= 13; ++x) {
        FrontColumn c;
        c.x = x;
        c.y = 120;
        c.south_owner = 2;
        c.north_owner = 6;
        c.contested = true;
        front.push_back(c);
    }
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

    // A hand-built front (the rank rule only reads the columns): a
    // contested run at x 7..13, row 120.
    std::vector<FrontColumn> front;
    for (int x = 7; x <= 13; ++x) {
        FrontColumn c;
        c.x = x;
        c.y = 120;
        c.south_owner = 2;
        c.north_owner = 6;
        c.contested = true;
        front.push_back(c);
    }
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
    // A hand-built front (the rank rule only reads the columns): a
    // contested run at x 7..13, row 120.
    std::vector<FrontColumn> front;
    for (int x = 7; x <= 13; ++x) {
        FrontColumn c;
        c.x = x;
        c.y = 120;
        c.south_owner = 2;
        c.north_owner = 6;
        c.contested = true;
        front.push_back(c);
    }
    const auto ranked = rank_battalion_targets(
        adapters.units, adapters.teams, front, 2, nullptr);
    ASSERT_EQ(ranked.size(), 2u);
    EXPECT_EQ(ranked[0], 500u);   // wire order
    EXPECT_EQ(ranked[1], 501u);
}
