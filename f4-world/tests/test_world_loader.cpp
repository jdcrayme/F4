// test_world_loader.cpp — populate f4-entities from WorldState.
//
// Phase 1: populate_teams tests.
// Phase 3: populate_objectives, populate_units, populate_campaign, populate_world tests.

#include <gtest/gtest.h>
#include <f4/world/f4_world.hpp>
#include <f4/world/detail/world_state.hpp>
#include <f4/entities/f4_entities.hpp>

#include <algorithm>
#include <filesystem>

using namespace f4::world;
using namespace f4::entities;

namespace {
WorldState make_test_world() {
    WorldState ws;
    ws.version = 63;
    ws.teams = {
        {0, 0, 0, "", ""},
        {1, 1, 1, "U.S.",  "E Pluribus"},
        {2, 2, 2, "ROK",   ""},
        {3, 3, 3, "Japan", ""},
    };
    return ws;
}

// Build a small WorldState with 3 objectives for bridge testing.
WorldState make_objective_world() {
    WorldState ws;
    ws.version = 63;

    // Objective 1: airbase with radar, supply, links, ground layout, features
    ObjectiveState o1;
    o1.type = 100; o1.entity_type = 100; o1.x = 500; o1.y = 400; o1.z = 100.0f;
    o1.owner = 2; o1.first_owner = 2; o1.priority = 5; o1.nameid = 10;
    o1.id_creator = 1; o1.id_num = 1001; o1.camp_id = 50;
    o1.obj_flags = 0x00FF; o1.parent_id = 0;
    o1.objective_type = 1;  // TYPE_AIRBASE
    o1.supply = 80; o1.fuel = 60; o1.losses = 10; o1.last_repair = 5000;
    o1.has_radar = true;
    for (int i = 0; i < 8; ++i) o1.detect_ratio[i] = 0.1f * i;
    o1.radar_range_km = 245.5f; o1.radar_name = "APG-68"; o1.radar_type_idx = 18;
    o1.class_name = "02_20 Airbase 2";
    ObjectiveLink link1; link1.neighbor_num = 1002; link1.is_road = true; link1.costs[1] = 25;
    o1.links.push_back(link1);
    GroundLayoutList gl1; gl1.type = 1; gl1.heading_deg = 90.0f;
    o1.ground_layout.push_back(gl1);
    o1.features_count = 3; o1.radar_feature = 2;
    FeatureEntryState fe1; fe1.name = "Control Tower"; fe1.hit_points = 500;
    o1.features.push_back(fe1);

    // Objective 2: bridge — no radar, minimal data
    ObjectiveState o2;
    o2.type = 200; o2.entity_type = 200; o2.x = 600; o2.y = 500; o2.z = 0.0f;
    o2.owner = 1; o2.first_owner = 1; o2.priority = 3; o2.nameid = 20;
    o2.id_creator = 1; o2.id_num = 1002; o2.camp_id = 51;
    o2.objective_type = 6;  // TYPE_BRIDGE
    o2.class_name = "Highway Bridge";

    // Objective 3: city with supply but no radar, with fstatus
    ObjectiveState o3;
    o3.type = 300; o3.entity_type = 300; o3.x = 700; o3.y = 600; o3.z = 50.0f;
    o3.owner = 2; o3.first_owner = 1; o3.priority = 7; o3.nameid = 30;
    o3.id_creator = 2; o3.id_num = 1003; o3.camp_id = 52;
    o3.objective_type = 8;  // TYPE_CITY
    o3.supply = 40; o3.fuel = 30; o3.losses = 5; o3.last_repair = 3000;
    o3.fstatus = {0, 1, 2, 3};
    o3.class_name = "Seoul";

    ws.objectives = {o1, o2, o3};
    return ws;
}

// Build a small WorldState with varied unit types for bridge testing.
WorldState make_unit_world() {
    WorldState ws;
    ws.version = 63;

    // Battalion with hierarchy
    UnitState u1;
    u1.type = 170; u1.unit_class = UnitClass::Battalion; u1.domain = 3;
    u1.unit_subtype = 1; u1.entity_type = 170; u1.x = 400; u1.y = 300; u1.z = 0.0f;
    u1.owner = 2; u1.id_creator = 1; u1.id_num = 2001; u1.camp_id = 60;
    u1.roster = 0xAAAAAAAA; u1.class_name = "Armor Battalion";
    u1.supply = 80; u1.morale = 70; u1.fatigue = 30;
    u1.heading = 128; u1.final_heading = 130; u1.position = 2;
    u1.last_move = 5000; u1.last_combat = 4000;
    u1.parent_id = 3001;  // parent brigade
    u1.vehicle_groups = {VehicleGroup{0, 100, 3, 3, "M-1A1", "MBT", 500, 60}};
    for (auto& s : u1.unit_class_scores) s = 0;
    u1.unit_class_scores[0] = 10; u1.unit_class_scores[3] = 80;

    // Brigade with children
    UnitState u2;
    u2.type = 180; u2.unit_class = UnitClass::Brigade; u2.domain = 3;
    u2.unit_subtype = 2; u2.entity_type = 180; u2.x = 410; u2.y = 310; u2.z = 0.0f;
    u2.owner = 2; u2.id_creator = 1; u2.id_num = 3001; u2.camp_id = 61;
    u2.class_name = "Armor Brigade";
    u2.supply = 90; u2.morale = 80;
    u2.element_ids = {2001};  // child battalion

    // Squadron
    UnitState u3;
    u3.type = 500; u3.unit_class = UnitClass::Squadron; u3.domain = 2;
    u3.unit_subtype = 3; u3.entity_type = 500; u3.x = 500; u3.y = 400; u3.z = 0.0f;
    u3.owner = 2; u3.id_creator = 1; u3.id_num = 4001; u3.camp_id = 70;
    u3.class_name = "18th Fighter Squadron";
    u3.airbase_id = 1001;  // references objective o1
    u3.specialty = 2; u3.fuel = 10000; u3.aa_kills = 5;
    PilotState p; p.pilot_id = 1; p.skill = 80; p.aa_kills = 3;
    u3.pilots.push_back(p);

    // Flight
    UnitState u4;
    u4.type = 600; u4.unit_class = UnitClass::Flight; u4.domain = 2;
    u4.unit_subtype = 4; u4.entity_type = 600; u4.x = 510; u4.y = 410; u4.z = 25000.0f;
    u4.owner = 2; u4.id_creator = 1; u4.id_num = 5001; u4.camp_id = 80;
    u4.class_name = "Strike Flight";
    u4.flight_altitude = 25000.0f; u4.fuel_burnt = 5000;
    u4.mission = 3; u4.package_id = 6001; u4.squadron_id = 4001;
    WaypointState wp; wp.x = 520; wp.y = 420; wp.arrive = 3600;
    u4.waypoints.push_back(wp);

    // Package
    UnitState u5;
    u5.type = 700; u5.unit_class = UnitClass::Package; u5.domain = 2;
    u5.unit_subtype = 5; u5.entity_type = 700; u5.x = 505; u5.y = 405; u5.z = 0.0f;
    u5.owner = 2; u5.id_creator = 1; u5.id_num = 6001; u5.camp_id = 90;
    u5.class_name = "Strike Package";
    u5.wait_cycles = 3; u5.interceptor_id = 0; u5.awacs_id = 0;

    ws.units = {u1, u2, u3, u4, u5};
    return ws;
}
}

