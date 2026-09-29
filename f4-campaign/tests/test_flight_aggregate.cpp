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

// ── CAMP-CMD-2 — the retask/scrub command writes ────────────────────────────

TEST(FlightAggregateCmd, RetaskResumesFromPositionOntoTheNewRoute) {
    Rig rig;
    rig.ws = std::make_unique<WorldState>(Rig::base());
    auto u = flight(5001, 2, 100, 100, 13,
                    {wp(100, 100, 0), wp(200, 100, 100),
                     wp(300, 100, 100)},
                    {group(2)});
    rig.ws->units.push_back(u);
    rig.make();

    ASSERT_NE(rig.engine->find(5001), nullptr);
    // Five minutes east: 5 updates × 12 grid = 60 along leg 1.
    rig.engine->tick(300);
    const auto* f = rig.engine->find(5001);
    ASSERT_NEAR(f->fx, 160.0, 1e-9);
    ASSERT_NEAR(f->fy, 100.0, 1e-9);
    ASSERT_FALSE(f->arrived);

    // Retask: from where it is, north instead of east. The head IS the
    // retask position; the cursor starts at index 1 (the new target).
    const std::int64_t now_abs = kEpoch + 300;
    ASSERT_TRUE(rig.engine->retask(
        5001, 20,
        {wp(160, 100, 100), wp(160, 220, 100)},
        static_cast<std::int32_t>(now_abs + 600),
        static_cast<std::int32_t>(now_abs + 3600)));
    f = rig.engine->find(5001);
    EXPECT_EQ(f->mission, 20);
    EXPECT_EQ(f->time_on_target, now_abs + 600);
    EXPECT_EQ(f->mission_over_time, now_abs + 3600);
    EXPECT_EQ(f->wp_index, 1u);
    EXPECT_TRUE(f->has_route);

    // The next advance walks the NEW leg (north) — no eastward drift,
    // the fuel clock uninterrupted.
    const auto fuel_before = f->fuel_burnt;
    rig.engine->tick(300);
    f = rig.engine->find(5001);
    EXPECT_NEAR(f->fx, 160.0, 1e-9);
    EXPECT_NEAR(f->fy, 160.0, 1e-9);
    EXPECT_GT(f->fuel_burnt, fuel_before);
    EXPECT_FALSE(f->arrived);
}

TEST(FlightAggregateCmd, RetaskFlipsATimeModeFlightOntoSpeedMode) {
    Rig rig;
    rig.ws = std::make_unique<WorldState>(Rig::base());
    // A save schedule: the flight is mid-leg at now + 300 (t = 200/300
    // of leg 1 → x = 140).
    rig.ws->units.push_back(
        flight(5002, 2, 100, 100, 13,
               {wp(100, 100, 0, 0, kEpoch + 100),
                wp(160, 100, 100, kEpoch + 400, kEpoch + 500),
                wp(220, 100, 100, kEpoch + 700)}));
    rig.make();

    rig.engine->tick(300);
    const auto* f = rig.engine->find(5002);
    ASSERT_NEAR(f->fx, 140.0, 1e-9);

    // The retask route carries no leg times — the flight flies it at
    // the cruise from the retask point (a TIME-mode flight whose new
    // legs had arrives = 0 would never move again; the flip is the
    // documented semantics).
    const std::int64_t now_abs = kEpoch + 300;
    ASSERT_TRUE(rig.engine->retask(
        5002, 14,
        {wp(140, 100, 100), wp(140, 190, 100)},
        static_cast<std::int32_t>(now_abs + 450),
        static_cast<std::int32_t>(now_abs + 3000)));
    rig.engine->tick(120);   // two speed updates → 24 grid north
    f = rig.engine->find(5002);
    EXPECT_NEAR(f->fx, 140.0, 1e-9);
    EXPECT_NEAR(f->fy, 124.0, 1e-9);
    EXPECT_EQ(f->mission, 14);
}

