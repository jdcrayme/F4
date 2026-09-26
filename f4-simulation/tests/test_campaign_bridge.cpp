// test_campaign_bridge.cpp — Phase 2 campaign bridge tests.
//
// Verifies the two new functions that close the §4.3 gap
// (campaign-derived scenarios):
//
//   1. derive_airfield_from_objective(obj, runway_id)
//      - Returns nullopt for objectives with no ground_layout.
//      - Returns nullopt if no runway-class list is present.
//      - Returns a ScenarioAirfield with the right threshold + runway_end
//        + taxi route when given a realistic objective.
//      - The threshold position equals the objective center + the first
//        runway point's offset.
//
//   2. spawn_aircraft_from_flights(world, ct, db, cfg, airfield, template)
//      - Returns an empty vector if no FlightPlanComponent exists.
//      - Spawns one aircraft entity per Flight unit found.
//      - Each spawned entity carries TransformComponent + VisualModelComponent
//        + FlightModelComponent + BrainComponent.
//      - Per-flight lateral offset is applied so multiple aircraft don't
//        overlap at the same airbase.
//
// These tests use synthetic ObjectiveState + EntityWorld data so they
// don't depend on a real .cam fixture. The end-to-end path (load world
// JSON + spawn) is exercised by the integration smoke test in
// f4-world-viewer/src/.

#include <gtest/gtest.h>

#include "f4/simulation/campaign_bridge.hpp"
#include "f4/simulation/combat_bridge.hpp"
#include "f4/simulation/visual_model_component.hpp"

#include <f4/entities/entity.hpp>
#include <f4/entities/types.hpp>
#include <f4/flight/flight_model_component.hpp>
#include <f4/ai/brain_component.hpp>
#include <f4/world/detail/world_state.hpp>
#include <f4/world_types/class_table.hpp>
#include <f4/world_types/layout_types.hpp>  // PLT_RUNWAY, PLT_PARK
#include <f4/data/aircraft_config.hpp>
#include <f4/data/config_loader.hpp>

#include <cstdio>
#include <fstream>

#include <cstdint>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <string>

using namespace f4::simulation;
using namespace f4::entities;
using namespace f4::world;
using namespace f4::world_types;

namespace {

// Locate the generated F-16 aircraft config fixture (built by f4-convert
// from f4-convert/tests/fixtures/f16.dat). Returns true on success.
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

// Build a minimal airbase ObjectiveState with a runway + parking + follow-me
// list. Grid coords (10, 20) → ENU (10240, 20480) ft. Altitude 50 ft.
ObjectiveState make_airbase_objective() {
    ObjectiveState obj;
    obj.type = 1;  // TYPE_AIRBASE
    obj.x = 10;
    obj.y = 20;
    obj.z = 50.0f;

    GroundLayoutList runway;
    runway.type = PLT_RUNWAY;
    runway.heading_deg = 0.0f;
    runway.points = {
        GroundLayoutPoint{ 0.0f,    0.0f,  1, 0},  // threshold
        GroundLayoutPoint{ 0.0f, 5000.0f,  1, 0},  // far end
    };
    obj.ground_layout.push_back(runway);

    GroundLayoutList park;
    park.type = PLT_PARK;
    park.points = {
        GroundLayoutPoint{-200.0f, -100.0f, 11, 0},
    };
    obj.ground_layout.push_back(park);

    GroundLayoutList follow;
    follow.type = PLT_FOLLOW_ME;
    follow.points = {
        GroundLayoutPoint{-100.0f,  -50.0f, 15, 0},
        GroundLayoutPoint{ -50.0f,  500.0f, 15, 0},
        GroundLayoutPoint{   0.0f, 2000.0f, 15, 0},
    };
    obj.ground_layout.push_back(follow);

    return obj;
}

// Build a minimal ObjectiveState with no ground layout (e.g. a bridge).
ObjectiveState make_non_airbase_objective() {
    ObjectiveState obj;
    obj.type = 6;  // TYPE_BRIDGE
    obj.x = 5;
    obj.y = 5;
    obj.z = 30.0f;
    return obj;
}

} // namespace

// =============================================================================
// derive_airfield_from_objective
// =============================================================================

TEST(CampaignBridge, ReturnsNulloptForNonAirbaseObjective) {
    auto obj = make_non_airbase_objective();
    auto af = derive_airfield_from_objective(obj);
    EXPECT_FALSE(af.has_value());
}

TEST(CampaignBridge, ReturnsNulloptForAirbaseWithoutRunwayList) {
    auto obj = make_airbase_objective();
    obj.ground_layout.clear();  // strip everything
    auto af = derive_airfield_from_objective(obj);
    EXPECT_FALSE(af.has_value());
}

TEST(CampaignBridge, ReturnsNulloptForDegenerateRunwayList) {
    auto obj = make_airbase_objective();
    obj.ground_layout[0].points.clear();  // empty runway
    auto af = derive_airfield_from_objective(obj);
    EXPECT_FALSE(af.has_value());
}

TEST(CampaignBridge, DerivesThresholdAndRunwayEnd) {
    auto obj = make_airbase_objective();
    auto af = derive_airfield_from_objective(obj, 36);
    ASSERT_TRUE(af.has_value());

    // Objective center: (10 * 1024, 20 * 1024, 50) = (10240, 20480, 50).
    // Runway points are offsets from this center.
    // Threshold = first runway point (0, 0) → (10240, 20480, 50).
    EXPECT_DOUBLE_EQ(af->threshold_position.x, 10240.0);
    EXPECT_DOUBLE_EQ(af->threshold_position.y, 20480.0);
    EXPECT_DOUBLE_EQ(af->threshold_position.z, 50.0);

    // Runway end = last runway point (0, 5000) → (10240, 25480, 50).
    EXPECT_DOUBLE_EQ(af->runway_end_position.x, 10240.0);
    EXPECT_DOUBLE_EQ(af->runway_end_position.y, 25480.0);
    EXPECT_DOUBLE_EQ(af->runway_end_position.z, 50.0);
}

