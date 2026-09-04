// f4-simulation/tests/test_campaign_spawner.cpp
//
// B.3 tranche tests — the campaign→sim loop:
//
//   1. build_mission_plan_from_flight — WaypointPlanComponent (grid,
//      campaign times, WP_ACTION bytes) → MissionPlan (ENU feet route),
//      including the leading-TAKEOFF drop and the altitude floor.
//   2. spawn_aircraft_from_flights — route attachment, TEAM-tag mapping
//      (owner_team_string), and the FlightSpawnFilter (team / mission /
//      cap).
//   3. CampaignSimSpawner — the bus-driven path: emit_flight_intents
//      publishes, the spawner materializes, duplicates skip, stats count.
//   4. Scenario JSON campaign_flight_filter parsing (name + byte form).

#include <f4/simulation/campaign_bridge.hpp>
#include <f4/simulation/campaign_spawner.hpp>
#include <f4/simulation/campaign_origin.hpp>
#include <f4/simulation/scenario.hpp>
#include <f4/campaign/campaign.hpp>
#include <f4/entities/entity.hpp>
#include <f4/world/world_adapters.hpp>
#include <f4/world/world_loader.hpp>
#include <f4/ai/brain_component.hpp>
#include <f4/flight/flight_model_component.hpp>
#include <f4/data/aircraft_config.hpp>
#include <f4/data/config_loader.hpp>

#include <gtest/gtest.h>

#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <string>

using namespace f4::simulation;
using f4::entities::EntityHandle;
using f4::entities::EntityId;
using f4::entities::EntityWorld;
using f4::entities::FlightPlanComponent;
using f4::entities::SquadronComponent;
using f4::entities::TransformComponent;
using f4::entities::UnitClass;
using f4::entities::UnitCoreComponent;
using f4::entities::WaypointPlanComponent;
using f4::entities::WaypointState;

namespace {

// The generated F-16 aircraft config fixture (same pattern as
// test_campaign_bridge.cpp).
bool loadF16Config(f4::data::AircraftConfig& cfg) {
    const char* env = std::getenv("F4_GENERATED_FIXTURES_DIR");
    std::string dir = env ? env : "";
#ifdef F4_GENERATED_FIXTURES_DIR
    if (dir.empty()) dir = F4_GENERATED_FIXTURES_DIR;
#endif
    if (dir.empty()) return false;
    const auto path = std::filesystem::path(dir) / "f16.json";
    if (!std::filesystem::exists(path)) return false;
    auto result = f4::data::loadConfig(path.string());
    if (!result.ok) return false;
    cfg = std::move(result.config);
    return true;
}

// A world shaped like a save: campaign (player team 2), two teams (2 and
// 6, at war), an airbase objective, a squadron, and flights with saved
// waypoint plans. Built in-memory via WorldState (the same shape the
// JSON loader produces).
f4::world::WorldState make_flight_world() {
    using f4::world::TeamState;
    using f4::world::UnitState;
    using f4::world::ObjectiveState;

    f4::world::WorldState ws;
    ws.version = 71;
    ws.campaign.current_time = 38574360;
    ws.campaign.te_team = 2;  // player

    ws.teams.resize(8);
    ws.teams[2] = TeamState{2, 2, 2, "ROK", ""};
    ws.teams[6] = TeamState{6, 6, 6, "DPRK", ""};
    // Stance is indexed by team slot: ROK[6] = -1 and DPRK[2] = -1 (war
    // between slots 2 and 6 — the PLAYER is team 2).
    ws.teams[2].stance = {0, 0, 0, 0, 0, 0, 5, 0};
    ws.teams[6].stance = {0, 0, 5, 0, 0, 0, 0, 0};

    ObjectiveState ab;
    ab.type = 100; ab.entity_type = 100; ab.x = 390; ab.y = 455;
    ab.owner = 2; ab.id_num = 4101; ab.camp_id = 50;
    ab.objective_type = 1;  // TYPE_AIRBASE
    ws.objectives = {ab};

    UnitState sq;
    sq.unit_class = UnitClass::Squadron;
    sq.domain = 2;
    sq.x = 390; sq.y = 455; sq.owner = 2; sq.id_num = 4281;
    sq.class_name = "52 TFS PAK";
    sq.airbase_id = 4101;

    auto make_flight = [&](uint32_t id, uint8_t owner, uint8_t mission,
                           bool with_waypoints) {
        UnitState fl;
        fl.unit_class = UnitClass::Flight;
        fl.domain = 2;
        fl.x = 392; fl.y = 451; fl.owner = owner; fl.id_num = id;
        fl.mission = mission;
        fl.time_on_target = 43739352;
        fl.package_id = 7029;
        fl.squadron_id = 4281;
        fl.callsign_id = 125; fl.callsign_num = 1;
        if (with_waypoints) {
            // A round-trip strike route: TAKEOFF at the airbase, an enroute
            // point at 2500 ft, the target, and LAND back home.
            WaypointState w1; w1.x = 392; w1.y = 451; w1.z = 0;    w1.action = 1;  // WP_TAKEOFF
            WaypointState w2; w2.x = 420; w2.y = 460; w2.z = 2500; w2.action = 15; // WP_NAVSTRIKE
            WaypointState w3; w3.x = 460; w3.y = 500; w3.z = 2500; w3.action = 17; // WP_STRIKE
            WaypointState w4; w4.x = 392; w4.y = 451; w4.z = 0;    w4.action = 7;  // WP_LAND
            fl.waypoints = {w1, w2, w3, w4};
        }
        return fl;
    };

    UnitState pkg;
    pkg.unit_class = UnitClass::Package;
    pkg.domain = 2;
    pkg.x = 392; pkg.y = 451; pkg.owner = 2; pkg.id_num = 7029;
    pkg.element_ids = {5001};

    ws.units = {sq,
                make_flight(5001, 2, 13, true),   // ROK INTSTRIKE, full route
                make_flight(5002, 2, 1, false),   // ROK BARCAP, no waypoints
                make_flight(5003, 6, 13, true),   // DPRK INTSTRIKE, full route
                pkg};
    return ws;
}

ScenarioAircraft make_template() {
    ScenarioAircraft tpl;
    tpl.callsign = "CAMPAIGN";
    tpl.vis_type_index = 1052;
    return tpl;
}

} // namespace

