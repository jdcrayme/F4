// f4-campaign/tests/test_naval_war.cpp
//
// CAMP-DOM-6 — the task-force movement engine, pinned over a
// hand-built rig (the test_ground_war discipline: a minimal in-memory
// WorldState where every number is chosen to expose one rule):
//
//   1. Snapshot: only the sea-domain TaskForce rows enter (the
//      battalion rows and everything else stay out), wire order, each
//      carrying its WorldState index (the sync's address).
//   2. War pair: the same named-slot rule the ground engine uses; a
//      war-less world is inert; a neutral task force stands down.
//   3. Movement: a war-pair force walks toward the wire's own dest at
//      its speed (the UCD enrichment when present, else the sea
//      family default), the fixed-point step pinned exactly, the
//      heading byte quantized to the wire's convention, last_move
//      stamped absolute.
//   4. Arrival: the snap lands exactly on the destination and the
//      force holds (no re-tasking exists to release it); a static
//      force (dest == position) never moves and never dirties a row.
//   5. Cadence: one big tick == N small ones (the C2 pin, naval
//      edition); determinism: two identically-driven engines produce
//      identical states.
//   6. Write-back: apply_naval_to writes only MOVED rows (x/y/heading/
//      last_move — dest, roster, supply untouched), verifies the row
//      identity before writing (a mismatch is loud), and is
//      idempotent (the second touch writes nothing); a movement-quiet
//      world round-trips byte-identically.

#include <f4/campaign/naval_war.hpp>
#include <f4/campaign/naval_writeback.hpp>
#include <f4/world/world_adapters.hpp>

#include <gtest/gtest.h>

#include <functional>
#include <memory>
#include <string>
#include <vector>

using namespace f4::campaign;
using f4::world::WorldState;
using f4::world::WorldStateAdapters;

namespace {

constexpr std::uint8_t kSeaCarrier = 3;    // STYPE_SEA_CARRIER
constexpr std::uint8_t kSeaFrigate = 6;    // STYPE_SEA_FRIGATE
constexpr std::uint8_t kSeaTransport = 10; // STYPE_SEA_SEA_TRANSPORT

f4::world::TeamState team(int slot, const std::string& name,
                          std::vector<int16_t> stance) {
    f4::world::TeamState t;
    t.slot = slot;
    t.name = name;
    t.stance = std::move(stance);
    t.tea_loaded = true;
    return t;
}

f4::world::UnitState taskforce(std::uint32_t vu, std::uint8_t owner,
                               std::uint8_t subtype, int x, int y,
                               int dest_x, int dest_y,
                               int movement_speed = 0,
                               std::uint32_t roster = 5) {
    f4::world::UnitState u;
    u.unit_class = f4::entities::UnitClass::TaskForce;
    u.domain = 4;   // DOMAIN_SEA
    u.unit_subtype = subtype;
    u.id_num = vu;
    u.owner = owner;
    u.x = static_cast<int16_t>(x);
    u.y = static_cast<int16_t>(y);
    u.dest_x = static_cast<int16_t>(dest_x);
    u.dest_y = static_cast<int16_t>(dest_y);
    u.movement_speed = static_cast<int16_t>(movement_speed);
    u.roster = roster;
    u.supply = 100;
    return u;
}

f4::world::UnitState battalion(std::uint32_t vu, std::uint8_t owner,
                               int x, int y) {
    f4::world::UnitState u;
    u.unit_class = f4::entities::UnitClass::Battalion;
    u.domain = 3;   // DOMAIN_LAND
    u.unit_subtype = 7;   // STYPE_LAND_INFANTRY
    u.id_num = vu;
    u.owner = owner;
    u.x = static_cast<int16_t>(x);
    u.y = static_cast<int16_t>(y);
    u.dest_x = static_cast<int16_t>(x + 10);
    u.dest_y = static_cast<int16_t>(y + 10);
    u.roster = 0xAAA;
    return u;
}

/// The rig: a two-side war (ROK 2 vs DPRK 6, mutual War rows), a
/// neutral team (3), a battalion row (the filter's counter-case), and
/// task forces chosen per test by mutation before make().
///
/// The base task forces (the kunsan pair's shape):
///   4040 — ROK carrier, long haul south (753,264 → 743,583), no UCD
///          enrichment (the sea default table drives it: carrier 25).
///   4624 — DPRK frigate one grid diagonal out (304,468 → 305,469):
///          the arrival test's raw material.
///   4700 — neutral transport with an open destination: stands down.
struct Rig {
    std::unique_ptr<WorldState> ws;
    std::unique_ptr<WorldStateAdapters> adapters;
    std::unique_ptr<NavalWar> war;