TEST(CampaignBridge, DerivesDepartureAltitude) {
    auto obj = make_airbase_objective();
    auto af = derive_airfield_from_objective(obj);
    ASSERT_TRUE(af.has_value());
    // Departure altitude = threshold altitude + 3000 ft (Tranche 44:
    // raised from 2500 — see campaign_bridge.cpp).
    EXPECT_DOUBLE_EQ(af->threshold_altitude_ft, 50.0);
    EXPECT_DOUBLE_EQ(af->departure_altitude_ft, 3050.0);
}

TEST(CampaignBridge, TaxiRouteIncludesParkingFollowMeAndThreshold) {
    auto obj = make_airbase_objective();
    auto af = derive_airfield_from_objective(obj);
    ASSERT_TRUE(af.has_value());
    ASSERT_GE(af->taxi_route.size(), 2u);

    // First waypoint = parking spot (-200, -100) → (10040, 20380, 50).
    EXPECT_DOUBLE_EQ(af->taxi_route.front().x, 10040.0);
    EXPECT_DOUBLE_EQ(af->taxi_route.front().y, 20380.0);

    // Last waypoint = threshold (10240, 20480).
    EXPECT_DOUBLE_EQ(af->taxi_route.back().x, 10240.0);
    EXPECT_DOUBLE_EQ(af->taxi_route.back().y, 20480.0);

    // The follow-me points should be in the middle (3 waypoints + parking +
    // threshold = 5 total; the threshold may be deduplicated if the last
    // follow-me point is close enough — here it's not).
    EXPECT_GE(af->taxi_route.size(), 4u)
        << "expected parking + 3 follow-me + threshold = 5 (or 4 if dedup)";
}

TEST(CampaignBridge, RunwayHeadingConvertedToRadians) {
    auto obj = make_airbase_objective();
    obj.ground_layout[0].heading_deg = 90.0;  // due east
    auto af = derive_airfield_from_objective(obj);
    ASSERT_TRUE(af.has_value());
    // 90 deg = π/2 radians. Allow small floating-point slack.
    EXPECT_NEAR(af->runway_heading_rad, 1.5707963267948966, 1e-9);
}

TEST(CampaignBridge, ActiveRunwayIdPropagated) {
    auto obj = make_airbase_objective();
    auto af = derive_airfield_from_objective(obj, 18);
    ASSERT_TRUE(af.has_value());
    EXPECT_EQ(af->active_runway_id, 18);
    EXPECT_EQ(af->active_runway_name, "Rwy 18");
}

// =============================================================================
// B.3+ synthetic airfields for layout-less airfield objectives
//
// Real .cam saves embed ground layouts ONLY for Airstrip-class objectives;
// all 50 TestCamp Airbases decode with an empty ground_layout (their
// runway geometry lives in theater static data we don't load). These
// tests pin the synthetic fallback that keeps ground ops LOCAL.
// =============================================================================

TEST(CampaignBridge, SynthesizesForLayoutlessAirbaseObjective) {
    ObjectiveState obj;
    obj.objective_type = 1;  // TYPE_AIRBASE — TestCamp shape: no layout
    obj.x = 390;
    obj.y = 455;
    obj.z = 100.0f;
    auto af = derive_airfield_from_objective(obj, 36);
    ASSERT_TRUE(af.has_value());

    // Objective center: (399360, 465920, 100) ft. Synthetic runway 36
    // (heading 0) straddles the center: threshold 3500 ft south.
    EXPECT_NEAR(af->threshold_position.x, 399360.0, 1e-6);
    EXPECT_NEAR(af->threshold_position.y, 465920.0 - 3500.0, 1e-6);
    EXPECT_NEAR(af->runway_end_position.y, 465920.0 + 3500.0, 1e-6);
    EXPECT_NEAR(af->runway_heading_rad, 0.0, 1e-9);  // 360 deg wraps to 0
    EXPECT_NEAR(af->runway_length_ft, 7000.0, 1e-6);
    EXPECT_NEAR(af->threshold_altitude_ft, 100.0, 1e-6);
    EXPECT_NEAR(af->departure_altitude_ft, 3100.0, 1e-6);

    // The taxi route is LOCAL: 2+ waypoints, last one is the threshold
    // (the TakeoffModule's hold-short), and the total route length stays
    // within a few thousand feet of the objective — never a cross-theater
    // route to some other airfield.
    ASSERT_GE(af->taxi_route.size(), 2u);
    const auto& last = af->taxi_route.back();
    EXPECT_NEAR(last.x, af->threshold_position.x, 1e-6);
    EXPECT_NEAR(last.y, af->threshold_position.y, 1e-6);
    double route_len = 0.0;
    for (std::size_t i = 1; i < af->taxi_route.size(); ++i) {
        route_len += std::hypot(af->taxi_route[i].x - af->taxi_route[i - 1].x,
                                af->taxi_route[i].y - af->taxi_route[i - 1].y);
    }
    EXPECT_LT(route_len, 5000.0) << "synthetic taxi route must stay local";

    // Parking row exists (the scenario-list path reads it).
    EXPECT_EQ(af->parking_spots.size(), 8u);
}

TEST(CampaignBridge, SynthesizesForLayoutlessAirstripObjective) {
    ObjectiveState obj;
    obj.objective_type = 2;  // TYPE_AIRSTRIP, layout stripped by a delta
    obj.x = 100;
    obj.y = 200;
    auto af = derive_airfield_from_objective(obj, 18);
    ASSERT_TRUE(af.has_value());
    // Heading 18 -> 180 deg: threshold NORTH of center (approach end).
    EXPECT_NEAR(af->runway_heading_rad, 3.14159265358979, 1e-9);
    EXPECT_NEAR(af->threshold_position.y, 200.0 * 1024.0 + 3500.0, 1e-6);
    EXPECT_EQ(af->active_runway_name, "Rwy 18");
}

TEST(CampaignBridge, LayoutlessNonAirfieldObjectiveStillNullopt) {
    // objective_type 3 = TYPE_ARMYBASE — army aviation squadrons sit at
    // these. They must NOT become synthetic airfields; the SPAWN side
    // relocates those flights to a real airfield instead (see
    // test_campaign_spawner.cpp ArmyBaseFlightParksAtFallbackAirfield).
    ObjectiveState obj;
    obj.objective_type = 3;
    obj.x = 392;
    obj.y = 451;
    auto af = derive_airfield_from_objective(obj, 36);
    EXPECT_FALSE(af.has_value());
}

