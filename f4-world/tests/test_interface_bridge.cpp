// test_interface_bridge.cpp — verify the IDataSource-based bridge works
// without going through WorldState.
//
// This test is the keystone of the ECS Phase 4 decoupling (see
// ECS_DECOUPLING_PLAN.md §6.2). It implements mock ICampaignSource /
// ITeamSource / IObjectiveSource / IUnitCoreSource adapters inline and
// calls populate_world() through the interface overloads. If this test
// passes, future data sources (BMS saves, DIS streams, procedural
// generation) can plug into the bridge without touching WorldState.
//
// This is also the contract test for f4-ai: the AI consumes EntityWorld
// populated through these interfaces. If the interface contract breaks,
// this test fails before any AI code can be written against it.

#include <gtest/gtest.h>
#include <f4/world/f4_world.hpp>
#include <f4/entities/f4_entities.hpp>

#include <array>
#include <string>
#include <vector>

using namespace f4::world;
using namespace f4::entities;

namespace {

// ============================================================================
// MockCampaignSource — minimal ICampaignSource implementation
// ============================================================================
class MockCampaignSource : public ICampaignSource {
public:
    int32_t current_time() const override { return 12345; }
    int32_t te_start_time() const override { return 0; }
    int32_t te_time_limit() const override { return 3600; }
    int32_t te_victory_points() const override { return 100; }
    int32_t te_type() const override { return 1; }
    int32_t te_number_teams() const override { return 2; }
    int32_t te_team() const override { return 0; }
    int32_t te_flags() const override { return 0; }
    const std::vector<int32_t>& te_number_aircraft() const override { return ac_; }
    const std::vector<int32_t>& te_team_pts() const override { return pts_; }
private:
    std::vector<int32_t> ac_{10, 20};
    std::vector<int32_t> pts_{0, 0};
};

// ============================================================================
// MockTeamSource — single-team mock
// ============================================================================
class MockTeamSource : public ITeamSource {
public:
    int team_count() const override { return 1; }
    int slot(int) const override { return 1; }
    uint8_t flags(int) const override { return 0; }
    uint8_t colour(int) const override { return 2; }
    const std::string& name(int) const override { return name_; }
    const std::string& motto(int) const override { return motto_; }
    bool tea_loaded(int) const override { return false; }
    const std::vector<int16_t>& stance(int) const override { return empty_s_; }
    const std::vector<uint8_t>& member(int) const override { return empty_m_; }
    uint8_t air_experience(int) const override { return 0; }
    uint8_t ground_experience(int) const override { return 0; }
    uint8_t naval_experience(int) const override { return 0; }
    uint8_t air_defense_experience(int) const override { return 0; }
    int16_t first_colonel(int) const override { return 0; }
    int16_t first_commander(int) const override { return 0; }
    int16_t first_wingman(int) const override { return 0; }
    int16_t last_wingman(int) const override { return 0; }
private:
    std::string name_{"MockTeam"};
    std::string motto_{"Test"};
    std::vector<int16_t> empty_s_;
    std::vector<uint8_t> empty_m_;
};

// ============================================================================
// MockObjectiveSource — single-objective mock
// ============================================================================
class MockObjectiveSource : public IObjectiveSource {
public:
    int objective_count() const override { return 1; }
    int16_t x(int) const override { return 100; }
    int16_t y(int) const override { return 200; }
    float z(int) const override { return 0.0f; }
    int16_t type(int) const override { return 5; }
    uint16_t entity_type(int) const override { return 100; }
    const std::string& class_name(int) const override { return name_; }
    uint8_t owner(int) const override { return 2; }
    uint8_t first_owner(int) const override { return 2; }
    uint8_t priority(int) const override { return 5; }
    int16_t nameid(int) const override { return 1; }
    uint32_t obj_flags(int) const override { return 0; }
    uint32_t parent_id(int) const override { return 0; }
    bool has_supply(int) const override { return false; }
    uint8_t supply(int) const override { return 0; }
    uint8_t fuel(int) const override { return 0; }
    uint8_t losses(int) const override { return 0; }
    int32_t last_repair(int) const override { return 0; }
    bool has_fstatus(int) const override { return false; }
    const std::vector<uint8_t>& fstatus(int) const override { return empty_; }
    bool has_radar(int) const override { return false; }
    const float* detect_ratio(int) const override { return dr_.data(); }
    float radar_range_km(int) const override { return 0.0f; }
    const std::string& radar_name(int) const override { return empty_s_; }
    int16_t radar_type_idx(int) const override { return 0; }
    bool has_links(int) const override { return false; }
    const std::vector<f4::entities::ObjectiveLink>& links(int) const override { return empty_l_; }
    bool has_ground_layout(int) const override { return false; }
    const std::vector<f4::entities::GroundLayoutList>& ground_layout(int) const override { return empty_gl_; }
    bool has_features(int) const override { return false; }
    uint8_t features_count(int) const override { return 0; }
    uint8_t radar_feature(int) const override { return 0; }
    uint8_t deag_distance(int) const override { return 0; }
    uint16_t pt_data_index(int) const override { return 0; }
    const std::array<uint8_t, 8>& objective_detection(int) const override { return od_; }
    const std::vector<f4::entities::FeatureEntryState>& features(int) const override { return empty_f_; }
    uint32_t id_num(int) const override { return 1001; }
    uint32_t id_creator(int) const override { return 1; }
    int16_t camp_id(int) const override { return 0; }
    uint8_t objective_type(int) const override { return 15; }
private:
    std::string name_{"MockObjective"};
    std::string empty_s_;
    std::vector<uint8_t> empty_;
    std::array<float, 8> dr_{};
    std::vector<f4::entities::ObjectiveLink> empty_l_;
    std::vector<f4::entities::GroundLayoutList> empty_gl_;
    std::vector<f4::entities::FeatureEntryState> empty_f_;
    std::array<uint8_t, 8> od_{};
};

// ============================================================================
// MockUnitSource — single ground-unit (battalion) mock
// ============================================================================
class MockUnitSource : public IUnitCoreSource,
                       public IGroundUnitSource,
                       public ISquadronSource,
                       public IFlightSource,
                       public IPackageSource {
public:
    int unit_count() const override { return 1; }
    int16_t x(int) const override { return 150; }
    int16_t y(int) const override { return 250; }
    float z(int) const override { return 0.0f; }
    f4::entities::UnitClass unit_class(int) const override {
        return f4::entities::UnitClass::Battalion;
    }
    uint8_t domain(int) const override { return 3; }  // land
    uint8_t unit_subtype(int) const override { return 1; }
    uint16_t entity_type(int) const override { return 200; }
    uint32_t roster(int) const override { return 0; }
    const std::string& class_name(int) const override { return name_; }
    uint8_t owner(int) const override { return 2; }
    bool has_waypoints(int) const override { return false; }
    const std::vector<f4::entities::WaypointState>& waypoints(int) const override { return empty_w_; }
    bool has_vehicle_groups(int) const override { return false; }
    const std::vector<f4::entities::VehicleGroup>& vehicle_groups(int) const override { return empty_v_; }
    const std::array<uint8_t, 16>& unit_class_scores(int) const override { return scores_; }
    uint32_t id_num(int) const override { return 2001; }
    uint32_t id_creator(int) const override { return 1; }
    int16_t camp_id(int) const override { return 0; }
    int16_t name_id(int) const override { return 0; }
    int16_t reinforcement(int) const override { return 0; }
    int16_t dest_x(int) const override { return 0; }
    int16_t dest_y(int) const override { return 0; }
    int32_t movement_type(int) const override { return 0; }
    int16_t movement_speed(int) const override { return 0; }
    int16_t max_range(int) const override { return 0; }
    const std::string& movement_type_name(int) const override { return empty_s_; }
    uint8_t losses(int) const override { return 0; }
    uint8_t wp_count(int) const override { return 0; }
    uint8_t elements(int) const override { return 0; }

    // Subclass accessors — return this for the matching class, nullptr otherwise.
    const IGroundUnitSource* as_ground_unit(int i) const override {
        return unit_class(i) == f4::entities::UnitClass::Battalion
            ? static_cast<const IGroundUnitSource*>(this) : nullptr;
    }
    const ISquadronSource* as_squadron(int) const override { return nullptr; }
    const IFlightSource* as_flight(int) const override { return nullptr; }
    const IPackageSource* as_package(int) const override { return nullptr; }

    // --- IGroundUnitSource ---
    uint8_t supply(int) const override { return 50; }
    uint8_t morale(int) const override { return 80; }
    uint8_t fatigue(int) const override { return 10; }
    uint8_t heading(int) const override { return 0; }
    uint8_t final_heading(int) const override { return 0; }
    uint8_t position(int) const override { return 0; }
    int32_t last_move(int) const override { return 0; }
    int32_t last_combat(int) const override { return 0; }
    uint32_t parent_id(int) const override { return 0; }
    const std::vector<uint32_t>& element_ids(int) const override { return empty_e_; }

    // --- ISquadronSource (unused, but must implement) ---
    uint32_t airbase_id(int) const override { return 0; }
    uint8_t specialty(int) const override { return 0; }
    int16_t aa_kills(int) const override { return 0; }
    int16_t ag_kills(int) const override { return 0; }
    int16_t as_kills(int) const override { return 0; }
    int16_t an_kills(int) const override { return 0; }
    int16_t missions_flown(int) const override { return 0; }
    int16_t mission_score(int) const override { return 0; }
    uint8_t total_losses(int) const override { return 0; }
    uint8_t pilot_losses(int) const override { return 0; }
    uint8_t squadron_patch(int) const override { return 0; }
    int32_t fuel(int) const override { return 0; }
    const std::vector<f4::entities::PilotState>& pilots(int) const override { return empty_p_; }

    // --- IFlightSource (unused) ---
    float flight_altitude(int) const override { return 0.0f; }
    int32_t fuel_burnt(int) const override { return 0; }
    int32_t time_on_target(int) const override { return 0; }
    int32_t mission_over_time(int) const override { return 0; }
    int16_t mission_target(int) const override { return 0; }
    uint8_t loadouts(int) const override { return 0; }
    std::vector<f4::entities::LoadoutStationState> loadout_stations(
        int) const override { return {}; }
    uint8_t mission(int) const override { return 0; }
    uint8_t flight_priority(int) const override { return 0; }
    uint8_t mission_id(int) const override { return 0; }
    uint8_t eval_flags(int) const override { return 0; }
    uint32_t package_id(int) const override { return 0; }
    uint32_t squadron_id(int) const override { return 0; }
    uint8_t callsign_id(int) const override { return 0; }
    uint8_t callsign_num(int) const override { return 0; }

    // --- IPackageSource (unused) ---
    uint8_t wait_cycles(int) const override { return 0; }
    uint32_t interceptor_id(int) const override { return 0; }
    uint32_t awacs_id(int) const override { return 0; }
    uint32_t jstar_id(int) const override { return 0; }
    uint32_t ecm_id(int) const override { return 0; }
    uint32_t tanker_id(int) const override { return 0; }

private:
    std::string name_{"MockBattalion"};
    std::string empty_s_;
    std::vector<f4::entities::WaypointState> empty_w_;
    std::vector<f4::entities::VehicleGroup> empty_v_;
    std::array<uint8_t, 16> scores_{};
    std::vector<uint32_t> empty_e_;
    std::vector<f4::entities::PilotState> empty_p_;
};

} // namespace

