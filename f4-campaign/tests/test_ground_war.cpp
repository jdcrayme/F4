// f4-campaign/tests/test_ground_war.cpp
//
// G1 — the ground war engine, pinned over a hand-built rig (the
// make_ledger_world discipline: a minimal in-memory WorldState where
// every number is chosen to expose one rule):
//
//   1. War pair: named slots with a War (5) stance row pair; neutral
//      teams stand down; a war-less world is inert.
//   2. Movement: a tasked mobile battalion walks toward its objective
//      (grid position moves, last_move stamps, the AD battalion stays
//      put, a neutral battalion never moves).
//   3. Engagement: opposing battalions in contact exchange attrition —
//      ledger ground losses book, the roster decays, morale erodes, the
//      weaker side bleeds faster, a battalion at zero is destroyed.
//   4. Capture: an undefended enemy objective flips (owner + capture
//      record + the capturing battalion garrisons); a DEFENDED
//      objective does not.
//   5. Resupply: the last_resupply cadence refills (catch-up-once on a
//      stale anchor).
//   6. Air losses: AG kills booked air-side thin the engine's line
//      exactly once (no double booking).
//   7. Determinism: two identically-driven wars produce byte-identical
//      ledger JSON (the C5 contract, ground edition).
//   8. Write-back: apply_ground_to lands positions/roster/losses/owner;
//      the zero-activity identity holds (an untouched battalion and an
//      unflipped objective keep the world byte-identical).
//   9. Ledger JSON: the ground block exists only with ground activity,
//      parses strictly, and carries no floats.

#include <f4/campaign/ground_war.hpp>
#include <f4/campaign/ground_writeback.hpp>
#include <f4/campaign/result_ledger.hpp>
#include <f4/campaign/world_writeback.hpp>
#include <f4/json/f4_json.hpp>
#include <f4/world/world_adapters.hpp>

#include <gtest/gtest.h>

#include <algorithm>
#include <functional>
#include <memory>
#include <string>

using namespace f4::campaign;
using f4::world::WorldState;
using f4::world::WorldStateAdapters;

namespace {

constexpr std::uint8_t kStMechanized = 9;   // STYPE_LAND_MECHANIZED
constexpr std::uint8_t kStArmor = 3;        // STYPE_LAND_ARMOR
constexpr std::uint8_t kStAirDefense = 1;   // STYPE_LAND_AIR_DEFENSE

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
                                    std::uint8_t owner,
                                    std::uint8_t priority = 30) {
    f4::world::ObjectiveState o;
    o.id_num = vu;
    o.x = static_cast<int16_t>(x);
    o.y = static_cast<int16_t>(y);
    o.owner = owner;
    o.first_owner = owner;   // the wire's own save-start semantics
    o.priority = priority;
    return o;
}

f4::world::UnitState battalion(std::uint32_t vu, std::uint8_t owner,
                               std::uint8_t subtype, int x, int y,
                               std::uint32_t roster = kRoster12,
                               int movement_speed = 0,
                               std::uint8_t supply = 100,
                               std::uint8_t morale = 100) {
    f4::world::UnitState u;
    u.unit_class = f4::entities::UnitClass::Battalion;
    u.domain = 3;   // DOMAIN_LAND
    u.unit_subtype = subtype;
    u.id_num = vu;
    u.owner = owner;
    u.x = static_cast<int16_t>(x);
    u.y = static_cast<int16_t>(y);
    u.roster = roster;
    u.movement_speed = static_cast<int16_t>(movement_speed);
    u.supply = supply;
    u.morale = morale;
    u.fatigue = 0;
    return u;
}

/// The rig: a two-side war (ROK 2 vs DPRK 6, mutual War rows), a
/// neutral team (3), objectives on both sides of a front, and a
/// battalion roster chosen per test by mutation before make().
struct Rig {
    std::unique_ptr<WorldState> ws;
    std::unique_ptr<WorldStateAdapters> adapters;
    std::unique_ptr<CampaignResultLedger> ledger;
    std::unique_ptr<GroundWar> war;

    static WorldState base_world() {
        WorldState ws;
        ws.version = 71;
        ws.campaign.current_time = 1'000'000;
        ws.campaign.te_number_aircraft = {0, 0, 0, 0, 0, 0, 0, 0};

        // 8-slot stance rows: 2 and 6 at war (RelType 5), 3 neutral.
        std::vector<int16_t> rok{0, 0, 0, 3, 0, 0, 5, 0};
        std::vector<int16_t> dprk{0, 0, 5, 3, 0, 0, 0, 0};
        std::vector<int16_t> neutral{0, 0, 3, 0, 0, 0, 3, 0};
        ws.teams = {team(2, "ROK", rok), team(6, "DPRK", dprk),
                    team(3, "Neutralia", neutral)};

        // The front: ROK objectives south (y 90), DPRK north (y 110),
        // same columns — contested front between them.
        ws.objectives = {
            objective(101, 50, 90, 2), objective(102, 60, 90, 2),
            objective(103, 70, 92, 2, /*priority=*/98),
            objective(201, 50, 110, 6), objective(202, 60, 110, 6),
            objective(203, 70, 108, 6, /*priority=*/40),
            objective(301, 200, 200, 3),   // neutral territory
        };
        return ws;
    }

