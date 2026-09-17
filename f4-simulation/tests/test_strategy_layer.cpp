// f4-simulation/tests/test_strategy_layer.cpp
//
// P7 tranche tests — the strategy layer's SIM-SIDE integration:
//   * build_mission_plan_from_route: the racetrack anchor's station
//     contract (station_time_s + loop_waypoints) rides into the
//     brain's MissionPlan (zero-contract waypoints ride zeros)
//   * Simulation::apply_flight_roe: the RoE gate application on an
//     armed campaign aircraft (HOLD = every fire control tight,
//     TIGHT = BVR suppressed, FREE/unknown = no change)
//   * a strategy-armed CampaignSession: stationed CAPs and support
//     flights spawn with racetrack plans; identical runs stay
//     byte-identical (the ON-state determinism, the same contract
//     every other session arm keeps)

#include <f4/campaign/campaign.hpp>
#include <f4/campaign/route_builder.hpp>
#include <f4/simulation/campaign_bridge.hpp>
#include <f4/simulation/campaign_origin.hpp>
#include <f4/simulation/campaign_session.hpp>
#include <f4/simulation/simulation.hpp>

#include <f4/ai/brain_component.hpp>
#include <f4/ai/modules/navigation_module.hpp>
#include <f4/data/aircraft_config.hpp>
#include <f4/data/config_loader.hpp>
#include <f4/entities/entity.hpp>
#include <f4/entities/types.hpp>
#include <f4/flight/flight_model_component.hpp>
#include <f4/geo/position.hpp>
#include <f4/json/f4_json.hpp>
#include <f4/weapons/f4_weapons.hpp>
#include <f4/weapons/weapon_class_table.hpp>
#include <f4/weapons/weapon_store.hpp>

#include <gtest/gtest.h>

#include <filesystem>
#include <string>
#include <vector>

using namespace f4::simulation;

