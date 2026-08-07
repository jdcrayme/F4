// test_entity.cpp — EntityId, EntityWorld lifecycle, components, tags.

#include <gtest/gtest.h>
#include <algorithm>
#include <f4/entities/f4_entities.hpp>
#include <f4/geo/f4_geo.hpp>

using namespace f4::entities;
using namespace f4::geo;

// ============================================================================
// EntityId
// ============================================================================
TEST(EntityId, DefaultIsInvalid) {
    EntityId id;
    EXPECT_FALSE(id.valid());
    EXPECT_FALSE(static_cast<bool>(id));
}

TEST(EntityId, MakePacksIndexAndGeneration) {
    EntityId id = EntityId::make(7u, 3u);
    EXPECT_EQ(id.index(), 7u);
    EXPECT_EQ(id.generation(), 3u);
    EXPECT_TRUE(id.valid());
}

TEST(EntityId, ComparisonIsValueBased) {
    EXPECT_EQ(EntityId::make(1, 2), EntityId::make(1, 2));
    EXPECT_NE(EntityId::make(1, 2), EntityId::make(1, 3));
}

// ============================================================================
// EntityWorld lifecycle
// ============================================================================
TEST(EntityWorld, CreateProducesValidHandle) {
    EntityWorld w;
    EntityHandle h = w.create();
    EXPECT_TRUE(h.valid());
    EXPECT_EQ(w.size(), 1u);
}

TEST(EntityWorld, DestroyInvalidatesHandle) {
    EntityWorld w;
    EntityHandle h = w.create();
    EntityId id = h.id();
    w.destroy(id);
    EXPECT_FALSE(h.valid());
    EXPECT_FALSE(w.alive(id));
}

TEST(EntityWorld, DestroyedSlotGenerationBumps) {
    // A stale handle captured before destroy must not resolve to the reused
    // slot after create() recycles it.
    EntityWorld w;
    EntityId first = w.create().id();
    w.destroy(first);
    EntityId second = w.create().id();
    // Same slot index, but generation differs.
    EXPECT_EQ(second.index(), first.index());
    EXPECT_NE(second.generation(), first.generation());
    EXPECT_FALSE(w.alive(first));   // stale handle stays dead
    EXPECT_TRUE(w.alive(second));
}

// ============================================================================
// Components
// ============================================================================
TEST(Components, AddGetHasRemove) {
    EntityWorld w;
    EntityHandle h = w.create();
    EXPECT_FALSE(h.has<TransformComponent>());
    auto& tf = h.add<TransformComponent>();
    tf.position = WorldPosition{100.0, 200.0, 300.0};
    EXPECT_TRUE(h.has<TransformComponent>());
    EXPECT_EQ(h.get<TransformComponent>()->position.z, 300.0);
    h.remove<TransformComponent>();
    EXPECT_FALSE(h.has<TransformComponent>());
}

TEST(Components, WithComponentFindsOnlyEntitiesThatHaveIt) {
    EntityWorld w;
    EntityHandle a = w.create();
    [[maybe_unused]] EntityHandle b = w.create();
    EntityHandle c = w.create();
    a.add<TransformComponent>();
    c.add<TransformComponent>();
    // b has none.
    auto ids = w.with_component<TransformComponent>();
    ASSERT_EQ(ids.size(), 2u);
    EXPECT_NE(std::find(ids.begin(), ids.end(), a.id()), ids.end());
    EXPECT_NE(std::find(ids.begin(), ids.end(), c.id()), ids.end());
}

TEST(Components, RequireThrowsWhenMissing) {
    EntityWorld w;
    EntityHandle h = w.create();
    // (void) cast silences [[nodiscard]] on require() inside EXPECT_THROW.
    EXPECT_THROW((void)h.require<TransformComponent>(), std::runtime_error);
}