TEST(FlightAggregateCmd, RetaskRefusesTerminalFlightsAndEmptyRoutes) {
    Rig rig;
    rig.ws = std::make_unique<WorldState>(Rig::base());
    rig.ws->units.push_back(
        flight(5003, 2, 0, 0, 13, {wp(0, 0, 0), wp(12, 0, 0)}));
    rig.make();

    // Unknown vu.
    EXPECT_FALSE(rig.engine->retask(999, 20, {wp(1, 1, 0), wp(2, 2, 0)}, 1, 2));
    // Empty route — a retask flies SOMEWHERE.
    EXPECT_FALSE(rig.engine->retask(5003, 20, {}, 1, 2));

    // Arrived: the books closed, nothing to retask.
    rig.engine->tick(3600);
    ASSERT_TRUE(rig.engine->find(5003)->arrived);
    EXPECT_FALSE(rig.engine->retask(5003, 20, {wp(5, 5, 0), wp(6, 6, 0)}, 1, 2));

    // Destroyed (the fold-back of a dead complement).
    Rig rig2;
    rig2.ws = std::make_unique<WorldState>(Rig::base());
    rig2.ws->units.push_back(
        flight(5004, 2, 0, 0, 13, {wp(0, 0, 0), wp(12, 0, 0)}));
    rig2.make();
    rig2.engine->mark_destroyed(5004);
    EXPECT_FALSE(rig2.engine->retask(5004, 20, {wp(5, 5, 0), wp(6, 6, 0)}, 1, 2));
}

TEST(FlightAggregateCmd, ScrubIsTerminalAndSkipsEveryWindow) {
    Rig rig;
    rig.ws = std::make_unique<WorldState>(Rig::base());
    auto u = flight(5005, 2, 30, 30, 13,
                    {wp(30, 30, 0, 0, kEpoch + 600),
                     wp(90, 30, 100, 0, kEpoch + 1200)});
    rig.ws->units.push_back(u);
    // The engine's snapshot reads mission_over/TOT from the flight row's
    // source view — set them through the unit state where the adapter
    // does; a row without them reports −1 and the test pins the guards
    // on the depart window (the one this route carries).
    rig.make();

    const auto idx = rig.engine->index_of(5005);
    ASSERT_NE(idx, std::size_t(-1));
    EXPECT_GT(rig.engine->seconds_to_depart(idx), 0);   // holds for takeoff

    ASSERT_TRUE(rig.engine->scrub(5005));
    EXPECT_EQ(rig.engine->stats().scrubbed, 1);
    // The windows go dark, the flight never moves, and the terminal
    // state refuses every further write.
    EXPECT_EQ(rig.engine->seconds_to_depart(idx), -1);
    rig.engine->tick(3600);
    const auto* f = rig.engine->find(5005);
    EXPECT_NEAR(f->fx, 30.0, 1e-9);
    EXPECT_NEAR(f->fy, 30.0, 1e-9);
    EXPECT_FALSE(f->arrived);
    EXPECT_FALSE(f->destroyed);
    EXPECT_FALSE(rig.engine->scrub(5005));
    EXPECT_FALSE(rig.engine->retask(5005, 20, {wp(1, 1, 0), wp(2, 2, 0)}, 1, 2));
}

// ── 9. Display position (FID-P1) — the GetRealPosition analogue ────────────
//
// The serving face extrapolates BETWEEN the engine's 60-s quanta so a
// UI never sees the pause-then-jump: pre-departure holds, mid-quanta
// SPEED walks from the departure gate / the last advance, the fold-back
// anchor agrees with the fold, and a suspended flight reports its
// stored position (the sim owns the truth).