// =============================================================================
// spawn_aircraft_from_flights
// =============================================================================

TEST(CampaignBridge, SpawnFromFlightsEmptyWorldReturnsEmpty) {
    // No flights in the world → spawn returns empty. We don't need a valid
    // AircraftConfig for this case (the function returns before calling
    // FlightModelComponent::init()).
    EntityWorld world;
    ClassTable ct;
    f4::data::AircraftConfig cfg;  // empty is fine here
    ScenarioAirfield airfield;
    ScenarioAircraft tpl;
    tpl.vis_type_index = 1052;

    auto spawned = spawn_aircraft_from_flights(world, ct, cfg, airfield, tpl);
    EXPECT_TRUE(spawned.empty());
}

TEST(CampaignBridge, SpawnFromFlightsReceiverRefuelLegGetsJoinStack) {
    // EMPL-2c — the receiver's JOIN STACK: a non-tanker flight whose
    // route carries a WP_REFUEL leg spawns with a WAITING orbit
    // anchored on the refuel waypoint — loop fields on the anchor,
    // corners carrying the REFUEL action (the leg flag must stay live
    // while the nav cycles the loop — the pairing keys the CURRENT
    // waypoint), and the post-hold recovery re-appended.
    f4::data::AircraftConfig cfg;
    if (!loadF16Config(cfg)) GTEST_SKIP() << "F-16 aircraft config fixture not available";

    EntityWorld world;

    auto airbase_h = world.create();
    auto& airbase_tf = airbase_h.add<TransformComponent>();
    airbase_tf.position = f4::geo::WorldPosition(0.0, 0.0, 50.0);

    auto sq_h = world.create();
    auto& sq = sq_h.add<SquadronComponent>();
    sq.airbase = airbase_h.id();
    auto& sq_uc = sq_h.add<UnitCoreComponent>();
    sq_uc.class_table_index = 273;

    auto f_h = world.create();
    auto& fp = f_h.add<FlightPlanComponent>();
    fp.squadron = sq_h.id();
    fp.callsign_id = 1;
    fp.callsign_num = 1;
    fp.mission = 2;   // AMIS_BARCAP2 — a receiver, not a tanker

    auto& wpc = f_h.add<WaypointPlanComponent>();
    auto wp = [&](int16_t x, int16_t y, int16_t z, uint8_t action) {
        WaypointState w;
        w.x = x; w.y = y; w.z = z; w.action = action;
        return w;
    };
    wpc.waypoints.push_back(wp(100, 100, 0, f4::campaign::kWpTakeoff));
    wpc.waypoints.push_back(wp(110, 110, 20, f4::campaign::kWpNothing));
    wpc.waypoints.push_back(wp(120, 120, 20, f4::campaign::kWpRefuel));
    wpc.waypoints.push_back(wp(130, 130, 10, f4::campaign::kWpLand));

    ClassTable ct;

    ScenarioAirfield airfield;
    airfield.runway_heading_rad = 0.0;
    airfield.threshold_position = f4::geo::WorldPosition(0.0, 5000.0, 50.0);
    airfield.departure_altitude_ft = 2550.0;

    ScenarioAircraft tpl;
    tpl.vis_type_index = 1052;
    tpl.callsign = "EAGLE";
    tpl.aircraft_config_path = "f16.json";

    auto spawned = spawn_aircraft_from_flights(world, ct, cfg, airfield, tpl);
    ASSERT_EQ(spawned.size(), 1u);

    EntityHandle h(spawned[0], &world);
    const auto* brain = h.get<f4::ai::BrainComponent>();
    ASSERT_NE(brain, nullptr);
    const auto& route = brain->mission_plan().route;
    // The plan drops the leading takeoff waypoint: [enroute, refuel,
    // land] + 3 stack corners = 6.
    ASSERT_EQ(route.size(), 6u);
    // The refuel waypoint (index 1) anchors the orbit.
    EXPECT_EQ(route[1].action, f4::campaign::kWpRefuel);
    EXPECT_EQ(route[1].loop_waypoints, 4);
    EXPECT_DOUBLE_EQ(route[1].station_time_s, 45.0 * 60.0);
    // LEVEL above the nav's terrain floor (the grid z=20 ft leg).
    EXPECT_GE(route[1].position.z, 3000.0);
    // The corners carry the REFUEL action — the leg flag stays live.
    for (int i = 2; i <= 4; ++i) {
        EXPECT_EQ(route[static_cast<std::size_t>(i)].action,
                  f4::campaign::kWpRefuel);
        EXPECT_EQ(route[static_cast<std::size_t>(i)].position.z,
                  route[1].position.z);
    }
    // The recovery rides behind the stack.
    EXPECT_EQ(route[5].action, f4::campaign::kWpLand);
}