// ============================================================================
// Tags
// ============================================================================
TEST(Tags, SetGetHas) {
    EntityWorld w;
    EntityHandle h = w.create();
    h.set_tag(tags::ROLE, TagValue::from(std::string("fighter")));
    h.set_tag(tags::TEAM, TagValue::from(std::string("blue")));
    EXPECT_TRUE(h.has_tag(tags::ROLE));
    EXPECT_EQ(h.get_tag(tags::ROLE)->str_val, "fighter");
    EXPECT_FALSE(h.has_tag(tags::STEALTH));
}

TEST(Tags,WithTagFiltersByValue) {
    EntityWorld w;
    auto red1 = w.create(); red1.set_tag(tags::TEAM, TagValue::from(std::string("red")));
    auto blue1 = w.create(); blue1.set_tag(tags::TEAM, TagValue::from(std::string("blue")));
    auto red2 = w.create(); red2.set_tag(tags::TEAM, TagValue::from(std::string("red")));
    auto reds = w.with_tag(tags::TEAM, TagValue::from(std::string("red")));
    ASSERT_EQ(reds.size(), 2u);
}

TEST(Tags, DestroyedEntityDoesNotAppearInTagQueries) {
    EntityWorld w;
    EntityHandle h = w.create();
    h.set_tag(tags::TEAM, TagValue::from(std::string("red")));
    ASSERT_EQ(w.with_tag(tags::TEAM, TagValue::from(std::string("red"))).size(), 1u);
    w.destroy(h.id());
    EXPECT_EQ(w.with_tag(tags::TEAM, TagValue::from(std::string("red"))).size(), 0u);
}

// ============================================================================
// TransformComponent uses the strong-typed WorldPosition
// ============================================================================
TEST(TransformComponent, PositionIsStrongTypedWorldPosition) {
    // This is the design decision made concrete: the entity's source-of-truth
    // position is f4::geo::WorldPosition, NOT a raw (x,y,z). A future DIS
    // adapter converts via to_ecef(pos, datum); a radio call converts via
    // to_bra(own, target). The sim frame is always the stored form.
    EntityWorld w;
    EntityHandle h = w.create();
    auto& tf = h.add<TransformComponent>();
    tf.position = WorldPosition{5000.0, -3000.0, 20000.0};  // 5k east, 3k south, 20k up

    // Demonstrate the cross-frame conversion using the entity's position and
    // a theater datum — the kind of call a reporting/DIS system would make.
    TheaterDatum datum(LatLonAlt{38.0 * DEG_TO_RAD, -77.0 * DEG_TO_RAD, 0.0});
    LatLonAlt lla = to_lla(tf.position, datum);
    // 3000 ft south of a 38N origin -> latitude slightly less than 38 deg,
    // but still in the right neighborhood (not tens of miles away).
    EXPECT_LT(lla.lat, 38.0 * DEG_TO_RAD);
    EXPECT_GT(lla.lat, 37.9 * DEG_TO_RAD);
    // 5000 ft east of -77W origin -> longitude slightly more east (greater).
    EXPECT_GT(lla.lon, -77.0 * DEG_TO_RAD);
    // Altitude preserved across the frame crossing.
    EXPECT_NEAR(lla.alt, 20000.0, 1e-6);

    // BRA from the datum origin to this entity: slant range exceeds the
    // 20000 ft vertical component (there's horizontal offset too).
    BRA bra = to_bra(WorldPosition{0, 0, 0}, tf.position);
    EXPECT_GT(bra.range_ft, 20000.0);
}

// ============================================================================
// within_radius (linear scan over transforms)
// ============================================================================
TEST(WithinRadius, FindsEntitiesInsideAndExcludesOutside) {
    EntityWorld w;
    auto a = w.create(); a.add<TransformComponent>().position = WorldPosition{0, 0, 0};
    auto b = w.create(); b.add<TransformComponent>().position = WorldPosition{100, 0, 0};
    auto c = w.create(); c.add<TransformComponent>().position = WorldPosition{10000, 0, 0};
    auto ids = w.within_radius(0, 0, 0, 500.0);
    ASSERT_EQ(ids.size(), 2u);   // a and b are within 500 ft; c is not
}