    static std::unique_ptr<Rig> make(
            const GroundWarConfig& cfg = {},
            const std::function<void(WorldState&)>& mutate = nullptr) {
        auto r = std::make_unique<Rig>();
        r->ws = std::make_unique<WorldState>(base_world());
        if (mutate) mutate(*r->ws);
        r->adapters = std::make_unique<WorldStateAdapters>(*r->ws);
        r->ledger = std::make_unique<CampaignResultLedger>(
            r->adapters->campaign, r->adapters->teams, r->adapters->units);
        r->war = std::make_unique<GroundWar>(
            r->adapters->campaign, r->adapters->teams,
            r->adapters->objectives, r->adapters->units,
            r->ledger.get(), cfg);
        return r;
    }
};

/// Fast-tick config: 10 s updates, orders every 30 s, 1 vehicle per
/// tick per side at parity (exchange 3600/h × 10 s = 10/tick).
GroundWarConfig fast_cfg() {
    GroundWarConfig c;
    c.update_sec = 10;
    c.orders_sec = 30;
    c.exchange_vehicles_per_hour = 3600;
    return c;
}

} // namespace

// ── 1. The war pair ────────────────────────────────────────────────────────

TEST(GroundWar, WarPairFromStanceAndNeutralsStandDown) {
    auto rig = Rig::make(fast_cfg());
    ASSERT_EQ(rig->war->belligerents().size(), 2u);
    EXPECT_EQ(rig->war->belligerents()[0], 2);
    EXPECT_EQ(rig->war->belligerents()[1], 6);
}

TEST(GroundWar, WarLessWorldIsInert) {
    auto rig = Rig::make(fast_cfg(), [](WorldState& w) {
        for (auto& t : w.teams) {
            for (auto& s : t.stance) s = 3;   // everyone neutral
        }
    });
    EXPECT_TRUE(rig->war->belligerents().empty());

    // The engine ticks its clock but fires nothing.
    rig->war->tick(3600);
    EXPECT_EQ(rig->war->stats().updates, 0);
    EXPECT_EQ(rig->war->clock(), 3600);
    EXPECT_TRUE(rig->ledger->empty());
}

// ── 1b. The two-war-pair limitation (CAMP-INIT-1's G1 test bed) ──────────
// Two DISJOINT at-war pairs in one world: the engine's own rule
// (belligerent_pair) fights the FIRST pair in wire order, and every
// other armed side's battalions stand down — G1's documented two-side
// machine (GROUND_WAR_PLAN.md: "a third armed team's battalions stand
// down; the multi-side generalization lands with the alliance mapping
// both sides wait on"). The generated wars (CAMP-INIT-1's twinwars
// pack) are the fixture source for this bed; the hand-built rig pins
// the rule itself.

TEST(GroundWar, SecondWarPairStandsDownWhileTheFirstFights) {
    auto rig = Rig::make(fast_cfg(), [](WorldState& w) {
        // Wire order matters: the FIRST at-war pair in source order is
        // (2,6); (3,7) is the second pair the engine must ignore.
        const std::vector<int16_t> rok{0, 0, 0, 3, 0, 0, 5, 0};
        const std::vector<int16_t> dprk{0, 0, 5, 3, 0, 0, 0, 0};
        std::vector<int16_t> neutralia{0, 0, 3, 0, 0, 0, 0, 5};
        neutralia[7] = 5;   // Neutralia is at war with Violetia
        std::vector<int16_t> violetia{0, 0, 0, 0, 0, 0, 0, 0};
        violetia[3] = 5;
        w.teams = {team(2, "ROK", rok), team(3, "Neutralia", neutralia),
                   team(6, "DPRK", dprk), team(7, "Violetia", violetia)};
        w.objectives = {
            objective(101, 50, 90, 2), objective(201, 50, 110, 6),
            // The (3,7) "front": bases for battalions that must never
            // march at each other.
            objective(301, 150, 90, 3), objective(701, 150, 110, 7),
        };
        // Mobile battalions on BOTH "fronts" — only the first pair's
        // are allowed to move.
        w.units = {
            battalion(1101, 2, kStMechanized, 50, 88, kRoster12, 360),
            battalion(1102, 6, kStMechanized, 50, 112, kRoster12, 360),
            battalion(1201, 3, kStMechanized, 150, 88, kRoster12, 360),
            battalion(1202, 7, kStMechanized, 150, 112, kRoster12, 360),
        };
    });

    // The first pair in wire order owns the war.
    ASSERT_EQ(rig->war->belligerents().size(), 2u);
    EXPECT_EQ(rig->war->belligerents()[0], 2);
    EXPECT_EQ(rig->war->belligerents()[1], 6);

    // The first pair's war runs: orders fire, the march starts.
    rig->war->tick(60);
    ASSERT_EQ(rig->war->units().size(), 4u);
    EXPECT_NE(rig->war->units()[0].target, 0u)
        << "the first pair's battalion has a target";
    EXPECT_GT(rig->war->units()[0].y, 88) << "and it marches north";

    // The second pair's battalions stand down: same grid they started
    // on, no target, no ledger sync — the engine never saw their war.
    EXPECT_EQ(rig->war->units()[2].x, 150);
    EXPECT_EQ(rig->war->units()[2].y, 88);
    EXPECT_EQ(rig->war->units()[2].target, 0u);
    EXPECT_EQ(rig->war->units()[3].x, 150);
    EXPECT_EQ(rig->war->units()[3].y, 112);
    EXPECT_EQ(rig->war->units()[3].target, 0u);
    EXPECT_TRUE(rig->ledger->ground_unit(1201) == nullptr);
    EXPECT_TRUE(rig->ledger->ground_unit(1202) == nullptr);

    // A longer pass does not wake them.
    for (int i = 0; i < 30; ++i) rig->war->tick(60);
    EXPECT_EQ(rig->war->units()[2].y, 88);
    EXPECT_EQ(rig->war->units()[3].y, 112);
    EXPECT_TRUE(rig->ledger->ground_unit(1201) == nullptr);
    EXPECT_TRUE(rig->ledger->ground_unit(1202) == nullptr);
}