TEST(CampaignBridge, SpawnFromFlightsUnratedTankerByteSpawnsAReceiver) {
    // 2026-09 EMPL-2 review — "fighters are receivers": a byte-39
    // flight whose squadron carries NO support rating (the stock war's
    // own shape — all 78 TestCamp tanker flights sit on fighter
    // squadrons) keeps a NORMAL brain and takes the receiver's join
    // stack from its refuel leg. The mission byte alone no longer
    // mints a tanker.
    f4::data::AircraftConfig cfg;
    if (!loadF16Config(cfg)) GTEST_SKIP() << "F-16 aircraft config fixture not available";

    EntityWorld world;

    auto airbase_h = world.create();
    auto& airbase_tf = airbase_h.add<TransformComponent>();
    airbase_tf.position = f4::geo::WorldPosition(0.0, 0.0, 50.0);

    auto sq_h = world.create();
    auto& sq = sq_h.add<SquadronComponent>();
    sq.airbase = airbase_h.id();
    // role_ratings left all-zero: the support row never rated.
    auto& sq_uc = sq_h.add<UnitCoreComponent>();
    sq_uc.class_table_index = 273;

    auto f_h = world.create();
    auto& fp = f_h.add<FlightPlanComponent>();
    fp.squadron = sq_h.id();
    fp.callsign_id = 1;
    fp.callsign_num = 1;
    fp.mission = 39;   // AMIS_TANK — the stock war's tanker byte

    auto& wpc = f_h.add<WaypointPlanComponent>();
    auto wp = [&](int16_t x, int16_t y, int16_t z, uint8_t action) {
        WaypointState w;
        w.x = x; w.y = y; w.z = z; w.action = action;
        return w;
    };
    wpc.waypoints.push_back(wp(100, 100, 0, f4::campaign::kWpTakeoff));
    wpc.waypoints.push_back(wp(110, 110, 20, f4::campaign::kWpNothing));
    wpc.waypoints.push_back(wp(120, 120, 20, f4::campaign::kWpRefuel));
    wpc.waypoints.push_back(wp(130, 130, 10, f4::campaign::kWpLand));

    ClassTable ct;

    ScenarioAirfield airfield;
    airfield.runway_heading_rad = 0.0;
    airfield.threshold_position = f4::geo::WorldPosition(0.0, 5000.0, 50.0);
    airfield.departure_altitude_ft = 2550.0;

    ScenarioAircraft tpl;
    tpl.vis_type_index = 1052;
    tpl.callsign = "EAGLE";
    tpl.aircraft_config_path = "f16.json";

    auto spawned = spawn_aircraft_from_flights(world, ct, cfg, airfield, tpl);
    ASSERT_EQ(spawned.size(), 1u);

    EntityHandle h(spawned[0], &world);
    const auto* brain = h.get<f4::ai::BrainComponent>();
    ASSERT_NE(brain, nullptr);
    EXPECT_FALSE(brain->is_tanker())
        << "an unrated byte-39 flight must not spawn into the tanker role";
    EXPECT_TRUE(brain->refuel_eligible())
        << "its refuel leg still makes it a receiver";
    // The receiver's join stack (the same shape the receiver test pins).
    const auto& route = brain->mission_plan().route;
    ASSERT_EQ(route.size(), 6u);
    EXPECT_EQ(route[1].action, f4::campaign::kWpRefuel);
    EXPECT_EQ(route[1].loop_waypoints, 4);
    EXPECT_EQ(route[5].action, f4::campaign::kWpLand);
}

TEST(CampaignBridge, SpawnFromFlightsRatedTankerByteSpawnsTheTanker) {
    // The positive face: the same byte-39 flight on a squadron the
    // save rates for support (role_ratings[kAroSupport] > 0) spawns
    // INTO the tanker role — the AAR discovery picture's own subject.
    f4::data::AircraftConfig cfg;
    if (!loadF16Config(cfg)) GTEST_SKIP() << "F-16 aircraft config fixture not available";

    EntityWorld world;

    auto airbase_h = world.create();
    auto& airbase_tf = airbase_h.add<TransformComponent>();
    airbase_tf.position = f4::geo::WorldPosition(0.0, 0.0, 50.0);

    auto sq_h = world.create();
    auto& sq = sq_h.add<SquadronComponent>();
    sq.airbase = airbase_h.id();
    sq.role_ratings[static_cast<std::size_t>(f4::campaign::kAroSupport)] = 70;
    auto& sq_uc = sq_h.add<UnitCoreComponent>();
    sq_uc.class_table_index = 273;

    auto f_h = world.create();
    auto& fp = f_h.add<FlightPlanComponent>();
    fp.squadron = sq_h.id();
    fp.callsign_id = 1;
    fp.callsign_num = 1;
    fp.mission = 39;

    auto& wpc = f_h.add<WaypointPlanComponent>();
    auto wp = [&](int16_t x, int16_t y, int16_t z, uint8_t action) {
        WaypointState w;
        w.x = x; w.y = y; w.z = z; w.action = action;
        return w;
    };
    // The AAR demo's tanker shape: no refuel leg of its own (the
    // receivers carry the rendezvous mark) — takeoff, station, home.
    wpc.waypoints.push_back(wp(100, 100, 0, f4::campaign::kWpTakeoff));
    wpc.waypoints.push_back(wp(120, 120, 20, f4::campaign::kWpNothing));
    wpc.waypoints.push_back(wp(130, 130, 10, f4::campaign::kWpLand));

    ClassTable ct;

    ScenarioAirfield airfield;
    airfield.runway_heading_rad = 0.0;
    airfield.threshold_position = f4::geo::WorldPosition(0.0, 5000.0, 50.0);
    airfield.departure_altitude_ft = 2550.0;

    ScenarioAircraft tpl;
    tpl.vis_type_index = 1052;
    tpl.callsign = "EAGLE";
    tpl.aircraft_config_path = "f16.json";

    auto spawned = spawn_aircraft_from_flights(world, ct, cfg, airfield, tpl);
    ASSERT_EQ(spawned.size(), 1u);

    EntityHandle h(spawned[0], &world);
    const auto* brain = h.get<f4::ai::BrainComponent>();
    ASSERT_NE(brain, nullptr);
    EXPECT_TRUE(brain->is_tanker())
        << "a support-rated byte-39 flight is a REAL tanker";
    EXPECT_FALSE(brain->refuel_eligible())
        << "the tanker's own refuel-marked leg must not arm the tanker";
}