// ============================================================================
// Tests
// ============================================================================

TEST(InterfaceBridge, PopulatesWorldFromMockSources) {
    EntityWorld ew;
    MockCampaignSource camp;
    MockTeamSource teams;
    MockObjectiveSource objs;
    MockUnitSource units;

    PopulatedWorld pop = populate_world(ew, camp, teams, objs, units);

    // Phase B: per-kind counts are now derived from tags, not stored on
    // PopulatedWorld (which only carries the two VU_ID maps now).
    auto camp_ids = ew.with_tag(tags::ROLE, TagValue::from(std::string("campaign")));
    ASSERT_EQ(camp_ids.size(), 1u);
    EXPECT_TRUE(camp_ids[0].valid());
    EXPECT_EQ(ew.with_tag(tags::ROLE, TagValue::from(std::string("team"))).size(), 1u);
    EXPECT_EQ(ew.with_tag(tags::ROLE, TagValue::from(std::string("objective"))).size(), 1u);
    // Units have one of six ROLE values; query by OPDOMAIN instead.
    std::vector<EntityId> unit_ids;
    for (const char* d : {"air", "ground", "naval", "unknown"}) {
        auto ids = ew.with_tag(tags::OPDOMAIN, TagValue::from(std::string(d)));
        unit_ids.insert(unit_ids.end(), ids.begin(), ids.end());
    }
    EXPECT_EQ(unit_ids.size(), 1u);
}