// ============================================================================
// Phase 1: populate_teams — existing tests
// ============================================================================

TEST(WorldLoader, CreatesEntityPerNonEmptyTeam) {
    EntityWorld ew;
    WorldState ws = make_test_world();
    auto ids = populate_teams(ew, ws);
    EXPECT_EQ(ids.size(), 3u);
    EXPECT_EQ(ew.size(), 3u);
}

TEST(WorldLoader, TeamEntitiesHaveCorrectTagsAndIdentity) {
    EntityWorld ew;
    WorldState ws = make_test_world();
    auto ids = populate_teams(ew, ws);

    auto rok_ids = ew.with_tag(tags::TEAM, TagValue::from(std::string("ROK")));
    ASSERT_EQ(rok_ids.size(), 1u);
    EntityHandle h(rok_ids[0], &ew);
    ASSERT_TRUE(h.valid());

    auto* cid = h.get<CampaignIdentityComponent>();
    ASSERT_NE(cid, nullptr);
    EXPECT_EQ(cid->team_id, 2);
    EXPECT_EQ(cid->callsign, "ROK");

    auto* tc = h.get<TeamComponent>();
    ASSERT_NE(tc, nullptr);
    EXPECT_EQ(tc->slot, 2);
    EXPECT_EQ(tc->flags, 2);
    EXPECT_EQ(tc->colour, 2);

    EXPECT_TRUE(h.has_tag(tags::ALIVE));
    EXPECT_TRUE(h.has_tag(tags::ROLE));
    EXPECT_EQ(*h.get_tag(tags::ROLE)->as_string(), "team");

    // Phase A: NAME tag — promotes the team callsign so consumers can
    // display/filter team names without querying CampaignIdentityComponent.
    ASSERT_TRUE(h.has_tag(tags::NAME));
    EXPECT_EQ(*h.get_tag(tags::NAME)->as_string(), "ROK");
}

TEST(WorldLoader, TeamComponentCarriesTeaEnrichment) {
    EntityWorld ew;
    WorldState ws;
    ws.teams = {
        {1, 1, 1, "U.S.", "E Pluribus"},
    };
    ws.teams[0].tea_loaded = true;
    ws.teams[0].stance = {50, -50, 0, 0, 0, 0, 0, 0};
    ws.teams[0].member = {1, 0, 1, 0, 0, 0, 0, 0};
    ws.teams[0].air_experience = 80;
    ws.teams[0].ground_experience = 60;

    auto ids = populate_teams(ew, ws);
    ASSERT_EQ(ids.size(), 1u);
    EntityHandle h(ids[0], &ew);

    auto* tc = h.get<TeamComponent>();
    ASSERT_NE(tc, nullptr);
    EXPECT_EQ(tc->motto, "E Pluribus");
    ASSERT_EQ(tc->stance.size(), 8u);
    EXPECT_EQ(tc->stance[0], 50);
    EXPECT_EQ(tc->stance[1], -50);
    ASSERT_EQ(tc->member.size(), 8u);
    EXPECT_EQ(tc->member[0], 1);
    EXPECT_EQ(tc->air_experience, 80);
    EXPECT_EQ(tc->ground_experience, 60);
}

TEST(WorldLoader, EmptyNameSlotsAreSkipped) {
    EntityWorld ew;
    WorldState ws;
    ws.teams = {
        {0, 0, 0, "", ""},
        {1, 0, 0, "Alpha", ""},
        {2, 0, 0, "", ""},
        {3, 0, 0, "Bravo", ""},
    };
    auto ids = populate_teams(ew, ws);
    EXPECT_EQ(ids.size(), 2u);
}

TEST(WorldLoader, CanQueryTeamsByTeamTag) {
    EntityWorld ew;
    WorldState ws = make_test_world();
    populate_teams(ew, ws);

    auto us = ew.with_tag(tags::TEAM, TagValue::from(std::string("U.S.")));
    auto rok = ew.with_tag(tags::TEAM, TagValue::from(std::string("ROK")));
    auto japan = ew.with_tag(tags::TEAM, TagValue::from(std::string("Japan")));
    auto none = ew.with_tag(tags::TEAM, TagValue::from(std::string("Nonexistent")));
    EXPECT_EQ(us.size(), 1u);
    EXPECT_EQ(rok.size(), 1u);
    EXPECT_EQ(japan.size(), 1u);
    EXPECT_EQ(none.size(), 0u);
}

TEST(WorldLoader, CampaignIdentityNoLongerHasUnitTypeName) {
    EntityWorld ew;
    WorldState ws = make_test_world();
    populate_teams(ew, ws);

    auto us_ids = ew.with_tag(tags::TEAM, TagValue::from(std::string("U.S.")));
    ASSERT_EQ(us_ids.size(), 1u);
    EntityHandle h(us_ids[0], &ew);

    auto* cid = h.get<CampaignIdentityComponent>();
    ASSERT_NE(cid, nullptr);
    EXPECT_EQ(cid->team_id, 1);
    EXPECT_EQ(cid->callsign, "U.S.");
}

// ============================================================================
// Phase 3: populate_campaign
// ============================================================================

TEST(PopulateCampaign, CreatesSingleEntityWithCampaignState) {
    EntityWorld ew;
    WorldState ws;
    ws.campaign.current_time = 1000;
    ws.campaign.te_victory_points = 42;
    ws.campaign.te_flags = 7;
    ws.campaign.te_number_aircraft = {1,2,3,4,5,6,7,8};
    ws.campaign.te_team_pts = {10,20,30,40,50,60,70,80};

    auto id = populate_campaign(ew, ws);
    EXPECT_TRUE(id.valid());
    EXPECT_EQ(ew.size(), 1u);

    EntityHandle h(id, &ew);
    EXPECT_TRUE(h.has<CampaignStateComponent>());

    auto* cs = h.get<CampaignStateComponent>();
    ASSERT_NE(cs, nullptr);
    EXPECT_EQ(cs->current_time, 1000);
    EXPECT_EQ(cs->te_victory_points, 42);
    EXPECT_EQ(cs->te_flags, 7);
    ASSERT_EQ(cs->te_number_aircraft.size(), 8u);
    EXPECT_EQ(cs->te_number_aircraft[3], 4);
}

TEST(PopulateCampaign, HasCorrectTags) {
    EntityWorld ew;
    WorldState ws;
    auto id = populate_campaign(ew, ws);

    EntityHandle h(id, &ew);
    EXPECT_TRUE(h.has_tag(tags::ROLE));
    EXPECT_EQ(*h.get_tag(tags::ROLE)->as_string(), "campaign");
    EXPECT_TRUE(h.has_tag(tags::ALIVE));
    // Phase A: NAME tag — campaign is a singleton, so a stable literal.
    ASSERT_TRUE(h.has_tag(tags::NAME));
    EXPECT_EQ(*h.get_tag(tags::NAME)->as_string(), "Campaign");
}

// ============================================================================
// Phase 3: populate_objectives
// ============================================================================

