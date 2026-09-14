// f4-campaign/tests/test_flight_aggregate.cpp
//
// FID-2 — the aggregate flight propagator, pinned over a hand-built
// rig (the test_ground_war discipline: a minimal in-memory WorldState
// where every number is chosen to expose one rule):
//
//   1. Filter + count: the FlightSpawnFilter semantics (team/mission/
//      max, wire order) and the aircraft count from the vehicle groups.
//   2. SPEED mode: a time-less route walks its legs at the cruise
//      speed, snaps waypoints, arrives, and burns cruise fuel per
//      aircraft while progressing (and stops burning at arrival).
//   3. TIME mode: a save's own arrive/depart schedule reproduces the
//      wire's timing — holds before the first depart, interpolates
//      mid-leg, holds between arrive and depart, arrives at the end.
//   4. Suspend/fold: a suspended flight never advances; reaggregate()
//      folds the sim's truth (position/altitude/fuel) and resumes from
//      the folded state; fuel is monotone (the fold never resurrects
//      burnt fuel).
//   5. Destroy: mark_destroyed stops the flight permanently.
//   6. Ops queries: seconds_to_depart / seconds_to_mission_over (the
//      session's ops-window inputs), including the no-schedule −1.
//   7. Write-back: apply_flights_to lands x/y/fuel/altitude for DIRTY
//      flights only (the zero-activity identity holds).
//   8. Determinism: two identically-driven engines finish byte-equal.

#include <f4/campaign/flight_aggregate.hpp>
#include <f4/campaign/flight_writeback.hpp>
#include <f4/world/world_adapters.hpp>

#include <gtest/gtest.h>

#include <memory>
#include <vector>

using namespace f4::campaign;
using f4::world::WorldState;
using f4::world::WorldStateAdapters;

namespace {

constexpr std::int64_t kEpoch = 1'000'000;

f4::entities::WaypointState wp(int x, int y, int z, std::int64_t arrive = 0,
                               std::int64_t depart = 0) {
    f4::entities::WaypointState w;
    w.x = static_cast<std::int16_t>(x);
    w.y = static_cast<std::int16_t>(y);
    w.z = static_cast<std::int16_t>(z);
    w.arrive = static_cast<std::int32_t>(arrive);
    w.depart = static_cast<std::int32_t>(depart);
    return w;
}

f4::entities::VehicleGroup group(int count) {
    f4::entities::VehicleGroup g;
    g.group = 0;
    g.count = count;
    g.live_count = count;
    return g;
}

f4::world::UnitState flight(std::uint32_t vu, std::uint8_t owner, int x,
                            int y, std::uint8_t mission = 13,
                            std::vector<f4::entities::WaypointState> wps = {},
                            std::vector<f4::entities::VehicleGroup> groups =
                                {}) {
    f4::world::UnitState u;
    u.unit_class = f4::entities::UnitClass::Flight;
    u.domain = 2;   // DOMAIN_AIR
    u.id_num = vu;
    u.owner = owner;
    u.x = static_cast<std::int16_t>(x);
    u.y = static_cast<std::int16_t>(y);
    u.mission = mission;
    u.waypoints = std::move(wps);
    u.vehicle_groups = std::move(groups);
    u.flight_altitude = 0.0f;
    u.fuel_burnt = 0;
    return u;
}

struct Rig {
    std::unique_ptr<WorldState> ws;
    std::unique_ptr<WorldStateAdapters> adapters;
    std::unique_ptr<FlightAggregateEngine> engine;

    static WorldState base() {
        WorldState w;
        w.version = 71;
        w.campaign.current_time = static_cast<std::int32_t>(kEpoch);
        return w;
    }

    void make(FlightAggregateConfig cfg = {},
              FlightAggregateFilter filter = {}) {
        adapters = std::make_unique<WorldStateAdapters>(*ws);
        engine = std::make_unique<FlightAggregateEngine>(
            adapters->campaign, adapters->units, adapters->units, cfg,
            filter);
    }
};

} // namespace

// ── 1. Filter + count ───────────────────────────────────────────────────────

TEST(FlightAggregate, FiltersAndCountsLikeTheSpawnFilter) {
    Rig rig;
    rig.ws = std::make_unique<WorldState>(Rig::base());
    rig.ws->units = {
        flight(1, 2, 10, 10, 13, {}, {group(2), group(3)}),
        flight(2, 6, 20, 20, 13, {}, {group(1)}),
        flight(3, 2, 30, 30, 18, {}, {group(4)}),   // other mission byte
        flight(4, 6, 40, 40, 13),                   // no groups → count 1
    };
    FlightAggregateFilter filter;
    filter.team = 2;
    filter.mission = 13;
    rig.make({}, filter);

    ASSERT_EQ(rig.engine->flights().size(), 1u);   // only vu 1 matches both
    EXPECT_EQ(rig.engine->flights()[0].vu, 1u);
    EXPECT_EQ(rig.engine->flights()[0].aircraft_count, 5);   // 2 + 3
}