// ── 1b. The front line ─────────────────────────────────────────────────────

TEST(GroundWar, FrontLineResolvesBetweenTheSides) {
    auto rig = Rig::make(fast_cfg(), [&](WorldState& w) {
        // A battalion on each side so orders fire over a live army (the
        // front rebuilds on the orders cadence).
        w.units = {
            battalion(1101, 2, kStMechanized, 60, 90, kRoster12, 360),
            battalion(1102, 6, kStArmor, 60, 110, kRoster12, 360),
        };
    });
    rig->war->tick(10);

    // The rig's geography: ROK holds y 90, DPRK y 110, objectives at
    // columns 50/60/70, plus a NEUTRAL objective at x 200 (the extent
    // widener — flank columns are uncontested). The front spans the
    // whole objective x extent (151 columns); the contested run is the
    // three ±3 bands (50-53, 57-63, 67-73 = 18 columns), and the front
    // row there is the midpoint: (90 + 110) / 2 = 100.
    ASSERT_EQ(rig->war->front_line().size(), std::size_t{151});
    EXPECT_EQ(rig->war->stats().front_columns, 18);
    const auto& col50 = rig->war->front_line()[0];
    EXPECT_EQ(col50.x, 50);
    EXPECT_TRUE(col50.contested);
    EXPECT_EQ(col50.y, 100);
    EXPECT_EQ(col50.south_owner, 2);   // centroid-y rule: ROK south
    EXPECT_EQ(col50.north_owner, 6);
    // The neutral flank (x 200): contested is false, the owners still
    // carry the pair identity (the viewer's labels).
    const auto& flank = rig->war->front_line().back();
    EXPECT_EQ(flank.x, 200);
    EXPECT_FALSE(flank.contested);
}

// ── 2. Movement ────────────────────────────────────────────────────────────

TEST(GroundWar, MobileBattalionMarchesTowardItsTarget) {
    auto rig = Rig::make(fast_cfg(), [&](WorldState& w) {
        // A fast mech battalion (360 kph = 1 grid per 10 s tick at this
        // rig's cadence) standing south of the DPRK objective 202.
        w.units = {battalion(1001, 2, kStMechanized, 60, 100,
                             kRoster12, /*movement_speed=*/360)};
    });

    // One orders cycle + one update: the battalion gets a target and
    // moves (the enemy objective is 10 grid north).
    rig->war->tick(10);
    EXPECT_EQ(rig->war->stats().orders_fired, 1);
    const auto& u = rig->war->units()[0];
    EXPECT_NE(u.target, 0u);
    EXPECT_GT(u.y, 100) << "the battalion advanced north";
    EXPECT_NE(u.last_move, 0);
    EXPECT_TRUE(rig->ledger->ground_unit(1001) != nullptr)
        << "the ledger carries the mover's state sync";

    // The march continues monotonically toward the objective.
    const int y1 = u.y;
    rig->war->tick(10);
    EXPECT_GE(rig->war->units()[0].y, y1);
}

TEST(GroundWar, StaticAndNeutralBattalionsDoNotMove) {
    auto rig = Rig::make(fast_cfg(), [&](WorldState& w) {
        w.units = {
            battalion(1002, 6, kStAirDefense, 50, 109, kRoster12, 360),
            battalion(1003, 3, kStArmor, 60, 100, kRoster12, 360),
        };
    });

    rig->war->tick(60);
    const auto* ad = rig->war->units().data();
    EXPECT_EQ(ad[0].x, 50) << "air defense never moves";
    EXPECT_EQ(ad[0].y, 109);
    EXPECT_EQ(ad[1].x, 60) << "a neutral team's battalion stands down";
    EXPECT_EQ(ad[1].y, 100);
    // The neutral battalion never even appears in the ledger's ground
    // books (no activity, no sync).
    EXPECT_TRUE(rig->ledger->ground_unit(1003) == nullptr);
}

// ── 3. Engagement ──────────────────────────────────────────────────────────

TEST(GroundWar, ContactBooksAttritionOnBothSides) {
    auto rig = Rig::make(fast_cfg(), [&](WorldState& w) {
        // Two battalions 1 grid apart, in contact.
        w.units = {
            battalion(2001, 2, kStMechanized, 60, 100, kRoster12, 360),
            battalion(2002, 6, kStArmor, 60, 101, kRoster12, 360),
        };
    });

    rig->war->tick(10);
    EXPECT_GE(rig->war->stats().update_engaged, 1);
    EXPECT_GT(rig->ledger->ground_vehicle_losses(), 0)
        << "the exchange booked vehicle kills";

    // Both books moved (mutual attrition at parity).
    const auto* a = rig->ledger->ground_unit(2001);
    const auto* b = rig->ledger->ground_unit(2002);
    ASSERT_NE(a, nullptr);
    ASSERT_NE(b, nullptr);
    EXPECT_GT(a->run_losses, 0);
    EXPECT_GT(b->run_losses, 0);
    EXPECT_EQ(rig->ledger->ground_vehicle_losses(),
              a->run_losses + b->run_losses);

    // The event log carries the pair with the attacker identity.
    ASSERT_FALSE(rig->ledger->ground_loss_log().empty());
    const auto& e = rig->ledger->ground_loss_log().front();
    EXPECT_FALSE(e.air);
    EXPECT_TRUE((e.victim == 2001 && e.attacker == 2002) ||
                (e.victim == 2002 && e.attacker == 2001));

    // Engaged battalions are pinned: neither advances while in contact
    // (their positions stay at the contact line).
    const auto& ua = rig->war->units()[0];
    const auto& ub = rig->war->units()[1];
    EXPECT_TRUE(ua.pinned);
    EXPECT_TRUE(ub.pinned);
}