TEST(PopulateObjectives, Count) {
    EntityWorld ew;
    WorldState ws = make_objective_world();
    std::unordered_map<uint32_t, EntityId> obj_id_map;
    auto ids = populate_objectives(ew, ws, obj_id_map);
    EXPECT_EQ(ids.size(), 3u);
}

TEST(PopulateObjectives, AllHaveTransform) {
    EntityWorld ew;
    WorldState ws = make_objective_world();
    std::unordered_map<uint32_t, EntityId> obj_id_map;
    auto ids = populate_objectives(ew, ws, obj_id_map);

    // Every objective entity must have a TransformComponent with correct
    // grid→feet position.
    for (size_t i = 0; i < ids.size(); ++i) {
        EntityHandle h(ids[i], &ew);
        ASSERT_TRUE(h.has<TransformComponent>());
        auto* tf = h.get<TransformComponent>();
        // Verify grid→feet conversion: 1 grid = 1024 ft, z already in ft.
        EXPECT_DOUBLE_EQ(tf->position.x, static_cast<double>(ws.objectives[i].x) * 1024.0);
        EXPECT_DOUBLE_EQ(tf->position.y, static_cast<double>(ws.objectives[i].y) * 1024.0);
        EXPECT_DOUBLE_EQ(tf->position.z, static_cast<double>(ws.objectives[i].z));
    }
}

TEST(PopulateObjectives, AlwaysPresentComponents) {
    EntityWorld ew;
    WorldState ws = make_objective_world();
    std::unordered_map<uint32_t, EntityId> obj_id_map;
    auto ids = populate_objectives(ew, ws, obj_id_map);

    // Every objective must have ObjectiveTypeComponent, OwnershipComponent,
    // ObjectivePriorityComponent, and PropertyBag.
    for (auto id : ids) {
        EntityHandle h(id, &ew);
        EXPECT_TRUE(h.has<ObjectiveTypeComponent>());
        EXPECT_TRUE(h.has<OwnershipComponent>());
        EXPECT_TRUE(h.has<ObjectivePriorityComponent>());
        EXPECT_TRUE(h.has<PropertyBag>());
    }
}

TEST(PopulateObjectives, ConditionalRadar) {
    EntityWorld ew;
    WorldState ws = make_objective_world();
    std::unordered_map<uint32_t, EntityId> obj_id_map;
    auto ids = populate_objectives(ew, ws, obj_id_map);

    // o1 has radar, o2 and o3 don't.
    EntityHandle h1(ids[0], &ew);
    EXPECT_TRUE(h1.has<RadarComponent>());
    auto* rad = h1.get<RadarComponent>();
    EXPECT_FLOAT_EQ(rad->range_km, 245.5f);
    EXPECT_EQ(rad->name, "APG-68");
    EXPECT_EQ(rad->radar_type_idx, 18);

    EntityHandle h2(ids[1], &ew);
    EXPECT_FALSE(h2.has<RadarComponent>());

    EntityHandle h3(ids[2], &ew);
    EXPECT_FALSE(h3.has<RadarComponent>());
}

TEST(PopulateObjectives, ConditionalSupply) {
    EntityWorld ew;
    WorldState ws = make_objective_world();
    std::unordered_map<uint32_t, EntityId> obj_id_map;
    auto ids = populate_objectives(ew, ws, obj_id_map);

    // o1 and o3 have supply, o2 doesn't.
    EntityHandle h1(ids[0], &ew);
    EXPECT_TRUE(h1.has<SupplyStateComponent>());
    auto* sup = h1.get<SupplyStateComponent>();
    EXPECT_EQ(sup->supply, 80);
    EXPECT_EQ(sup->fuel, 60);

    EntityHandle h2(ids[1], &ew);
    EXPECT_FALSE(h2.has<SupplyStateComponent>());

    EntityHandle h3(ids[2], &ew);
    EXPECT_TRUE(h3.has<SupplyStateComponent>());
}

TEST(PopulateObjectives, ConditionalDamageBitmap) {
    EntityWorld ew;
    WorldState ws = make_objective_world();
    std::unordered_map<uint32_t, EntityId> obj_id_map;
    auto ids = populate_objectives(ew, ws, obj_id_map);

    // o3 has fstatus, o1 and o2 don't.
    EntityHandle h3(ids[2], &ew);
    EXPECT_TRUE(h3.has<DamageBitmapComponent>());
    EXPECT_EQ(h3.get<DamageBitmapComponent>()->fstatus.size(), 4u);

    EntityHandle h1(ids[0], &ew);
    EXPECT_FALSE(h1.has<DamageBitmapComponent>());
}

TEST(PopulateObjectives, NetworkLinksRoundTrip) {
    EntityWorld ew;
    WorldState ws = make_objective_world();
    std::unordered_map<uint32_t, EntityId> obj_id_map;
    auto ids = populate_objectives(ew, ws, obj_id_map);

    // o1 has links, o2 and o3 don't.
    EntityHandle h1(ids[0], &ew);
    EXPECT_TRUE(h1.has<NetworkLinksComponent>());
    auto* nl = h1.get<NetworkLinksComponent>();
    ASSERT_EQ(nl->links.size(), 1u);
    EXPECT_EQ(nl->links[0].neighbor_num, 1002u);
    EXPECT_TRUE(nl->links[0].is_road);
    EXPECT_EQ(nl->links[0].costs[1], 25);

    EntityHandle h2(ids[1], &ew);
    EXPECT_FALSE(h2.has<NetworkLinksComponent>());
}

TEST(PopulateObjectives, GroundLayoutAndFeatures) {
    EntityWorld ew;
    WorldState ws = make_objective_world();
    std::unordered_map<uint32_t, EntityId> obj_id_map;
    auto ids = populate_objectives(ew, ws, obj_id_map);

    // o1 has ground layout and features.
    EntityHandle h1(ids[0], &ew);
    EXPECT_TRUE(h1.has<GroundLayoutComponent>());
    ASSERT_EQ(h1.get<GroundLayoutComponent>()->layouts.size(), 1u);
    EXPECT_FLOAT_EQ(h1.get<GroundLayoutComponent>()->layouts[0].heading_deg, 90.0f);

    EXPECT_TRUE(h1.has<FeatureSetComponent>());
    auto* fs = h1.get<FeatureSetComponent>();
    EXPECT_EQ(fs->features_count, 3);
    EXPECT_EQ(fs->radar_feature, 2);
    ASSERT_EQ(fs->features.size(), 1u);
    EXPECT_EQ(fs->features[0].name, "Control Tower");

    // o2 doesn't have ground layout or features.
    EntityHandle h2(ids[1], &ew);
    EXPECT_FALSE(h2.has<GroundLayoutComponent>());
    EXPECT_FALSE(h2.has<FeatureSetComponent>());
}

TEST(PopulateObjectives, PropertyBagCarriesFormatResidue) {
    EntityWorld ew;
    WorldState ws = make_objective_world();
    std::unordered_map<uint32_t, EntityId> obj_id_map;
    auto ids = populate_objectives(ew, ws, obj_id_map);

    EntityHandle h1(ids[0], &ew);
    auto* pb = h1.get<PropertyBag>();
    ASSERT_NE(pb, nullptr);
    EXPECT_EQ(pb->ints.at("vu_id_creator"), 1);
    EXPECT_EQ(pb->ints.at("vu_id_num"), 1001);
    EXPECT_EQ(pb->ints.at("entity_type"), 100);
    EXPECT_EQ(pb->ints.at("camp_id"), 50);
}