TEST(InterfaceBridge, CampaignEntityHasCorrectComponent) {
    EntityWorld ew;
    MockCampaignSource camp;
    MockTeamSource teams;
    MockObjectiveSource objs;
    MockUnitSource units;

    PopulatedWorld pop = populate_world(ew, camp, teams, objs, units);

    // Phase B: campaign entity derived from tags.
    auto camp_ids = ew.with_tag(tags::ROLE, TagValue::from(std::string("campaign")));
    ASSERT_EQ(camp_ids.size(), 1u);
    EntityHandle h(camp_ids[0], &ew);
    ASSERT_TRUE(h.has<CampaignStateComponent>());
    auto* cs = h.get<CampaignStateComponent>();
    EXPECT_EQ(cs->current_time, 12345);
    EXPECT_EQ(cs->te_time_limit, 3600);
}

TEST(InterfaceBridge, TeamEntityHasIdentityAndTeamComponents) {
    EntityWorld ew;
    MockCampaignSource camp;
    MockTeamSource teams;
    MockObjectiveSource objs;
    MockUnitSource units;

    PopulatedWorld pop = populate_world(ew, camp, teams, objs, units);

    // Phase B: team entity derived from tags.
    auto team_ids = ew.with_tag(tags::ROLE, TagValue::from(std::string("team")));
    ASSERT_EQ(team_ids.size(), 1u);
    EntityHandle h(team_ids[0], &ew);
    ASSERT_TRUE(h.has<CampaignIdentityComponent>());
    ASSERT_TRUE(h.has<TeamComponent>());
    auto* cid = h.get<CampaignIdentityComponent>();
    EXPECT_EQ(cid->team_id, 1);
    EXPECT_EQ(cid->callsign, "MockTeam");
    auto* tc = h.get<TeamComponent>();
    EXPECT_EQ(tc->slot, 1);
    EXPECT_EQ(tc->colour, 2);
}

