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
// f4-scenario-player/tests/.

#include <gtest/gtest.h>

#include "f4/simulation/campaign_bridge.hpp"
#include "f4/simulation/visual_model_component.hpp"

#include <f4/entities/entity.hpp>
#include <f4/entities/types.hpp>
#include <f4/flight/flight_model_component.hpp>
#include <f4/ai/brain_component.hpp>
#include <f4/world/detail/world_state.hpp>
#include <f4/world_convert/class_table.hpp>
#include <f4/world_convert/theater_data.hpp>  // PLT_RUNWAY, PLT_PARK
#include <f4/models/model_database.hpp>
#include <f4/data/aircraft_config.hpp>
#include <f4/data/config_loader.hpp>

#include <cstdlib>
#include <filesystem>
#include <string>

using namespace f4::simulation;
using namespace f4::entities;
using namespace f4::world;
using namespace f4::world_convert;

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
    // Departure altitude = threshold altitude + 2500 ft.
    EXPECT_DOUBLE_EQ(af->threshold_altitude_ft, 50.0);
    EXPECT_DOUBLE_EQ(af->departure_altitude_ft, 2550.0);
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
// spawn_aircraft_from_flights
// =============================================================================

TEST(CampaignBridge, SpawnFromFlightsEmptyWorldReturnsEmpty) {
    // No flights in the world → spawn returns empty. We don't need a valid
    // AircraftConfig for this case (the function returns before calling
    // FlightModelComponent::init()).
    EntityWorld world;
    ClassTable ct;
    f4::models::ModelDatabase db;
    f4::data::AircraftConfig cfg;  // empty is fine here
    ScenarioAirfield airfield;
    ScenarioAircraft tpl;
    tpl.vis_type_index = 1052;

    auto spawned = spawn_aircraft_from_flights(world, ct, db, cfg, airfield, tpl);
    EXPECT_TRUE(spawned.empty());
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
    f4::models::ModelDatabase db;

    ScenarioAirfield airfield;
    airfield.runway_heading_rad = 0.0;
    airfield.threshold_position = f4::geo::WorldPosition(0.0, 5000.0, 50.0);
    airfield.departure_altitude_ft = 2550.0;

    ScenarioAircraft tpl;
    tpl.vis_type_index = 1052;
    tpl.callsign = "EAGLE";
    tpl.aircraft_config_path = "f16.json";

    auto spawned = spawn_aircraft_from_flights(world, ct, db, cfg, airfield, tpl);
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
    f4::models::ModelDatabase db;

    ScenarioAirfield airfield;
    airfield.runway_heading_rad = 0.0;
    airfield.threshold_position = f4::geo::WorldPosition(0.0, 5000.0, 50.0);

    ScenarioAircraft tpl;
    tpl.vis_type_index = 1052;

    auto spawned = spawn_aircraft_from_flights(world, ct, db, cfg, airfield, tpl);
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
    f4::models::ModelDatabase db;

    ScenarioAirfield airfield;
    airfield.runway_heading_rad = 0.0;
    airfield.threshold_position = f4::geo::WorldPosition(0.0, 5000.0, 50.0);

    ScenarioAircraft tpl;
    tpl.vis_type_index = 1052;  // F-16 fallback

    auto spawned = spawn_aircraft_from_flights(world, ct, db, cfg, airfield, tpl);
    ASSERT_EQ(spawned.size(), 1u);

    // The spawned aircraft should be valid. We can't easily verify the
    // vis_type_index was used (ModelDatabase is also empty, so model_record
    // is null), but the entity should at least exist with a VisualModelComponent.
    EntityHandle h(spawned[0], &world);
    auto* vis = h.get<VisualModelComponent>();
    ASSERT_NE(vis, nullptr);
    EXPECT_EQ(vis->model_record, nullptr);  // db was empty
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
    f4::models::ModelDatabase db;

    ScenarioAirfield airfield;
    airfield.runway_heading_rad = 0.0;
    airfield.threshold_position = f4::geo::WorldPosition(500.0, 8000.0, 50.0);

    ScenarioAircraft tpl;
    tpl.vis_type_index = 1052;

    auto spawned = spawn_aircraft_from_flights(world, ct, db, cfg, airfield, tpl);
    ASSERT_EQ(spawned.size(), 1u);

    EntityHandle h(spawned[0], &world);
    auto* tf = h.get<TransformComponent>();
    ASSERT_NE(tf, nullptr);
    // The threshold is at (500, 8000). The per-flight offset is applied on
    // top: flight_index=0 → +80 ft east → (580, 8000).
    EXPECT_NEAR(tf->position.x, 580.0, 1e-6);
    EXPECT_NEAR(tf->position.y, 8000.0, 1e-6);
}