// ============================================================================
// Phase 1: TeamComponent + narrowed CampaignIdentityComponent
// ============================================================================
TEST(TeamComponent, CanBeAddedAndQueried) {
    EntityWorld w;
    auto h = w.create();
    auto& tc = h.add<TeamComponent>();
    tc.slot = 2;
    tc.flags = 1;
    tc.colour = 3;
    tc.motto = "E Pluribus";
    tc.stance = {50, -50, 0, 0, 0, 0, 0, 0};
    tc.member = {1, 0, 1, 0, 0, 0, 0, 0};
    tc.air_experience = 80;
    tc.ground_experience = 60;
    tc.naval_experience = 40;
    tc.air_defense_experience = 70;

    EXPECT_TRUE(h.has<TeamComponent>());
    auto* got = h.get<TeamComponent>();
    ASSERT_NE(got, nullptr);
    EXPECT_EQ(got->slot, 2);
    EXPECT_EQ(got->motto, "E Pluribus");
    ASSERT_EQ(got->stance.size(), 8u);
    EXPECT_EQ(got->stance[0], 50);
    EXPECT_EQ(got->air_experience, 80);
}

TEST(CampaignIdentityComponent, IsNarrowedToTeamIdAndCallsign) {
    EntityWorld w;
    auto h = w.create();
    auto& cid = h.add<CampaignIdentityComponent>();
    cid.team_id = 3;
    cid.callsign = "Viper";
    // unit_type_name field no longer exists — the fact that this compiles
    // confirms the Phase 1 narrowing.

    EXPECT_TRUE(h.has<CampaignIdentityComponent>());
    auto* got = h.get<CampaignIdentityComponent>();
    ASSERT_NE(got, nullptr);
    EXPECT_EQ(got->team_id, 3);
    EXPECT_EQ(got->callsign, "Viper");
}

// ============================================================================
// Phase 2b: Objective components — compile and instantiate
// ============================================================================
TEST(ObjectiveComponents, ObjectiveTypeComponent) {
    EntityWorld w;
    auto h = w.create();
    auto& c = h.add<ObjectiveTypeComponent>();
    c.type = 100;
    c.class_table_index = 6;
    c.class_name = "02_20 Airbase 2";
    EXPECT_TRUE(h.has<ObjectiveTypeComponent>());
    EXPECT_EQ(h.get<ObjectiveTypeComponent>()->type, 100);
}

TEST(ObjectiveComponents, OwnershipComponent) {
    EntityWorld w;
    auto h = w.create();
    auto& c = h.add<OwnershipComponent>();
    c.team = 2;
    c.first_owner = 1;
    EXPECT_TRUE(h.has<OwnershipComponent>());
    EXPECT_EQ(h.get<OwnershipComponent>()->team, 2);
}

TEST(ObjectiveComponents, SupplyStateComponent) {
    EntityWorld w;
    auto h = w.create();
    auto& c = h.add<SupplyStateComponent>();
    c.supply = 80;
    c.fuel = 60;
    c.losses = 10;
    c.last_repair = 1000;
    EXPECT_TRUE(h.has<SupplyStateComponent>());
}

TEST(ObjectiveComponents, DamageBitmapComponent) {
    EntityWorld w;
    auto h = w.create();
    auto& c = h.add<DamageBitmapComponent>();
    c.fstatus = {0, 1, 2, 3};
    EXPECT_TRUE(h.has<DamageBitmapComponent>());
    EXPECT_EQ(h.get<DamageBitmapComponent>()->fstatus.size(), 4u);
}

TEST(ObjectiveComponents, RadarComponent) {
    EntityWorld w;
    auto h = w.create();
    auto& c = h.add<RadarComponent>();
    c.detect_ratio[0] = 0.8f;
    c.range_km = 245.5f;
    c.name = "Pat Hand SA-10";
    c.radar_type_idx = 18;
    EXPECT_TRUE(h.has<RadarComponent>());
    EXPECT_FLOAT_EQ(h.get<RadarComponent>()->range_km, 245.5f);
}