TEST(FlightAggregate, DisplayPositionHoldsBeforeDepartureThenExtrapolates) {
    Rig rig;
    rig.ws = std::make_unique<WorldState>(Rig::base());
    rig.make();   // no save flights — a synthetic-only war

    SyntheticFlightSeed seed;
    seed.vu = 900;
    seed.team = 2;
    seed.mission = 17;
    seed.aircraft_count = 2;
    seed.time_on_target = kEpoch + 3600;
    seed.mission_over_time = kEpoch + 7200;   // RECOV: the recovery books
    seed.route = {wp(0, 0, 0, 0, kEpoch + 600), wp(0, 60, 8000),
                  wp(0, 120, 8000)};
    const auto idx = rig.engine->register_synthetic(seed);
    ASSERT_NE(idx, std::size_t(-1));

    // The seed's recovery deadline rides into the engine (the
    // recovery-ops window opens for generated flights now).
    EXPECT_EQ(rig.engine->seconds_to_mission_over(idx), 7200);

    double fx = -1, fy = -1;
    float alt = -1;

    // Pre-departure: the flight holds at its base regardless of when
    // the query lands within the hold.
    rig.engine->display_position(idx, kEpoch + 300, fx, fy, alt);
    EXPECT_DOUBLE_EQ(fx, 0.0);
    EXPECT_DOUBLE_EQ(fy, 0.0);

    // Mid-quanta after departure (no update has fired — the engine's
    // stored position is still the base): the display walks 30 s of
    // cruise = 6 grid north, anchored at the DEPARTURE GATE (not at the
    // registration — a pre-departure anchor would race a full hold's
    // worth of distance ahead of the engine's own first step).
    rig.engine->display_position(idx, kEpoch + 630, fx, fy, alt);
    EXPECT_NEAR(fy, 6.0, 1e-9);
    EXPECT_DOUBLE_EQ(fx, 0.0);
    EXPECT_NEAR(alt, 800.0f, 1e-6);   // lerp toward wp1's 8000 by 6/60

    // The engine's own updates at the 600-s and 660-s boundaries fly it
    // 12 grid each: the display AT that boundary agrees exactly (coarse
    // simulation, smooth display, one truth).
    rig.engine->tick(660);
    rig.engine->display_position(idx, kEpoch + 660, fx, fy, alt);
    EXPECT_NEAR(fy, rig.engine->flights()[idx].fy, 1e-9);
    EXPECT_NEAR(fy, 24.0, 1e-9);

    // Between quanta the display keeps walking (the pause-then-jump is
    // dead): 45 s past the last update = 9 more grid.
    rig.engine->display_position(idx, kEpoch + 705, fx, fy, alt);
    EXPECT_NEAR(fy, 33.0, 1e-9);
}

TEST(FlightAggregate, DisplayPositionFoldAnchorsAtTheFoldTime) {
    Rig rig;
    rig.ws = std::make_unique<WorldState>(Rig::base());
    rig.make();

    SyntheticFlightSeed seed;
    seed.vu = 901;
    seed.team = 2;
    seed.mission = 17;
    seed.route = {wp(0, 0, 0), wp(40, 0, 5000)};   // no depart gate
    const auto idx = rig.engine->register_synthetic(seed);
    ASSERT_NE(idx, std::size_t(-1));

    // Fly two updates (24 grid east), then the sim materializes the
    // flight and flies the lead 6 grids further before the fold.
    rig.engine->tick(120);
    rig.engine->set_suspended(seed.vu, true);
    rig.engine->tick(600);
    rig.engine->reaggregate(seed.vu, 30.0, 0.0, 5000.0f, 500);

    // The fold stamps last_move at the fold's clock (720): the display
    // extrapolates FROM the folded position 30 s later (6 grid east) —
    // the fold and the serving face agree, no snap.
    double fx = 0, fy = 0;
    float alt = 0;
    rig.engine->display_position(idx, kEpoch + 750, fx, fy, alt);
    EXPECT_NEAR(fx, 36.0, 1e-9);
    EXPECT_NEAR(fy, 0.0, 1e-9);

    // The display extrapolates ahead of the engine's quanta: 60 s past
    // the fold the walk has covered the 10-grid leg and clamps at the
    // route's end waypoint.
    rig.engine->display_position(idx, kEpoch + 780, fx, fy, alt);
    EXPECT_NEAR(fx, 40.0, 1e-9);

    // The engine's next update lands on the same spot (one truth — the
    // walk, engine and display, agree everywhere).
    rig.engine->tick(60);   // clock 780: the boundary update fires
    rig.engine->display_position(idx, kEpoch + 780, fx, fy, alt);
    EXPECT_NEAR(fx, rig.engine->flights()[idx].fx, 1e-9);
    EXPECT_NEAR(fx, 40.0, 1e-9);

    // A suspended flight reports its stored position (the sim owns the
    // truth — the session overlays the lead's transform).
    rig.engine->set_suspended(seed.vu, true);
    rig.engine->display_position(idx, kEpoch + 900, fx, fy, alt);
    EXPECT_NEAR(fx, rig.engine->flights()[idx].fx, 1e-9);
}