// ============================================================================
// build_mission_plan_from_flight
// ============================================================================

TEST(BuildMissionPlan, ConvertsGridToEnuAndDropsLeadingTakeoff) {
    auto ws = make_flight_world();
    EntityWorld ew;
    auto pw = f4::world::populate_world(ew, ws);

    EntityHandle flight(pw.unit_id_map.at(5001), &ew);
    auto* wp = flight.get<WaypointPlanComponent>();
    ASSERT_NE(wp, nullptr);
    ASSERT_EQ(wp->waypoints.size(), 4u);

    auto plan = build_mission_plan_from_flight(ew, pw.unit_id_map.at(5001));
    ASSERT_TRUE(plan.has_value());

    // Leading TAKEOFF dropped: 4 waypoints → 3 route legs.
    ASSERT_EQ(plan->route.size(), 3u);
    // Grid → ENU: waypoint 2 at grid (420, 460) → (430080, 471040) ft.
    EXPECT_NEAR(plan->route[0].position.x, 420.0 * 1024.0, 1e-6);
    EXPECT_NEAR(plan->route[0].position.y, 460.0 * 1024.0, 1e-6);
    // Altitude floor: the LAND waypoints' z=0 must NOT floor the 2500 ft
    // legs; the 0 ft legs floor to 500.
    EXPECT_NEAR(plan->route[0].position.z, 2500.0, 1e-6);
    EXPECT_NEAR(plan->route.back().position.z, 500.0, 1e-6);
    // Names carry the authoritative WP_ACTION text (campwp.h casing).
    EXPECT_NE(plan->route[0].name.find("NAVSTRIKE"), std::string::npos);
    EXPECT_NE(plan->route.back().name.find("LAND"), std::string::npos);
    // Default cruise speed.
    EXPECT_NEAR(plan->route[0].speed_kts, 400.0, 1e-6);
}

TEST(BuildMissionPlan, NoWaypointsNoPlan) {
    auto ws = make_flight_world();
    EntityWorld ew;
    auto pw = f4::world::populate_world(ew, ws);

    // Flight 5002 has no WaypointPlanComponent at all.
    auto plan = build_mission_plan_from_flight(ew, pw.unit_id_map.at(5002));
    EXPECT_FALSE(plan.has_value());
}

// ============================================================================
// spawn_aircraft_from_flights — routes, TEAM tags, filter
// ============================================================================