TEST(FlightAggregate, MaxFlightsCapsInWireOrder) {
    Rig rig;
    rig.ws = std::make_unique<WorldState>(Rig::base());
    rig.ws->units = {
        flight(1, 2, 10, 10), flight(2, 2, 20, 20), flight(3, 2, 30, 30),
    };
    FlightAggregateFilter filter;
    filter.max_flights = 2;
    rig.make({}, filter);

    ASSERT_EQ(rig.engine->flights().size(), 2u);
    EXPECT_EQ(rig.engine->flights()[0].vu, 1u);
    EXPECT_EQ(rig.engine->flights()[1].vu, 2u);
}

// ── 2. SPEED mode ───────────────────────────────────────────────────────────

TEST(FlightAggregate, SpeedModeWalksSnapsAndArrives) {
    Rig rig;
    rig.ws = std::make_unique<WorldState>(Rig::base());
    rig.ws->units = {flight(1, 2, 10, 10, 13,
                           {wp(22, 10, 1000), wp(22, 22, 1000)},
                           {group(1)})};
    rig.make();   // cruise 12 grid/min, update 60 s → 12 grid per update

    // One update: the 12-grid east leg is reached exactly; the cursor
    // advances to the second waypoint; fuel burns one minute.
    rig.engine->tick(60);
    ASSERT_EQ(rig.engine->stats().updates, 1);
    EXPECT_DOUBLE_EQ(rig.engine->flights()[0].fx, 22.0);
    EXPECT_DOUBLE_EQ(rig.engine->flights()[0].fy, 10.0);
    EXPECT_EQ(rig.engine->flights()[0].wp_index, 1u);
    EXPECT_FALSE(rig.engine->flights()[0].arrived);
    EXPECT_EQ(rig.engine->flights()[0].fuel_burnt, 70);
    EXPECT_EQ(rig.engine->flights()[0].last_move, kEpoch + 60);

    // Second update: the 12-grid north leg completes → arrived; no
    // fuel burns on the arriving update (progress ended the mission).
    rig.engine->tick(60);
    EXPECT_TRUE(rig.engine->flights()[0].arrived);
    EXPECT_DOUBLE_EQ(rig.engine->flights()[0].fy, 22.0);
    EXPECT_EQ(rig.engine->flights()[0].fuel_burnt, 70);

    // A arrived flight stays put (10 updates in the 600-second tick).
    rig.engine->tick(600);
    EXPECT_DOUBLE_EQ(rig.engine->flights()[0].fx, 22.0);
    EXPECT_EQ(rig.engine->stats().updates, 12);
}

TEST(FlightAggregate, SpeedModePartialLegLerpsAltitude) {
    Rig rig;
    rig.ws = std::make_unique<WorldState>(Rig::base());
    // A 24-grid leg: half walked per update.
    rig.ws->units = {flight(1, 2, 0, 0, 13, {wp(0, 24, 8000), wp(0, 48, 8000)},
                            {group(1)})};
    rig.make();

    rig.engine->tick(60);
    EXPECT_DOUBLE_EQ(rig.engine->flights()[0].fy, 12.0);
    // Altitude lerped halfway to the waypoint's z (start z = 0).
    EXPECT_FLOAT_EQ(rig.engine->flights()[0].altitude_ft, 4000.0f);
}

// ── 3. TIME mode ────────────────────────────────────────────────────────────

TEST(FlightAggregate, TimeModeFollowsTheWireSchedule) {
    Rig rig;
    rig.ws = std::make_unique<WorldState>(Rig::base());
    rig.ws->units = {flight(1, 2, 10, 10, 13,
                            {wp(10, 10, 0, 0, kEpoch + 600),
                             wp(22, 10, 1000, kEpoch + 1200)},
                            {group(1)})};
    rig.make();   // TIME mode: the arrive time is usable

    // Before the first depart: holds at the save position (6 updates).
    rig.engine->tick(600);
    EXPECT_DOUBLE_EQ(rig.engine->flights()[0].fx, 10.0);
    EXPECT_EQ(rig.engine->flights()[0].fuel_burnt, 0);
    EXPECT_EQ(rig.engine->stats().updates, 10);

    // Mid-leg (t = 0.5 through the 600-second leg): the interpolated
    // midpoint, fuel burning while progressing.
    rig.engine->tick(300);
    EXPECT_DOUBLE_EQ(rig.engine->flights()[0].fx, 16.0);
    EXPECT_FLOAT_EQ(rig.engine->flights()[0].altitude_ft, 500.0f);
    EXPECT_EQ(rig.engine->flights()[0].fuel_burnt, 70 * 5);   // 5 moving updates

    // Past the arrival: the waypoint, arrived. Fuel burned on every
    // MOVING update (the depart-boundary update holds; the five mid-leg
    // updates in the 300 tick + the four in this tick before the
    // arrival snap) — never on the arriving one.
    rig.engine->tick(600);
    EXPECT_TRUE(rig.engine->flights()[0].arrived);
    EXPECT_DOUBLE_EQ(rig.engine->flights()[0].fx, 22.0);
    EXPECT_EQ(rig.engine->flights()[0].fuel_burnt, 70 * 9);
}