// ── 10. Live tracking (FID-P1b) — the row follows a suspended flight ───────
//
// The tier rules and the fold's not-killed path read the ROW, so a
// suspended row must track the lead: a row frozen at the deagg point
// made the machinery judge a live flight by where it materialized.

TEST(FlightAggregate, UpdateLiveTracksTheSuspendedRow) {
    Rig rig;
    rig.ws = std::make_unique<WorldState>(Rig::base());
    rig.ws->units = {flight(1, 2, 0, 0, 13,
                            {wp(30, 0, 5000, 0, kEpoch + 60)}, {group(1)})};
    rig.make();

    rig.engine->set_suspended(1, true);
    rig.engine->update_live(1, 5.0, 7.0, 9000.0f, 200);
    EXPECT_TRUE(rig.engine->flights()[0].suspended);
    EXPECT_DOUBLE_EQ(rig.engine->flights()[0].fx, 5.0);
    EXPECT_DOUBLE_EQ(rig.engine->flights()[0].fy, 7.0);
    EXPECT_FLOAT_EQ(rig.engine->flights()[0].altitude_ft, 9000.0f);
    EXPECT_EQ(rig.engine->flights()[0].fuel_burnt, 200);

    // Monotone fuel (the fold's own rule): a lower read never unburns.
    rig.engine->update_live(1, 9.0, 7.0, 9000.0f, 150);
    EXPECT_EQ(rig.engine->flights()[0].fuel_burnt, 200);
    EXPECT_DOUBLE_EQ(rig.engine->flights()[0].fx, 9.0);

    // Suspended flights are still skipped by the tick (the tracking
    // never turns the row into an aggregate again).
    rig.engine->tick(600);
    EXPECT_DOUBLE_EQ(rig.engine->flights()[0].fx, 9.0);
    EXPECT_TRUE(rig.engine->flights()[0].suspended);

    // An AGGREGATE row refuses the write (it owns its own kinematics).
    rig.engine->set_suspended(1, false);
    rig.engine->update_live(1, 50.0, 50.0, 0.0f, 900);
    EXPECT_DOUBLE_EQ(rig.engine->flights()[0].fx, 9.0);

    // Unknown vu: a no-op (nothing to crash on).
    rig.engine->update_live(1234, 1.0, 1.0, 0.0f, 0);
}

// ── 11. The TIME-mode fold re-anchor (FID-P1's missing half) ───────────────
//
// The fold lands the lead's TRUE position — but a TIME-mode row's
// display and advance re-derive the position from the wire schedule,
// which still says the flight is where it was 20 live minutes ago: the
// glyph snapped back at the first read. The re-anchor slides the whole
// schedule (shape, leg durations, dwells preserved) so it passes
// through the folded position at the fold time.