TEST(GroundWar, WeakerSideBleedsFasterAndBattalionsDie) {
    auto rig = Rig::make(fast_cfg(), [&](WorldState& w) {
        // 12 vehicles vs 2 vehicles (roster 0b1010 = 2 groups of 2...
        // actually 2 vehicles in one group: 0b10 = 2).
        w.units = {
            battalion(2101, 2, kStMechanized, 60, 100, kRoster12, 360),
            battalion(2102, 6, kStArmor, 60, 101, /*roster=*/0b10, 360),
        };
    });

    // Tick until the weak battalion dies (at 10 vehicles/tick the
    // exchange resolves fast; the loop bounds the test).
    for (int i = 0; i < 10; ++i) {
        rig->war->tick(10);
        if (rig->war->units()[1].destroyed) break;
    }
    EXPECT_TRUE(rig->war->units()[1].destroyed)
        << "the 2-vehicle battalion died";
    EXPECT_EQ(rig->war->units()[1].strength, 0);
    EXPECT_GT(rig->ledger->ground_battalions_destroyed(), 0);
    // The ledger's destruction view (booked on the sync transition).
    const auto* dead = rig->ledger->ground_unit(2102);
    ASSERT_NE(dead, nullptr);
    EXPECT_TRUE(dead->destroyed);

    // The strong side took far fewer losses than it dealt; the dead
    // side's own book is exactly its roster (no overbooking past 0).
    const auto* strong = rig->ledger->ground_unit(2101);
    ASSERT_NE(strong, nullptr);
    EXPECT_LE(strong->run_losses, 4);
    EXPECT_EQ(dead->run_losses, 2);
}

// ── 4. Capture ─────────────────────────────────────────────────────────────

TEST(GroundWar, UndefendedObjectiveFlipsToTheAttacker) {
    auto rig = Rig::make(fast_cfg(), [&](WorldState& w) {
        // A strong ROK battalion sitting on the (undefended) DPRK
        // objective 202 at (60, 110).
        w.units = {battalion(3001, 2, kStMechanized, 60, 110, kRoster12,
                             360)};
    });

    rig->war->tick(10);
    const auto obj = std::find_if(
        rig->war->objectives().begin(), rig->war->objectives().end(),
        [](const GroundObjectiveState& o) { return o.vu == 202; });
    ASSERT_NE(obj, rig->war->objectives().end());
    EXPECT_EQ(obj->owner, 2) << "the objective flipped to ROK";

    // The capture book.
    ASSERT_EQ(rig->ledger->objective_captures().size(), 1u);
    const auto& c = rig->ledger->objective_captures().front();
    EXPECT_EQ(c.objective, 202u);
    EXPECT_EQ(c.from_team, 6);
    EXPECT_EQ(c.to_team, 2);
    EXPECT_EQ(c.by_battalion, 3001u);

    // The capturer garrisons its prize (it keeps the target).
    EXPECT_EQ(rig->war->units()[0].target, 202u);
}

TEST(GroundWar, DefendedObjectiveDoesNotFlip) {
    auto rig = Rig::make(fast_cfg(), [&](WorldState& w) {
        // The same attack, but a DPRK battalion holds the objective in
        // contact.
        w.units = {
            battalion(3101, 2, kStMechanized, 60, 110, kRoster12, 360),
            battalion(3102, 6, kStArmor, 61, 110, kRoster12, 360),
        };
    });

    rig->war->tick(10);
    const auto obj = std::find_if(
        rig->war->objectives().begin(), rig->war->objectives().end(),
        [](const GroundObjectiveState& o) { return o.vu == 202; });
    ASSERT_NE(obj, rig->war->objectives().end());
    EXPECT_EQ(obj->owner, 6) << "the defender held";
    EXPECT_TRUE(rig->ledger->objective_captures().empty());
}

// ── 5. Resupply ────────────────────────────────────────────────────────────

TEST(GroundWar, ResupplyCadenceRefillsCatchUpOnce) {
    GroundWarConfig cfg = fast_cfg();
    cfg.resupply_period_sec = 100;
    auto rig = Rig::make(cfg, [&](WorldState& w) {
        w.units = {battalion(4001, 2, kStMechanized, 60, 100, kRoster12,
                             360, /*supply=*/10, /*morale=*/40)};
        // The .cmp header's anchor is STALE (epoch − huge): the
        // catch-up-once rule fires one tick, not years of ticks.
        w.campaign.last_resupply = 0;
    });

    rig->war->tick(10);
    EXPECT_EQ(rig->war->stats().resupply_fires, 1);
    const auto& u = rig->war->units()[0];
    EXPECT_EQ(u.supply, 35) << "10 + 25";
    EXPECT_EQ(u.fatigue, 0);
    EXPECT_EQ(u.morale, 50) << "40 + 10";
    // One more tick in the same period: no double fire.
    rig->war->tick(10);
    EXPECT_EQ(rig->war->stats().resupply_fires, 1);
}