// ── 4. Suspend / fold ───────────────────────────────────────────────────────

TEST(FlightAggregate, SuspendedFlightSkipsAdvanceAndFoldsBack) {
    Rig rig;
    rig.ws = std::make_unique<WorldState>(Rig::base());
    rig.ws->units = {flight(1, 2, 0, 0, 13, {wp(0, 24, 8000)}, {group(1)})};
    rig.make();

    rig.engine->tick(60);   // (0,12), 70 lbs burnt
    const auto burnt_at_deagg = rig.engine->flights()[0].fuel_burnt;

    // The deagg: suspended flights are frozen — the sim owns the truth.
    rig.engine->set_suspended(1, true);
    rig.engine->tick(600);
    EXPECT_DOUBLE_EQ(rig.engine->flights()[0].fx, 0.0);
    EXPECT_EQ(rig.engine->stats().suspended, 1);
    EXPECT_EQ(rig.engine->stats().aggregate, 0);

    // The reagg (the sim flew 6 grids east and burned 30 more lbs):
    // the fold lands position + monotone fuel, and the flight resumes.
    rig.engine->reaggregate(1, 6.0, 12.0, 8000.0f, burnt_at_deagg + 30);
    EXPECT_FALSE(rig.engine->flights()[0].suspended);
    EXPECT_TRUE(rig.engine->flights()[0].dirty);
    EXPECT_EQ(rig.engine->flights()[0].fuel_burnt, burnt_at_deagg + 30);
    EXPECT_FLOAT_EQ(rig.engine->flights()[0].altitude_ft, 8000.0f);

    // The cursor reset: the folded position (0,12) sits mid-leg toward
    // (0,24) — the engine resumes the leg from there, along the DIRECT
    // line to the waypoint (sqrt(6²+12²) = 13.42 grid; the 12-grid
    // step covers k = 0.894 of it).
    rig.engine->tick(60);
    EXPECT_NEAR(rig.engine->flights()[0].fx, 0.6334368540, 1e-6);
    EXPECT_NEAR(rig.engine->flights()[0].fy, 22.7331262920, 1e-6);
    EXPECT_GT(rig.engine->flights()[0].fuel_burnt, burnt_at_deagg + 30);
}

TEST(FlightAggregate, FoldNeverResurrectsFuel) {
    Rig rig;
    rig.ws = std::make_unique<WorldState>(Rig::base());
    rig.ws->units = {flight(1, 2, 0, 0, 13, {wp(10, 0, 0), wp(20, 0, 0)},
                            {group(1)})};
    rig.ws->units[0].fuel_burnt = 500;   // the save's own burn
    rig.make();

    rig.engine->tick(60);   // 500 → 570 (snapped the first leg, not arrived)
    EXPECT_EQ(rig.engine->flights()[0].fuel_burnt, 570);
    rig.engine->set_suspended(1, true);
    // A bogus fold (LOWER burnt) is clamped to the aggregate's own.
    rig.engine->reaggregate(1, 5.0, 0.0, 0.0f, 100);
    EXPECT_EQ(rig.engine->flights()[0].fuel_burnt, 570);
}

// ── 5. Destroy ──────────────────────────────────────────────────────────────

TEST(FlightAggregate, DestroyedFlightStops) {
    Rig rig;
    rig.ws = std::make_unique<WorldState>(Rig::base());
    rig.ws->units = {flight(1, 2, 0, 0, 13, {wp(10, 0, 0)}, {group(1)})};
    rig.make();

    rig.engine->mark_destroyed(1);
    rig.engine->tick(600);
    EXPECT_TRUE(rig.engine->flights()[0].destroyed);
    EXPECT_DOUBLE_EQ(rig.engine->flights()[0].fx, 0.0);
    EXPECT_EQ(rig.engine->stats().destroyed, 1);
    EXPECT_EQ(rig.engine->stats().aggregate, 0);
}