TEST(ObjectiveComponents, NetworkLinksComponent) {
    EntityWorld w;
    auto h = w.create();
    auto& c = h.add<NetworkLinksComponent>();
    ObjectiveLink link;
    link.neighbor_num = 12345;
    link.is_road = true;
    link.costs[1] = 25;
    c.links.push_back(link);
    EXPECT_TRUE(h.has<NetworkLinksComponent>());
    ASSERT_EQ(h.get<NetworkLinksComponent>()->links.size(), 1u);
    EXPECT_EQ(h.get<NetworkLinksComponent>()->links[0].neighbor_num, 12345u);
}

TEST(ObjectiveComponents, GroundLayoutComponent) {
    EntityWorld w;
    auto h = w.create();
    auto& c = h.add<GroundLayoutComponent>();
    GroundLayoutList gl;
    gl.type = 1;
    gl.heading_deg = 90.0f;
    c.layouts.push_back(gl);
    EXPECT_TRUE(h.has<GroundLayoutComponent>());
}

TEST(ObjectiveComponents, FeatureSetComponent) {
    EntityWorld w;
    auto h = w.create();
    auto& c = h.add<FeatureSetComponent>();
    c.features_count = 5;
    c.radar_feature = 2;
    FeatureEntryState fe;
    fe.name = "Control Tower";
    fe.hit_points = 500;
    c.features.push_back(fe);
    EXPECT_TRUE(h.has<FeatureSetComponent>());
    EXPECT_EQ(h.get<FeatureSetComponent>()->features[0].name, "Control Tower");
}

TEST(ObjectiveComponents, ObjectivePriorityComponent) {
    EntityWorld w;
    auto h = w.create();
    auto& c = h.add<ObjectivePriorityComponent>();
    c.priority = 5;
    c.nameid = 42;
    c.obj_flags = 0x1234;
    c.parent_id = 999;
    EXPECT_TRUE(h.has<ObjectivePriorityComponent>());
}

// ============================================================================
// Phase 2b: Unit components — compile and instantiate
// ============================================================================
TEST(UnitComponents, UnitCoreComponent) {
    EntityWorld w;
    auto h = w.create();
    auto& c = h.add<UnitCoreComponent>();
    c.unit_class = UnitClass::Battalion;
    c.domain = 3;
    c.unit_subtype = 1;
    c.class_table_index = 170;
    c.roster = 0xAAAAAAAA;
    c.class_name = "Armor Battalion";
    EXPECT_TRUE(h.has<UnitCoreComponent>());
    EXPECT_EQ(h.get<UnitCoreComponent>()->unit_class, UnitClass::Battalion);
}

TEST(UnitComponents, WaypointPlanComponent) {
    EntityWorld w;
    auto h = w.create();
    auto& c = h.add<WaypointPlanComponent>();
    WaypointState wp;
    wp.x = 500;
    wp.y = 300;
    wp.arrive = 3600;
    c.waypoints.push_back(wp);
    EXPECT_TRUE(h.has<WaypointPlanComponent>());
    ASSERT_EQ(h.get<WaypointPlanComponent>()->waypoints.size(), 1u);
}

TEST(UnitComponents, GroundTacticalComponent) {
    EntityWorld w;
    auto h = w.create();
    auto& c = h.add<GroundTacticalComponent>();
    c.supply = 80;
    c.morale = 70;
    c.fatigue = 30;
    c.heading = 128;
    c.last_move = 5000;
    EXPECT_TRUE(h.has<GroundTacticalComponent>());
}

TEST(UnitComponents, HierarchyComponent) {
    EntityWorld w;
    auto h = w.create();
    auto& c = h.add<HierarchyComponent>();
    // The raw VU_ID fields (parent_id, element_ids) were removed;
    // only the resolved EntityId fields remain.
    EntityId fake_parent = EntityId::make(42, 1);
    EntityId fake_child = EntityId::make(10, 1);
    c.parent = fake_parent;
    c.children.push_back(fake_child);
    EXPECT_TRUE(h.has<HierarchyComponent>());
    EXPECT_EQ(h.get<HierarchyComponent>()->parent, fake_parent);
    ASSERT_EQ(h.get<HierarchyComponent>()->children.size(), 1u);
    EXPECT_EQ(h.get<HierarchyComponent>()->children[0], fake_child);
}

