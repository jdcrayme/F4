// f4-world/src/world_loader.cpp — populate f4-entities from data sources.
//
// Phase 1: populate_teams adds TeamComponent (with .tea enrichment data) and
// a narrowed CampaignIdentityComponent (team_id + callsign only).
//
// Phase 3: populate_objectives, populate_units, populate_campaign, populate_world
// bridge all entity types via WorldState.
//
// Phase 4: Interface-based bridge overloads accept ICampaignSource, ITeamSource,
// IObjectiveSource, IUnitSource. These are the PRIMARY implementations.
// The monolithic WorldStateAdapter has been replaced by four separate adapters
// (CampaignAdapter, TeamAdapter, ObjectiveAdapter, UnitAdapter) to eliminate
// name collisions between IObjectiveSource and IUnitSource. WorldState-based
// functions are thin convenience wrappers that create a WorldStateAdapters
// bundle and delegate. load()/load_from_string() hide WorldState entirely.
//
// HEADER-LEAK FIX: The four *Adapter structs + WorldStateAdapters bundle
// previously lived in the public header (world_loader.hpp). They dereference
// `const WorldState*` in their method bodies, so their definitions required
// the full WorldState layout — which meant every consumer of
// <f4/world/f4_world.hpp> transitively pulled in <f4/world/detail/world_state.hpp>.
// They are now defined here (in the .cpp), so the public header only needs a
// forward declaration of WorldState.

#include <f4/world/world_loader.hpp>
#include <f4/world/detail/world_state.hpp>
#include <f4/geo/constants.hpp>

namespace f4::world {

// using namespace f4::entities — retained because 30+ entity types are used
// in this file. Explicit using-declarations for each would be ~30 lines of
// noise with no readability benefit. using-namespace is acceptable in .cpp
// implementation files (only headers are the real risk).
using namespace f4::entities;

// ============================================================================
// WorldState → I*Source adapters (implementation detail, defined here so the
// public header doesn't need the full WorldState layout).
// ============================================================================

struct CampaignAdapter : ICampaignSource {
    explicit CampaignAdapter(const WorldState& ws) : ws_(&ws) {}

    int32_t current_time() const override { return ws_->campaign.current_time; }
    int32_t te_start_time() const override { return ws_->campaign.te_start_time; }
    int32_t te_time_limit() const override { return ws_->campaign.te_time_limit; }
    int32_t te_victory_points() const override { return ws_->campaign.te_victory_points; }
    int32_t te_type() const override { return ws_->campaign.te_type; }
    int32_t te_number_teams() const override { return ws_->campaign.te_number_teams; }
    int32_t te_team() const override { return ws_->campaign.te_team; }
    int32_t te_flags() const override { return ws_->campaign.te_flags; }
    const std::vector<int32_t>& te_number_aircraft() const override { return ws_->campaign.te_number_aircraft; }
    const std::vector<int32_t>& te_team_pts() const override { return ws_->campaign.te_team_pts; }

private:
    const WorldState* ws_;
};

struct TeamAdapter : ITeamSource {
    explicit TeamAdapter(const WorldState& ws) : ws_(&ws) {}

    int team_count() const override { return static_cast<int>(ws_->teams.size()); }
    int slot(int i) const override { return ws_->teams[i].slot; }
    uint8_t flags(int i) const override { return ws_->teams[i].flags; }
    uint8_t colour(int i) const override { return ws_->teams[i].colour; }
    const std::string& name(int i) const override { return ws_->teams[i].name; }
    const std::string& motto(int i) const override { return ws_->teams[i].motto; }
    bool tea_loaded(int i) const override { return ws_->teams[i].tea_loaded; }
    const std::vector<int16_t>& stance(int i) const override { return ws_->teams[i].stance; }
    const std::vector<uint8_t>& member(int i) const override { return ws_->teams[i].member; }
    uint8_t air_experience(int i) const override { return ws_->teams[i].air_experience; }
    uint8_t ground_experience(int i) const override { return ws_->teams[i].ground_experience; }
    uint8_t naval_experience(int i) const override { return ws_->teams[i].naval_experience; }
    uint8_t air_defense_experience(int i) const override { return ws_->teams[i].air_defense_experience; }
    int16_t first_colonel(int i) const override { return ws_->teams[i].first_colonel; }
    int16_t first_commander(int i) const override { return ws_->teams[i].first_commander; }
    int16_t first_wingman(int i) const override { return ws_->teams[i].first_wingman; }
    int16_t last_wingman(int i) const override { return ws_->teams[i].last_wingman; }

private:
    const WorldState* ws_;
};

struct ObjectiveAdapter : IObjectiveSource {
    explicit ObjectiveAdapter(const WorldState& ws) : ws_(&ws) {}