TEST(CampaignBridge, SpawnFromFlightsCarriesAimpointFeature) {
    // EMPL-2d — the save's per-mission AIM-POINT ELEMENT rides the
    // route: the strike waypoint's `target_building` byte (the feature
    // index on the target objective) lands on the plan waypoint's
    // aimpoint_feature verbatim; non-delivery legs carry the wire's
    // "none" (255).
    f4::data::AircraftConfig cfg;
    if (!loadF16Config(cfg)) GTEST_SKIP() << "F-16 aircraft config fixture not available";

    EntityWorld world;

    auto airbase_h = world.create();
    auto& airbase_tf = airbase_h.add<TransformComponent>();
    airbase_tf.position = f4::geo::WorldPosition(0.0, 0.0, 50.0);

    auto sq_h = world.create();
    auto& sq = sq_h.add<SquadronComponent>();
    sq.airbase = airbase_h.id();
    auto& sq_uc = sq_h.add<UnitCoreComponent>();
    sq_uc.class_table_index = 273;

    auto f_h = world.create();
    auto& fp = f_h.add<FlightPlanComponent>();
    fp.squadron = sq_h.id();
    fp.callsign_id = 1;
    fp.callsign_num = 1;
    fp.mission = 13;   // AMIS_INTSTRIKE

    auto& wpc = f_h.add<WaypointPlanComponent>();
    auto wp = [&](int16_t x, int16_t y, int16_t z, uint8_t action,
                  uint8_t building) {
        WaypointState w;
        w.x = x; w.y = y; w.z = z; w.action = action;
        w.target_building = building;
        return w;
    };
    wpc.waypoints.push_back(wp(100, 100, 0, f4::campaign::kWpTakeoff, 255));
    wpc.waypoints.push_back(wp(110, 110, 20, f4::campaign::kWpNothing, 255));
    // The stick's aim point: feature 7 on the target objective.
    wpc.waypoints.push_back(wp(120, 120, 20, 17 /*WP_STRIKE*/, 7));
    wpc.waypoints.push_back(wp(130, 130, 10, f4::campaign::kWpLand, 255));

    ClassTable ct;

    ScenarioAirfield airfield;
    airfield.runway_heading_rad = 0.0;
    airfield.threshold_position = f4::geo::WorldPosition(0.0, 5000.0, 50.0);
    airfield.departure_altitude_ft = 2550.0;

    ScenarioAircraft tpl;
    tpl.vis_type_index = 1052;
    tpl.callsign = "EAGLE";
    tpl.aircraft_config_path = "f16.json";

    auto spawned = spawn_aircraft_from_flights(world, ct, cfg, airfield, tpl);
    ASSERT_EQ(spawned.size(), 1u);

    EntityHandle h(spawned[0], &world);
    const auto* brain = h.get<f4::ai::BrainComponent>();
    ASSERT_NE(brain, nullptr);
    const auto& route = brain->mission_plan().route;
    ASSERT_EQ(route.size(), 3u);   // the leading takeoff waypoint drops
    EXPECT_EQ(route[1].action, 17);
    EXPECT_EQ(route[1].aimpoint_feature, 7);
    // Enroute legs carry the wire's "none" sentinel.
    EXPECT_EQ(route[0].aimpoint_feature, 255);
    EXPECT_EQ(route[2].aimpoint_feature, 255);
}

TEST(CampaignBridge, SpawnFromFlightsCreatesOneEntityPerFlight) {
    f4::data::AircraftConfig cfg;
    if (!loadF16Config(cfg)) GTEST_SKIP() << "F-16 aircraft config fixture not available";

    EntityWorld world;

    // Create a squadron entity with a SquadronComponent + an airbase EntityId.
    // The airbase is just a TransformComponent at (0, 0, 50).
    auto airbase_h = world.create();
    auto& airbase_tf = airbase_h.add<TransformComponent>();
    airbase_tf.position = f4::geo::WorldPosition(0.0, 0.0, 50.0);

    auto sq_h = world.create();
    auto& sq = sq_h.add<SquadronComponent>();
    sq.airbase = airbase_h.id();
    auto& sq_uc = sq_h.add<UnitCoreComponent>();
    sq_uc.unit_class = UnitClass::Squadron;
    sq_uc.class_table_index = 273;  // F-16 vehicle-class entity_type

    // Create two Flight entities, each pointing at the squadron.
    for (int i = 0; i < 2; ++i) {
        auto f_h = world.create();
        auto& fp = f_h.add<FlightPlanComponent>();
        fp.squadron = sq_h.id();
        fp.callsign_id = 1;
        fp.callsign_num = static_cast<uint8_t>(i + 1);
    }

    ClassTable ct;

    ScenarioAirfield airfield;
    airfield.runway_heading_rad = 0.0;
    airfield.threshold_position = f4::geo::WorldPosition(0.0, 5000.0, 50.0);
    airfield.departure_altitude_ft = 2550.0;

    ScenarioAircraft tpl;
    tpl.vis_type_index = 1052;
    tpl.callsign = "EAGLE";
    tpl.aircraft_config_path = "f16.json";

    auto spawned = spawn_aircraft_from_flights(world, ct, cfg, airfield, tpl);
    ASSERT_EQ(spawned.size(), 2u);

    // Each spawned entity must carry all four aircraft components.
    for (const auto eid : spawned) {
        EntityHandle h(eid, &world);
        EXPECT_NE(h.get<TransformComponent>(), nullptr);
        EXPECT_NE(h.get<f4::flight::FlightModelComponent>(), nullptr);
        EXPECT_NE(h.get<VisualModelComponent>(), nullptr);
        EXPECT_NE(h.get<f4::ai::BrainComponent>(), nullptr);
    }
}

TEST(CampaignBridge, SpawnFromFlightsAppliesPerFlightOffset) {
    f4::data::AircraftConfig cfg;
    if (!loadF16Config(cfg)) GTEST_SKIP() << "F-16 aircraft config fixture not available";

    // Two flights at the same airbase should NOT spawn at the same position
    // — the bridge applies a lateral offset so they don't overlap.
    EntityWorld world;

    auto airbase_h = world.create();
    auto& airbase_tf = airbase_h.add<TransformComponent>();
    airbase_tf.position = f4::geo::WorldPosition(0.0, 0.0, 50.0);

    auto sq_h = world.create();
    auto& sq = sq_h.add<SquadronComponent>();
    sq.airbase = airbase_h.id();
    auto& sq_uc = sq_h.add<UnitCoreComponent>();
    sq_uc.class_table_index = 273;

    for (int i = 0; i < 2; ++i) {
        auto f_h = world.create();
        auto& fp = f_h.add<FlightPlanComponent>();
        fp.squadron = sq_h.id();
    }

    ClassTable ct;

    ScenarioAirfield airfield;
    airfield.runway_heading_rad = 0.0;
    airfield.threshold_position = f4::geo::WorldPosition(0.0, 5000.0, 50.0);

    ScenarioAircraft tpl;
    tpl.vis_type_index = 1052;

    auto spawned = spawn_aircraft_from_flights(world, ct, cfg, airfield, tpl);
    ASSERT_EQ(spawned.size(), 2u);

    EntityHandle h0(spawned[0], &world);
    EntityHandle h1(spawned[1], &world);
    auto* tf0 = h0.get<TransformComponent>();
    auto* tf1 = h1.get<TransformComponent>();
    ASSERT_NE(tf0, nullptr);
    ASSERT_NE(tf1, nullptr);

    // The first flight should be at +offset (east), the second at -offset.
    // |x0| and |x1| should both be > 0, and they should be on opposite sides.
    EXPECT_GT(tf0->position.x, 0.0) << "first flight should be east of airbase";
    EXPECT_LT(tf1->position.x, 0.0) << "second flight should be west of airbase";
    EXPECT_NEAR(std::abs(tf0->position.x), std::abs(tf1->position.x), 1e-6)
        << "offsets should be symmetric";
}