TEST(PopulateObjectives, IdMapBuilt) {
    EntityWorld ew;
    WorldState ws = make_objective_world();
    std::unordered_map<uint32_t, EntityId> obj_id_map;
    auto ids = populate_objectives(ew, ws, obj_id_map);

    // All three objectives have non-zero id_num, so they should be in the map.
    EXPECT_EQ(obj_id_map.size(), 3u);
    EXPECT_TRUE(obj_id_map.count(1001));
    EXPECT_TRUE(obj_id_map.count(1002));
    EXPECT_TRUE(obj_id_map.count(1003));
    EXPECT_EQ(obj_id_map.at(1001), ids[0]);
}

TEST(PopulateObjectives, TagsAreCorrect) {
    EntityWorld ew;
    WorldState ws = make_objective_world();
    std::unordered_map<uint32_t, EntityId> obj_id_map;
    auto ids = populate_objectives(ew, ws, obj_id_map);

    EntityHandle h1(ids[0], &ew);
    EXPECT_TRUE(h1.has_tag(tags::ROLE));
    EXPECT_EQ(*h1.get_tag(tags::ROLE)->as_string(), "objective");
    EXPECT_TRUE(h1.has_tag(tags::ALIVE));
    // Owner 2 → team tag is int 2
    EXPECT_EQ(*h1.get_tag(tags::TEAM)->as_int(), 2);
}

// Phase A: NAME / CLASS / ICON tag enrichment for objectives.
TEST(PopulateObjectives, PhaseA_NameClassIconTags) {
    EntityWorld ew;
    WorldState ws = make_objective_world();
    std::unordered_map<uint32_t, EntityId> obj_id_map;
    auto ids = populate_objectives(ew, ws, obj_id_map);

    // o1: "02_20 Airbase 2", objective_type=1 (AIRBASE)
    EntityHandle h1(ids[0], &ew);
    ASSERT_TRUE(h1.has_tag(tags::NAME));
    EXPECT_EQ(*h1.get_tag(tags::NAME)->as_string(), "02_20 Airbase 2");
    ASSERT_TRUE(h1.has_tag(tags::CLASS));
    EXPECT_EQ(*h1.get_tag(tags::CLASS)->as_int(), 1);
    ASSERT_TRUE(h1.has_tag(tags::ICON));
    EXPECT_EQ(*h1.get_tag(tags::ICON)->as_int(), 1);

    // o2: "Highway Bridge", objective_type=6 (BRIDGE)
    EntityHandle h2(ids[1], &ew);
    ASSERT_TRUE(h2.has_tag(tags::NAME));
    EXPECT_EQ(*h2.get_tag(tags::NAME)->as_string(), "Highway Bridge");
    EXPECT_EQ(*h2.get_tag(tags::CLASS)->as_int(), 6);
    EXPECT_EQ(*h2.get_tag(tags::ICON)->as_int(), 6);

    // o3: "Seoul", objective_type=8 (CITY)
    EntityHandle h3(ids[2], &ew);
    ASSERT_TRUE(h3.has_tag(tags::NAME));
    EXPECT_EQ(*h3.get_tag(tags::NAME)->as_string(), "Seoul");
    EXPECT_EQ(*h3.get_tag(tags::CLASS)->as_int(), 8);
    EXPECT_EQ(*h3.get_tag(tags::ICON)->as_int(), 8);
}

// ============================================================================
// Phase 3: populate_units
// ============================================================================

TEST(PopulateUnits, Count) {
    EntityWorld ew;
    WorldState ws = make_unit_world();
    // Need an objective id map for Squadron→airbase resolution.
    std::unordered_map<uint32_t, EntityId> obj_id_map;
    // Pre-populate with the airbase objective (id_num=1001).
    auto obj_h = ew.create();
    obj_id_map[1001] = obj_h.id();

    std::unordered_map<uint32_t, EntityId> unit_id_map;
    auto ids = populate_units(ew, ws, obj_id_map, unit_id_map);
    EXPECT_EQ(ids.size(), 5u);
}

TEST(PopulateUnits, AllHaveTransformAndCoreComponent) {
    EntityWorld ew;
    WorldState ws = make_unit_world();
    std::unordered_map<uint32_t, EntityId> obj_id_map;
    std::unordered_map<uint32_t, EntityId> unit_id_map;
    auto ids = populate_units(ew, ws, obj_id_map, unit_id_map);

    for (size_t i = 0; i < ids.size(); ++i) {
        EntityHandle h(ids[i], &ew);
        ASSERT_TRUE(h.has<TransformComponent>());
        ASSERT_TRUE(h.has<UnitCoreComponent>());
        ASSERT_TRUE(h.has<PropertyBag>());

        // Verify grid→feet position
        auto* tf = h.get<TransformComponent>();
        EXPECT_DOUBLE_EQ(tf->position.x, static_cast<double>(ws.units[i].x) * 1024.0);
        EXPECT_DOUBLE_EQ(tf->position.y, static_cast<double>(ws.units[i].y) * 1024.0);
        EXPECT_DOUBLE_EQ(tf->position.z, static_cast<double>(ws.units[i].z));
    }
}

TEST(PopulateUnits, SubclassComponents) {
    EntityWorld ew;
    WorldState ws = make_unit_world();
    std::unordered_map<uint32_t, EntityId> obj_id_map;
    std::unordered_map<uint32_t, EntityId> unit_id_map;
    auto ids = populate_units(ew, ws, obj_id_map, unit_id_map);

    // Battalion → GroundTacticalComponent + HierarchyComponent
    EntityHandle h_bat(ids[0], &ew);
    EXPECT_TRUE(h_bat.has<GroundTacticalComponent>());
    EXPECT_TRUE(h_bat.has<HierarchyComponent>());
    auto* gt = h_bat.get<GroundTacticalComponent>();
    EXPECT_EQ(gt->supply, 80);
    EXPECT_EQ(gt->morale, 70);
    EXPECT_EQ(gt->heading, 128);

    // Brigade → GroundTacticalComponent + HierarchyComponent
    EntityHandle h_bri(ids[1], &ew);
    EXPECT_TRUE(h_bri.has<GroundTacticalComponent>());
    EXPECT_TRUE(h_bri.has<HierarchyComponent>());

    // Squadron → SquadronComponent
    EntityHandle h_sq(ids[2], &ew);
    EXPECT_TRUE(h_sq.has<SquadronComponent>());
    EXPECT_FALSE(h_sq.has<GroundTacticalComponent>());
    auto* sq = h_sq.get<SquadronComponent>();
    EXPECT_EQ(sq->specialty, 2);
    EXPECT_EQ(sq->fuel, 10000);
    ASSERT_EQ(sq->pilots.size(), 1u);
    EXPECT_EQ(sq->pilots[0].pilot_id, 1);

    // Flight → FlightPlanComponent
    EntityHandle h_fl(ids[3], &ew);
    EXPECT_TRUE(h_fl.has<FlightPlanComponent>());
    EXPECT_FALSE(h_fl.has<GroundTacticalComponent>());
    auto* fp = h_fl.get<FlightPlanComponent>();
    EXPECT_FLOAT_EQ(fp->altitude, 25000.0f);
    EXPECT_EQ(fp->mission, 3);

    // Package → PackageSupportComponent
    EntityHandle h_pk(ids[4], &ew);
    EXPECT_TRUE(h_pk.has<PackageSupportComponent>());
    auto* ps = h_pk.get<PackageSupportComponent>();
    EXPECT_EQ(ps->wait_cycles, 3);
}