// ── 6. Ops queries ──────────────────────────────────────────────────────────

TEST(FlightAggregate, OpsWindowQueries) {
    Rig rig;
    rig.ws = std::make_unique<WorldState>(Rig::base());
    auto f = flight(1, 2, 0, 0, 13,
                    {wp(10, 0, 0, 0, kEpoch + 1200)}, {group(1)});
    f.mission_over_time = static_cast<std::int32_t>(kEpoch + 3000);
    rig.ws->units = {f};
    rig.make();

    EXPECT_EQ(rig.engine->seconds_to_depart(0), 1200);
    EXPECT_EQ(rig.engine->seconds_to_mission_over(0), 3000);

    rig.engine->tick(1500);
    EXPECT_LT(rig.engine->seconds_to_depart(0), 0);      // departed
    EXPECT_EQ(rig.engine->seconds_to_mission_over(0), 1500);

    // No schedule → −1 (never a false takeoff window).
    Rig rig2;
    rig2.ws = std::make_unique<WorldState>(Rig::base());
    rig2.ws->units = {flight(1, 2, 0, 0, 13, {wp(10, 0, 0)}, {group(1)})};
    rig2.make();
    EXPECT_EQ(rig2.engine->seconds_to_depart(0), -1);
    EXPECT_EQ(rig2.engine->seconds_to_mission_over(0), -1);
}

// ── 7. Write-back ───────────────────────────────────────────────────────────

TEST(FlightAggregate, WriteBackMovesDirtyFlightsOnly) {
    Rig rig;
    rig.ws = std::make_unique<WorldState>(Rig::base());
    rig.ws->units = {
        flight(1, 2, 0, 0, 13, {wp(10, 0, 0), wp(20, 0, 0)}, {group(1)}),
        flight(2, 2, 40, 40, 13, {wp(50, 40, 0)}, {group(1)}),
    };
    rig.make();

    // Zero activity: the identity (nothing writes).
    auto r0 = apply_flights_to(*rig.engine, *rig.ws);
    EXPECT_EQ(r0.flights_synced, 0);
    EXPECT_EQ(rig.ws->units[0].x, 0);
    EXPECT_EQ(rig.ws->units[1].x, 40);

    // One update: flight 1 snaps its first leg and burns (not arrived);
    // flight 2 ARRIVES (a single 10-grid leg) — moves, no fuel.
    rig.engine->tick(60);
    auto r = apply_flights_to(*rig.engine, *rig.ws);
    EXPECT_EQ(r.flights_synced, 2);          // both moved (dirty)
    EXPECT_EQ(r.positions_moved, 2);
    EXPECT_EQ(r.fuels_updated, 1);           // only the non-arrived burn
    // Flight 1: the first leg snapped at 10, the step's leftover 2
    // grids carried into the second leg → 12 on the x axis.
    EXPECT_EQ(rig.ws->units[0].x, 12);
    EXPECT_EQ(rig.ws->units[0].fuel_burnt, 70);
    EXPECT_EQ(rig.ws->units[1].x, 50);
    EXPECT_EQ(rig.ws->units[1].fuel_burnt, 0);
}

// ── 8. Determinism ──────────────────────────────────────────────────────────

TEST(FlightAggregate, DeterministicEnginesFinishEqual) {
    auto build = []() {
        Rig rig;
        rig.ws = std::make_unique<WorldState>(Rig::base());
        rig.ws->units = {
            flight(1, 2, 0, 0, 13, {wp(24, 0, 1000), wp(24, 24, 1000)},
                   {group(2)}),
            flight(2, 6, 50, 50, 13,
                   {wp(44, 50, 0, 0, kEpoch + 300),
                    wp(30, 50, 500, kEpoch + 900)},
                   {group(1)}),
        };
        rig.make();
        return rig;
    };
    auto a = build();
    auto b = build();
    for (int i = 0; i < 30; ++i) {
        a.engine->tick(60);
        b.engine->tick(60);
    }
    ASSERT_EQ(a.engine->flights().size(), b.engine->flights().size());
    for (std::size_t i = 0; i < a.engine->flights().size(); ++i) {
        const auto& fa = a.engine->flights()[i];
        const auto& fb = b.engine->flights()[i];
        EXPECT_DOUBLE_EQ(fa.fx, fb.fx);
        EXPECT_DOUBLE_EQ(fa.fy, fb.fy);
        EXPECT_EQ(fa.fuel_burnt, fb.fuel_burnt);
        EXPECT_EQ(fa.arrived, fb.arrived);
        EXPECT_EQ(fa.wp_index, fb.wp_index);
    }
}
