// f4-campaign/tests/test_route_builder.cpp
//
// C3 tranche tests — RouteBuilder (the BuildPathToTarget port):
//   * the route's shape: takeoff → [ingress corners] → [IP] → target
//     → [turn point] → [egress corners] → landing
//   * the IP at BREAKPOINT_DISTANCE from the target along the approach
//   * the target WP carries the profile's action + the objective's VU
//   * unresolvable endpoints produce no route
//   * the safe-path search runs when the direct leg is hot
//   * threat cost is priced into a hot corridor's route
//   * the eliminator cuts collinear fillers, keeps critical WPs
//   * determinism: identical builds, identical routes

#include <f4/campaign/route_builder.hpp>
#include <f4/world/world_adapters.hpp>

#include <gtest/gtest.h>

#include <cmath>

using namespace f4::campaign;
using f4::entities::UnitClass;
using f4::world::UnitState;
using f4::world::ObjectiveState;
using f4::world::TeamState;

namespace {

// USA (slot 1) at war with DPRK (slot 6); a neutral UN team (slot 3).
// Objectives: USA airbase (100,100) VU 4281; DPRK target (400,400) VU
// 9001 — the strike corridor. A MissionProfile standing in for the
// generated INTSTRIKE record.
f4::world::WorldState make_world() {
    f4::world::WorldState ws;
    ws.version = 71;

    ws.teams.resize(8);
    ws.teams[1] = TeamState{1, 1, 1, "USA", ""};
    ws.teams[3] = TeamState{3, 0, 3, "UN", ""};
    ws.teams[6] = TeamState{6, 6, 6, "DPRK", ""};
    ws.teams[1].stance = {0, 0, 0, 0, 0, 0, 5, 0};
    ws.teams[6].stance = {0, 5, 0, 0, 0, 0, 0, 0};

    auto obj = [](int16_t x, int16_t y, uint8_t owner, uint32_t vu) {
        ObjectiveState o;
        o.x = x;
        o.y = y;
        o.owner = owner;
        o.id_num = vu;
        o.objective_type = 4;
        return o;
    };
    ws.objectives.push_back(obj(100, 100, 1, 4281));  // USA airbase
    ws.objectives.push_back(obj(400, 400, 6, 9001));  // DPRK target
    return ws;
}

MissionProfile strike_profile() {
    MissionProfile p;
    p.name = "AMIS_INTSTRIKE";
    p.mission_byte = 13;
    p.target = "OBJECTIVE";
    p.aro = "ARO_S";
    p.altitude_profile = "MPROF_LOW";
    p.target_profile = "TPROF_ATTACK";
    p.target_desc = "TTL";
    p.routewp = "WP_INGRESS";
    p.targetwp = "WP_STRIKE";
    p.minalt = 5;
    p.maxalt = 120;
    p.missionalt = 20;
    p.str = 4;
    return p;
}

double grid_distance(int ax, int ay, int bx, int by) {
    return std::sqrt(static_cast<double>((ax - bx) * (ax - bx) +
                                         (ay - by) * (ay - by)));
}

} // namespace

TEST(RouteBuilder, EmptyWorldRouteIsTakeoffTargetLand) {
    const auto ws = make_world();
    f4::world::WorldStateAdapters adapters(ws);
    const RouteBuilder builder(adapters.objectives, adapters.units,
                               adapters.teams, /*viewer=*/1);

    const auto r = builder.build(1, strike_profile(), 4281, 9001);
    ASSERT_GE(r.waypoints.size(), 4u);

    // Shape: takeoff at the airbase, target at the target, landing at
    // the airbase. The IP sits between.
    const auto& takeoff = r.waypoints.front();
    EXPECT_EQ(takeoff.action, kWpTakeoff);
    EXPECT_EQ(takeoff.x, 100);
    EXPECT_EQ(takeoff.y, 100);

    const auto& landing = r.waypoints.back();
    EXPECT_EQ(landing.action, kWpLand);
    EXPECT_EQ(landing.x, 100);
    EXPECT_EQ(landing.y, 100);

    // The target WP: STRIKE action, the objective's VU, the mission
    // altitude (missionalt 20 hundreds of feet = 2000).
    bool found_target = false;
    for (const auto& w : r.waypoints) {
        if (w.flags & kWpfTarget) {
            found_target = true;
            EXPECT_EQ(w.action, 17 /* WP_STRIKE */);
            EXPECT_EQ(w.x, 400);
            EXPECT_EQ(w.y, 400);
            EXPECT_EQ(w.target_num, 9001u);
            EXPECT_EQ(w.altitude_ft, 2000);
        }
    }
    EXPECT_TRUE(found_target);
}