    static WorldState base_world() {
        WorldState ws;
        ws.version = 71;
        ws.campaign.current_time = 1'000'000;

        // 8-slot stance rows: 2 and 6 at war (RelType 5), 3 neutral.
        std::vector<int16_t> rok{0, 0, 0, 3, 0, 0, 5, 0};
        std::vector<int16_t> dprk{0, 0, 5, 3, 0, 0, 0, 0};
        std::vector<int16_t> neutral{0, 0, 3, 0, 0, 0, 3, 0};
        ws.teams = {team(2, "ROK", rok), team(6, "DPRK", dprk),
                    team(3, "Neutralia", neutral)};

        ws.units = {
            battalion(3001, 2, 50, 90),                     // the filter's counter-case
            taskforce(4040, 2, kSeaCarrier, 753, 264, 743, 583),
            taskforce(4624, 6, kSeaFrigate, 304, 468, 305, 469),
            taskforce(4700, 3, kSeaTransport, 100, 100, 200, 200),
        };
        return ws;
    }

    static std::unique_ptr<Rig> make(
            const NavalWarConfig& cfg = {},
            const std::function<void(WorldState&)>& mutate = nullptr) {
        auto r = std::make_unique<Rig>();
        r->ws = std::make_unique<WorldState>(base_world());
        if (mutate) mutate(*r->ws);
        r->adapters = std::make_unique<WorldStateAdapters>(*r->ws);
        r->war = std::make_unique<NavalWar>(
            r->adapters->campaign, r->adapters->teams,
            r->adapters->units, cfg);
        return r;
    }
};

/// 360 kph: 6.0 grid per 60-s update (the enrichment override — the
/// fixed-point step is exactly 1536 = 6 × 256).
constexpr int kFastKph = 360;

NavalWarConfig fast_cfg() {
    NavalWarConfig c;
    c.update_sec = 60;
    return c;
}

} // namespace

// ── 1. The snapshot ────────────────────────────────────────────────────────

TEST(NavalWar, SnapshotTakesSeaTaskForcesInWireOrderWithWorldIndices) {
    auto rig = Rig::make(fast_cfg());
    ASSERT_EQ(rig->war->units().size(), 3u);   // the battalion stays out
    EXPECT_EQ(rig->war->stats().task_forces, 3);

    const auto& u = rig->war->units();
    EXPECT_EQ(u[0].vu, 4040u);
    EXPECT_EQ(u[0].ws_index, 1u);              // ws.units[1]
    EXPECT_EQ(u[0].owner, 2);
    EXPECT_EQ(u[0].subtype, kSeaCarrier);
    EXPECT_EQ(u[0].x, 753);
    EXPECT_EQ(u[0].y, 264);
    EXPECT_EQ(u[0].dest_x, 743);
    EXPECT_EQ(u[0].dest_y, 583);
    EXPECT_EQ(u[1].vu, 4624u);
    EXPECT_EQ(u[1].ws_index, 2u);
    EXPECT_EQ(u[2].vu, 4700u);
    EXPECT_EQ(u[2].ws_index, 3u);
}

// ── 2. The war pair ────────────────────────────────────────────────────────

TEST(NavalWar, WarPairFromStanceAndNeutralsStandDown) {
    auto rig = Rig::make(fast_cfg());
    ASSERT_EQ(rig->war->belligerents().size(), 2u);
    EXPECT_EQ(rig->war->belligerents()[0], 2);
    EXPECT_EQ(rig->war->belligerents()[1], 6);

    // The neutral force arrived at snapshot (it will never move).
    EXPECT_TRUE(rig->war->units()[2].arrived);
}

TEST(NavalWar, WarLessWorldIsInert) {
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
}

// ── 3. Movement ────────────────────────────────────────────────────────────

TEST(NavalWar, EnrichedSpeedDrivesTheFixedPointStep) {
    auto rig = Rig::make(fast_cfg(), [](WorldState& w) {
        w.units[1].movement_speed = kFastKph;   // 6 grid / update
    });
    // Step precompute: 360 kph × 256 × 60 s / 3600 = 1536 (exact).
    EXPECT_EQ(rig->war->units()[0].step_fp, 1536);
    EXPECT_EQ(rig->war->units()[0].speed_kph, kFastKph);
}