TEST(SpawnFromFlightsB3, AttachesRoutesAndTeamTags) {
    f4::data::AircraftConfig cfg;
    if (!loadF16Config(cfg)) GTEST_SKIP() << "F-16 config fixture missing";

    auto ws = make_flight_world();
    EntityWorld ew;
    auto pw = f4::world::populate_world(ew, ws);

    ScenarioAirfield airfield;
    airfield.runway_heading_rad = 0.0;
    airfield.threshold_position = f4::geo::WorldPosition(0.0, 5000.0, 50.0);

    auto spawned = spawn_aircraft_from_flights(
        ew, f4::world_convert::ClassTable{}, f4::models::ModelDatabase{},
        cfg, airfield, make_template());
    // All three flights spawn (no filter).
    ASSERT_EQ(spawned.size(), 3u);

    int with_route = 0, blue = 0, red = 0;
    for (const auto eid : spawned) {
        EntityHandle h(eid, &ew);
        auto* brain = h.get<f4::ai::BrainComponent>();
        ASSERT_NE(brain, nullptr);
        if (!brain->mission_plan().route.empty()) ++with_route;
        auto team_tag = h.get_tag(f4::entities::tags::TEAM);
        ASSERT_TRUE(team_tag && team_tag->as_string());
        if (*team_tag->as_string() == "blue") ++blue;
        if (*team_tag->as_string() == "red") ++red;

        // The B.3 fix: the FM must know its parking position — spawn at
        // the SQUADRON AIRBASE (390,455 grid → 399360, 465920 ft), not
        // the theater datum.
        auto* tf = h.get<TransformComponent>();
        auto* fm = h.get<f4::flight::FlightModelComponent>();
        ASSERT_NE(tf, nullptr);
        ASSERT_NE(fm, nullptr);
        const auto& kin = fm->model().state().kin;
        EXPECT_NEAR(kin.y, tf->position.x, 600.0)  // NED x ~ ENU east
            << "FM must initialize at the parking spot, not (0,0)";
    }
    // Flights 5001 & 5003 have routes; 5002 doesn't.
    EXPECT_EQ(with_route, 2);
    // Player team (ROK) → blue (2 flights), hostile DPRK → red (1).
    EXPECT_EQ(blue, 2);
    EXPECT_EQ(red, 1);
}

TEST(SpawnFromFlightsB3, FilterTeamMissionAndCap) {
    f4::data::AircraftConfig cfg;
    if (!loadF16Config(cfg)) GTEST_SKIP() << "F-16 config fixture missing";

    auto ws = make_flight_world();
    EntityWorld ew;
    auto pw = f4::world::populate_world(ew, ws);
    ScenarioAirfield airfield;

    // Team filter: only the DPRK flight (5003).
    {
        FlightSpawnFilter f;
        f.team = 6;
        auto spawned = spawn_aircraft_from_flights(
            ew, f4::world_convert::ClassTable{}, f4::models::ModelDatabase{},
            cfg, airfield, make_template(), f);
        ASSERT_EQ(spawned.size(), 1u);
    }
    // Mission filter: BARCAP (only flight 5002).
    {
        FlightSpawnFilter f;
        f.mission = 1;
        auto spawned = spawn_aircraft_from_flights(
            ew, f4::world_convert::ClassTable{}, f4::models::ModelDatabase{},
            cfg, airfield, make_template(), f);
        ASSERT_EQ(spawned.size(), 1u);
    }
    // Cap.
    {
        FlightSpawnFilter f;
        f.max_flights = 2;
        auto spawned = spawn_aircraft_from_flights(
            ew, f4::world_convert::ClassTable{}, f4::models::ModelDatabase{},
            cfg, airfield, make_template(), f);
        EXPECT_EQ(spawned.size(), 2u);
    }
}

// ============================================================================
// CampaignSimSpawner — the bus-driven B.3 loop
// ============================================================================

TEST(CampaignSimSpawner, EndToEndLoop) {
    f4::data::AircraftConfig cfg;
    if (!loadF16Config(cfg)) GTEST_SKIP() << "F-16 config fixture missing";

    auto ws = make_flight_world();
    EntityWorld ew;
    auto pw = f4::world::populate_world(ew, ws);
    f4::world::WorldStateAdapters adapters(ws);
    f4::messaging::MessageBus bus;

    ScenarioAirfield airfield;
    CampaignSimSpawner spawner(ew, pw.unit_id_map,
                               f4::world_convert::ClassTable{},
                               f4::models::ModelDatabase{},
                               cfg, airfield, make_template());
    spawner.attach(bus);

    // THE LOOP: live flights → intents → bus → spawner → aircraft.
    const auto intents = f4::campaign::emit_flight_intents(
        static_cast<const f4::world::IUnitCoreSource&>(adapters.units),
        static_cast<const f4::world::IFlightSource&>(adapters.units),
        bus, ws.campaign.current_time,
        &static_cast<const f4::world::ITeamSource&>(adapters.teams));

    // 3 tasked flights → 3 intents → 3 aircraft.
    ASSERT_EQ(intents.size(), 3u);
    EXPECT_EQ(spawner.spawned().size(), 3u);
    EXPECT_EQ(spawner.stats().aircraft_spawned, 3);
    EXPECT_EQ(spawner.stats().intents_seen, 3);
    EXPECT_EQ(spawner.stats().unknown_flight_ids, 0);

    // Two of the spawned aircraft carry routes (the two with waypoints).
    EXPECT_EQ(spawner.stats().routes_attached, 2);

    // Re-emitting the same intents must NOT double-spawn.
    const auto again = f4::campaign::emit_flight_intents(
        static_cast<const f4::world::IUnitCoreSource&>(adapters.units),
        static_cast<const f4::world::IFlightSource&>(adapters.units),
        bus, ws.campaign.current_time,
        &static_cast<const f4::world::ITeamSource&>(adapters.teams));
    (void)again;
    EXPECT_EQ(spawner.spawned().size(), 3u);
    EXPECT_EQ(spawner.stats().duplicate_skips, 3);
}