TEST(FlightAggregate, ReaggregateReanchorsTheTimeSchedule) {
    Rig rig;
    rig.ws = std::make_unique<WorldState>(Rig::base());
    // wp0 (0,0) departs E+600; leg 0→1 (60 grid east) spans E+600..E+1200
    // (0.1 grid/s); hold 300 s; leg 1→2 (60 more) spans E+1500..E+2700.
    rig.ws->units = {flight(1, 2, 0, 0, 13,
                            {wp(0, 0, 0, 0, kEpoch + 600),
                             wp(60, 0, 8000, kEpoch + 1200, kEpoch + 1500),
                             wp(120, 0, 8000, kEpoch + 2700)},
                            {group(1)})};
    rig.make();

    // Fly to E+700 (the schedule says (10,0)); the sim materializes and
    // the lead is at (40,0) — 30 grids AHEAD of the wire — at the fold.
    rig.engine->tick(700);
    rig.engine->set_suspended(1, true);
    rig.engine->reaggregate(1, 40.0, 0.0, 8000.0f, 100);

    // The schedule passes through (40,0) AT E+700 now: the serving face
    // reads the folded position — no snap-back to the wire's (10,0).
    double fx = -1, fy = -1;
    float alt = -1;
    rig.engine->display_position(0, kEpoch + 700, fx, fy, alt);
    EXPECT_NEAR(fx, 40.0, 1e-9);
    EXPECT_NEAR(fy, 0.0, 1e-9);

    // Leg durations and dwells are the save's own (shape preserved):
    // leg 0→1 still spans 600 s, the hold still 300 s, leg 1→2 still
    // 1200 s — only the clock moved (by +300: the aircraft was ahead).
    const auto& r = rig.engine->routes()[0];
    EXPECT_EQ(r[1].arrive - r[0].depart, 600);
    EXPECT_EQ(r[1].depart - r[1].arrive, 300);
    EXPECT_EQ(r[2].arrive - r[1].depart, 1200);
    // And the shift is the one the geometry demands (40 grid at
    // 0.1 grid/s = 400 s after depart → t_P = E+1000, delta = +300).
    EXPECT_EQ(r[1].arrive, kEpoch + 900);
    EXPECT_EQ(r[2].arrive, kEpoch + 2400);

    // The engine's own updates follow the shifted schedule — the next
    // quanta point (E+720) continues from the folded position at the
    // leg's own 0.1 grid/s (40 + 20 s × 0.1 = 42), never a jump back.
    rig.engine->tick(60);   // clock E+760: the E+720 boundary update fires
    EXPECT_NEAR(rig.engine->flights()[0].fx, 42.0, 1e-9);
    // One truth: the display AT the quanta boundary equals the row, and
    // between quanta it keeps walking the same schedule (E+760 → 46).
    rig.engine->display_position(0, kEpoch + 720, fx, fy, alt);
    EXPECT_NEAR(fx, rig.engine->flights()[0].fx, 1e-9);
    rig.engine->display_position(0, kEpoch + 760, fx, fy, alt);
    EXPECT_NEAR(fx, 46.0, 1e-9);
}

TEST(FlightAggregate, PreDepartureFoldKeepsTheWireSchedule) {
    Rig rig;
    rig.ws = std::make_unique<WorldState>(Rig::base());
    rig.ws->units = {flight(1, 2, 0, 0, 13,
                            {wp(0, 0, 0, 0, kEpoch + 600),
                             wp(60, 0, 8000, kEpoch + 1200)},
                            {group(1)})};
    rig.make();

    // A fold BEFORE the first depart (a ground-staged flight the sim
    // parked at some offset): the takeoff gate still owns the schedule.
    rig.engine->set_suspended(1, true);
    rig.engine->reaggregate(1, 3.0, 0.0, 0.0f, 0);
    EXPECT_EQ(rig.engine->routes()[0][0].depart, kEpoch + 600);
    EXPECT_EQ(rig.engine->routes()[0][1].arrive, kEpoch + 1200);
    // The gate holds the row at the folded spot until depart.
    double fx = -1, fy = -1;
    float alt = -1;
    rig.engine->display_position(0, kEpoch + 300, fx, fy, alt);
    EXPECT_NEAR(fx, 3.0, 1e-9);
}

// ── 12. The TIME-mode arrival through an unscheduled tail ──────────────────
//
// Legs without an arrival are skipped by the walk — a route whose last
// legs are unscheduled never fired the arrival, and the flight sat at
// the last scheduled waypoint FOREVER (a frozen glyph mid-map).