TEST(UnitComponents, SquadronComponent) {
    EntityWorld w;
    auto h = w.create();
    auto& c = h.add<SquadronComponent>();
    // airbase_id (raw VU_ID) was removed; use the resolved EntityId field.
    EntityId fake_airbase = EntityId::make(555, 1);
    c.airbase = fake_airbase;
    c.specialty = 2;
    c.fuel = 10000;
    c.aa_kills = 5;
    PilotState p;
    p.pilot_id = 1;
    p.skill = 80;
    p.aa_kills = 3;
    c.pilots.push_back(p);
    EXPECT_TRUE(h.has<SquadronComponent>());
    EXPECT_EQ(h.get<SquadronComponent>()->airbase, fake_airbase);
    ASSERT_EQ(h.get<SquadronComponent>()->pilots.size(), 1u);
}

TEST(UnitComponents, FlightPlanComponent) {
    EntityWorld w;
    auto h = w.create();
    auto& c = h.add<FlightPlanComponent>();
    c.altitude = 25000.0f;
    c.fuel_burnt = 5000;
    c.mission = 3;
    // package_id / squadron_id (raw VU_IDs) were removed;
    // use the resolved EntityId fields.
    EntityId fake_pkg = EntityId::make(999, 1);
    EntityId fake_sqn = EntityId::make(888, 1);
    c.package = fake_pkg;
    c.squadron = fake_sqn;
    EXPECT_TRUE(h.has<FlightPlanComponent>());
    EXPECT_FLOAT_EQ(h.get<FlightPlanComponent>()->altitude, 25000.0f);
    EXPECT_EQ(h.get<FlightPlanComponent>()->package, fake_pkg);
    EXPECT_EQ(h.get<FlightPlanComponent>()->squadron, fake_sqn);
}

TEST(UnitComponents, PackageSupportComponent) {
    EntityWorld w;
    auto h = w.create();
    auto& c = h.add<PackageSupportComponent>();
    c.wait_cycles = 3;
    // Raw VU_ID fields (interceptor_id, awacs_id, etc.) were removed;
    // use the resolved EntityId fields.
    EntityId fake_int = EntityId::make(111, 1);
    EntityId fake_awacs = EntityId::make(222, 1);
    EntityId fake_tanker = EntityId::make(555, 1);
    c.interceptor = fake_int;
    c.awacs = fake_awacs;
    c.tanker = fake_tanker;
    EXPECT_TRUE(h.has<PackageSupportComponent>());
    EXPECT_EQ(h.get<PackageSupportComponent>()->interceptor, fake_int);
    EXPECT_EQ(h.get<PackageSupportComponent>()->awacs, fake_awacs);
    EXPECT_EQ(h.get<PackageSupportComponent>()->tanker, fake_tanker);
}

TEST(UnitComponents, VehicleCompositionComponent) {
    EntityWorld w;
    auto h = w.create();
    auto& c = h.add<VehicleCompositionComponent>();
    VehicleGroup vg;
    vg.group = 0;
    vg.vehicle_name = "M-1A1";
    vg.count = 3;
    c.groups.push_back(vg);
    EXPECT_TRUE(h.has<VehicleCompositionComponent>());
    EXPECT_EQ(h.get<VehicleCompositionComponent>()->groups[0].vehicle_name, "M-1A1");
}

TEST(UnitComponents, UnitClassScoreComponent) {
    EntityWorld w;
    auto h = w.create();
    auto& c = h.add<UnitClassScoreComponent>();
    c.scores[0] = 10;
    c.scores[15] = 160;
    EXPECT_TRUE(h.has<UnitClassScoreComponent>());
    EXPECT_EQ(h.get<UnitClassScoreComponent>()->scores[0], 10);
}