TEST(CampaignSimSpawner, IntentFilterAndUnknownIds) {
    f4::data::AircraftConfig cfg;
    if (!loadF16Config(cfg)) GTEST_SKIP() << "F-16 config fixture missing";

    auto ws = make_flight_world();
    EntityWorld ew;
    auto pw = f4::world::populate_world(ew, ws);
    ScenarioAirfield airfield;

    FlightSpawnFilter filter;
    filter.team = 6;             // only the DPRK flight
    CampaignSimSpawner spawner(ew, pw.unit_id_map,
                               f4::world_convert::ClassTable{},
                               f4::models::ModelDatabase{},
                               cfg, airfield, make_template(), filter);

    // Manual feed (no bus): a matching intent, a filtered-out intent, and
    // a synthetic intent whose flight_id resolves to nothing.
    f4::campaign::MissionIntent ok;
    ok.flight_id = 5003; ok.team = 6; ok.mission_byte = 13;
    spawner.handle(ok);
    EXPECT_EQ(spawner.stats().aircraft_spawned, 1);

    f4::campaign::MissionIntent filtered;
    filtered.flight_id = 5001; filtered.team = 2; filtered.mission_byte = 13;
    spawner.handle(filtered);
    EXPECT_EQ(spawner.stats().aircraft_spawned, 1);  // unchanged

    f4::campaign::MissionIntent synthetic;
    synthetic.flight_id = 999999; synthetic.team = 6; synthetic.mission_byte = 13;
    spawner.handle(synthetic);
    EXPECT_EQ(spawner.stats().unknown_flight_ids, 1);
    EXPECT_EQ(spawner.stats().aircraft_spawned, 1);
}

// ============================================================================
// B.3+ parking vs. per-airbase airfields — the non-airfield-base guard
//
// TestCamp's army-aviation flights sit at ARMY BASE objectives (no
// runway). With the per-base airfield map in play, those flights must
// park at the FALLBACK airfield (local departure) instead of the army
// base (cross-theater taxi to whatever default field the ATC answers
// with). Flights at real (or synthetic) airfields keep parking at their
// home base.
// =============================================================================

namespace {

// A world with an airbase, an ARMY BASE, a squadron on each, and one
// flight per squadron — the minimal TestCamp shape for the guard test.
f4::world::WorldState make_army_base_world() {
    using f4::world::TeamState;
    using f4::world::UnitState;
    using f4::world::ObjectiveState;

    f4::world::WorldState ws;
    ws.version = 71;
    ws.campaign.current_time = 38574360;
    ws.campaign.te_team = 2;
    ws.teams.resize(8);
    ws.teams[2] = TeamState{2, 2, 2, "ROK", ""};

    ObjectiveState ab;                      // real airbase (layout-less)
    ab.objective_type = 1;                  // TYPE_AIRBASE
    ab.x = 390; ab.y = 455; ab.owner = 2;
    ab.id_num = 4101; ab.camp_id = 50;

    ObjectiveState army;                    // army base — NOT an airfield
    army.objective_type = 3;                // TYPE_ARMYBASE
    army.x = 392; army.y = 451; army.owner = 2;
    army.id_num = 4102; army.camp_id = 51;
    ws.objectives = {ab, army};

    UnitState sq_air;                       // squadron at the airbase
    sq_air.unit_class = UnitClass::Squadron;
    sq_air.domain = 2;
    sq_air.x = 390; sq_air.y = 455; sq_air.owner = 2;
    sq_air.id_num = 4281; sq_air.airbase_id = 4101;

    UnitState sq_army;                      // army aviation squadron
    sq_army.unit_class = UnitClass::Squadron;
    sq_army.domain = 2;
    sq_army.x = 392; sq_army.y = 451; sq_army.owner = 2;
    sq_army.id_num = 4282; sq_army.airbase_id = 4102;

    auto make_flight = [&](uint32_t id, uint32_t sq_id) {
        UnitState fl;
        fl.unit_class = UnitClass::Flight;
        fl.domain = 2;
        fl.x = 391; fl.y = 453; fl.owner = 2; fl.id_num = id;
        fl.mission = 2;                     // AMIS_ESCORT (any tasked)
        fl.squadron_id = sq_id;
        fl.callsign_id = 125; fl.callsign_num = 1;
        return fl;
    };

    ws.units = {sq_air, sq_army,
                make_flight(5001, 4281),    // parks at its airbase
                make_flight(5002, 4282)};   // parks at the fallback field
    return ws;
}

} // namespace