TEST(PopulateUnits, ConditionalWaypointsAndVehicles) {
    EntityWorld ew;
    WorldState ws = make_unit_world();
    std::unordered_map<uint32_t, EntityId> obj_id_map;
    std::unordered_map<uint32_t, EntityId> unit_id_map;
    auto ids = populate_units(ew, ws, obj_id_map, unit_id_map);

    // Flight (u4) has waypoints
    EntityHandle h_fl(ids[3], &ew);
    EXPECT_TRUE(h_fl.has<WaypointPlanComponent>());
    ASSERT_EQ(h_fl.get<WaypointPlanComponent>()->waypoints.size(), 1u);
    EXPECT_EQ(h_fl.get<WaypointPlanComponent>()->waypoints[0].x, 520);

    // Battalion (u1) has no waypoints
    EntityHandle h_bat(ids[0], &ew);
    EXPECT_FALSE(h_bat.has<WaypointPlanComponent>());

    // Battalion (u1) has vehicle groups
    EXPECT_TRUE(h_bat.has<VehicleCompositionComponent>());
    ASSERT_EQ(h_bat.get<VehicleCompositionComponent>()->groups.size(), 1u);
    EXPECT_EQ(h_bat.get<VehicleCompositionComponent>()->groups[0].vehicle_name, "M-1A1");

    // Squadron (u3) has no vehicle groups
    EntityHandle h_sq(ids[2], &ew);
    EXPECT_FALSE(h_sq.has<VehicleCompositionComponent>());
}

TEST(PopulateUnits, ConditionalUnitClassScores) {
    EntityWorld ew;
    WorldState ws = make_unit_world();
    std::unordered_map<uint32_t, EntityId> obj_id_map;
    std::unordered_map<uint32_t, EntityId> unit_id_map;
    auto ids = populate_units(ew, ws, obj_id_map, unit_id_map);

    // Battalion has non-zero scores
    EntityHandle h_bat(ids[0], &ew);
    EXPECT_TRUE(h_bat.has<UnitClassScoreComponent>());
    EXPECT_EQ(h_bat.get<UnitClassScoreComponent>()->scores[0], 10);
    EXPECT_EQ(h_bat.get<UnitClassScoreComponent>()->scores[3], 80);

    // Brigade has all-zero scores → no component
    EntityHandle h_bri(ids[1], &ew);
    EXPECT_FALSE(h_bri.has<UnitClassScoreComponent>());
}

TEST(PopulateUnits, HierarchyResolved) {
    EntityWorld ew;
    WorldState ws = make_unit_world();
    std::unordered_map<uint32_t, EntityId> obj_id_map;
    std::unordered_map<uint32_t, EntityId> unit_id_map;
    auto ids = populate_units(ew, ws, obj_id_map, unit_id_map);

    // Battalion's parent should resolve to the Brigade entity.
    EntityHandle h_bat(ids[0], &ew);
    auto* hier = h_bat.get<HierarchyComponent>();
    ASSERT_NE(hier, nullptr);
    EXPECT_TRUE(hier->parent.valid());
    EXPECT_EQ(hier->parent, ids[1]);  // Brigade

    // Brigade's children should include the Battalion.
    EntityHandle h_bri(ids[1], &ew);
    auto* bri_hier = h_bri.get<HierarchyComponent>();
    ASSERT_NE(bri_hier, nullptr);
    ASSERT_EQ(bri_hier->children.size(), 1u);
    EXPECT_EQ(bri_hier->children[0], ids[0]);  // Battalion
}

TEST(PopulateUnits, SquadronAirbaseResolved) {
    EntityWorld ew;
    WorldState ws = make_unit_world();
    // Need an objective entity for the airbase (id_num=1001).
    std::unordered_map<uint32_t, EntityId> obj_id_map;
    auto obj_h = ew.create();
    obj_id_map[1001] = obj_h.id();

    std::unordered_map<uint32_t, EntityId> unit_id_map;
    auto ids = populate_units(ew, ws, obj_id_map, unit_id_map);

    // Squadron's airbase should resolve to the objective entity.
    EntityHandle h_sq(ids[2], &ew);
    auto* sq = h_sq.get<SquadronComponent>();
    ASSERT_NE(sq, nullptr);
    EXPECT_TRUE(sq->airbase.valid());
    EXPECT_EQ(sq->airbase, obj_h.id());
}

TEST(PopulateUnits, FlightPackageAndSquadronResolved) {
    EntityWorld ew;
    WorldState ws = make_unit_world();
    std::unordered_map<uint32_t, EntityId> obj_id_map;
    std::unordered_map<uint32_t, EntityId> unit_id_map;
    auto ids = populate_units(ew, ws, obj_id_map, unit_id_map);

    // Flight's package and squadron should be resolved.
    EntityHandle h_fl(ids[3], &ew);
    auto* fp = h_fl.get<FlightPlanComponent>();
    ASSERT_NE(fp, nullptr);
    EXPECT_TRUE(fp->package.valid());
    EXPECT_EQ(fp->package, ids[4]);  // Package
    EXPECT_TRUE(fp->squadron.valid());
    EXPECT_EQ(fp->squadron, ids[2]);  // Squadron
}

TEST(PopulateUnits, DomainTags) {
    EntityWorld ew;
    WorldState ws = make_unit_world();
    std::unordered_map<uint32_t, EntityId> obj_id_map;
    std::unordered_map<uint32_t, EntityId> unit_id_map;
    auto ids = populate_units(ew, ws, obj_id_map, unit_id_map);

    // Battalion → domain="ground"
    EntityHandle h_bat(ids[0], &ew);
    EXPECT_EQ(*h_bat.get_tag(tags::OPDOMAIN)->as_string(), "ground");

    // Squadron → domain="air"
    EntityHandle h_sq(ids[2], &ew);
    EXPECT_EQ(*h_sq.get_tag(tags::OPDOMAIN)->as_string(), "air");
}

TEST(PopulateUnits, RoleTags) {
    EntityWorld ew;
    WorldState ws = make_unit_world();
    std::unordered_map<uint32_t, EntityId> obj_id_map;
    std::unordered_map<uint32_t, EntityId> unit_id_map;
    auto ids = populate_units(ew, ws, obj_id_map, unit_id_map);

    EntityHandle h_bat(ids[0], &ew);
    EXPECT_EQ(*h_bat.get_tag(tags::ROLE)->as_string(), "battalion");

    EntityHandle h_sq(ids[2], &ew);
    EXPECT_EQ(*h_sq.get_tag(tags::ROLE)->as_string(), "squadron");

    EntityHandle h_fl(ids[3], &ew);
    EXPECT_EQ(*h_fl.get_tag(tags::ROLE)->as_string(), "flight");

    EntityHandle h_pk(ids[4], &ew);
    EXPECT_EQ(*h_pk.get_tag(tags::ROLE)->as_string(), "package");
}