namespace {

std::filesystem::path kunsan_world() {
    return std::filesystem::path(F4_SIMULATION_TEST_FIXTURES_DIR) /
           "kunsan_campaign.world.json";
}

// The ROUTED fixture (the session tests' own spawn rig — the USA
// squadron patched onto a real airbase, so routed synthetics spawn).
std::filesystem::path kunsan_routed_world() {
    return std::filesystem::path(F4_SIMULATION_TEST_FIXTURES_DIR) /
           "kunsan_session.world.json";
}

// The strategy rig: the routed fixture with its based squadron owned
// by a BELLIGERENT (squadron 4041 sits at the ROK airbase 2659; the
// routed fixture keeps it USA-flagged — a non-belligerent, which the
// strategy ladder never tasks). One field differs: owner 1 → 2 (the
// ROK wing at the ROK base — the honest reading of that base's
// garrison), so stationed CAPs and support filings draw a squadron
// whose airbase resolves and the racetrack routes build.
std::filesystem::path kunsan_strategy_world() {
    return std::filesystem::path(F4_SIMULATION_TEST_FIXTURES_DIR) /
           "kunsan_strategy.world.json";
}

std::filesystem::path class_table() {
    return std::filesystem::path(F4_SOURCE_FIXTURES_DIR) / "falcon4.ct.json";
}

std::filesystem::path f16_config() {
    return std::filesystem::path(F4_GENERATED_FIXTURES_DIR) / "f16.json";
}

CampaignSessionOptions make_opts(const std::filesystem::path& world) {
    CampaignSessionOptions o;
    o.world_json = world;
    o.class_table = class_table();
    o.aircraft_config = f16_config();
    o.mission_profiles = F4_MISSION_PROFILES_JSON;
    o.tasking_cycle_sec = 5;
    o.reinforce_period_sec = 0;
    o.max_flights = 8;
    o.atm_pipeline = true;
    return o;
}

// A racetrack route the strategy tranche's RouteBuilder emits: takeoff
// (dropped by the plan builder), the station anchor, three corners,
// landing.
std::vector<f4::campaign::RouteWaypoint> make_racetrack_route(
        std::int32_t station_time_s) {
    using f4::campaign::RouteWaypoint;
    RouteWaypoint takeoff;
    takeoff.x = 390; takeoff.y = 455; takeoff.action = 1;  // TAKEOFF
    RouteWaypoint anchor;
    anchor.x = 430; anchor.y = 470; anchor.altitude_ft = 25000;
    anchor.action = 12;              // WP_CAP (the orbit rides it too)
    anchor.target_num = 4101;
    anchor.station_time_s = station_time_s;
    anchor.loop_waypoints = station_time_s > 0 ? 4 : 0;
    RouteWaypoint c1; c1.x = 450; c1.y = 470; c1.altitude_ft = 25000;
    RouteWaypoint c2; c2.x = 450; c2.y = 485; c2.altitude_ft = 25000;
    RouteWaypoint c3; c3.x = 430; c3.y = 485; c3.altitude_ft = 25000;
    RouteWaypoint land;
    land.x = 390; land.y = 455; land.action = 7;           // LAND
    return {takeoff, anchor, c1, c2, c3, land};
}

// The campaign-shape late-comer (the composition the spawner
// materializes): transform + FM + brain (enroute plan) + store + the
// C1 origin stamp — the surface arm_campaign_aircraft and
// apply_flight_roe operate on.
f4::entities::EntityId make_campaign_aircraft(
        f4::entities::EntityWorld& world,
        const f4::data::AircraftConfig& cfg,
        std::uint8_t mission_byte, std::uint32_t flight_vu) {
    using namespace f4::entities;
    auto h = world.create();
    auto& tf = h.add<TransformComponent>();
    tf.position = f4::geo::WorldPosition(0.0, 600000.0, 10000.0);
    auto& fm = h.add<f4::flight::FlightModelComponent>();
    fm.init(cfg, /*alt_ft=*/10000.0, /*vt_fps=*/450.0,
            /*heading_rad=*/0.0, /*inAir=*/true,
            /*north_ft=*/600000.0, /*east_ft=*/0.0);
    auto& brain = h.add<f4::ai::BrainComponent>();
    f4::ai::MissionPlan plan;
    plan.route.push_back(f4::ai::modules::NavigationModule::Waypoint{
        "FAR_NORTH", f4::geo::WorldPosition(0.0, 1000000.0, 10000.0),
        450.0});
    plan.start_phase = f4::ai::MissionPlan::StartPhase::Enroute;
    brain.set_mission_plan(std::move(plan));
    h.set_tag(tags::TEAM, TagValue::from(std::string("red")));
    (void)h.add<f4::weapons::WeaponStoreComponent>();
    auto& origin = h.add<f4::simulation::CampaignOriginComponent>();
    origin.flight_vu = flight_vu;
    origin.squadron_vu = 777u;
    origin.team_slot = 6;
    origin.mission_byte = mission_byte;
    return h.id();
}

} // namespace

// ============================================================================
// The plan builder: the station contract rides the anchor
// ============================================================================

TEST(BuildMissionPlanFromRoute, StationContractRidesTheRacetrackAnchor) {
    const auto route = make_racetrack_route(15 * 60);
    auto plan = f4::simulation::build_mission_plan_from_route(
        route, 0, nullptr);
    ASSERT_TRUE(plan.has_value());
    // Leading TAKEOFF dropped: 6 waypoints → 5 route legs.
    ASSERT_EQ(plan->route.size(), 5u);

    // The anchor (first leg after the takeoff): the station contract.
    const auto& anchor = plan->route[0];
    EXPECT_DOUBLE_EQ(anchor.station_time_s, 900.0);
    EXPECT_EQ(anchor.loop_waypoints, 4);
    EXPECT_EQ(anchor.action, 12);   // WP_CAP

    // Corners and the landing leg carry no contract — the loop spans
    // exactly the anchor + its three corners.
    for (std::size_t i = 1; i < plan->route.size(); ++i) {
        EXPECT_DOUBLE_EQ(plan->route[i].station_time_s, 0.0);
        EXPECT_EQ(plan->route[i].loop_waypoints, 0);
    }
}

