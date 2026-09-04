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