    int objective_count() const override { return static_cast<int>(ws_->objectives.size()); }
    int16_t x(int i) const override { return ws_->objectives[i].x; }
    int16_t y(int i) const override { return ws_->objectives[i].y; }
    float z(int i) const override { return ws_->objectives[i].z; }
    int16_t type(int i) const override { return ws_->objectives[i].type; }
    uint16_t entity_type(int i) const override { return ws_->objectives[i].entity_type; }
    const std::string& class_name(int i) const override { return ws_->objectives[i].class_name; }
    uint8_t owner(int i) const override { return ws_->objectives[i].owner; }
    uint8_t first_owner(int i) const override { return ws_->objectives[i].first_owner; }
    uint8_t priority(int i) const override { return ws_->objectives[i].priority; }
    int16_t nameid(int i) const override { return ws_->objectives[i].nameid; }
    uint32_t obj_flags(int i) const override { return ws_->objectives[i].obj_flags; }
    uint32_t parent_id(int i) const override { return ws_->objectives[i].parent_id; }
    bool has_supply(int i) const override {
        const auto& o = ws_->objectives[i];
        return o.supply != 0 || o.fuel != 0 || o.losses != 0 || o.last_repair != 0;
    }
    uint8_t supply(int i) const override { return ws_->objectives[i].supply; }
    uint8_t fuel(int i) const override { return ws_->objectives[i].fuel; }
    uint8_t losses(int i) const override { return ws_->objectives[i].losses; }
    int32_t last_repair(int i) const override { return ws_->objectives[i].last_repair; }
    bool has_fstatus(int i) const override { return !ws_->objectives[i].fstatus.empty(); }
    const std::vector<uint8_t>& fstatus(int i) const override { return ws_->objectives[i].fstatus; }
    bool has_radar(int i) const override { return ws_->objectives[i].has_radar; }
    const float* detect_ratio(int i) const override { return ws_->objectives[i].detect_ratio; }
    float radar_range_km(int i) const override { return ws_->objectives[i].radar_range_km; }
    const std::string& radar_name(int i) const override { return ws_->objectives[i].radar_name; }
    int16_t radar_type_idx(int i) const override { return ws_->objectives[i].radar_type_idx; }
    bool has_links(int i) const override { return !ws_->objectives[i].links.empty(); }
    const std::vector<f4::entities::ObjectiveLink>& links(int i) const override { return ws_->objectives[i].links; }
    bool has_ground_layout(int i) const override { return !ws_->objectives[i].ground_layout.empty(); }
    const std::vector<f4::entities::GroundLayoutList>& ground_layout(int i) const override { return ws_->objectives[i].ground_layout; }
    bool has_features(int i) const override {
        const auto& o = ws_->objectives[i];
        return o.features_count > 0 || !o.features.empty();
    }
    uint8_t features_count(int i) const override { return ws_->objectives[i].features_count; }
    uint8_t radar_feature(int i) const override { return ws_->objectives[i].radar_feature; }
    uint8_t deag_distance(int i) const override { return ws_->objectives[i].deag_distance; }
    uint16_t pt_data_index(int i) const override { return ws_->objectives[i].pt_data_index; }
    const std::array<uint8_t, 8>& objective_detection(int i) const override { return ws_->objectives[i].objective_detection; }
    const std::vector<f4::entities::FeatureEntryState>& features(int i) const override { return ws_->objectives[i].features; }
    uint32_t id_num(int i) const override { return ws_->objectives[i].id_num; }
    uint32_t id_creator(int i) const override { return ws_->objectives[i].id_creator; }
    int16_t camp_id(int i) const override { return ws_->objectives[i].camp_id; }
    uint8_t objective_type(int i) const override { return ws_->objectives[i].objective_type; }

private:
    const WorldState* ws_;
};

// ============================================================================
// UnitAdapter — implements ALL unit interfaces (core + 4 subclass).
//
// The WorldState UnitState struct is a flat god-struct with all fields for
// all subclasses, zero-initialized for unused ones. This adapter therefore
// implements every interface trivially — each method reads the field
// directly from UnitState, regardless of unit class.
//
// The as_*() methods return `this` when unit_class(i) matches the subclass,
// nullptr otherwise. A future adapter (BMS, procedural) that only supports
// some subclasses would return nullptr for the others without having to
// implement the unsupported interface at all.
// ============================================================================
struct UnitAdapter : IUnitCoreSource,
                     IGroundUnitSource,
                     ISquadronSource,
                     IFlightSource,
                     IPackageSource {
    explicit UnitAdapter(const WorldState& ws) : ws_(&ws) {}

    // --- IUnitCoreSource ---
    int unit_count() const override { return static_cast<int>(ws_->units.size()); }
    int16_t x(int i) const override { return ws_->units[i].x; }
    int16_t y(int i) const override { return ws_->units[i].y; }
    float z(int i) const override { return ws_->units[i].z; }
    f4::entities::UnitClass unit_class(int i) const override { return ws_->units[i].unit_class; }
    uint8_t domain(int i) const override { return ws_->units[i].domain; }
    uint8_t unit_subtype(int i) const override { return ws_->units[i].unit_subtype; }
    uint16_t entity_type(int i) const override { return ws_->units[i].entity_type; }
    uint32_t roster(int i) const override { return ws_->units[i].roster; }
    const std::string& class_name(int i) const override { return ws_->units[i].class_name; }
    uint8_t owner(int i) const override { return ws_->units[i].owner; }
    bool has_waypoints(int i) const override { return !ws_->units[i].waypoints.empty(); }
    const std::vector<f4::entities::WaypointState>& waypoints(int i) const override { return ws_->units[i].waypoints; }
    bool has_vehicle_groups(int i) const override { return !ws_->units[i].vehicle_groups.empty(); }
    const std::vector<f4::entities::VehicleGroup>& vehicle_groups(int i) const override { return ws_->units[i].vehicle_groups; }
    const std::array<uint8_t, 16>& unit_class_scores(int i) const override { return ws_->units[i].unit_class_scores; }
    uint32_t id_num(int i) const override { return ws_->units[i].id_num; }
    uint32_t id_creator(int i) const override { return ws_->units[i].id_creator; }
    int16_t camp_id(int i) const override { return ws_->units[i].camp_id; }
    int16_t name_id(int i) const override { return ws_->units[i].name_id; }
    int16_t reinforcement(int i) const override { return ws_->units[i].reinforcement; }
    int16_t dest_x(int i) const override { return ws_->units[i].dest_x; }
    int16_t dest_y(int i) const override { return ws_->units[i].dest_y; }
    int32_t movement_type(int i) const override { return ws_->units[i].movement_type; }
    int16_t movement_speed(int i) const override { return ws_->units[i].movement_speed; }
    int16_t max_range(int i) const override { return ws_->units[i].max_range; }
    const std::string& movement_type_name(int i) const override { return ws_->units[i].movement_type_name; }
    uint8_t losses(int i) const override { return ws_->units[i].losses; }
    uint8_t wp_count(int i) const override { return ws_->units[i].wp_count; }
    uint8_t elements(int i) const override { return ws_->units[i].elements; }

    // --- Subclass accessors ---
    // WorldState-backed adapter implements all subclass interfaces, so
    // return `this` when the class matches. The ambiguous base class is
    // resolved by the explicit static_cast.
    const IGroundUnitSource* as_ground_unit(int i) const override {
        using f4::entities::UnitClass;
        const auto uc = unit_class(i);
        return (uc == UnitClass::Battalion || uc == UnitClass::Brigade || uc == UnitClass::TaskForce)
            ? static_cast<const IGroundUnitSource*>(this) : nullptr;
    }
    const ISquadronSource* as_squadron(int i) const override {
        return (unit_class(i) == f4::entities::UnitClass::Squadron)
            ? static_cast<const ISquadronSource*>(this) : nullptr;
    }
    const IFlightSource* as_flight(int i) const override {
        return (unit_class(i) == f4::entities::UnitClass::Flight)
            ? static_cast<const IFlightSource*>(this) : nullptr;
    }
    const IPackageSource* as_package(int i) const override {
        return (unit_class(i) == f4::entities::UnitClass::Package)
            ? static_cast<const IPackageSource*>(this) : nullptr;
    }

    // --- IGroundUnitSource ---
    uint8_t supply(int i) const override { return ws_->units[i].supply; }
    uint8_t morale(int i) const override { return ws_->units[i].morale; }
    uint8_t fatigue(int i) const override { return ws_->units[i].fatigue; }
    uint8_t heading(int i) const override { return ws_->units[i].heading; }
    uint8_t final_heading(int i) const override { return ws_->units[i].final_heading; }
    uint8_t position(int i) const override { return ws_->units[i].position; }
    int32_t last_move(int i) const override { return ws_->units[i].last_move; }
    int32_t last_combat(int i) const override { return ws_->units[i].last_combat; }
    uint32_t parent_id(int i) const override { return ws_->units[i].parent_id; }
    const std::vector<uint32_t>& element_ids(int i) const override { return ws_->units[i].element_ids; }

    // --- ISquadronSource ---
    uint32_t airbase_id(int i) const override { return ws_->units[i].airbase_id; }
    uint8_t specialty(int i) const override { return ws_->units[i].specialty; }
    int16_t aa_kills(int i) const override { return ws_->units[i].aa_kills; }
    int16_t ag_kills(int i) const override { return ws_->units[i].ag_kills; }
    int16_t as_kills(int i) const override { return ws_->units[i].as_kills; }
    int16_t an_kills(int i) const override { return ws_->units[i].an_kills; }
    int16_t missions_flown(int i) const override { return ws_->units[i].missions_flown; }
    int16_t mission_score(int i) const override { return ws_->units[i].mission_score; }
    uint8_t total_losses(int i) const override { return ws_->units[i].total_losses; }
    uint8_t pilot_losses(int i) const override { return ws_->units[i].pilot_losses; }
    uint8_t squadron_patch(int i) const override { return ws_->units[i].squadron_patch; }
    int32_t fuel(int i) const override { return ws_->units[i].fuel; }
    const std::vector<f4::entities::PilotState>& pilots(int i) const override { return ws_->units[i].pilots; }

    // --- IFlightSource ---
    float flight_altitude(int i) const override { return ws_->units[i].flight_altitude; }
    int32_t fuel_burnt(int i) const override { return ws_->units[i].fuel_burnt; }
    int32_t time_on_target(int i) const override { return ws_->units[i].time_on_target; }
    int32_t mission_over_time(int i) const override { return ws_->units[i].mission_over_time; }
    int16_t mission_target(int i) const override { return ws_->units[i].mission_target; }
    uint8_t loadouts(int i) const override { return ws_->units[i].loadouts; }
    uint8_t mission(int i) const override { return ws_->units[i].mission; }
    uint8_t flight_priority(int i) const override { return ws_->units[i].flight_priority; }
    uint8_t mission_id(int i) const override { return ws_->units[i].mission_id; }
    uint8_t eval_flags(int i) const override { return ws_->units[i].eval_flags; }
    uint32_t package_id(int i) const override { return ws_->units[i].package_id; }
    uint32_t squadron_id(int i) const override { return ws_->units[i].squadron_id; }
    uint8_t callsign_id(int i) const override { return ws_->units[i].callsign_id; }
    uint8_t callsign_num(int i) const override { return ws_->units[i].callsign_num; }

    // --- IPackageSource ---
    uint8_t wait_cycles(int i) const override { return ws_->units[i].wait_cycles; }
    uint32_t interceptor_id(int i) const override { return ws_->units[i].interceptor_id; }
    uint32_t awacs_id(int i) const override { return ws_->units[i].awacs_id; }
    uint32_t jstar_id(int i) const override { return ws_->units[i].jstar_id; }
    uint32_t ecm_id(int i) const override { return ws_->units[i].ecm_id; }
    uint32_t tanker_id(int i) const override { return ws_->units[i].tanker_id; }

private:
    const WorldState* ws_;
};

struct WorldStateAdapters {
    CampaignAdapter campaign;
    TeamAdapter teams;
    ObjectiveAdapter objectives;
    UnitAdapter units;