TEST(BuildMissionPlanFromRoute, ZeroContractStaysThePlainPlan) {
    // The pre-strategy shape: every waypoint zero-contract — nothing
    // to ride, the plan is byte-shape-identical to the pre-P7 build.
    const auto route = make_racetrack_route(0);
    auto plan = f4::simulation::build_mission_plan_from_route(
        route, 0, nullptr);
    ASSERT_TRUE(plan.has_value());
    ASSERT_EQ(plan->route.size(), 5u);
    for (const auto& wp : plan->route) {
        EXPECT_DOUBLE_EQ(wp.station_time_s, 0.0);
        EXPECT_EQ(wp.loop_waypoints, 0);
    }
}

// ============================================================================
// Simulation::apply_flight_roe — the post-arm RoE gates
// ============================================================================

TEST(ApplyFlightRoe, HoldTightensEveryFireControl) {
    f4::data::AircraftConfig cfg;
    {
        const auto path = f16_config();
        if (!std::filesystem::exists(path)) {
            GTEST_SKIP() << "f16.json fixture not generated";
        }
        auto result = f4::data::loadConfig(path.string());
        ASSERT_TRUE(result.ok);
        cfg = std::move(result.config);
    }
    const std::string json = R"({
  "name": "roe_gates",
  "theater": "korea",
  "combat": { "enabled": true, "campaign_armed": true },
  "aircraft": [
    { "callsign": "ANCHOR", "aircraft_config_path": ")" +
        f4::json::escape_string(f16_config().string()) + R"(",
      "aircraft_name": "F-16C_50", "vis_type_index": 1052,
      "parking_spot": { "x": 0.0, "y": 0.0, "z": 10000.0 },
      "heading_rad": 0.0, "initial_vt_fps": 500.0,
      "spawn_in_air": true, "team": "blue" }
  ],
  "airfield": {
    "active_runway_id": 36, "active_runway_name": "Rwy 36",
    "runway_heading_rad": 0.0,
    "threshold_position": { "x": 0.0, "y": -5000.0, "z": 0.0 },
    "runway_end_position":  { "x": 0.0, "y": 5000.0, "z": 0.0 },
    "threshold_altitude_ft": 0.0, "departure_altitude_ft": 10000.0,
    "taxi_route": [ { "x": 0.0, "y": -5000.0, "z": 0.0 },
                    { "x": 0.0, "y": 0.0, "z": 0.0 } ]
  },
  "sim_dt": 0.016666666666666,
  "total_ticks": 60000,
  "record": false
})";
    auto scenario = load_scenario_from_string(json);
    f4::simulation::Simulation sim(std::move(scenario),
                                   std::filesystem::path("."));
    sim.initialize();

    // A BARCAP late-comer arms as a fighter; the flight's own RoE then
    // rides on top (WEAPONS HOLD = 2: every fire control tight).
    const auto id = make_campaign_aircraft(sim.world(), cfg,
                                           /*mission_byte=*/1, 9001);
    ASSERT_TRUE(sim.register_aircraft(id));
    ASSERT_TRUE(sim.arm_campaign_aircraft(id));

    {
        f4::entities::EntityHandle h(id, &sim.world());
        auto* brain = h.get<f4::ai::BrainComponent>();
        ASSERT_NE(brain, nullptr);
        EXPECT_TRUE(brain->combat_enabled());
        // WEAPONS HOLD (2).
        sim.apply_flight_roe(id, 2);
        EXPECT_TRUE(brain->hold_fire());
        EXPECT_TRUE(brain->bvr_hold());
        EXPECT_TRUE(brain->bvr().fire().config().hold_fire);
        EXPECT_TRUE(brain->wvr().fire().config().hold_fire);
        EXPECT_TRUE(brain->wvr().guns().config().hold_fire);
    }
}