TEST(CampaignBridge, SpawnFromFlightsFallsBackToTemplateVisType) {
    f4::data::AircraftConfig cfg;
    if (!loadF16Config(cfg)) GTEST_SKIP() << "F-16 aircraft config fixture not available";

    // If the ClassTable lookup fails (returns 0), the bridge should fall
    // back to the template's vis_type_index. We verify this by NOT loading
    // the class table — ct.vis_type_for() returns 0 for every entity_type.
    EntityWorld world;

    auto airbase_h = world.create();
    auto& airbase_tf = airbase_h.add<TransformComponent>();
    airbase_tf.position = f4::geo::WorldPosition(0.0, 0.0, 50.0);

    auto sq_h = world.create();
    auto& sq = sq_h.add<SquadronComponent>();
    sq.airbase = airbase_h.id();
    auto& sq_uc = sq_h.add<UnitCoreComponent>();
    sq_uc.class_table_index = 999;  // unknown entity_type → vis_type_for returns 0

    auto f_h = world.create();
    auto& fp = f_h.add<FlightPlanComponent>();
    fp.squadron = sq_h.id();

    ClassTable ct;  // empty — vis_type_for returns 0 for everything

    ScenarioAirfield airfield;
    airfield.runway_heading_rad = 0.0;
    airfield.threshold_position = f4::geo::WorldPosition(0.0, 5000.0, 50.0);

    ScenarioAircraft tpl;
    tpl.vis_type_index = 1052;  // F-16 fallback

    auto spawned = spawn_aircraft_from_flights(world, ct, cfg, airfield, tpl);
    ASSERT_EQ(spawned.size(), 1u);

    // The spawned aircraft should be valid. Tranche 0d: vis_type is always
    // set at spawn (the identity, independent of any ModelDatabase).
    EntityHandle h(spawned[0], &world);
    auto* vis = h.get<VisualModelComponent>();
    ASSERT_NE(vis, nullptr);
    EXPECT_EQ(vis->vis_type, 1052);  // F-16 vis type
}

TEST(CampaignBridge, SpawnFromFlightsFallsBackToThresholdWithoutSquadron) {
    f4::data::AircraftConfig cfg;
    if (!loadF16Config(cfg)) GTEST_SKIP() << "F-16 aircraft config fixture not available";

    // If a Flight has no resolved squadron (squadron.value == 0), the bridge
    // should fall back to the airfield's threshold position. The aircraft
    // still spawns — just on the runway instead of at a parking spot.
    EntityWorld world;

    auto f_h = world.create();
    [[maybe_unused]] auto& fp = f_h.add<FlightPlanComponent>();
    // fp.squadron defaults to EntityId{0} — unresolved.

    ClassTable ct;

    ScenarioAirfield airfield;
    airfield.runway_heading_rad = 0.0;
    airfield.threshold_position = f4::geo::WorldPosition(500.0, 8000.0, 50.0);

    ScenarioAircraft tpl;
    tpl.vis_type_index = 1052;

    auto spawned = spawn_aircraft_from_flights(world, ct, cfg, airfield, tpl);
    ASSERT_EQ(spawned.size(), 1u);

    EntityHandle h(spawned[0], &world);
    auto* tf = h.get<TransformComponent>();
    ASSERT_NE(tf, nullptr);
    // The threshold is at (500, 8000). The per-flight offset is applied on
    // top: flight_index=0 → +80 ft east → (580, 8000).
    EXPECT_NEAR(tf->position.x, 580.0, 1e-6);
    EXPECT_NEAR(tf->position.y, 8000.0, 1e-6);
}

// ============================================================================
// CAMP-SCALE-1 — the converted tables' data flows: the pilot-skill flow
// (the squadron roster → the brain's fusion cadence) and the VCD
// countermeasure supply (spawn resolves, the arm path consumes).
// ============================================================================