TEST(FlightAggregate, TimeModeArrivesPastAnUnscheduledTail) {
    Rig rig;
    rig.ws = std::make_unique<WorldState>(Rig::base());
    rig.ws->units = {flight(1, 2, 0, 0, 13,
                            {wp(0, 0, 0, 0, kEpoch + 100),
                             wp(10, 0, 5000, kEpoch + 200),
                             wp(20, 0, 5000)},   // no arrive: the tail
                            {group(1)})};
    rig.make();

    rig.engine->tick(300);   // past the last scheduled arrival
    EXPECT_TRUE(rig.engine->flights()[0].arrived);
    // The row holds at the last SCHEDULED waypoint (the walk snaps
    // through scheduled legs only).
    EXPECT_DOUBLE_EQ(rig.engine->flights()[0].fx, 10.0);
    EXPECT_EQ(rig.engine->stats().arrived, 1);
}

// ── 13. CAMP-SAVE-WIRE — the stock saves' multi-day wires ──────────────────
//
// The stock campaign saves' waypoint times are the ATO planner's
// horizon: computable legs at ~0.01 grid/min (weeks per leg). Strict
// TIME-mode interpolation froze the whole war into imperceptible
// creep — the "none of the flights move" report. A timed route whose
// computable legs all crawl below the floor constructs as SPEED mode;
// sane wires and mixed wires (a slow loiter leg inside a fast route)
// keep the TIME identity.

TEST(FlightAggregate, GarbageWireConstructsAsSpeedMode) {
    Rig rig;
    rig.ws = std::make_unique<WorldState>(Rig::base());
    // 60 grid across 600,000 s = 0.006 grid/min: the stock-save shape.
    rig.ws->units = {flight(1, 2, 0, 0, 13,
                            {wp(0, 0, 0, 0, kEpoch + 600),
                             wp(60, 0, 8000, kEpoch + 600600)},
                            {group(1)})};
    rig.make();
    EXPECT_FALSE(rig.engine->is_time_mode(0));
    // The depart gate on wp0 still owns a SPEED route (the wire's slot
    // survives the mode fallback): gated at E+60, walking the cruise
    // (12 grid per update) once E+600 passes — and never a 0.006-grid
    // TIME-mode crawl.
    rig.engine->tick(60);
    EXPECT_DOUBLE_EQ(rig.engine->flights()[0].fx, 0.0);
    rig.engine->tick(600);   // clock E+660: the E+600 boundary update fires
    EXPECT_NEAR(rig.engine->flights()[0].fx, 24.0, 1e-9);
}

TEST(FlightAggregate, SaneAndMixedWiresStayTimeMode) {
    Rig rig;
    rig.ws = std::make_unique<WorldState>(Rig::base());
    // Sane: 60 grid in 600 s = 6 grid/min.
    // Mixed: a fast leg + one slow "loiter" leg — the loiter is a
    // schedule, not garbage; the wire stays trusted.
    rig.ws->units = {
        flight(1, 2, 0, 0, 13,
               {wp(0, 0, 0, 0, kEpoch + 600),
                wp(60, 0, 8000, kEpoch + 1200)},
               {group(1)}),
        flight(2, 2, 0, 50, 13,
               {wp(0, 50, 0, 0, kEpoch + 600),
                wp(60, 50, 8000, kEpoch + 1200),
                wp(62, 50, 8000, kEpoch + 4800)},   // 2 grid in 3600 s
               {group(1)})};
    rig.make();
    EXPECT_TRUE(rig.engine->is_time_mode(0));
    EXPECT_TRUE(rig.engine->is_time_mode(1));

    // The fallback is configurable: 0 trusts every wire.
    Rig rig2;
    rig2.ws = std::make_unique<WorldState>(Rig::base());
    rig2.ws->units = {flight(1, 2, 0, 0, 13,
                             {wp(0, 0, 0, 0, kEpoch + 600),
                              wp(60, 0, 8000, kEpoch + 600600)},
                             {group(1)})};
    FlightAggregateConfig cfg;
    cfg.min_leg_speed_grid_per_min = 0.0;
    rig2.make(cfg);
    EXPECT_TRUE(rig2.engine->is_time_mode(0));
}

// ── 14. The fold's pace — a folded flight keeps the speed it flew ─────────