TEST(SpawnFromFlightsB3, PerBaseAirfieldMapParksAtHomeBase) {
    f4::data::AircraftConfig cfg;
    if (!loadF16Config(cfg)) GTEST_SKIP() << "F-16 config fixture missing";

    auto ws = make_army_base_world();
    EntityWorld ew;
    auto pw = f4::world::populate_world(ew, ws);

    // Per-base map: only the real airbase (synthetic derivation — the
    // layout-less TestCamp shape).
    AirbaseAirfieldMap map;
    auto derived = derive_airfield_from_objective(ws.objectives[0], 36);
    ASSERT_TRUE(derived.has_value());
    map[4101] = *derived;

    ScenarioAirfield fallback;
    fallback.threshold_position = f4::geo::WorldPosition(1.0e6, 1.0e6, 0.0);

    auto spawned = spawn_aircraft_from_flights(
        ew, f4::world_convert::ClassTable{}, f4::models::ModelDatabase{},
        cfg, fallback, make_template(),
        FlightSpawnFilter{}, &map);
    ASSERT_EQ(spawned.size(), 2u);

    // Where did each aircraft park? Resolve by position against the two
    // candidate fields: home airbase (390,455 grid = 399360, 465920 ft)
    // and the fallback airfield (1e6, 1e6).
    int at_home = 0, at_fallback = 0;
    for (const auto eid : spawned) {
        EntityHandle h(eid, &ew);
        auto* tf = h.get<TransformComponent>();
        ASSERT_NE(tf, nullptr);
        const double d_home = std::hypot(tf->position.x - 399360.0,
                                         tf->position.y - 465920.0);
        const double d_fb = std::hypot(tf->position.x - 1.0e6,
                                       tf->position.y - 1.0e6);
        if (d_home < 500.0) ++at_home;
        if (d_fb < 500.0) ++at_fallback;
    }
    // One flight at its home airbase...
    EXPECT_EQ(at_home, 1);
    // ...and the army-based flight at the FALLBACK airfield (1e6,1e6),
    // NOT at the army base (392,451 grid = 401408, 461824 ft).
    EXPECT_EQ(at_fallback, 1);
}

TEST(SpawnFromFlightsB3, NullMapKeepsLegacySquadronBaseParking) {
    // No per-base map (the b3 spawner / single-field tests): parking
    // keeps the legacy behavior — at the squadron's base objective
    // whatever it is. This pins the guard's null-map branch.
    f4::data::AircraftConfig cfg;
    if (!loadF16Config(cfg)) GTEST_SKIP() << "F-16 config fixture missing";

    auto ws = make_army_base_world();
    EntityWorld ew;
    auto pw = f4::world::populate_world(ew, ws);

    ScenarioAirfield fallback;
    fallback.threshold_position = f4::geo::WorldPosition(1.0e6, 1.0e6, 0.0);

    auto spawned = spawn_aircraft_from_flights(
        ew, f4::world_convert::ClassTable{}, f4::models::ModelDatabase{},
        cfg, fallback, make_template());  // no map
    ASSERT_EQ(spawned.size(), 2u);

    // BOTH flights park at their squadron base objectives (airbase
    // 399360/465920 and army base 401408/461824) — never the fallback.
    for (const auto eid : spawned) {
        EntityHandle h(eid, &ew);
        auto* tf = h.get<TransformComponent>();
        ASSERT_NE(tf, nullptr);
        const double d_air = std::hypot(tf->position.x - 399360.0,
                                        tf->position.y - 465920.0);
        const double d_army = std::hypot(tf->position.x - 401408.0,
                                         tf->position.y - 461824.0);
        EXPECT_TRUE(d_air < 500.0 || d_army < 500.0)
            << "null map: park at the squadron base objective";
    }
}

// ============================================================================
// Scenario campaign_flight_filter parsing
// ============================================================================

TEST(ScenarioFilter, ParsesTeamMissionNameAndCap) {
    const auto s = load_scenario_from_string(R"({
        "name": "filter_test",
        "spawn_mode": "campaign_flights",
        "world_json_path": "w.json",
        "class_table_path": "ct",
        "campaign_flight_filter": {
            "team": 2,
            "mission": "AMIS_BARCAP2",
            "max_flights": 12
        },
        "aircraft": [{"callsign": "A1", "aircraft_config_path": "f16.json", "vis_type_index": 1052}]
    })");
    EXPECT_EQ(s.spawn_mode, SpawnMode::CampaignFlights);
    EXPECT_EQ(s.campaign_flight_filter.team, 2);
    EXPECT_EQ(s.campaign_flight_filter.mission, 2);  // AMIS_BARCAP2 byte
    EXPECT_EQ(s.campaign_flight_filter.max_flights, 12);
}