namespace {

/// Build a falcon4.ct.json whose entry at index `vehicle_index`
/// (entity_type = 100 + vehicle_index) is a DTYPE_VEHICLE row pointing
/// at VCD row 0 (the F-16X's position in scale_tables_json). Every
/// other entry is an inert NOTHING row so the file has the full
/// positional span.
std::string build_ct_json(int vehicle_index, int entries) {
    std::string s = "{\"count\": " + std::to_string(entries) +
                    ", \"entries\": [";
    for (int i = 0; i < entries; ++i) {
        if (i) s += ", ";
        if (i == vehicle_index) {
            s += "{\"entity_type\": " + std::to_string(100 + i) +
                 ", \"domain\": 2, \"cls\": 4, \"type\": 0, \"stype\": 3,"
                 " \"vis_type\": [0,0,0,0,0,0,0], \"data_type\": 5,"
                 " \"data_ptr_index\": 0}";
        } else {
            s += "{\"entity_type\": " + std::to_string(100 + i) +
                 ", \"domain\": 2, \"cls\": 4, \"type\": 0, \"stype\": 3,"
                 " \"vis_type\": [0,0,0,0,0,0,0], \"data_type\": 0,"
                 " \"data_ptr_index\": 0}";
        }
    }
    s += "]}";
    return s;
}

/// A minimal tables document: VCD row 1 ("F-16X") carries hardpoints
/// 7/8/20 = Chaff 30 / Flare 15 / gun 0; VCD row 2 ("Truck") carries no
/// dispenser rows. WCD rows 7/8 are named Chaff/Flare.
std::string scale_tables_json() {
    // WCD rows are POSITIONAL: hardpoint weapon IDs index the table by row
    // position (Chaff at row 7, Flare at row 8, the gun rounds at 20) —
    // the same convention the real ~600-row WCD carries.
    std::string s = "{\n";
    s += "  \"format\": \"f4.theater.tables/1\",\n";
    s += "  \"counts\": {\"units\": 0, \"vehicles\": 2, \"weapons\": 21},\n";
    s += "  \"units\": [],\n";
    s += "  \"vehicles\": [\n";
    s +=
        "    {\"index\": 273, \"name\": \"F-16X\", \"nctr\": \"F16\","
        " \"hit_points\": 150, \"flags\": 0, \"rcs_factor\": 0.0,"
        " \"max_wt\": 0, \"empty_wt\": 0, \"fuel_wt\": 0, \"fuel_econ\": 0,"
        " \"engine_sound\": 0, \"high_alt\": 0, \"low_alt\": 0,"
        " \"cruise_alt\": 0, \"max_speed\": 800, \"radar_type\": 0,"
        " \"number_of_pilots\": 1, \"rack_flags\": 0, \"visible_flags\": 0,"
        " \"callsign_index\": 0, \"callsign_slots\": 0,"
        " \"hit_chance\": [0,0,0,0,0,0,0,0], \"strength\": [0,0,0,0,0,0,0,0],"
        " \"range\": [0,0,0,0,0,0,0,0], \"detection\": [0,0,0,0,0,0,0,0],"
        " \"weapon\": [7, 8, 20, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0],"
        " \"weapons\": [30, 15, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0],"
        " \"damage_mod\": [0,0,0,0,0,0,0,0,0,0,0]},\n";
    s +=
        "    {\"index\": 274, \"name\": \"Truck\", \"nctr\": \"TRK\","
        " \"hit_points\": 60, \"flags\": 0, \"rcs_factor\": 2.0,"
        " \"max_wt\": 0, \"empty_wt\": 0, \"fuel_wt\": 0, \"fuel_econ\": 0,"
        " \"engine_sound\": 0, \"high_alt\": 0, \"low_alt\": 0,"
        " \"cruise_alt\": 0, \"max_speed\": 80, \"radar_type\": 0,"
        " \"number_of_pilots\": 0, \"rack_flags\": 0, \"visible_flags\": 0,"
        " \"callsign_index\": 0, \"callsign_slots\": 0,"
        " \"hit_chance\": [0,0,0,0,0,0,0,0], \"strength\": [0,0,0,0,0,0,0,0],"
        " \"range\": [0,0,0,0,0,0,0,0], \"detection\": [0,0,0,0,0,0,0,0],"
        " \"weapon\": [20, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0],"
        " \"weapons\": [200, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0],"
        " \"damage_mod\": [0,0,0,0,0,0,0,0,0,0,0]}\n";
    s += "  ],\n";
    s += "  \"weapons\": [\n";
    for (int i = 0; i < 21; ++i) {
        const char* name = (i == 7)  ? "Chaff"
                         : (i == 8)  ? "Flare"
                         : (i == 20) ? "M-61 rounds" : "W";
        const int strength = (i == 20) ? 4 : (i == 7 || i == 8) ? 1 : 0;
        const int damage_type = (i == 20) ? 6 : 0;
        const int range_km = (i == 20) ? 2 : 0;
        const int fire_rate = (i == 20) ? 10 : (i == 7 || i == 8) ? 1 : 0;
        char row[512];
        std::snprintf(row, sizeof(row),
            "    {\"index\": %d, \"name\": \"%s\", \"strength\": %d,"
            " \"damage_type\": %d, \"range_km\": %d, \"flags\": 0,"
            " \"fire_rate\": %d, \"rarity\": 100, \"guidance_flags\": 0,"
            " \"collective\": 0, \"simweap_index\": 0, \"weight\": 0,"
            " \"drag_index\": 0, \"blast_radius\": 0, \"radar_type\": 0,"
            " \"sim_data_idx\": 0, \"max_alt\": 0,"
            " \"hit_chance\": [0,0,0,0,0,0,0,0]}%s\n",
            i, name, strength, damage_type, range_km, fire_rate,
            (i == 20) ? "" : ",");
        s += row;
    }
    s += "  ]\n";
    s += "}\n";
    return s;
}

} // namespace

TEST(CampScale1, PilotSkillFromRosterMapsTheWireNibble) {
    // The documented monotone map: 0-2 Recruit | 3-5 Rookie |
    // 6-7 Veteran | 8-9 Ace; no AVAILABLE pilot → the Veteran default.
    using f4::entities::PilotState;
    using SK = f4::ai::SkillLevel;

    EXPECT_EQ(pilot_skill_from_roster({}), SK::Veteran);  // no roster
    // A default-constructed pilot is AVAILABLE with skill 0 — the map's
    // bottom rung (data, not absence, drives the cadence).
    EXPECT_EQ(pilot_skill_from_roster({PilotState{}}), SK::Recruit);

    // Only dead pilots → default (the sortie flies the stock cadence).
    PilotState dead{};
    dead.status = 1;
    dead.skill = 9;
    EXPECT_EQ(pilot_skill_from_roster({dead}), SK::Veteran);

    // Boundaries.
    auto p = [](int skill, int id) {
        PilotState ps{};
        ps.skill = static_cast<uint8_t>(skill);
        ps.pilot_id = static_cast<int16_t>(id);
        return ps;
    };
    EXPECT_EQ(pilot_skill_from_roster({p(0, 1)}), SK::Recruit);
    EXPECT_EQ(pilot_skill_from_roster({p(2, 1)}), SK::Recruit);
    EXPECT_EQ(pilot_skill_from_roster({p(3, 1)}), SK::Rookie);
    EXPECT_EQ(pilot_skill_from_roster({p(5, 1)}), SK::Rookie);
    EXPECT_EQ(pilot_skill_from_roster({p(6, 1)}), SK::Veteran);
    EXPECT_EQ(pilot_skill_from_roster({p(7, 1)}), SK::Veteran);
    EXPECT_EQ(pilot_skill_from_roster({p(8, 1)}), SK::Ace);
    EXPECT_EQ(pilot_skill_from_roster({p(9, 1)}), SK::Ace);

    // Best AVAILABLE pilot wins (a dead ace never flies), ties break by
    // rating then the lowest pilot id — deterministic.
    PilotState dead_ace{dead};
    PilotState rookie{}; rookie.skill = 3; rookie.pilot_id = 5;
    EXPECT_EQ(pilot_skill_from_roster({dead_ace, rookie}), SK::Rookie);
    EXPECT_EQ(pilot_skill_from_roster({p(5, 2), p(5, 1)}), SK::Rookie);
}