// ── 6. Air-caused losses ───────────────────────────────────────────────────

TEST(GroundWar, AirLossesThinTheLineWithoutDoubleBooking) {
    auto rig = Rig::make(fast_cfg(), [&](WorldState& w) {
        w.units = {battalion(5001, 2, kStMechanized, 60, 100, kRoster12,
                             360)};
    });

    // The sink books an AG kill against the battalion (air=true).
    rig->ledger->apply_ground_loss(/*t=*/5.0, 5001, 2, 0, 0, 3,
                                   /*air=*/true, /*killer_squadron=*/4281);
    // The engine pulls it on the next update.
    rig->war->tick(10);

    const auto& u = rig->war->units()[0];
    EXPECT_EQ(u.strength, 9) << "12 − 3 air kills";
    EXPECT_EQ(u.run_losses, 3);

    // No double booking: exactly ONE event in the ledger.
    ASSERT_EQ(rig->ledger->ground_loss_log().size(), 1u);
    EXPECT_TRUE(rig->ledger->ground_loss_log()[0].air);
    EXPECT_EQ(rig->ledger->ground_vehicle_losses(), 3);
}

// ── 7. Determinism ─────────────────────────────────────────────────────────

TEST(GroundWar, IdenticalWarsProduceIdenticalLedgerBytes) {
    const auto drive = [](Rig& r) {
        for (int i = 0; i < 50; ++i) r.war->tick(10);
    };

    auto a = Rig::make(fast_cfg(), [&](WorldState& w) {
        w.units = {
            battalion(6001, 2, kStMechanized, 60, 100, kRoster12, 360),
            battalion(6002, 6, kStArmor, 61, 101, kRoster12, 360),
            battalion(6003, 6, kStAirDefense, 50, 109, kRoster12),
        };
    });
    auto b = Rig::make(fast_cfg(), [&](WorldState& w) {
        w.units = {
            battalion(6001, 2, kStMechanized, 60, 100, kRoster12, 360),
            battalion(6002, 6, kStArmor, 61, 101, kRoster12, 360),
            battalion(6003, 6, kStAirDefense, 50, 109, kRoster12),
        };
    });

    drive(*a);
    drive(*b);
    EXPECT_EQ(a->ledger->to_json(), b->ledger->to_json());
}

// ── 8. The write-back ──────────────────────────────────────────────────────

TEST(GroundWar, WriteBackLandsGroundState) {
    auto rig = Rig::make(fast_cfg(), [&](WorldState& w) {
        w.units = {
            battalion(7001, 2, kStMechanized, 60, 100, kRoster12, 360),
            battalion(7002, 6, kStArmor, 61, 101, kRoster12, 360),
            battalion(7003, 6, kStAirDefense, 200, 200, kRoster12),
        };
    });
    rig->war->tick(30);

    // The untouched AD battalion: identity (no write, no ledger entry).
    const auto* untouched_before = &rig->ws->units[2];

    auto res = apply_ground_to(*rig->war, *rig->ws);
    EXPECT_GT(res.battalions_written, 0);
    EXPECT_TRUE(res.unmatched_battalions.empty());
    EXPECT_TRUE(res.unmatched_objectives.empty());

    // The mover's grid position landed.
    const auto moved = std::find_if(
        rig->ws->units.begin(), rig->ws->units.end(),
        [](const f4::world::UnitState& u) { return u.id_num == 7001; });
    ASSERT_NE(moved, rig->ws->units.end());
    const auto& eng = rig->war->units()[0];
    EXPECT_EQ(moved->x, static_cast<int16_t>(eng.x));
    EXPECT_EQ(moved->y, static_cast<int16_t>(eng.y));
    EXPECT_EQ(moved->roster, eng.roster);
    EXPECT_EQ(moved->losses, static_cast<std::uint8_t>(eng.run_losses));

    // A capture (if any) flips the world's owner. The rig above sits a
    // mech battalion 10 grid from an undefended objective at 360 kph =
    // 1 grid/tick, so 3 ticks reach nothing yet; force the capture case
    // with a second rig.
    auto cap = Rig::make(fast_cfg(), [&](WorldState& w) {
        w.units = {battalion(7101, 2, kStMechanized, 60, 110, kRoster12,
                             360)};
    });
    cap->war->tick(10);
    auto cres = apply_ground_to(*cap->war, *cap->ws);
    const auto flipped = std::find_if(
        cap->ws->objectives.begin(), cap->ws->objectives.end(),
        [](const f4::world::ObjectiveState& o) { return o.id_num == 202; });
    ASSERT_NE(flipped, cap->ws->objectives.end());
    EXPECT_EQ(flipped->owner, 2);
    EXPECT_EQ(flipped->first_owner, 6)
        << "first_owner keeps the wire's save-start semantics";
    EXPECT_EQ(cres.objectives_flipped, 1);

    // The untouched identity: the AD battalion never moved, never lost,
    // never synced — the write-back left the world byte-identical there.
    (void)untouched_before;
    const auto ad = std::find_if(
        rig->ws->units.begin(), rig->ws->units.end(),
        [](const f4::world::UnitState& u) { return u.id_num == 7003; });
    ASSERT_NE(ad, rig->ws->units.end());
    EXPECT_EQ(ad->x, 200);
    EXPECT_EQ(ad->y, 200);
    EXPECT_EQ(ad->roster, kRoster12);
    EXPECT_EQ(ad->losses, 0);
}

// ── 9. The ledger's ground block ───────────────────────────────────────────