TEST(InterfaceBridge, ObjectiveEntityHasTransformAndType) {
    EntityWorld ew;
    MockCampaignSource camp;
    MockTeamSource teams;
    MockObjectiveSource objs;
    MockUnitSource units;

    PopulatedWorld pop = populate_world(ew, camp, teams, objs, units);

    // Phase B: objective entity derived from tags.
    auto obj_ids = ew.with_tag(tags::ROLE, TagValue::from(std::string("objective")));
    ASSERT_EQ(obj_ids.size(), 1u);
    EntityHandle h(obj_ids[0], &ew);
    ASSERT_TRUE(h.has<TransformComponent>());
    ASSERT_TRUE(h.has<ObjectiveTypeComponent>());
    ASSERT_TRUE(h.has<OwnershipComponent>());

    auto* tr = h.get<TransformComponent>();
    // Grid (100, 200) → sim-local feet. The bridge uses FT_PER_GRID (1024).
    // Just verify the transform is populated and non-zero.
    EXPECT_NE(tr->position.x, 0.0);
    EXPECT_NE(tr->position.y, 0.0);

    auto* ot = h.get<ObjectiveTypeComponent>();
    EXPECT_EQ(ot->type, 5);
}

TEST(InterfaceBridge, GroundUnitHasGroundTacticalComponent) {
    EntityWorld ew;
    MockCampaignSource camp;
    MockTeamSource teams;
    MockObjectiveSource objs;
    MockUnitSource units;

    PopulatedWorld pop = populate_world(ew, camp, teams, objs, units);

    // Phase B: unit entity derived from tags (units have one of six
    // ROLE values, so query by OPDOMAIN).
    std::vector<EntityId> unit_ids;
    for (const char* d : {"air", "ground", "naval", "unknown"}) {
        auto ids = ew.with_tag(tags::OPDOMAIN, TagValue::from(std::string(d)));
        unit_ids.insert(unit_ids.end(), ids.begin(), ids.end());
    }
    ASSERT_EQ(unit_ids.size(), 1u);
    EntityHandle h(unit_ids[0], &ew);
    ASSERT_TRUE(h.has<UnitCoreComponent>());
    ASSERT_TRUE(h.has<GroundTacticalComponent>());

    auto* uc = h.get<UnitCoreComponent>();
    EXPECT_EQ(uc->unit_class, f4::entities::UnitClass::Battalion);

    auto* gt = h.get<GroundTacticalComponent>();
    EXPECT_EQ(gt->supply, 50);
    EXPECT_EQ(gt->morale, 80);
    EXPECT_EQ(gt->fatigue, 10);
}