TEST(UnitComponents, MovementOrdersComponent) {
    EntityWorld w;
    auto h = w.create();
    auto& c = h.add<MovementOrdersComponent>();
    c.dest_x = 500;
    c.dest_y = -300;
    c.movement_type = 4;
    c.movement_speed = 30;
    c.max_range = 250;
    c.movement_type_name = "Wheeled";
    EXPECT_TRUE(h.has<MovementOrdersComponent>());
    auto* got = h.get<MovementOrdersComponent>();
    EXPECT_EQ(got->dest_x, 500);
    EXPECT_EQ(got->dest_y, -300);
    EXPECT_EQ(got->movement_type, 4);
    EXPECT_EQ(got->movement_speed, 30);
    EXPECT_EQ(got->max_range, 250);
    EXPECT_EQ(got->movement_type_name, "Wheeled");
}

// ============================================================================
// Phase 2b: Utility components — compile and instantiate
// ============================================================================
TEST(UtilityComponents, PropertyBag) {
    EntityWorld w;
    auto h = w.create();
    auto& c = h.add<PropertyBag>();
    c.ints["vu_id_creator"] = 42;
    c.floats["some_ratio"] = 0.75;
    c.strings["notes"] = "reverse-engineered";
    EXPECT_TRUE(h.has<PropertyBag>());
    EXPECT_EQ(h.get<PropertyBag>()->ints["vu_id_creator"], 42);
}

TEST(UtilityComponents, CampaignStateComponent) {
    EntityWorld w;
    auto h = w.create();
    auto& c = h.add<CampaignStateComponent>();
    c.current_time = 1000;
    c.te_victory_points = 42;
    c.te_flags = 7;
    c.te_number_aircraft = {1,2,3,4,5,6,7,8};
    c.te_team_pts = {10,20,30,40,50,60,70,80};
    EXPECT_TRUE(h.has<CampaignStateComponent>());
    EXPECT_EQ(h.get<CampaignStateComponent>()->current_time, 1000);
}

// ============================================================================
// Phase 2b: Conditional component pattern — the key ECS design
// ============================================================================
TEST(ConditionalComponents, ObjectiveWithAndWithoutRadar) {
    EntityWorld w;

    // Airbase with radar — gets RadarComponent.
    auto with_radar = w.create();
    with_radar.add<TransformComponent>();
    with_radar.add<ObjectiveTypeComponent>();
    with_radar.add<OwnershipComponent>();
    with_radar.add<RadarComponent>();
    EXPECT_TRUE(with_radar.has<RadarComponent>());

    // Bridge without radar — no RadarComponent.
    auto no_radar = w.create();
    no_radar.add<TransformComponent>();
    no_radar.add<ObjectiveTypeComponent>();
    no_radar.add<OwnershipComponent>();
    EXPECT_FALSE(no_radar.has<RadarComponent>());

    // Systems query only entities that have radar:
    auto radar_entities = w.with_component<RadarComponent>();
    EXPECT_EQ(radar_entities.size(), 1u);
}

TEST(ConditionalComponents, UnitSubclassDispatchViaComponents) {
    EntityWorld w;

    // Battalion — gets GroundTacticalComponent + HierarchyComponent.
    auto battalion = w.create();
    battalion.add<UnitCoreComponent>().unit_class = UnitClass::Battalion;
    battalion.add<GroundTacticalComponent>();
    battalion.add<HierarchyComponent>();
    EXPECT_TRUE(battalion.has<GroundTacticalComponent>());
    EXPECT_TRUE(battalion.has<HierarchyComponent>());

    // Flight — gets FlightPlanComponent, NOT GroundTacticalComponent.
    auto flight = w.create();
    flight.add<UnitCoreComponent>().unit_class = UnitClass::Flight;
    flight.add<FlightPlanComponent>();
    EXPECT_TRUE(flight.has<FlightPlanComponent>());
    EXPECT_FALSE(flight.has<GroundTacticalComponent>());

    // Systems query by component type, not by if/else on unit_class:
    auto ground_units = w.with_component<GroundTacticalComponent>();
    auto air_units = w.with_component<FlightPlanComponent>();
    EXPECT_EQ(ground_units.size(), 1u);
    EXPECT_EQ(air_units.size(), 1u);
}