TEST(GroundWar, GroundBlockShapeAndQuietIdentity) {
    // A ground-active ledger emits the ground block; a ground-quiet one
    // does not (the zero-event identity — pre-G1 bytes).
    auto active = Rig::make(fast_cfg(), [&](WorldState& w) {
        // The contact rig's own geometry (60,100)/(60,101): after the
        // first tick's movement the pair stays inside contact range,
        // so attrition books and the ground block carries team rows.
        w.units = {
            battalion(8001, 2, kStMechanized, 60, 100, kRoster12, 360),
            battalion(8002, 6, kStArmor, 60, 101, kRoster12, 360),
        };
    });
    active->war->tick(10);
    const auto json = active->ledger->to_json();
    EXPECT_NE(json.find("\"ground\""), std::string::npos);
    EXPECT_NE(json.find("\"vehicle_losses\""), std::string::npos);
    EXPECT_NE(json.find("\"units\""), std::string::npos);
    EXPECT_NE(json.find("\"losses\""), std::string::npos);

    // Strict structural validity: the Reader walks the WHOLE document
    // (the test_result_ledger discipline). The Reader is lenient about
    // DELIMITERS, so the comma shapes are pinned explicitly — the
    // artifact's consumers (python, the viewer) are strict JSON parsers
    // and a missing comma between the ground totals and the team rows
    // once shipped past the Reader walk (found parsing the QC
    // artifact; pinned here).
    f4::json::Reader r(json);
    r.skip_ws();
    r.expect('{');
    r.skip_value();
    EXPECT_NE(json.find(",\n    \"teams\": ["), std::string::npos);
    EXPECT_NE(json.find(",\n    \"units\": ["), std::string::npos);
    EXPECT_NE(json.find(",\n    \"losses\": ["), std::string::npos);
    // No floats anywhere in the document.
    EXPECT_EQ(json.find("e-"), std::string::npos);
    EXPECT_EQ(json.find("0."), std::string::npos);

    // A quiet war keeps the pre-G1 document shape.
    auto quiet = Rig::make(fast_cfg());
    quiet->war->tick(60);   // no units at all: nothing happens
    EXPECT_EQ(quiet->ledger->to_json().find("\"ground\""),
              std::string::npos);
    EXPECT_TRUE(quiet->ledger->empty());
}

// ── 10. DOM-2 — the per-objective supply pool ─────────────────────────────
// The deepened pool: the team's strategic stock (.tea supply_avail /
// fuel_avail) regenerates its held objectives' stocks, battalions DRAW
// from the nearest own-held objective within the supply radius (cut
// off beyond it), and the repair cadence spends that stock restoring
// features. Every knob defaults OFF — the G1 flat refill above is the
// byte-identical golden.

TEST(GroundWar, ObjectiveSupplySeedsClampedFromTheSource) {
    // Real saves carry 0xEB garbage in the supply bytes (kunsan: 235)
    // where the original game never wrote them — the seed clamps to
    // the wire's own 0..100 domain, and a negative last_repair (a
    // kunsan row carries one) clamps to 0.
    auto rig = Rig::make(fast_cfg(), [](WorldState& w) {
        w.objectives[0].supply = 235;      // the garbage byte
        w.objectives[0].fuel = 250;
        w.objectives[0].last_repair = -2033333296;
    });
    const auto& o = rig->war->objectives()[0];
    EXPECT_EQ(o.supply, 100);
    EXPECT_EQ(o.fuel, 100);
    EXPECT_EQ(o.last_repair, 0);
    // An untouched mirror row is NOT logistics-dirty: the write-back
    // must never normalize the save's own garbage on a quiet run.
    EXPECT_FALSE(o.logistics_dirty);
}

TEST(GroundWar, SourcedResupplyRegeneratesFromTeamStockAndDraws) {
    GroundWarConfig cfg = fast_cfg();
    cfg.resupply_period_sec = 100;
    cfg.objective_supply = true;
    auto rig = Rig::make(cfg, [&](WorldState& w) {
        // The strategic stock: ROK (slot 2) carries 1000 supply /
        // 1000 fuel; the objective seeds 40.
        w.teams[0].supply_avail = 1000;
        w.teams[0].fuel_avail = 1000;
        w.objectives[0].supply = 40;
        w.objectives[0].fuel = 0;
        w.units = {battalion(4001, 2, kStMechanized, 50, 90, kRoster12,
                             360, /*supply=*/10, /*morale=*/40)};
    });

    rig->war->tick(10);
    EXPECT_EQ(rig->war->stats().resupply_fires, 1);
    // REGEN first: EVERY ROK-held objective takes 10 from the team
    // pool (101: 40 → 50; 102/103: 0 → 10 each — 30 total; the DPRK
    // pool is empty in this rig, so 201..203 stay dry). Fuel mirrors
    // on the fuel pool (101: 0 → 10).
    const auto& o = rig->war->objectives()[0];
    EXPECT_EQ(rig->war->stats().supply_regen_total, 30);
    EXPECT_EQ(o.fuel, 10);
    EXPECT_TRUE(o.logistics_dirty);
    // Then the DRAW: the battalion tops up min(25, stock 50, room 90)
    // = 25 from its own-held depot (objective 101 at 50,90 — same
    // cell), the stock pays for it (50 → 25).
    const auto& u = rig->war->units()[0];
    EXPECT_EQ(u.supply, 35) << "10 + 25 drawn";
    EXPECT_EQ(o.supply, 25);
    EXPECT_EQ(rig->war->stats().supply_drawn_total, 25);
    // Fatigue/morale recover flat in the sourced path too.
    EXPECT_EQ(u.morale, 50);
}