TEST(NavalWar, SeaDefaultSpeedsWhenNoEnrichment) {
    auto rig = Rig::make(fast_cfg());
    // Carrier 25 kph → 25 × 256 × 60 / 3600 = 106 (integer).
    EXPECT_EQ(rig->war->units()[0].speed_kph, 25);
    EXPECT_EQ(rig->war->units()[0].step_fp, 106);
    // Frigate 30 kph → 128.
    EXPECT_EQ(rig->war->units()[1].speed_kph, 30);
    EXPECT_EQ(rig->war->units()[1].step_fp, 128);
    // Neutral transport 15 kph (arrived anyway — the pair gate).
    EXPECT_EQ(rig->war->units()[2].speed_kph, 15);
}

TEST(NavalWar, WalksTowardDestinationAndStampsHeadingAndLastMove) {
    auto rig = Rig::make(fast_cfg(), [](WorldState& w) {
        // East-bound: (100,100) → (130,100), 30 grid. 6 grid/update.
        w.units[1] = taskforce(4040, 2, kSeaCarrier, 100, 100, 130, 100,
                               kFastKph);
        // The frigate holds (the fleet stats then reflect one mover).
        w.units[2] = taskforce(4624, 6, kSeaFrigate, 304, 468, 304, 468);
    });
    auto& u = rig->war->units()[0];

    // The due-time wheel fires every boundary <= the new clock: the
    // first tick(60) fires updates at t=0 AND t=60 (the engine's own
    // first-update rule — the ground engine's shape; the wire's
    // pending order executes as soon as the war starts).
    rig->war->tick(60);
    ASSERT_EQ(rig->war->stats().updates, 2);
    EXPECT_EQ(u.x, 112);          // 100 + 2 × 6
    EXPECT_EQ(u.y, 100);
    EXPECT_FALSE(u.arrived);
    EXPECT_TRUE(u.dirty);
    // East = 90 deg = 90/1.40625 = 64 (the wire's byte convention).
    EXPECT_EQ(u.heading, 64);
    // last_move: the LAST fired update's boundary (epoch + 60).
    EXPECT_EQ(u.last_move, 1'000'060);
    EXPECT_EQ(rig->war->stats().moved_events, 2);   // both updates moved
    EXPECT_EQ(rig->war->stats().fleet_distance_fp, 3072u);

    // The next tick(60) crosses exactly one boundary (t=120).
    rig->war->tick(60);
    EXPECT_EQ(rig->war->stats().updates, 3);
    EXPECT_EQ(u.x, 118);
    EXPECT_EQ(rig->war->stats().fleet_distance_fp, 4608u);
}

TEST(NavalWar, WestboundHeadingQuantizesTo192) {
    auto rig = Rig::make(fast_cfg(), [](WorldState& w) {
        w.units[1] = taskforce(4040, 2, kSeaCarrier, 130, 100, 100, 100,
                               kFastKph);
        w.units[2] = taskforce(4624, 6, kSeaFrigate, 304, 468, 304, 468);
    });
    rig->war->tick(60);
    // West = 270 deg → −64 → +256 = 192.
    EXPECT_EQ(rig->war->units()[0].heading, 192);
}

// ── 4. Arrival and the static hold ─────────────────────────────────────────

TEST(NavalWar, ArrivalSnapsExactlyAndHolds) {
    auto rig = Rig::make(fast_cfg(), [](WorldState& w) {
        // 30 grid north-bound, 5 exact updates of 6 grid.
        w.units[1] = taskforce(4040, 2, kSeaCarrier, 100, 100, 100, 130,
                               kFastKph);
        w.units[2] = taskforce(4624, 6, kSeaFrigate, 304, 468, 304, 468);
    });
    auto& u = rig->war->units()[0];

    // tick(240) fires every boundary ≤ 240: t=0,60,120,180,240 —
    // exactly the 5 updates the haul needs.
    rig->war->tick(240);
    ASSERT_EQ(rig->war->stats().updates, 5);
    EXPECT_TRUE(u.arrived);
    EXPECT_EQ(u.x, 100);
    EXPECT_EQ(u.y, 130);
    EXPECT_EQ(u.fx, 0);
    EXPECT_EQ(u.fy, 0);
    EXPECT_EQ(rig->war->stats().arrivals, 1);
    EXPECT_EQ(rig->war->stats().fleet_distance_fp, 5 * 1536u);

    // Holds: no re-tasking exists; further updates move nothing.
    const auto dist = rig->war->stats().fleet_distance_fp;
    const auto moves = rig->war->stats().moved_events;
    rig->war->tick(600);
    EXPECT_EQ(u.x, 100);
    EXPECT_EQ(u.y, 130);
    EXPECT_EQ(rig->war->stats().arrivals, 1);
    EXPECT_EQ(rig->war->stats().fleet_distance_fp, dist);
    EXPECT_EQ(rig->war->stats().moved_events, moves);
}