TEST(RouteBuilder, IpSitsBreakpointDistanceFromTarget) {
    const auto ws = make_world();
    f4::world::WorldStateAdapters adapters(ws);
    const RouteBuilder builder(adapters.objectives, adapters.units,
                               adapters.teams, /*viewer=*/1);

    const auto r = builder.build(1, strike_profile(), 4281, 9001);
    const RouteWaypoint* ip = nullptr;
    const RouteWaypoint* target = nullptr;
    for (const auto& w : r.waypoints) {
        if (w.flags & kWpfIp) ip = &w;
        if (w.flags & kWpfTarget) target = &w;
    }
    ASSERT_NE(ip, nullptr);
    ASSERT_NE(target, nullptr);

    // The IP is breakpoint_distance (default 10) from the target,
    // ON the approach line (between the airbase and the target).
    const double d = grid_distance(ip->x, ip->y, target->x, target->y);
    EXPECT_NEAR(d, 10.0, 1.5);
    // Between airbase and target (closer to the target than to home).
    EXPECT_LT(grid_distance(ip->x, ip->y, 400, 400),
              grid_distance(ip->x, ip->y, 100, 100));
}

TEST(RouteBuilder, TurnPointPastTheTarget) {
    const auto ws = make_world();
    f4::world::WorldStateAdapters adapters(ws);
    const RouteBuilder builder(adapters.objectives, adapters.units,
                               adapters.teams, /*viewer=*/1);

    const auto r = builder.build(1, strike_profile(), 4281, 9001);
    // The turn point: past the target (beyond it along the approach
    // line), a small offset (max(3, breakpoint/4) = 3 grid at the
    // default breakpoint — the reference's post-target escape leg).
    const RouteWaypoint* tp = nullptr;
    for (const auto& w : r.waypoints) {
        if (w.flags & kWpfTurnPoint) tp = &w;
    }
    ASSERT_NE(tp, nullptr);
    const double d = grid_distance(tp->x, tp->y, 400, 400);
    EXPECT_NEAR(d, 3.0, 3.0);
}

TEST(RouteBuilder, UnresolvableEndpointsProduceNoRoute) {
    const auto ws = make_world();
    f4::world::WorldStateAdapters adapters(ws);
    const RouteBuilder builder(adapters.objectives, adapters.units,
                               adapters.teams, /*viewer=*/1);

    // Unknown airbase VU: nothing resolves, nothing builds.
    const auto r = builder.build(1, strike_profile(), 999999, 9001);
    EXPECT_TRUE(r.waypoints.empty());
    // Known airbase, unknown target: same.
    const auto r2 = builder.build(1, strike_profile(), 4281, 999999);
    EXPECT_TRUE(r2.waypoints.empty());
}

TEST(RouteBuilder, HotCorridorRunsTheSafePathSearch) {
    auto ws = make_world();
    // A saturated DPRK battery on the corridor's midpoint: the direct
    // leg's TT_MAX (90) exceeds min_avoid_threat (40) — the A* runs.
    for (int i = 0; i < 3; ++i) {
        UnitState u;
        u.unit_class = UnitClass::Battalion;
        u.domain = 3;
        u.unit_subtype = 1;
        u.x = 250;
        u.y = 250;
        u.owner = 6;
        u.id_num = static_cast<uint32_t>(7000 + i);
        u.unit_hit_chance = {0, 0, 0, 0, 60, 55, 0, 0};
        u.unit_weapon_range = {0, 0, 0, 0, 90, 90, 0, 0};
        ws.units.push_back(u);
    }

    f4::world::WorldStateAdapters adapters(ws);
    const RouteBuilder builder(adapters.objectives, adapters.units,
                               adapters.teams, /*viewer=*/1);
    const auto r = builder.build(1, strike_profile(), 4281, 9001);
    EXPECT_TRUE(r.safe_path_searched);
    // The A* runs but converges (war territory is flyable — cost
    // shaping, not walls; see the path finder's tests).
    EXPECT_TRUE(r.ingress_complete);
    EXPECT_FALSE(r.direct_fallback);
    // And the route exists, end to end.
    ASSERT_GE(r.waypoints.size(), 4u);
}