TEST(GroundWar, CutOffBattalionDrawsNothing) {
    GroundWarConfig cfg = fast_cfg();
    cfg.resupply_period_sec = 100;
    cfg.objective_supply = true;
    auto rig = Rig::make(cfg, [&](WorldState& w) {
        // The battalion sits 40+ grid from every own objective —
        // beyond the default 10-grid line-of-supply radius.
        w.teams[0].supply_avail = 1000;
        w.objectives[0].supply = 100;
        w.units = {battalion(4001, 2, kStMechanized, 90, 130, kRoster12,
                             360, /*supply=*/10)};
    });

    rig->war->tick(10);
    const auto& u = rig->war->units()[0];
    EXPECT_EQ(u.supply, 10) << "cut off: no depot in radius";
    EXPECT_EQ(rig->war->stats().cut_off_events, 1);
    EXPECT_EQ(rig->war->stats().supply_drawn_total, 0);
    // Rest still happens (fatigue/morale are unit-level, not
    // logistics).
    EXPECT_EQ(u.morale, 100);
    // The depot keeps its stock (nobody drew).
    EXPECT_EQ(rig->war->objectives()[0].supply, 100);
}

TEST(GroundWar, DepletedStockDriesTheDepotAndTheDraw) {
    GroundWarConfig cfg = fast_cfg();
    cfg.resupply_period_sec = 100;
    cfg.objective_supply = true;
    auto rig = Rig::make(cfg, [&](WorldState& w) {
        // No strategic stock at all (a legal .tea state): the depot
        // cannot regenerate, and the draw drains what it seeded with.
        w.objectives[0].supply = 10;
        w.units = {battalion(4001, 2, kStMechanized, 50, 90, kRoster12,
                             360, /*supply=*/10)};
    });

    rig->war->tick(10);
    // Regen: the pool is 0 → the depot stays at 10. Draw: min(25,
    // stock 10, room 90) = 10 → the battalion gets 20, the depot 0.
    EXPECT_EQ(rig->war->objectives()[0].supply, 0);
    EXPECT_EQ(rig->war->units()[0].supply, 20);
    EXPECT_EQ(rig->war->stats().supply_regen_total, 0);
    EXPECT_EQ(rig->war->stats().supply_drawn_total, 10);
}

// ── 11. DOM-2 — the objective-feature repair cadence ──────────────────────

/// Damage face builder: 2 bits per feature, 0 normal / 1 repaired /
/// 2 damaged / 3 destroyed (f4vu.h). Features 0..7 fit one byte.
TEST(GroundWar, RepairFiresFlipsBitsAndBooksTheRecord) {
    GroundWarConfig cfg = fast_cfg();
    cfg.repair_period_sec = 100;
    auto rig = Rig::make(cfg, [&](WorldState& w) {
        // Objective 101 (ROK-held): feature 0 destroyed, feature 1
        // damaged, stock 50 (above the repair_min_supply gate).
        w.objectives[0].supply = 50;
        w.objectives[0].fstatus = {0x0B};  // f0=3 (destroyed), f1=2
        w.units = {battalion(4001, 2, kStMechanized, 50, 90, kRoster12,
                             360)};
    });

    const std::int64_t epoch = 1'000'000;
    rig->war->tick(10);   // the first update (t = 0 on the engine's
                          // own clock) fires the stale anchor

    EXPECT_EQ(rig->war->stats().repair_fires, 1);
    EXPECT_EQ(rig->war->stats().features_repaired, 1);
    // Lowest feature index first: feature 0 (destroyed) → repaired;
    // feature 1 stays damaged. Supply pays the cost (50 − 5).
    const auto& o = rig->war->objectives()[0];
    EXPECT_EQ(o.fstatus[0] & 0x03, 1) << "f0 repaired";
    EXPECT_EQ((o.fstatus[0] >> 2) & 0x03, 2) << "f1 still damaged";
    EXPECT_EQ(o.supply, 45);
    EXPECT_EQ(o.last_repair, epoch) << "the fire landed at t = 0";
    EXPECT_TRUE(o.logistics_dirty);

    // The books: one repair record, and the damage-state face
    // upserted with the post-repair bitmap (the write-back's source).
    ASSERT_EQ(rig->ledger->repair_log().size(), 1u);
    const auto& rec = rig->ledger->repair_log()[0];
    EXPECT_EQ(rec.objective, 101u);
    EXPECT_EQ(rec.owner, 2);
    EXPECT_EQ(rec.features_repaired, 1);
    EXPECT_EQ(rec.features_destroyed, 0);
    EXPECT_EQ(rec.supply, 45);
    EXPECT_EQ(rec.last_repair, epoch);
    EXPECT_EQ(rec.fstatus[0], 0x09);  // f0=1 (repaired), f1=2
    EXPECT_EQ(rig->ledger->features_repaired(), 1);
    ASSERT_EQ(rig->ledger->objective_damage().size(), 1u);
    EXPECT_EQ(rig->ledger->objective_damage()[0].fstatus[0], 0x09);
    EXPECT_FALSE(rig->ledger->empty());
}