    explicit WorldStateAdapters(const WorldState& ws)
        : campaign(ws), teams(ws), objectives(ws), units(ws) {}
};

// ============================================================================
// Grid-to-feet conversion
//
// In the Falcon 4.0 campaign, x and y are grid indices (int16). One grid
// unit = 1024 ft. Altitude (z) is already in feet.
// ============================================================================
static constexpr double FT_PER_GRID = 1024.0;

static f4::geo::WorldPosition grid_to_feet(int16_t gx, int16_t gy, float gz) {
    return f4::geo::WorldPosition{
        static_cast<double>(gx) * FT_PER_GRID,
        static_cast<double>(gy) * FT_PER_GRID,
        static_cast<double>(gz)
    };
}

/// Map a VU_DOMAIN byte to a human-readable domain tag value.
static const char* domain_name(uint8_t domain) {
    switch (domain) {
        case 2:  return "air";
        case 3:  return "ground";
        case 4:  return "naval";
        default: return "unknown";
    }
}

// ============================================================================
// populate_campaign — interface-based (Phase 4 primary)
// ============================================================================
EntityId populate_campaign(EntityWorld& world, const ICampaignSource& src) {
    auto h = world.create();
    h.set_tag(tags::ROLE, TagValue::from(std::string("campaign")));
    h.set_tag(tags::ALIVE, TagValue::from(true));

    auto& cs = h.add<CampaignStateComponent>();
    cs.current_time       = src.current_time();
    cs.te_start_time      = src.te_start_time();
    cs.te_time_limit      = src.te_time_limit();
    cs.te_victory_points  = src.te_victory_points();
    cs.te_type            = src.te_type();
    cs.te_number_teams    = src.te_number_teams();
    cs.te_team            = src.te_team();
    cs.te_flags           = src.te_flags();
    cs.te_number_aircraft = src.te_number_aircraft();
    cs.te_team_pts        = src.te_team_pts();

    return h.id();
}

// ============================================================================
// populate_campaign — WorldState convenience wrapper
// ============================================================================
EntityId populate_campaign(EntityWorld& world, const WorldState& ws) {
    CampaignAdapter adapter(ws);
    return populate_campaign(world, static_cast<const ICampaignSource&>(adapter));
}

// ============================================================================
// populate_teams — interface-based (Phase 4 primary)
// ============================================================================
std::vector<EntityId> populate_teams(EntityWorld& world, const ITeamSource& src) {
    std::vector<EntityId> ids;
    for (int i = 0; i < src.team_count(); ++i) {
        // Skip slots with empty names (neutral/unused).
        if (src.name(i).empty()) continue;

        auto h = world.create();
        h.set_tag(tags::ROLE, TagValue::from(std::string("team")));
        h.set_tag(tags::TEAM, TagValue::from(src.name(i)));
        h.set_tag(tags::ALIVE, TagValue::from(true));

        // Narrowed CampaignIdentityComponent — only team_id + callsign.
        auto& cid = h.add<CampaignIdentityComponent>();
        cid.team_id = src.slot(i);
        cid.callsign = src.name(i);

        // TeamComponent — carries all team-specific data including .tea
        // enrichment (stance, member, experience, pilot slots).
        auto& tc = h.add<TeamComponent>();
        tc.slot = src.slot(i);
        tc.flags = src.flags(i);
        tc.colour = src.colour(i);
        tc.motto = src.motto(i);
        tc.stance = src.stance(i);
        tc.member = src.member(i);
        tc.air_experience = src.air_experience(i);
        tc.ground_experience = src.ground_experience(i);
        tc.naval_experience = src.naval_experience(i);
        tc.air_defense_experience = src.air_defense_experience(i);
        tc.first_colonel = src.first_colonel(i);
        tc.first_commander = src.first_commander(i);
        tc.first_wingman = src.first_wingman(i);
        tc.last_wingman = src.last_wingman(i);

        ids.push_back(h.id());
    }
    return ids;
}

// ============================================================================
// populate_teams — WorldState convenience wrapper
// ============================================================================
std::vector<EntityId> populate_teams(EntityWorld& world, const WorldState& ws) {
    TeamAdapter adapter(ws);
    return populate_teams(world, static_cast<const ITeamSource&>(adapter));
}

// ============================================================================
// populate_objectives — interface-based (Phase 4 primary)
// ============================================================================
std::vector<EntityId> populate_objectives(
    EntityWorld& world,
    const IObjectiveSource& src,
    std::unordered_map<uint32_t, EntityId>& obj_id_map)
{
    std::vector<EntityId> ids;
    ids.reserve(static_cast<size_t>(src.objective_count()));

    for (int i = 0; i < src.objective_count(); ++i) {
        auto h = world.create();

        // --- Tags ---
        h.set_tag(tags::ROLE, TagValue::from(std::string("objective")));
        h.set_tag(tags::TEAM, TagValue::from(static_cast<int64_t>(src.owner(i))));
        h.set_tag(tags::ALIVE, TagValue::from(true));

        // --- Transform (grid → feet) ---
        auto& tf = h.add<TransformComponent>();
        tf.position = grid_to_feet(src.x(i), src.y(i), src.z(i));

        // --- Always-present objective components ---
        auto& ot = h.add<ObjectiveTypeComponent>();
        ot.type = src.type(i);
        ot.class_table_index = static_cast<int16_t>(src.entity_type(i));
        ot.class_name = src.class_name(i);

        auto& own = h.add<OwnershipComponent>();
        own.team = src.owner(i);
        own.first_owner = src.first_owner(i);

        auto& pri = h.add<ObjectivePriorityComponent>();
        pri.priority = src.priority(i);
        pri.nameid = src.nameid(i);
        pri.obj_flags = src.obj_flags(i);
        pri.parent_id = src.parent_id(i);

        // --- Conditional components ---

        // Supply/fuel/losses — add when supply data is present
        if (src.has_supply(i)) {
            auto& sup = h.add<SupplyStateComponent>();
            sup.supply = src.supply(i);
            sup.fuel = src.fuel(i);
            sup.losses = src.losses(i);
            sup.last_repair = src.last_repair(i);
        }

        // Damage bitmap — add when features have damage data
        if (src.has_fstatus(i)) {
            auto& dmg = h.add<DamageBitmapComponent>();
            dmg.fstatus = src.fstatus(i);
        }

        // Radar — only when the objective has radar
        if (src.has_radar(i)) {
            auto& rad = h.add<RadarComponent>();
            const float* dr = src.detect_ratio(i);
            if (dr) {
                for (int j = 0; j < 8; ++j) rad.detect_ratio[j] = dr[j];
            }
            rad.range_km = src.radar_range_km(i);
            rad.name = src.radar_name(i);
            rad.radar_type_idx = src.radar_type_idx(i);
        }

        // Network links — when the objective has road/rail connections
        if (src.has_links(i)) {
            auto& nl = h.add<NetworkLinksComponent>();
            nl.links = src.links(i);
        }

        // Ground layout — when the objective is an airbase with layout data
        if (src.has_ground_layout(i)) {
            auto& gl = h.add<GroundLayoutComponent>();
            gl.layouts = src.ground_layout(i);
        }

        // Feature set — when the objective has features (buildings, structures)
        if (src.has_features(i)) {
            auto& fs = h.add<FeatureSetComponent>();
            fs.features_count = src.features_count(i);
            fs.radar_feature = src.radar_feature(i);
            fs.deag_distance = src.deag_distance(i);
            fs.pt_data_index = src.pt_data_index(i);
            fs.objective_detection = src.objective_detection(i);
            fs.features = src.features(i);
        }

        // --- PropertyBag for format residue ---
        {
            auto& pb = h.add<PropertyBag>();
            pb.ints["vu_id_creator"] = static_cast<int64_t>(src.id_creator(i));
            pb.ints["vu_id_num"]     = static_cast<int64_t>(src.id_num(i));
            pb.ints["entity_type"]   = static_cast<int64_t>(src.entity_type(i));
            pb.ints["camp_id"]       = static_cast<int64_t>(src.camp_id(i));
            pb.ints["objective_type"]= static_cast<int64_t>(src.objective_type(i));
        }

        // Build VU_ID→EntityId map for cross-reference resolution
        if (src.id_num(i) != 0) {
            obj_id_map[src.id_num(i)] = h.id();
        }

        ids.push_back(h.id());
    }
    return ids;
}

// ============================================================================
// populate_objectives — WorldState convenience wrapper
// ============================================================================
std::vector<EntityId> populate_objectives(
    EntityWorld& world,
    const WorldState& ws,
    std::unordered_map<uint32_t, EntityId>& obj_id_map)
{
    ObjectiveAdapter adapter(ws);
    return populate_objectives(world,
                               static_cast<const IObjectiveSource&>(adapter),
                               obj_id_map);
}

// ============================================================================
// populate_units — interface-based (Phase 4 primary)
// ============================================================================
std::vector<EntityId> populate_units(
    EntityWorld& world,
    const IUnitCoreSource& src,
    const std::unordered_map<uint32_t, EntityId>& obj_id_map,
    std::unordered_map<uint32_t, EntityId>& unit_id_map)
{
    std::vector<EntityId> ids;
    ids.reserve(static_cast<size_t>(src.unit_count()));

    // --- First pass: create all unit entities ---
    for (int i = 0; i < src.unit_count(); ++i) {
        auto h = world.create();

        // --- Tags ---
        h.set_tag(tags::ROLE, TagValue::from(std::string(unit_class_name(src.unit_class(i)))));
        h.set_tag(tags::TEAM, TagValue::from(static_cast<int64_t>(src.owner(i))));
        h.set_tag(tags::OPDOMAIN, TagValue::from(std::string(domain_name(src.domain(i)))));
        h.set_tag(tags::ALIVE, TagValue::from(true));

        // --- Transform (grid → feet) ---
        auto& tf = h.add<TransformComponent>();
        tf.position = grid_to_feet(src.x(i), src.y(i), src.z(i));

        // --- UnitCoreComponent (all units) ---
        auto& uc = h.add<UnitCoreComponent>();
        uc.unit_class = src.unit_class(i);
        uc.domain = src.domain(i);
        uc.unit_subtype = src.unit_subtype(i);
        uc.class_table_index = static_cast<int16_t>(src.entity_type(i));
        uc.roster = src.roster(i);
        uc.class_name = src.class_name(i);

        // --- Subclass-specific components ---
        // Each subclass interface is queried via as_*(). A nullptr return
        // means the unit isn't of that class — the corresponding component
        // is simply not added. A future adapter that doesn't support a
        // subclass returns nullptr and the bridge skips it gracefully.

        // Ground unit (Battalion / Brigade / TaskForce)
        if (auto* gu = src.as_ground_unit(i)) {
            auto& gt = h.add<GroundTacticalComponent>();
            gt.supply = gu->supply(i);
            gt.morale = gu->morale(i);
            gt.fatigue = gu->fatigue(i);
            gt.heading = gu->heading(i);
            gt.final_heading = gu->final_heading(i);
            gt.position = gu->position(i);
            gt.last_move = gu->last_move(i);
            gt.last_combat = gu->last_combat(i);

            // Hierarchy — Battalion has parent brigade, Brigade has child battalions.
            if (gu->parent_id(i) != 0 || !gu->element_ids(i).empty()) {
                h.add<HierarchyComponent>();
            }
        }

        // Squadron
        if (auto* sq_src = src.as_squadron(i)) {
            auto& sq = h.add<SquadronComponent>();
            sq.specialty = sq_src->specialty(i);
            sq.aa_kills = sq_src->aa_kills(i);
            sq.ag_kills = sq_src->ag_kills(i);
            sq.as_kills = sq_src->as_kills(i);
            sq.an_kills = sq_src->an_kills(i);
            sq.missions_flown = sq_src->missions_flown(i);
            sq.mission_score = sq_src->mission_score(i);
            sq.total_losses = sq_src->total_losses(i);
            sq.pilot_losses = sq_src->pilot_losses(i);
            sq.squadron_patch = sq_src->squadron_patch(i);
            sq.fuel = sq_src->fuel(i);
            sq.pilots = sq_src->pilots(i);

            // Resolve Squadron→airbase cross-reference
            if (sq_src->airbase_id(i) != 0) {
                auto it = obj_id_map.find(sq_src->airbase_id(i));
                if (it != obj_id_map.end()) {
                    sq.airbase = it->second;
                }
            }
        }

        // Flight
        if (auto* fl = src.as_flight(i)) {
            auto& fp = h.add<FlightPlanComponent>();
            fp.altitude = fl->flight_altitude(i);
            fp.fuel_burnt = fl->fuel_burnt(i);
            fp.time_on_target = fl->time_on_target(i);
            fp.mission_over_time = fl->mission_over_time(i);
            fp.mission_target = fl->mission_target(i);
            fp.loadouts = fl->loadouts(i);
            fp.mission = fl->mission(i);
            fp.flight_priority = fl->flight_priority(i);
            fp.mission_id = fl->mission_id(i);
            fp.eval_flags = fl->eval_flags(i);
            fp.callsign_id = fl->callsign_id(i);
            fp.callsign_num = fl->callsign_num(i);
            // Cross-references (package, squadron) resolved in second pass.
        }

        // Package
        if (auto* pk = src.as_package(i)) {
            auto& ps = h.add<PackageSupportComponent>();
            ps.wait_cycles = pk->wait_cycles(i);
            // Cross-references resolved in second pass.
        }

        // --- Conditional components ---

        // Waypoint plan — when the unit has waypoints
        if (src.has_waypoints(i)) {
            auto& wp = h.add<WaypointPlanComponent>();
            wp.waypoints = src.waypoints(i);
        }

        // Vehicle composition — when the unit has vehicle groups
        if (src.has_vehicle_groups(i)) {
            auto& vc = h.add<VehicleCompositionComponent>();
            vc.groups = src.vehicle_groups(i);
        }

        // Unit class scores — when any score is non-zero
        {
            const auto& scores = src.unit_class_scores(i);
            bool has_scores = false;
            for (auto s : scores) {
                if (s != 0) { has_scores = true; break; }
            }
            if (has_scores) {
                auto& ucs = h.add<UnitClassScoreComponent>();
                ucs.scores = scores;
            }
        }

        // --- MovementOrdersComponent (conditional) ---
        // Promoted from PropertyBag residue (Phase 5 cleanup). These fields
        // have clear domain meaning — they describe where a ground unit is
        // ordered to move and how fast it can get there. Only add the
        // component when at least one field is non-zero/non-empty.
        {
            const int16_t dx = src.dest_x(i);
            const int16_t dy = src.dest_y(i);
            const int32_t mt = src.movement_type(i);
            const int16_t ms = src.movement_speed(i);
            const int16_t mr = src.max_range(i);
            const std::string& mtn = src.movement_type_name(i);
            if (dx != 0 || dy != 0 || mt != 0 || ms != 0 || mr != 0 || !mtn.empty()) {
                auto& mo = h.add<MovementOrdersComponent>();
                mo.dest_x = dx;
                mo.dest_y = dy;
                mo.movement_type = mt;
                mo.movement_speed = ms;
                mo.max_range = mr;
                mo.movement_type_name = mtn;
            }
        }

        // --- PropertyBag for format residue (genuinely unclassified fields) ---
        {
            auto& pb = h.add<PropertyBag>();
            pb.ints["vu_id_creator"] = static_cast<int64_t>(src.id_creator(i));
            pb.ints["vu_id_num"]     = static_cast<int64_t>(src.id_num(i));
            pb.ints["entity_type"]   = static_cast<int64_t>(src.entity_type(i));
            pb.ints["camp_id"]       = static_cast<int64_t>(src.camp_id(i));
            pb.ints["name_id"]       = static_cast<int64_t>(src.name_id(i));
            pb.ints["reinforcement"] = static_cast<int64_t>(src.reinforcement(i));
            pb.ints["losses"]        = static_cast<int64_t>(src.losses(i));
            pb.ints["wp_count"]      = static_cast<int64_t>(src.wp_count(i));
            // Store elements count for Brigade (ground unit with children)
            if (auto* gu = src.as_ground_unit(i)) {
                if (src.unit_class(i) == UnitClass::Brigade) {
                    pb.ints["elements"] = static_cast<int64_t>(gu->element_ids(i).size());
                }
            }
        }

        // Build VU_ID→EntityId map
        if (src.id_num(i) != 0) {
            unit_id_map[src.id_num(i)] = h.id();
        }

        ids.push_back(h.id());
    }

    // --- Second pass: resolve unit→unit cross-references ---
    // Now that all units have EntityIds, resolve Flight→Package, Flight→Squadron,
    // Battalion→Brigade, and Package support flights.
    //
    // The raw VU_IDs are queried from the subclass interfaces directly
    // (gu->parent_id(i), fl->package_id(i), pk->interceptor_id(i), etc.)
    // rather than being stored on the components. This eliminates the
    // "is this raw _id field live or stale?" ambiguity that existed when
    // the components carried both the resolved EntityId and the raw VU_ID.
    for (int i = 0; i < src.unit_count(); ++i) {
        EntityHandle h(ids[static_cast<size_t>(i)], &world);

        // Ground unit hierarchy (Battalion→Brigade, Brigade→children)
        if (auto* gu = src.as_ground_unit(i)) {
            auto* hier = h.get<HierarchyComponent>();
            if (hier) {
                // Resolve parent (battalion→brigade)
                const uint32_t pid = gu->parent_id(i);
                if (pid != 0) {
                    auto it = unit_id_map.find(pid);
                    if (it != unit_id_map.end()) {
                        hier->parent = it->second;
                    }
                }
                // Resolve children (brigade→battalions)
                const auto& elems = gu->element_ids(i);
                if (!elems.empty()) {
                    hier->children.clear();
                    hier->children.reserve(elems.size());
                    for (auto eid : elems) {
                        auto it = unit_id_map.find(eid);
                        if (it != unit_id_map.end()) {
                            hier->children.push_back(it->second);
                        }
                    }
                }
            }
        }

        // Flight → Package and Squadron
        if (auto* fl = src.as_flight(i)) {
            auto* fp = h.get<FlightPlanComponent>();
            if (fp) {
                const uint32_t pkg_id = fl->package_id(i);
                if (pkg_id != 0) {
                    auto it = unit_id_map.find(pkg_id);
                    if (it != unit_id_map.end()) {
                        fp->package = it->second;
                    }
                }
                const uint32_t sqn_id = fl->squadron_id(i);
                if (sqn_id != 0) {
                    auto it = unit_id_map.find(sqn_id);
                    if (it != unit_id_map.end()) {
                        fp->squadron = it->second;
                    }
                }
            }
        }

        // Package → support flights
        if (auto* pk = src.as_package(i)) {
            auto* ps = h.get<PackageSupportComponent>();
            if (ps) {
                const uint32_t int_id = pk->interceptor_id(i);
                if (int_id != 0) {
                    auto it = unit_id_map.find(int_id);
                    if (it != unit_id_map.end()) ps->interceptor = it->second;
                }
                const uint32_t aw_id = pk->awacs_id(i);
                if (aw_id != 0) {
                    auto it = unit_id_map.find(aw_id);
                    if (it != unit_id_map.end()) ps->awacs = it->second;
                }
                const uint32_t js_id = pk->jstar_id(i);
                if (js_id != 0) {
                    auto it = unit_id_map.find(js_id);
                    if (it != unit_id_map.end()) ps->jstar = it->second;
                }
                const uint32_t ecm_id = pk->ecm_id(i);
                if (ecm_id != 0) {
                    auto it = unit_id_map.find(ecm_id);
                    if (it != unit_id_map.end()) ps->ecm = it->second;
                }
                const uint32_t tnk_id = pk->tanker_id(i);
                if (tnk_id != 0) {
                    auto it = unit_id_map.find(tnk_id);
                    if (it != unit_id_map.end()) ps->tanker = it->second;
                }
            }
        }
    }

    return ids;
}

// ============================================================================
// populate_units — WorldState convenience wrapper
// ============================================================================
std::vector<EntityId> populate_units(
    EntityWorld& world,
    const WorldState& ws,
    const std::unordered_map<uint32_t, EntityId>& obj_id_map,
    std::unordered_map<uint32_t, EntityId>& unit_id_map)
{
    UnitAdapter adapter(ws);
    return populate_units(world,
                          static_cast<const IUnitCoreSource&>(adapter),
                          obj_id_map,
                          unit_id_map);
}

// ============================================================================
// populate_world — interface-based (Phase 4 primary)
// ============================================================================
PopulatedWorld populate_world(EntityWorld& world,
                              const ICampaignSource& camp_src,
                              const ITeamSource& team_src,
                              const IObjectiveSource& obj_src,
                              const IUnitCoreSource& unit_src)
{
    PopulatedWorld pw;

    pw.campaign   = populate_campaign(world, camp_src);
    pw.teams      = populate_teams(world, team_src);
    pw.objectives = populate_objectives(world, obj_src, pw.objective_id_map);
    pw.units      = populate_units(world, unit_src, pw.objective_id_map, pw.unit_id_map);

    return pw;
}

// ============================================================================
// populate_world — WorldState convenience wrapper
// ============================================================================
PopulatedWorld populate_world(EntityWorld& world, const WorldState& ws) {
    WorldStateAdapters adapters(ws);
    return populate_world(world,
                          static_cast<const ICampaignSource&>(adapters.campaign),
                          static_cast<const ITeamSource&>(adapters.teams),
                          static_cast<const IObjectiveSource&>(adapters.objectives),
                          static_cast<const IUnitCoreSource&>(adapters.units));
}

// ============================================================================
// Convenience API — load from file
// ============================================================================
PopulatedWorld load(const std::filesystem::path& json_path,
                    EntityWorld& world)
{
    WorldState ws;
    ws.load(json_path);

    WorldStateAdapters adapters(ws);
    return populate_world(world,
                          static_cast<const ICampaignSource&>(adapters.campaign),
                          static_cast<const ITeamSource&>(adapters.teams),
                          static_cast<const IObjectiveSource&>(adapters.objectives),
                          static_cast<const IUnitCoreSource&>(adapters.units));
}

// ============================================================================
// Convenience API — load from string
// ============================================================================
PopulatedWorld load_from_string(const std::string& json,
                                EntityWorld& world)
{
    WorldState ws;
    ws.load_from_string(json);

    WorldStateAdapters adapters(ws);
    return populate_world(world,
                          static_cast<const ICampaignSource&>(adapters.campaign),
                          static_cast<const ITeamSource&>(adapters.teams),
                          static_cast<const IObjectiveSource&>(adapters.objectives),
                          static_cast<const IUnitCoreSource&>(adapters.units));
}

} // namespace f4::world