// Phase A: NAME / CLASS / ICON tag enrichment for units.
TEST(PopulateUnits, PhaseA_NameClassIconTags) {
    EntityWorld ew;
    WorldState ws = make_unit_world();
    std::unordered_map<uint32_t, EntityId> obj_id_map;
    std::unordered_map<uint32_t, EntityId> unit_id_map;
    auto ids = populate_units(ew, ws, obj_id_map, unit_id_map);

    // u1: Battalion, unit_class=1, unit_subtype=1
    //   NAME  = "Armor Battalion"
    //   CLASS = 1 (unit_subtype)
    //   ICON  = (1 << 8) | 1 = 0x101 = 257
    EntityHandle h_bat(ids[0], &ew);
    ASSERT_TRUE(h_bat.has_tag(tags::NAME));
    EXPECT_EQ(*h_bat.get_tag(tags::NAME)->as_string(), "Armor Battalion");
    ASSERT_TRUE(h_bat.has_tag(tags::CLASS));
    EXPECT_EQ(*h_bat.get_tag(tags::CLASS)->as_int(), 1);
    ASSERT_TRUE(h_bat.has_tag(tags::ICON));
    EXPECT_EQ(*h_bat.get_tag(tags::ICON)->as_int(),
              static_cast<int64_t>((1u << 8) | 1u));

    // u3: Squadron, unit_class=3, unit_subtype=3
    //   NAME  = "18th Fighter Squadron"
    //   CLASS = 3
    //   ICON  = (3 << 8) | 3 = 0x303 = 771
    EntityHandle h_sq(ids[2], &ew);
    ASSERT_TRUE(h_sq.has_tag(tags::NAME));
    EXPECT_EQ(*h_sq.get_tag(tags::NAME)->as_string(), "18th Fighter Squadron");
    EXPECT_EQ(*h_sq.get_tag(tags::CLASS)->as_int(), 3);
    EXPECT_EQ(*h_sq.get_tag(tags::ICON)->as_int(),
              static_cast<int64_t>((3u << 8) | 3u));

    // u5: Package, unit_class=6, unit_subtype=5
    //   ICON  = (6 << 8) | 5 = 0x605 = 1541
    EntityHandle h_pk(ids[4], &ew);
    ASSERT_TRUE(h_pk.has_tag(tags::NAME));
    EXPECT_EQ(*h_pk.get_tag(tags::NAME)->as_string(), "Strike Package");
    EXPECT_EQ(*h_pk.get_tag(tags::CLASS)->as_int(), 5);
    EXPECT_EQ(*h_pk.get_tag(tags::ICON)->as_int(),
              static_cast<int64_t>((6u << 8) | 5u));
}

TEST(PopulateUnits, IdMapBuilt) {
    EntityWorld ew;
    WorldState ws = make_unit_world();
    std::unordered_map<uint32_t, EntityId> obj_id_map;
    std::unordered_map<uint32_t, EntityId> unit_id_map;
    auto ids = populate_units(ew, ws, obj_id_map, unit_id_map);

    EXPECT_EQ(unit_id_map.size(), 5u);
    EXPECT_TRUE(unit_id_map.count(2001));
    EXPECT_TRUE(unit_id_map.count(3001));
    EXPECT_TRUE(unit_id_map.count(4001));
    EXPECT_TRUE(unit_id_map.count(5001));
    EXPECT_TRUE(unit_id_map.count(6001));
}

// ============================================================================
// Phase 3: populate_world (end-to-end)
// ============================================================================

TEST(PopulateWorld, CreatesAllEntityKinds) {
    EntityWorld ew;
    WorldState ws = make_test_world();
    ws.objectives = make_objective_world().objectives;
    ws.units = make_unit_world().units;

    auto pw = populate_world(ew, ws);

    // Phase B: the per-kind EntityId vectors are no longer stored on
    // PopulatedWorld — derive them from tags instead. This is the same
    // pattern the world viewer uses (snapshot once at load time).
    auto camp_ids  = ew.with_tag(tags::ROLE, TagValue::from(std::string("campaign")));
    auto team_ids  = ew.with_tag(tags::ROLE, TagValue::from(std::string("team")));
    auto obj_ids   = ew.with_tag(tags::ROLE, TagValue::from(std::string("objective")));
    // Units have one of six ROLE values; query by OPDOMAIN instead.
    std::vector<EntityId> unit_ids;
    for (const char* d : {"air", "ground", "naval", "unknown"}) {
        auto ids = ew.with_tag(tags::OPDOMAIN, TagValue::from(std::string(d)));
        unit_ids.insert(unit_ids.end(), ids.begin(), ids.end());
    }

    // Campaign: 1 entity
    ASSERT_EQ(camp_ids.size(), 1u);
    EXPECT_TRUE(camp_ids[0].valid());

    // Teams: 3 non-empty teams
    EXPECT_EQ(team_ids.size(), 3u);

    // Objectives: 3
    EXPECT_EQ(obj_ids.size(), 3u);

    // Units: 5
    EXPECT_EQ(unit_ids.size(), 5u);

    // Total entities: 1 + 3 + 3 + 5 = 12
    EXPECT_EQ(ew.size(), 12u);

    // ID maps populated
    EXPECT_EQ(pw.objective_id_map.size(), 3u);
    EXPECT_EQ(pw.unit_id_map.size(), 5u);
}

TEST(PopulateWorld, CanQueryByComponent) {
    EntityWorld ew;
    WorldState ws = make_test_world();
    ws.objectives = make_objective_world().objectives;
    ws.units = make_unit_world().units;

    auto pw = populate_world(ew, ws);

    // Query all objectives (by component type)
    auto obj_entities = ew.with_component<ObjectiveTypeComponent>();
    EXPECT_EQ(obj_entities.size(), 3u);

    // Query all units (by component type)
    auto unit_entities = ew.with_component<UnitCoreComponent>();
    EXPECT_EQ(unit_entities.size(), 5u);

    // Query all teams (by component type)
    auto team_entities = ew.with_component<TeamComponent>();
    EXPECT_EQ(team_entities.size(), 3u);

    // Query campaign
    auto campaign_entities = ew.with_component<CampaignStateComponent>();
    EXPECT_EQ(campaign_entities.size(), 1u);

    // Query radar objectives
    auto radar_entities = ew.with_component<RadarComponent>();
    EXPECT_EQ(radar_entities.size(), 1u);

    // Query ground units
    auto ground_units = ew.with_component<GroundTacticalComponent>();
    EXPECT_EQ(ground_units.size(), 2u);  // battalion + brigade

    // Query flights
    auto flights = ew.with_component<FlightPlanComponent>();
    EXPECT_EQ(flights.size(), 1u);
}

// ============================================================================
// Phase 3: Real fixture end-to-end test
// ============================================================================