TEST(InterfaceBridge, UnitIdMapPopulated) {
    EntityWorld ew;
    MockCampaignSource camp;
    MockTeamSource teams;
    MockObjectiveSource objs;
    MockUnitSource units;

    PopulatedWorld pop = populate_world(ew, camp, teams, objs, units);

    // The mock unit has id_num = 2001. The bridge should have mapped it.
    auto it = pop.unit_id_map.find(2001);
    ASSERT_NE(it, pop.unit_id_map.end());
    // Phase B: cross-check against the tag-derived unit list (was
    // previously pop.units[0]).
    std::vector<EntityId> unit_ids;
    for (const char* d : {"air", "ground", "naval", "unknown"}) {
        auto ids = ew.with_tag(tags::OPDOMAIN, TagValue::from(std::string(d)));
        unit_ids.insert(unit_ids.end(), ids.begin(), ids.end());
    }
    ASSERT_EQ(unit_ids.size(), 1u);
    EXPECT_EQ(it->second, unit_ids[0]);
}

TEST(InterfaceBridge, ObjectiveIdMapPopulated) {
    EntityWorld ew;
    MockCampaignSource camp;
    MockTeamSource teams;
    MockObjectiveSource objs;
    MockUnitSource units;

    PopulatedWorld pop = populate_world(ew, camp, teams, objs, units);

    auto it = pop.objective_id_map.find(1001);
    ASSERT_NE(it, pop.objective_id_map.end());
    // Phase B: cross-check against the tag-derived objective list (was
    // previously pop.objectives[0]).
    auto obj_ids = ew.with_tag(tags::ROLE, TagValue::from(std::string("objective")));
    ASSERT_EQ(obj_ids.size(), 1u);
    EXPECT_EQ(it->second, obj_ids[0]);
}