TEST(GroundWar, RepairGatedBySupplyAndBelligerentHold) {
    GroundWarConfig cfg = fast_cfg();
    cfg.repair_period_sec = 100;
    auto rig = Rig::make(cfg, [&](WorldState& w) {
        // 101: ROK-held but stock 10 — below repair_min_supply (25):
        // no crews. 203: DPRK-held with damage and stock — repairs.
        w.objectives[0].supply = 10;
        w.objectives[0].fstatus = {0x02};  // f0 damaged
        w.objectives[5].supply = 80;       // vu 203 (DPRK)
        w.objectives[5].fstatus = {0x03};  // f0 destroyed
        w.units = {battalion(4001, 2, kStMechanized, 50, 90, kRoster12,
                             360)};
    });

    rig->war->tick(10);
    EXPECT_EQ(rig->war->stats().repair_fires, 1);
    EXPECT_EQ(rig->war->stats().features_repaired, 1);
    // 101 untouched (starved), 203 healed.
    EXPECT_EQ(rig->war->objectives()[0].fstatus[0], 0x02);
    EXPECT_EQ(rig->war->objectives()[5].fstatus[0] & 0x03, 1);
    ASSERT_EQ(rig->ledger->repair_log().size(), 1u);
    EXPECT_EQ(rig->ledger->repair_log()[0].objective, 203u);
    // A starved objective is not logistics-dirty (nothing moved).
    EXPECT_FALSE(rig->war->objectives()[0].logistics_dirty);
}

TEST(GroundWar, RepairAdoptsSimSideDamageBeforeWalking) {
    GroundWarConfig cfg = fast_cfg();
    cfg.repair_period_sec = 100;
    auto rig = Rig::make(cfg, [&](WorldState& w) {
        w.objectives[0].supply = 60;
        // The mirror seeds CLEAN; the damage arrives mid-run (the
        // sink's synced record — a strike the sim flew).
        w.units = {battalion(4001, 2, kStMechanized, 50, 90, kRoster12,
                             360)};
    });
    f4::campaign::ObjectiveDamageRecord strike;
    strike.objective = 101;
    strike.features_total = 8;
    strike.features_destroyed = 1;
    strike.destroyed_pct = 12;
    strike.fstatus = {0x03};   // f0 destroyed
    rig->ledger->apply_objective_damage(strike);

    rig->war->tick(10);
    // The engine adopted the strike's face, then repaired feature 0.
    const auto& o = rig->war->objectives()[0];
    EXPECT_EQ(o.fstatus[0] & 0x03, 1);
    EXPECT_EQ(rig->ledger->repair_log().size(), 1u);
    // The damage face now reads post-repair (0 destroyed).
    EXPECT_EQ(rig->ledger->objective_damage()[0].features_destroyed, 0);
    EXPECT_EQ(rig->ledger->objective_damage()[0].fstatus[0], 0x01);
}

TEST(GroundWar, RepairsAndStocksRideTheWriteBack) {
    GroundWarConfig cfg = fast_cfg();
    cfg.resupply_period_sec = 100;
    cfg.objective_supply = true;
    cfg.repair_period_sec = 100;
    auto rig = Rig::make(cfg, [&](WorldState& w) {
        w.teams[0].supply_avail = 1000;
        w.objectives[0].supply = 40;
        w.objectives[0].fstatus = {0x0B};  // f0 destroyed, f1 damaged
        w.units = {battalion(4001, 2, kStMechanized, 50, 90, kRoster12,
                             360, /*supply=*/10)};
    });

    rig->war->tick(10);

    // The ground write-back lands the logistics face for the DIRTY
    // objectives only: 101 (regen + draw + repair), plus 102/103 (the
    // regen fed them from the ROK pool — three ROK rows, the neutral
    // 301 untouched); the C1 write-back lands the repaired fstatus.
    const auto g = apply_ground_to(*rig->war, *rig->ws);
    EXPECT_EQ(g.objectives_resupplied, 3);
    EXPECT_EQ(g.objectives_flipped, 0);
    EXPECT_EQ(rig->ws->objectives[0].supply,
              rig->war->objectives()[0].supply);
    EXPECT_EQ(rig->ws->objectives[0].last_repair,
              rig->war->objectives()[0].last_repair);
    const auto wv = apply_to(*rig->ledger, *rig->ws);
    EXPECT_EQ(wv.objectives_written, 1);
    EXPECT_EQ(rig->ws->objectives[0].fstatus,
              rig->war->objectives()[0].fstatus);
    // The neutral objective (301) — untouched by the flow — keeps the
    // world's own bytes (the garbage-normalization guard).
    EXPECT_EQ(rig->ws->objectives[6].owner, 3);
}

TEST(GroundWar, RepairCadenceCatchUpOnce) {
    GroundWarConfig cfg = fast_cfg();
    cfg.repair_period_sec = 3600;
    auto rig = Rig::make(cfg, [&](WorldState& w) {
        // Stale anchor (0 against the epoch): fires ONE tick.
        w.objectives[0].supply = 50;
        w.objectives[0].fstatus = {0x0B};
        w.units = {battalion(4001, 2, kStMechanized, 50, 90, kRoster12,
                             360)};
    });

    rig->war->tick(10);
    EXPECT_EQ(rig->war->stats().repair_fires, 1);
    rig->war->tick(10);
    rig->war->tick(10);
    EXPECT_EQ(rig->war->stats().repair_fires, 1) << "catch-up-once";
    // Next boundary: one PERIOD after the catch-up fire.
    rig->war->tick(3590);
    EXPECT_EQ(rig->war->stats().repair_fires, 2);
    // Two fires, one feature each (the budget is per fire).
    EXPECT_EQ(rig->war->stats().features_repaired, 2);
    EXPECT_EQ((rig->war->objectives()[0].fstatus[0] >> 2) & 0x03, 1);
}