TEST(CampScale1, SpawnFlowsPilotSkillAndCountermeasureSupply) {
    f4::data::AircraftConfig cfg;
    if (!loadF16Config(cfg)) GTEST_SKIP() << "F-16 aircraft config fixture not available";

    // Class table: entity_type 273 → VCD row 1 (the F-16X's dispensers).
    f4::world_types::ClassTable ct;
    const auto ct_path = std::filesystem::temp_directory_path() /
                         "f4_scale1_ct.json";
    {
        std::ofstream f(ct_path);
        f << build_ct_json(/*vehicle_index=*/173, /*entries=*/300);
    }
    ct.load_json(ct_path);
    std::filesystem::remove(ct_path);
    ASSERT_TRUE(ct.loaded());

    const auto tables = f4::world::TheaterTables::parse(scale_tables_json());

    // The world lives in the harness struct — guaranteed copy elision
    // (C++17) keeps the EntityWorld in place, so the returned EntityIds
    // stay valid for exactly the harness object's lifetime.
    struct SpawnHarness {
        EntityWorld world;
        f4::entities::EntityId squadron;
        f4::entities::EntityId flight;
    };
    auto make_world = [](std::vector<f4::entities::PilotState> pilots) {
        SpawnHarness h{};
        auto airbase_h = h.world.create();
        airbase_h.add<TransformComponent>().position =
            f4::geo::WorldPosition(0.0, 0.0, 50.0);
        auto sq_h = h.world.create();
        auto& sq = sq_h.add<SquadronComponent>();
        sq.airbase = airbase_h.id();
        sq.pilots = std::move(pilots);
        auto& sq_uc = sq_h.add<UnitCoreComponent>();
        sq_uc.unit_class = UnitClass::Squadron;
        sq_uc.class_table_index = 273;
        auto& sq_vc = sq_h.add<VehicleCompositionComponent>();
        f4::entities::VehicleGroup g{};
        g.vehicle_type = 273;  // the F-16X vehicle entity type
        g.count = 12;
        sq_vc.groups.push_back(g);
        auto f_h = h.world.create();
        auto& fp = f_h.add<FlightPlanComponent>();
        fp.squadron = sq_h.id();
        fp.callsign_id = 1;
        fp.callsign_num = 1;
        h.squadron = sq_h.id();
        h.flight = f_h.id();
        return h;
    };

    ScenarioAirfield airfield;
    airfield.runway_heading_rad = 0.0;
    airfield.threshold_position = f4::geo::WorldPosition(0.0, 5000.0, 50.0);
    ScenarioAircraft tpl;
    tpl.vis_type_index = 1052;
    tpl.callsign = "EAGLE";
    tpl.aircraft_config_path = "f16.json";

    auto ace = [](int id, int skill, uint8_t status) {
        f4::entities::PilotState ps{};
        ps.pilot_id = static_cast<int16_t>(id);
        ps.skill = static_cast<uint8_t>(skill);
        ps.status = status;
        return ps;
    };

    // 1. The flow ON: the best available pilot (skill 9) sets the brain's
    //    fusion cadence to Ace, and the vehicle's VCD/WCD supply stamps
    //    the countermeasure counts on the spawned aircraft.
    {
        auto sh = make_world({ace(1, 9, 1), ace(2, 9, 0), ace(3, 4, 0)});
        const auto spawned = spawn_aircraft_for_flight(
            sh.world, sh.flight, ct, cfg, airfield, tpl, 0, nullptr, nullptr,
            nullptr, nullptr, nullptr, &tables, /*pilot_skill_flow=*/true);
        ASSERT_TRUE(spawned.has_value());
        EntityHandle eh(*spawned, &sh.world);
        auto* brain = eh.get<f4::ai::BrainComponent>();
        ASSERT_NE(brain, nullptr);
        EXPECT_EQ(brain->pilot_skill(), f4::ai::SkillLevel::Ace);
        auto* supply = eh.get<CountermeasureSupplyComponent>();
        ASSERT_NE(supply, nullptr);
        EXPECT_EQ(supply->chaff_rounds, 30);
        EXPECT_EQ(supply->flare_rounds, 15);
    }

    // 2. The flow OFF (the pre-SCALE identity): the same roster, the
    //    Veteran cadence, no supply stamp without tables.
    {
        auto sh = make_world({ace(2, 9, 0)});
        const auto spawned = spawn_aircraft_for_flight(
            sh.world, sh.flight, ct, cfg, airfield, tpl, 0);
        ASSERT_TRUE(spawned.has_value());
        EntityHandle eh(*spawned, &sh.world);
        EXPECT_EQ(eh.get<f4::ai::BrainComponent>()->pilot_skill(),
                  f4::ai::SkillLevel::Veteran);
        EXPECT_EQ(eh.get<CountermeasureSupplyComponent>(), nullptr);
    }

    // 3. Tables but nothing resolved (a dispenser-less vehicle) → no
    //    stamp; the arm path keeps the documented 30/15 defaults.
    {
        auto sh = make_world({});
        EntityHandle(sh.squadron, &sh.world)
            .get<VehicleCompositionComponent>()
            ->groups.front()
            .vehicle_type = 274;  // the Truck — no dispensers
        const auto spawned = spawn_aircraft_for_flight(
            sh.world, sh.flight, ct, cfg, airfield, tpl, 0, nullptr, nullptr,
            nullptr, nullptr, nullptr, &tables, true);
        ASSERT_TRUE(spawned.has_value());
        EXPECT_EQ(EntityHandle(*spawned, &sh.world)
                      .get<CountermeasureSupplyComponent>(),
                  nullptr);
    }
}