TEST(PopulateWorld, RealFixture) {
    // Load the real save1.cam-derived JSON and populate EntityWorld.
    // This validates the entire pipeline:
    //   .cam → JSON → WorldState → EntityWorld
    const char* path = WORLD_JSON_FIXTURE;
    ASSERT_NE(path, nullptr);

    WorldState ws;
    ws.load(path);

    // The real Korea save has 8 teams, 2659 objectives, 683 units.
    EXPECT_EQ(ws.teams.size(), 8u);
    EXPECT_EQ(ws.objectives.size(), 2659u);
    EXPECT_EQ(ws.units.size(), 683u);

    EntityWorld ew;
    auto pw = populate_world(ew, ws);

    // Phase B: derive per-kind lists from tags (PopulatedWorld no longer
    // carries the per-kind EntityId vectors).
    auto camp_ids = ew.with_tag(tags::ROLE, TagValue::from(std::string("campaign")));
    ASSERT_EQ(camp_ids.size(), 1u);
    EXPECT_TRUE(camp_ids[0].valid());
    EntityHandle camp_h(camp_ids[0], &ew);
    EXPECT_TRUE(camp_h.has<CampaignStateComponent>());

    // Team entities: some slots may be empty, so count via component
    auto team_entities = ew.with_component<TeamComponent>();
    EXPECT_GE(team_entities.size(), 4u);  // at least U.S., ROK, Japan, DPRK
    EXPECT_LE(team_entities.size(), 8u);

    // Objective entities: one per WorldState objective
    auto obj_ids = ew.with_tag(tags::ROLE, TagValue::from(std::string("objective")));
    EXPECT_EQ(obj_ids.size(), 2659u);
    auto obj_with_transform = ew.with_component<ObjectiveTypeComponent>();
    EXPECT_EQ(obj_with_transform.size(), 2659u);

    // Every objective has TransformComponent, OwnershipComponent, PropertyBag
    for (auto id : obj_ids) {
        EntityHandle h(id, &ew);
        ASSERT_TRUE(h.has<TransformComponent>());
        ASSERT_TRUE(h.has<OwnershipComponent>());
        ASSERT_TRUE(h.has<PropertyBag>());
    }

    // Unit entities: one per WorldState unit
    std::vector<EntityId> unit_ids;
    for (const char* d : {"air", "ground", "naval", "unknown"}) {
        auto ids = ew.with_tag(tags::OPDOMAIN, TagValue::from(std::string(d)));
        unit_ids.insert(unit_ids.end(), ids.begin(), ids.end());
    }
    EXPECT_EQ(unit_ids.size(), 683u);
    auto unit_with_core = ew.with_component<UnitCoreComponent>();
    EXPECT_EQ(unit_with_core.size(), 683u);

    // Every unit has TransformComponent, UnitCoreComponent, PropertyBag
    for (auto id : unit_ids) {
        EntityHandle h(id, &ew);
        ASSERT_TRUE(h.has<TransformComponent>());
        ASSERT_TRUE(h.has<UnitCoreComponent>());
        ASSERT_TRUE(h.has<PropertyBag>());
    }

    // Verify some radar objectives exist
    auto radar_objs = ew.with_component<RadarComponent>();
    EXPECT_GT(radar_objs.size(), 0u);

    // Verify unit class distribution via components.
    // The Korea save1 fixture has battalions, brigades, taskforces, and
    // squadrons — but no flights or packages (those are created at runtime
    // by the ATO, not saved in the campaign file).
    auto ground_tactical = ew.with_component<GroundTacticalComponent>();
    auto squadrons = ew.with_component<SquadronComponent>();
    EXPECT_GT(ground_tactical.size(), 0u);
    EXPECT_GT(squadrons.size(), 0u);

    // VU_ID→EntityId maps: populated when WorldState entries have non-zero
    // id_num. The current save1.cam fixture does not emit VU_ID fields
    // (id_num, id_creator), so the maps may be empty. They will be
    // populated once f4-world-convert emits VU_ID data.
    // Cross-reference resolution requires VU_ID data to work.

    // Verify the structural payoff: we can query entities by component
    // type, not by indexing into WorldState vectors.
    auto all_transforms = ew.with_component<TransformComponent>();
    EXPECT_EQ(all_transforms.size(), obj_ids.size() + unit_ids.size());

    auto all_ownership = ew.with_component<OwnershipComponent>();
    EXPECT_EQ(all_ownership.size(), obj_ids.size());
}

// ============================================================================
// Phase 4: Convenience API (load / load_from_string)
// ============================================================================
//
// NOTE: The per-Adapter tests (CampaignAdapterImplementsInterface, etc.) and
// the PopulateViaInterfacesMatchesWorldState test were removed when the
// Adapter structs moved from the public header to world_loader.cpp (header-
// leak fix). The Adapters are now an implementation detail; their correctness
// is verified end-to-end by the populate_world(eworld, ws) tests above,
// which internally construct Adapters and delegate to the interface-based
// bridge. A future test that wants to verify the interface path independently
// of WorldState should implement I*Source with a mock and call
// populate_world(eworld, mock_camp, mock_teams, mock_obj, mock_units)
// directly — that tests the bridge without depending on private internals.
// ============================================================================

TEST(ConvenienceAPI, LoadFromString) {
    std::string json = R"({
        "version": 63,
        "campaign": {
            "current_time": 1000,
            "te_start_time": 0,
            "te_time_limit": 0,
            "te_victory_points": 42,
            "te_type": 0,
            "te_number_teams": 0,
            "te_team": 0,
            "te_flags": 7,
            "te_number_aircraft": [1,2,3,4,5,6,7,8],
            "te_team_pts": [10,20,30,40,50,60,70,80],
            "teams": [
                {"slot": 1, "flags": 1, "colour": 1, "name": "U.S.", "motto": ""}
            ],
            "decoded_bytes": 100,
            "undecoded_bytes": 0
        },
        "objectives": {"count": 0, "decoded": 0, "bytes_consumed": 0, "inner_size": 0, "items": []},
        "units": {"count": 0, "decoded": 0, "bytes_consumed": 0, "inner_size": 0, "items": []},
        "raw_subfiles": {}
    })";

    EntityWorld ew;
    auto pw = load_from_string(json, ew);

    // Phase B: campaign is now derived from tags, not stored on PopulatedWorld.
    auto camp_entities = ew.with_component<CampaignStateComponent>();
    EXPECT_EQ(camp_entities.size(), 1u);
    EXPECT_TRUE(camp_entities[0].valid());
    EntityHandle camp_h(camp_entities[0], &ew);
    auto* cs = camp_h.get<CampaignStateComponent>();
    ASSERT_NE(cs, nullptr);
    EXPECT_EQ(cs->current_time, 1000);
    EXPECT_EQ(cs->te_victory_points, 42);

    // Team entity (derived from tags)
    auto team_ids = ew.with_tag(tags::ROLE, TagValue::from(std::string("team")));
    EXPECT_EQ(team_ids.size(), 1u);
    auto team_entities = ew.with_component<TeamComponent>();
    EXPECT_EQ(team_entities.size(), 1u);
}