TEST(NavalWar, StaticForceNeverMovesAndNeverDirties) {
    auto rig = Rig::make(fast_cfg(), [](WorldState& w) {
        // dest == position: the wire's own "no orders pending" — for
        // BOTH war-pair forces (the whole fleet holds).
        w.units[1] = taskforce(4040, 2, kSeaCarrier, 500, 500, 500, 500,
                               kFastKph);
        w.units[2] = taskforce(4624, 6, kSeaFrigate, 304, 468, 304, 468);
    });
    EXPECT_TRUE(rig->war->units()[0].arrived);
    EXPECT_TRUE(rig->war->units()[1].arrived);

    rig->war->tick(3600);
    EXPECT_EQ(rig->war->stats().updates, 61);   // t=0 .. t=3600
    EXPECT_EQ(rig->war->stats().moved_events, 0);
    EXPECT_EQ(rig->war->stats().arrivals, 0);
    EXPECT_FALSE(rig->war->units()[0].dirty);
    EXPECT_FALSE(rig->war->units()[1].dirty);
}

// ── 5. Cadence and determinism ─────────────────────────────────────────────

TEST(NavalWar, OneBigTickEqualsNSmallOnes) {
    auto big = Rig::make(fast_cfg(), [](WorldState& w) {
        w.units[1].movement_speed = kFastKph;
    });
    auto small = Rig::make(fast_cfg(), [](WorldState& w) {
        w.units[1].movement_speed = kFastKph;
    });

    big->war->tick(3600);
    for (int i = 0; i < 60; ++i) small->war->tick(60);

    ASSERT_EQ(big->war->units().size(), small->war->units().size());
    for (std::size_t i = 0; i < big->war->units().size(); ++i) {
        const auto& b = big->war->units()[i];
        const auto& s = small->war->units()[i];
        EXPECT_EQ(b.x, s.x);
        EXPECT_EQ(b.y, s.y);
        EXPECT_EQ(b.fx, s.fx);
        EXPECT_EQ(b.fy, s.fy);
        EXPECT_EQ(b.heading, s.heading);
        EXPECT_EQ(b.arrived, s.arrived);
        EXPECT_EQ(b.last_move, s.last_move);
    }
    EXPECT_EQ(big->war->stats().arrivals, small->war->stats().arrivals);
}

TEST(NavalWar, TwoEnginesSameTicksIdenticalStates) {
    auto a = Rig::make(fast_cfg(), [](WorldState& w) {
        w.units[1].movement_speed = kFastKph;
    });
    auto b = Rig::make(fast_cfg(), [](WorldState& w) {
        w.units[1].movement_speed = kFastKph;
    });

    // The same tick sequence, an arbitrary pattern.
    const int pattern[] = {60, 60, 120, 600, 60, 900, 60};
    for (const int d : pattern) {
        a->war->tick(d);
        b->war->tick(d);
    }

    ASSERT_EQ(a->war->units().size(), b->war->units().size());
    for (std::size_t i = 0; i < a->war->units().size(); ++i) {
        const auto& ua = a->war->units()[i];
        const auto& ub = b->war->units()[i];
        EXPECT_EQ(ua.x, ub.x);
        EXPECT_EQ(ua.y, ub.y);
        EXPECT_EQ(ua.fx, ub.fx);
        EXPECT_EQ(ua.fy, ub.fy);
        EXPECT_EQ(ua.heading, ub.heading);
        EXPECT_EQ(ua.arrived, ub.arrived);
    }
    EXPECT_EQ(a->war->stats().fleet_distance_fp,
              b->war->stats().fleet_distance_fp);
}

// ── 6. The WorldState sync ─────────────────────────────────────────────────