TEST(FlightAggregate, FoldBooksTheLeadPace) {
    Rig rig;
    rig.ws = std::make_unique<WorldState>(Rig::base());
    rig.ws->units = {flight(1, 2, 0, 0, 13, {wp(200, 0, 5000)}, {group(1)})};
    rig.make();

    // The lead flew past the camera at ~360 kts (609.6 fps = 35.7
    // grid/min); the fold books it (clamped into [config, 40]).
    rig.engine->set_suspended(1, true);
    rig.engine->reaggregate(1, 10.0, 0.0, 5000.0f, 100, 35.7, true);
    EXPECT_DOUBLE_EQ(rig.engine->flights()[0].cruise_grid_per_min, 35.7);

    // The walk and the display move at the BOOKED pace (35.7 grid/min
    // × 60 s = 35.7 grid per update), not the 12-grid default. The
    // fold stamped last_move at clock 0 — the display extrapolates
    // from there.
    rig.engine->tick(60);
    EXPECT_NEAR(rig.engine->flights()[0].fx, 45.7, 1e-9);
    double fx = -1, fy = -1;
    float alt = -1;
    rig.engine->display_position(0, kEpoch + 90, fx, fy, alt);
    EXPECT_NEAR(fx, 63.55, 1e-9);   // 45.7 + 30 s × 35.7/60

    // A fold without a booking resets the row to the config default.
    rig.engine->set_suspended(1, true);
    rig.engine->reaggregate(1, 70.0, 0.0, 5000.0f, 200);
    EXPECT_DOUBLE_EQ(rig.engine->flights()[0].cruise_grid_per_min, 0.0);
    rig.engine->display_position(0, kEpoch + 90, fx, fy, alt);
    EXPECT_NEAR(fx, 76.0, 1e-9);   // 70 + 30 s × 12/60
}

// ── 15. The airborne fold re-anchors past a still-closed gate ──────────────
//
// A bubble/ops deagg can take an unlaunched (wire-gated) flight off the
// ramp early; when it folds back, the wire's depart is still in the
// future. A grounded complement keeps the wire (the gate owns the
// schedule) — but an AIRBORNE lead already flew off the wire, so the
// re-anchor proceeds and lands the gate in the past: the folded glyph
// keeps flying from where the aircraft is, not frozen on the ramp.

TEST(FlightAggregate, AirborneFoldReanchorsPastTheClosedGate) {
    Rig rig;
    rig.ws = std::make_unique<WorldState>(Rig::base());
    rig.ws->units = {flight(1, 2, 0, 0, 13,
                            {wp(0, 0, 0, 0, kEpoch + 600),
                             wp(60, 0, 8000, kEpoch + 1200),
                             wp(120, 0, 8000, kEpoch + 2400)},
                            {group(1)})};
    rig.make();
    EXPECT_TRUE(rig.engine->is_time_mode(0));

    // E+100: the wire says the flight is still on the ramp (depart at
    // E+600). The sim materialized it early and the lead is at (30,0).
    rig.engine->set_suspended(1, true);
    rig.engine->reaggregate(1, 30.0, 0.0, 8000.0f, 0, 0.0, true);

    // The schedule now passes through (30,0) AT the fold (clock 0):
    // the gate slid into the past (E+600 − 900 s of shift), and the
    // serving face reads the folded position — no snap back to the
    // ramp, no freeze until E+600.
    const auto& r = rig.engine->routes()[0];
    EXPECT_LT(r[0].depart, kEpoch);   // the gate is OPEN (in the past)
    double fx = -1, fy = -1;
    float alt = -1;
    rig.engine->display_position(0, kEpoch, fx, fy, alt);
    EXPECT_NEAR(fx, 30.0, 1e-9);
    // The leg's own pace survives: 60 grid in 600 s = 0.1 grid/s —
    // 160 s after the fold the walk is 16 more grid along it.
    rig.engine->tick(60);   // the E+60 boundary update lands at 36
    EXPECT_NEAR(rig.engine->flights()[0].fx, 36.0, 1e-9);
    rig.engine->display_position(0, kEpoch + 160, fx, fy, alt);
    EXPECT_NEAR(fx, 46.0, 1e-9);
}