TEST(ScenarioFilter, ParsesMissionAsRawByteAndRejectsUnknownName) {
    {
        const auto s = load_scenario_from_string(R"({
            "name": "byte_test",
            "spawn_mode": "campaign_flights",
            "world_json_path": "w.json",
            "class_table_path": "ct",
            "campaign_flight_filter": {"mission": 9},
            "aircraft": [{"callsign": "A1", "aircraft_config_path": "f16.json", "vis_type_index": 1052}]
        })");
        EXPECT_EQ(s.campaign_flight_filter.mission, 9);
    }
    {
        // Unknown mission name fails loudly (the loud-failure discipline).
        EXPECT_THROW(load_scenario_from_string(R"({
            "name": "bad_test",
            "spawn_mode": "campaign_flights",
            "world_json_path": "w.json",
            "class_table_path": "ct",
            "campaign_flight_filter": {"mission": "NOT_A_MISSION"},
            "aircraft": [{"callsign": "A1", "aircraft_config_path": "f16.json", "vis_type_index": 1052}]
        })"), std::runtime_error);
    }
}

TEST(ScenarioFilter, DefaultsToNoFilter) {
    const auto s = load_scenario_from_string(R"({
        "name": "default_test",
        "spawn_mode": "campaign_flights",
        "world_json_path": "w.json",
        "class_table_path": "ct",
        "aircraft": [{"callsign": "A1", "aircraft_config_path": "f16.json", "vis_type_index": 1052}]
    })");
    EXPECT_EQ(s.campaign_flight_filter.team, -1);
    EXPECT_EQ(s.campaign_flight_filter.mission, -1);
    EXPECT_EQ(s.campaign_flight_filter.max_flights, 0);
}

// ============================================================================
// C3 — synthetic-intent missions fly their BUILT routes
// ============================================================================
//
// The campaign's route planner arms MissionIntents with a route; these
// tests cover the sim-side halves: the route → MissionPlan conversion,
// the aircraft spawn from an intent (no live flight entity), and the
// spawner's generation-to-spawn path.

TEST(BuildMissionPlanFromRoute, ConvertsAndDropsLeadingTakeoff) {
    // A built route: takeoff at the airbase, an enroute corner, the
    // STRIKE target (VU 4101 = the airbase objective here — a route
    // builder would carry the real target's VU), and landing home.
    std::vector<f4::campaign::RouteWaypoint> route;
    f4::campaign::RouteWaypoint w1;
    w1.x = 390; w1.y = 455; w1.altitude_ft = 0;    w1.action = 1;  // TAKEOFF
    f4::campaign::RouteWaypoint w2;
    w2.x = 420; w2.y = 460; w2.altitude_ft = 2500; w2.action = 0;  // filler
    f4::campaign::RouteWaypoint w3;
    w3.x = 460; w3.y = 500; w3.altitude_ft = 2000; w3.action = 17; // STRIKE
    w3.target_num = 4101;
    f4::campaign::RouteWaypoint w4;
    w4.x = 390; w4.y = 455; w4.altitude_ft = 0;    w4.action = 7;  // LAND
    route = {w1, w2, w3, w4};

    auto ws = make_flight_world();
    EntityWorld ew;
    auto pw = f4::world::populate_world(ew, ws);

    auto plan = build_mission_plan_from_route(route, 4101,
                                              &pw.objective_id_map);
    ASSERT_TRUE(plan.has_value());
    // Leading TAKEOFF dropped: 4 waypoints → 3 route legs.
    ASSERT_EQ(plan->route.size(), 3u);
    EXPECT_NEAR(plan->route[0].position.x, 420.0 * 1024.0, 1e-6);
    EXPECT_NEAR(plan->route[0].position.z, 2500.0, 1e-6);
    // The STRIKE leg floors HIGHER (delivery floor 1500 < 2000: keeps
    // the route altitude) and carries the target's EntityId::value.
    EXPECT_EQ(plan->route[1].action, 17);
    EXPECT_NEAR(plan->route[1].position.z, 2000.0, 1e-6);
    EXPECT_NE(plan->route[1].target_id, 0u);
    // The terminal leg is the approach entry fix (WP_LAND's position).
    EXPECT_NEAR(plan->route[2].position.x, 390.0 * 1024.0, 1e-6);
    EXPECT_NEAR(plan->route[2].position.z, 500.0, 1e-6);

    // Too-short routes build nothing.
    auto empty = build_mission_plan_from_route({w1}, 0, nullptr);
    EXPECT_FALSE(empty.has_value());
}