TEST(NavalWar, WriteBackLandsMovedRowsOnly) {
    auto rig = Rig::make(fast_cfg(), [](WorldState& w) {
        // East-bound fast carrier (6 grid/update); the frigate holds.
        w.units[1] = taskforce(4040, 2, kSeaCarrier, 100, 100, 130, 100,
                               kFastKph);
        w.units[2] = taskforce(4624, 6, kSeaFrigate, 304, 468, 304, 468);
    });

    const auto before = rig->ws->to_json_string();
    rig->war->tick(120);   // boundaries t=0, t=60, t=120 → 3 updates

    auto res = f4::campaign::apply_naval_to(*rig->war, *rig->ws);
    EXPECT_EQ(res.task_forces_written, 1);   // the carrier only
    EXPECT_TRUE(res.unmatched_task_forces.empty());

    auto& carrier = rig->ws->units[1];
    EXPECT_EQ(carrier.x, 118);
    EXPECT_EQ(carrier.y, 100);
    EXPECT_EQ(carrier.heading, 64);
    EXPECT_EQ(carrier.last_move, 1'000'120);
    // Consumed/carried fields untouched.
    EXPECT_EQ(carrier.dest_x, 130);
    EXPECT_EQ(carrier.dest_y, 100);
    EXPECT_EQ(carrier.roster, 5u);
    EXPECT_EQ(carrier.supply, 100);
    // The static frigate's row kept every byte.
    auto& frigate = rig->ws->units[2];
    EXPECT_EQ(frigate.x, 304);
    EXPECT_EQ(frigate.y, 468);
    // The untouched battalion and the neutral transport too.
    EXPECT_EQ(rig->ws->units[0].x, 50);
    EXPECT_EQ(rig->ws->units[3].x, 100);

    // The world actually changed (the carrier's row moved).
    EXPECT_NE(rig->ws->to_json_string(), before);

    // Idempotent: the second touch writes nothing.
    auto res2 = f4::campaign::apply_naval_to(*rig->war, *rig->ws);
    EXPECT_EQ(res2.task_forces_written, 0);
}

TEST(NavalWar, WriteBackQuietWorldIsByteIdentical) {
    auto rig = Rig::make(fast_cfg());
    // No ticks: nothing dirty, nothing written, the world untouched.
    const auto before = rig->ws->to_json_string();
    auto res = f4::campaign::apply_naval_to(*rig->war, *rig->ws);
    EXPECT_EQ(res.task_forces_written, 0);
    EXPECT_EQ(rig->ws->to_json_string(), before);
}

TEST(NavalWar, WriteBackRowMismatchIsLoud) {
    auto rig = Rig::make(fast_cfg(), [](WorldState& w) {
        w.units[1].movement_speed = kFastKph;   // the carrier moves
        // The frigate stays static (dest == position): the ONLY dirty
        // row is the sabotaged one.
        w.units[2] = taskforce(4624, 6, kSeaFrigate, 304, 468, 304, 468);
    });
    rig->war->tick(60);

    // Sabotage the row's identity AFTER the snapshot: the engine's
    // address no longer matches — loud, no write.
    rig->ws->units[1].id_num = 9999;
    auto res = f4::campaign::apply_naval_to(*rig->war, *rig->ws);
    EXPECT_EQ(res.task_forces_written, 0);
    ASSERT_EQ(res.unmatched_task_forces.size(), 1u);
    EXPECT_EQ(res.unmatched_task_forces[0], 4040u);
    EXPECT_EQ(rig->ws->units[1].x, 753);   // untouched
}

TEST(NavalWar, KunsanCarrierShapeArrivesWithinADay) {
    // The kunsan pair's actual geometry (no enrichment, sea defaults):
    // the carrier's 25 kph over a ~319-grid haul sails it home inside
    // the 24-hour certificate's horizon.
    auto rig = Rig::make(fast_cfg());
    auto& carrier = rig->war->units()[0];

    rig->war->tick(86'400);   // 24 h
    EXPECT_TRUE(carrier.arrived);
    EXPECT_EQ(carrier.x, 743);
    EXPECT_EQ(carrier.y, 583);
    // The one-grid frigate arrived on the first update.
    EXPECT_TRUE(rig->war->units()[1].arrived);
    EXPECT_EQ(rig->war->units()[1].x, 305);
    EXPECT_EQ(rig->war->units()[1].y, 469);
}