TEST(ConvenienceAPI, LoadRealFixture) {
    const char* path = WORLD_JSON_FIXTURE;
    ASSERT_NE(path, nullptr);

    EntityWorld ew;
    auto pw = load(path, ew);

    // Same results as the manual WorldState → populate_world pipeline
    // (Phase B: counts derived from tags, not stored on PopulatedWorld)
    auto camp_ids = ew.with_tag(tags::ROLE, TagValue::from(std::string("campaign")));
    EXPECT_EQ(camp_ids.size(), 1u);
    EXPECT_TRUE(camp_ids[0].valid());
    auto team_ids = ew.with_tag(tags::ROLE, TagValue::from(std::string("team")));
    EXPECT_GE(team_ids.size(), 4u);
    auto obj_ids = ew.with_tag(tags::ROLE, TagValue::from(std::string("objective")));
    EXPECT_EQ(obj_ids.size(), 2659u);
    std::vector<EntityId> unit_ids;
    for (const char* d : {"air", "ground", "naval", "unknown"}) {
        auto ids = ew.with_tag(tags::OPDOMAIN, TagValue::from(std::string(d)));
        unit_ids.insert(unit_ids.end(), ids.begin(), ids.end());
    }
    EXPECT_EQ(unit_ids.size(), 683u);

    // Can query by components
    auto radar = ew.with_component<RadarComponent>();
    EXPECT_GT(radar.size(), 0u);
    auto ground = ew.with_component<GroundTacticalComponent>();
    EXPECT_GT(ground.size(), 0u);
}

// ============================================================================
// B.3 tranche — package elements, ATM mission request, flight target
// ============================================================================

namespace {

// Small world: objective 1001 (strike target), squadron 4001 at airbase,
// flight 5001 (mission AMIS_INTSTRIKE=13, target objective 1001, in package
// 6001), package 6001 with element [5001] + a mission request targeting
// objective 1001 requested by battalion 2001.
WorldState make_b3_world() {
    WorldState ws;
    ws.version = 71;
    ws.campaign.current_time = 38574360;

    ObjectiveState tgt;
    tgt.type = 1337; tgt.entity_type = 1337; tgt.x = 620; tgt.y = 520;
    tgt.owner = 1; tgt.first_owner = 1; tgt.priority = 30;
    tgt.id_creator = 0; tgt.id_num = 1001; tgt.camp_id = 51;
    tgt.objective_type = 6; tgt.class_name = "Strike Target";
    ws.objectives = {tgt};

    UnitState sq;
    sq.type = 500; sq.unit_class = UnitClass::Squadron; sq.domain = 2;
    sq.x = 500; sq.y = 400; sq.owner = 2; sq.id_num = 4001;
    sq.class_name = "35th Fighter Squadron";
    sq.airbase_id = 1001;  // same objective: fine for the test
    UnitState fl;
    fl.type = 600; fl.unit_class = UnitClass::Flight; fl.domain = 2;
    fl.x = 510; fl.y = 410; fl.owner = 2; fl.id_num = 5001;
    fl.class_name = "Strike Flight";
    fl.mission = 13; fl.mission_target = 1001;
    fl.time_on_target = 43739352; fl.package_id = 6001; fl.squadron_id = 4001;
    fl.callsign_id = 125; fl.callsign_num = 1;
    WaypointState w1; w1.x = 510; w1.y = 410; w1.z = 0;      w1.action = 1;   // WP_TAKEOFF
    WaypointState w2; w2.x = 560; w2.y = 460; w2.z = 2500;   w2.action = 15;  // WP_NAVSTRIKE
    WaypointState w3; w3.x = 620; w3.y = 520; w3.z = 2500;   w3.action = 17;  // WP_STRIKE
    WaypointState w4; w4.x = 510; w4.y = 410; w4.z = 0;      w4.action = 7;   // WP_LAND
    fl.waypoints = {w1, w2, w3, w4};
    UnitState pkg;
    pkg.type = 700; pkg.unit_class = UnitClass::Package; pkg.domain = 2;
    pkg.x = 505; pkg.y = 405; pkg.owner = 2; pkg.id_num = 6001;
    pkg.class_name = "Strike Package";
    pkg.wait_cycles = 2;
    pkg.element_ids = {5001};
    pkg.request_present = true;
    pkg.request_mission = 13;
    pkg.request_tot = 43739352;
    pkg.request_priority = 230;
    pkg.request_action_type = 0;
    pkg.request_target_num = 1001;
    pkg.request_requester_num = 1001;
    UnitState bn;
    bn.type = 170; bn.unit_class = UnitClass::Battalion; bn.domain = 3;
    bn.x = 400; bn.y = 300; bn.owner = 2; bn.id_num = 2001;
    bn.class_name = "Armor Battalion";
    ws.units = {sq, fl, pkg, bn};
    return ws;
}

} // namespace

TEST(PopulateWorldB3, PackageElementsAndRequestResolve) {
    EntityWorld ew;
    WorldState ws = make_b3_world();
    auto pw = populate_world(ew, ws);

    // Package → element flight cross-reference.
    EntityId pkg_eid = pw.unit_id_map.at(6001);
    auto pkg_h = EntityHandle(pkg_eid, &ew);
    auto* ps = pkg_h.get<PackageSupportComponent>();
    ASSERT_NE(ps, nullptr);
    ASSERT_EQ(ps->elements.size(), 1u);
    EXPECT_EQ(ps->elements[0], pw.unit_id_map.at(5001));

    // Flight → package reverse link still resolves.
    auto fl_h = EntityHandle(pw.unit_id_map.at(5001), &ew);
    auto* fp = fl_h.get<FlightPlanComponent>();
    ASSERT_NE(fp, nullptr);
    EXPECT_EQ(fp->package, pkg_eid);

    // Flight mission target resolves to the objective entity.
    EXPECT_EQ(fp->target, pw.objective_id_map.at(1001));

    // The mission request carried over and resolved target + requester.
    EXPECT_TRUE(ps->request.present);
    EXPECT_EQ(ps->request.mission, 13);
    EXPECT_EQ(ps->request.tot, 43739352);
    EXPECT_EQ(ps->request.priority, 230);
    EXPECT_EQ(ps->request.target, pw.objective_id_map.at(1001));
}

TEST(WorldStateB3, ParsesMisRequestBlock) {
    // v71 world JSON carries the package mis_request block — the parser
    // must decode it into UnitState.request_* (B.3 tranche).
    WorldState ws;
    ws.load_from_string(R"({
        "version": 71,
        "units": {
            "count": 1,
            "items": [{
                "type": 700, "unit_class": "package", "id_num": 6001,
                "x": 505, "y": 405, "owner": 2,
                "wait_cycles": 2, "element_ids": [5001],
                "mis_request": {
                    "mission": 13, "tot": 43739352, "priority": 230,
                    "action_type": 0, "target_num": 1001,
                    "target_creator": 0, "requester_num": 1001
                }
            }]
        }
    })");
    ASSERT_EQ(ws.units.size(), 1u);
    const auto& u = ws.units[0];
    EXPECT_TRUE(u.request_present);
    EXPECT_EQ(u.request_mission, 13);
    EXPECT_EQ(u.request_tot, 43739352);
    EXPECT_EQ(u.request_priority, 230);
    EXPECT_EQ(u.request_action_type, 0);
    EXPECT_EQ(u.request_target_num, 1001u);
    EXPECT_EQ(u.request_requester_num, 1001u);
    ASSERT_EQ(u.element_ids.size(), 1u);
    EXPECT_EQ(u.element_ids[0], 5001u);
}