TEST(SpawnAircraftForIntent, ComposesAircraftWithRouteAndOriginStamp) {
    f4::data::AircraftConfig cfg;
    if (!loadF16Config(cfg)) GTEST_SKIP() << "F-16 config fixture missing";

    auto ws = make_flight_world();
    EntityWorld ew;
    auto pw = f4::world::populate_world(ew, ws);
    ScenarioAirfield airfield;

    // A synthetic INTSTRIKE intent from the ROK strike squadron: the
    // campaign drew from squadron 4281 (airbase 4101), the route runs
    // airbase → target → airbase.
    f4::campaign::MissionIntent intent;
    intent.team = 2;
    intent.mission_byte = 13;  // AMIS_INTSTRIKE
    intent.squadron_id = 4281;
    intent.flight_id = 12345;  // synthetic counter id
    intent.package_id = 12345;
    intent.target_objective_id = 4101;
    intent.synthetic = true;
    f4::campaign::RouteWaypoint w1;
    w1.x = 390; w1.y = 455; w1.altitude_ft = 0;    w1.action = 1;
    f4::campaign::RouteWaypoint w2;
    w2.x = 460; w2.y = 500; w2.altitude_ft = 2000; w2.action = 17;
    w2.target_num = 4101;
    f4::campaign::RouteWaypoint w3;
    w3.x = 390; w3.y = 455; w3.altitude_ft = 0;    w3.action = 7;
    intent.route = {w1, w2, w3};

    const auto spawned = spawn_aircraft_for_intent(
        ew, intent, pw.unit_id_map, f4::world_convert::ClassTable{},
        f4::models::ModelDatabase{}, cfg, airfield, make_template(), 0);
    ASSERT_TRUE(spawned.has_value());

    EntityHandle h(*spawned, &ew);
    // The aircraft parks at the squadron's airbase objective.
    auto* tf = h.get<TransformComponent>();
    ASSERT_NE(tf, nullptr);
    EXPECT_NEAR(tf->position.x, 390.0 * 1024.0, 200.0);
    EXPECT_NEAR(tf->position.y, 455.0 * 1024.0, 200.0);
    // The brain carries the MissionPlan built from the intent's route.
    auto* brain = h.get<f4::ai::BrainComponent>();
    ASSERT_NE(brain, nullptr);
    EXPECT_EQ(brain->mission_plan().route.size(), 2u);
    // The C1 origin stamp: the intent's own identity (kills over the
    // target write back to the tasked squadron).
    auto* origin = h.get<f4::simulation::CampaignOriginComponent>();
    ASSERT_NE(origin, nullptr);
    EXPECT_EQ(origin->flight_vu, 12345u);
    EXPECT_EQ(origin->squadron_vu, 4281u);
    EXPECT_EQ(origin->home_airbase_vu, 4101u);
    EXPECT_EQ(origin->team_slot, 2);
    // The TEAM tag maps the campaign owner slot.
    const auto team_tag = h.get_tag(f4::entities::tags::TEAM);
    ASSERT_TRUE(team_tag && team_tag->as_string());
    EXPECT_EQ(*team_tag->as_string(), "blue");  // team 2 = the player

    // An intent with no route spawns nothing.
    f4::campaign::MissionIntent bare = intent;
    bare.route.clear();
    EXPECT_FALSE(spawn_aircraft_for_intent(
        ew, bare, pw.unit_id_map, f4::world_convert::ClassTable{},
        f4::models::ModelDatabase{}, cfg, airfield, make_template(), 0)
        .has_value());
}