TEST(ApplyFlightRoe, TightHoldsBvrOnlyAndFreeChangesNothing) {
    f4::data::AircraftConfig cfg;
    {
        const auto path = f16_config();
        if (!std::filesystem::exists(path)) {
            GTEST_SKIP() << "f16.json fixture not generated";
        }
        auto result = f4::data::loadConfig(path.string());
        ASSERT_TRUE(result.ok);
        cfg = std::move(result.config);
    }
    const std::string json = R"({
  "name": "roe_gates_tight",
  "theater": "korea",
  "combat": { "enabled": true, "campaign_armed": true },
  "aircraft": [
    { "callsign": "ANCHOR", "aircraft_config_path": ")" +
        f4::json::escape_string(f16_config().string()) + R"(",
      "aircraft_name": "F-16C_50", "vis_type_index": 1052,
      "parking_spot": { "x": 0.0, "y": 0.0, "z": 10000.0 },
      "heading_rad": 0.0, "initial_vt_fps": 500.0,
      "spawn_in_air": true, "team": "blue" }
  ],
  "airfield": {
    "active_runway_id": 36, "active_runway_name": "Rwy 36",
    "runway_heading_rad": 0.0,
    "threshold_position": { "x": 0.0, "y": -5000.0, "z": 0.0 },
    "runway_end_position":  { "x": 0.0, "y": 5000.0, "z": 0.0 },
    "threshold_altitude_ft": 0.0, "departure_altitude_ft": 10000.0,
    "taxi_route": [ { "x": 0.0, "y": -5000.0, "z": 0.0 },
                    { "x": 0.0, "y": 0.0, "z": 0.0 } ]
  },
  "sim_dt": 0.016666666666666,
  "total_ticks": 60000,
  "record": false
})";
    auto scenario = load_scenario_from_string(json);
    f4::simulation::Simulation sim(std::move(scenario),
                                   std::filesystem::path("."));
    sim.initialize();

    // WEAPONS TIGHT (1): BVR employment suppressed, WVR heaters and
    // guns stay at their armed-doctrine baseline (the in-close fight
    // keeps whatever the doctrine armed). WEAPONS FREE (0): nothing
    // changes at all. Both pinned as DELTAS against the pre-RoE state
    // (the doctrine's own gun hold rides the arm — the test pins the
    // RoE's DELTA, not the doctrine's baseline).
    const auto tight = make_campaign_aircraft(sim.world(), cfg,
                                              /*mission_byte=*/1, 9002);
    ASSERT_TRUE(sim.register_aircraft(tight));
    ASSERT_TRUE(sim.arm_campaign_aircraft(tight));
    bool guns_held_baseline = false;
    {
        f4::entities::EntityHandle h(tight, &sim.world());
        auto* brain = h.get<f4::ai::BrainComponent>();
        ASSERT_NE(brain, nullptr);
        guns_held_baseline = brain->wvr().guns().config().hold_fire;
        sim.apply_flight_roe(tight, 1);
        EXPECT_FALSE(brain->hold_fire());   // the brain-level hold: OFF
        EXPECT_TRUE(brain->bvr().fire().config().hold_fire);
        EXPECT_FALSE(brain->wvr().fire().config().hold_fire);
        EXPECT_EQ(brain->wvr().guns().config().hold_fire,
                  guns_held_baseline);   // untouched by TIGHT
    }

    // WEAPONS FREE (0) — the pre-P7 default: nothing changes.
    const auto free_air = make_campaign_aircraft(sim.world(), cfg,
                                                 /*mission_byte=*/1, 9003);
    ASSERT_TRUE(sim.register_aircraft(free_air));
    ASSERT_TRUE(sim.arm_campaign_aircraft(free_air));
    {
        f4::entities::EntityHandle h(free_air, &sim.world());
        auto* brain = h.get<f4::ai::BrainComponent>();
        ASSERT_NE(brain, nullptr);
        const bool bvr_baseline = brain->bvr().fire().config().hold_fire;
        const bool wvr_baseline = brain->wvr().fire().config().hold_fire;
        const bool guns_baseline = brain->wvr().guns().config().hold_fire;
        sim.apply_flight_roe(free_air, 0);
        EXPECT_FALSE(brain->hold_fire());
        EXPECT_EQ(brain->bvr().fire().config().hold_fire, bvr_baseline);
        EXPECT_EQ(brain->wvr().fire().config().hold_fire, wvr_baseline);
        EXPECT_EQ(brain->wvr().guns().config().hold_fire, guns_baseline);
    }
}