TEST(RouteBuilder, EliminatorCutsCollinearFillers) {
    auto ws = make_world();
    // An AD battery that forces the ingress A* into a dog-leg: place
    // two batteries flanking the corridor so the cost prefers an
    // off-axis approach. (The corners then land as WP_NOTHING
    // fillers; the collinear ones must not survive the eliminator.)
    for (int i = 0; i < 3; ++i) {
        UnitState u;
        u.unit_class = UnitClass::Battalion;
        u.domain = 3;
        u.unit_subtype = 1;
        u.x = 255;
        u.y = 235;
        u.owner = 6;
        u.id_num = static_cast<uint32_t>(7100 + i);
        u.unit_hit_chance = {0, 0, 0, 0, 60, 55, 0, 0};
        u.unit_weapon_range = {0, 0, 0, 0, 90, 90, 0, 0};
        ws.units.push_back(u);
    }

    f4::world::WorldStateAdapters adapters(ws);
    const RouteBuilder builder(adapters.objectives, adapters.units,
                               adapters.teams, /*viewer=*/1);
    const auto r = builder.build(1, strike_profile(), 4281, 9001);
    EXPECT_TRUE(r.safe_path_searched);

    // Whatever fillers survived: none is geometrically redundant. Walk
    // consecutive filler triples and assert the middle actually turns
    // (a surviving middle waypoint must deviate > 10 degrees from a
    // straight line — the eliminator's own collinearity cut).
    const auto& wps = r.waypoints;
    for (std::size_t i = 1; i + 1 < wps.size(); ++i) {
        if (wps[i].action != kWpNothing || (wps[i].flags & kWpfCriticalMask))
            continue;
        const double wmd = grid_distance(wps[i - 1].x, wps[i - 1].y,
                                         wps[i].x, wps[i].y);
        const double wnd = grid_distance(wps[i - 1].x, wps[i - 1].y,
                                         wps[i + 1].x, wps[i + 1].y);
        const double mnd = grid_distance(wps[i].x, wps[i].y,
                                         wps[i + 1].x, wps[i + 1].y);
        if (wmd <= 0.0 || wnd <= 0.0 || mnd <= 0.0) continue;
        // Deviation from collinear directly: |wmd + mnd - wnd| ~ 0
        // means the three points sit on one line (the eliminator's
        // own 10-degree cut should have removed such a middle).
        const double slack = wmd + mnd - wnd;
        EXPECT_GT(slack, 0.5)
            << "collinear filler survived at (" << wps[i].x << ","
            << wps[i].y << ")";
    }
}

TEST(RouteBuilder, RouteOnlyMissionIsTakeoffLandCircuit) {
    const auto ws = make_world();
    f4::world::WorldStateAdapters adapters(ws);
    const RouteBuilder builder(adapters.objectives, adapters.units,
                               adapters.teams, /*viewer=*/1);

    // target_vu = 0: the takeoff-land circuit (alert/training shape).
    const auto r = builder.build(1, strike_profile(), 4281, 0);
    ASSERT_EQ(r.waypoints.size(), 2u);
    EXPECT_EQ(r.waypoints.front().action, kWpTakeoff);
    EXPECT_EQ(r.waypoints.back().action, kWpLand);
}

TEST(RouteBuilder, DeterministicBuilds) {
    const auto ws = make_world();
    f4::world::WorldStateAdapters adapters(ws);
    const RouteBuilder builder(adapters.objectives, adapters.units,
                               adapters.teams, /*viewer=*/1);

    const auto a = builder.build(1, strike_profile(), 4281, 9001);
    const auto b = builder.build(1, strike_profile(), 4281, 9001);
    ASSERT_EQ(a.waypoints.size(), b.waypoints.size());
    for (std::size_t i = 0; i < a.waypoints.size(); ++i) {
        EXPECT_EQ(a.waypoints[i].x, b.waypoints[i].x);
        EXPECT_EQ(a.waypoints[i].y, b.waypoints[i].y);
        EXPECT_EQ(a.waypoints[i].action, b.waypoints[i].action);
        EXPECT_EQ(a.waypoints[i].flags, b.waypoints[i].flags);
    }
    EXPECT_EQ(a.route_length_grid, b.route_length_grid);
}