TEST(CampaignSimSpawner, SyntheticIntentSpawnsAndCounts) {
    f4::data::AircraftConfig cfg;
    if (!loadF16Config(cfg)) GTEST_SKIP() << "F-16 config fixture missing";

    auto ws = make_flight_world();
    EntityWorld ew;
    auto pw = f4::world::populate_world(ew, ws);
    f4::world::WorldStateAdapters adapters(ws);
    f4::messaging::MessageBus bus;
    ScenarioAirfield airfield;

    CampaignSimSpawner spawner(ew, pw.unit_id_map,
                               f4::world_convert::ClassTable{},
                               f4::models::ModelDatabase{},
                               cfg, airfield, make_template());
    spawner.attach(bus);

    // THE C3 LOOP: the campaign (route planner attached) publishes a
    // synthetic intent — the spawner materializes it WITHOUT a live
    // flight entity.
    f4::campaign::MissionIntent intent;
    intent.team = 2;
    intent.mission_byte = 13;
    intent.squadron_id = 4281;
    intent.flight_id = 1;  // counter id — resolves no flight entity
    intent.package_id = 1;
    intent.target_objective_id = 4101;
    intent.synthetic = true;
    f4::campaign::RouteWaypoint w1;
    w1.x = 390; w1.y = 455; w1.altitude_ft = 0;    w1.action = 1;
    f4::campaign::RouteWaypoint w2;
    w2.x = 460; w2.y = 500; w2.altitude_ft = 2000; w2.action = 17;
    w2.target_num = 4101;
    f4::campaign::RouteWaypoint w3;
    w3.x = 390; w3.y = 455; w3.altitude_ft = 0;    w3.action = 7;
    intent.route = {w1, w2, w3};

    bus.publish(intent);

    EXPECT_EQ(spawner.stats().intents_seen, 1);
    EXPECT_EQ(spawner.stats().aircraft_spawned, 1);
    EXPECT_EQ(spawner.stats().synthetic_spawned, 1);
    EXPECT_EQ(spawner.stats().routes_attached, 1);
    EXPECT_EQ(spawner.stats().unknown_flight_ids, 0);
    EXPECT_EQ(spawner.spawned().size(), 1u);

    // Re-publish: duplicate guard (same flight_id).
    bus.publish(intent);
    EXPECT_EQ(spawner.stats().duplicate_skips, 1);
    EXPECT_EQ(spawner.spawned().size(), 1u);

    // A route-less synthetic intent: skipped, counted (not unknown).
    f4::campaign::MissionIntent routeless = intent;
    routeless.flight_id = 2;
    routeless.package_id = 2;
    routeless.route.clear();
    bus.publish(routeless);
    EXPECT_EQ(spawner.stats().unknown_flight_ids, 1);
    EXPECT_EQ(spawner.spawned().size(), 1u);
}

TEST(BuildMissionPlanFromRoute, ResolvesUnitTargetsThroughTheUnitMap) {
    // G2 — the interdiction link's plan resolution: a CAS route's
    // delivery waypoint (the CAS action mapping = WP_GNDSTRIKE 14)
    // carries a BATTALION VU as target_num; the objective map misses,
    // the UNIT map resolves the aggregate battalion entity. The
    // flight-level fallback resolves units too (a CAS intent's target
    // is the battalion's VU).
    std::vector<f4::campaign::RouteWaypoint> route;
    f4::campaign::RouteWaypoint w1;
    w1.x = 390; w1.y = 455; w1.altitude_ft = 0;    w1.action = 1;  // TAKEOFF
    f4::campaign::RouteWaypoint w2;
    w2.x = 460; w2.y = 500; w2.altitude_ft = 2000; w2.action = 14; // GNDSTRIKE
    w2.target_num = 4621;                                          // battalion
    f4::campaign::RouteWaypoint w3;
    w3.x = 390; w3.y = 455; w3.altitude_ft = 0;    w3.action = 7;  // LAND
    route = {w1, w2, w3};

    auto ws = make_flight_world();
    EntityWorld ew;
    auto pw = f4::world::populate_world(ew, ws);

    // A battalion entity in the populated world (the G1 mirror shape).
    auto bn = ew.create();
    auto& uc = bn.add<f4::entities::UnitCoreComponent>();
    uc.unit_class = f4::entities::UnitClass::Battalion;
    uc.domain = 3;
    uc.roster = 0xAAA;
    auto& pb = bn.add<f4::entities::PropertyBag>();
    pb.ints["vu_id_num"] = 4621;
    auto& tf = bn.add<f4::entities::TransformComponent>();
    tf.position = f4::geo::WorldPosition{460.0 * 1024.0, 500.0 * 1024.0,
                                         0.0};
    std::unordered_map<std::uint32_t, f4::entities::EntityId> unit_map;
    unit_map[4621] = bn.id();

    // Waypoint target_num resolves through the unit map.
    auto plan = build_mission_plan_from_route(
        route, 4621, &pw.objective_id_map, &unit_map);
    ASSERT_TRUE(plan.has_value());
    ASSERT_EQ(plan->route.size(), 2u);
    EXPECT_EQ(plan->route[0].action, 14);
    EXPECT_EQ(plan->route[0].target_id, bn.id().value);

    // Flight-level fallback: no waypoint target_num, the intent's own
    // battalion VU resolves.
    auto w2b = w2;
    w2b.target_num = 0;
    auto plan2 = build_mission_plan_from_route(
        {w1, w2b, w3}, 4621, &pw.objective_id_map, &unit_map);
    ASSERT_TRUE(plan2.has_value());
    EXPECT_EQ(plan2->route[0].target_id, bn.id().value);

    // No maps at all: the target stays unresolved (the pre-G2 shape).
    auto plan3 = build_mission_plan_from_route({w1, w2b, w3}, 4621,
                                               nullptr, nullptr);
    ASSERT_TRUE(plan3.has_value());
    EXPECT_EQ(plan3->route[0].target_id, 0u);
}