// ============================================================================
// The strategy-armed session: stationed CAPs spawn with racetrack plans
// ============================================================================

TEST(StrategySession, StationsCapsAndSupportsAndStaysDeterministic) {
    if (!std::filesystem::exists(f16_config())) {
        GTEST_SKIP() << "f16.json fixture not generated";
    }
    CampaignSessionOptions o = make_opts(kunsan_strategy_world());
    o.strategy_layer = true;
    o.tasking_cycle_sec = 5;
    o.max_flights = 8;

    std::string err;
    auto a = f4::simulation::CampaignSession::create(o, &err);
    ASSERT_NE(a, nullptr) << "create failed: " << err;
    a->set_paused(false);
    for (int frame = 0; frame < 60; ++frame) a->advance(1.0);

    EXPECT_GT(a->stats().cycles, 0);
    EXPECT_GT(a->stats().intents, 0);

    // The strategy layer's own counters: the CAP family got stations.
    const auto* atm = a->campaign().atm_stats();
    ASSERT_NE(atm, nullptr);
    EXPECT_GT(atm->stations_targeted, 0);

    // At least one spawned aircraft's plan carries a racetrack anchor
    // (the station contract rode intent → route → plan → brain).
    bool anchor_seen = false;
    for (const auto id : a->sim().aircraft_entities()) {
        f4::entities::EntityHandle h(id, &a->sim().world());
        auto* brain = h.get<f4::ai::BrainComponent>();
        if (brain == nullptr) continue;
        for (const auto& wp : brain->mission_plan().route) {
            if (wp.loop_waypoints >= 2 && wp.station_time_s > 0.0) {
                anchor_seen = true;
                break;
            }
        }
        if (anchor_seen) break;
    }
    EXPECT_TRUE(anchor_seen);

    // The ON-state determinism: an identically-driven session lands
    // on the same bytes (the same contract every other arm keeps).
    auto b = f4::simulation::CampaignSession::create(o, &err);
    ASSERT_NE(b, nullptr) << err;
    b->set_paused(false);
    for (int frame = 0; frame < 60; ++frame) b->advance(1.0);
    EXPECT_EQ(a->campaign().to_summary_json(),
              b->campaign().to_summary_json());
    EXPECT_EQ(a->ledger_json(), b->ledger_json());
}

TEST(StrategySession, StrategyOffIsThePreP7SessionShape) {
    // The golden identity at the session level: strategy off → the
    // ATM still runs (packages built) but stations nothing, files
    // nothing, and no spawned plan carries a racetrack anchor.
    if (!std::filesystem::exists(f16_config())) {
        GTEST_SKIP() << "f16.json fixture not generated";
    }
    CampaignSessionOptions o = make_opts(kunsan_strategy_world());
    o.strategy_layer = false;
    o.tasking_cycle_sec = 5;
    o.max_flights = 8;

    std::string err;
    auto a = f4::simulation::CampaignSession::create(o, &err);
    ASSERT_NE(a, nullptr) << err;
    a->set_paused(false);
    for (int frame = 0; frame < 60; ++frame) a->advance(1.0);

    const auto* atm = a->campaign().atm_stats();
    ASSERT_NE(atm, nullptr);
    EXPECT_EQ(atm->stations_targeted, 0);
    EXPECT_EQ(atm->supports_filed, 0);
    EXPECT_EQ(atm->supports_shared, 0);
    EXPECT_EQ(atm->enemy_caps_filed, 0);

    for (const auto id : a->sim().aircraft_entities()) {
        f4::entities::EntityHandle h(id, &a->sim().world());
        auto* brain = h.get<f4::ai::BrainComponent>();
        if (brain == nullptr) continue;
        for (const auto& wp : brain->mission_plan().route) {
            EXPECT_EQ(wp.loop_waypoints, 0);
            EXPECT_DOUBLE_EQ(wp.station_time_s, 0.0);
        }
    }
}
