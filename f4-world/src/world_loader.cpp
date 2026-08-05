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

#include <f4/world/world_loader.hpp>
#include <f4/geo/constants.hpp>

namespace f4::world {

using namespace f4::entities;

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
    const IUnitSource& src,
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
        switch (src.unit_class(i)) {
            case UnitClass::Battalion:
            case UnitClass::Brigade:
            case UnitClass::TaskForce: {
                // Ground tactical state
                auto& gt = h.add<GroundTacticalComponent>();
                gt.supply = src.supply(i);
                gt.morale = src.morale(i);
                gt.fatigue = src.fatigue(i);
                gt.heading = src.heading(i);
                gt.final_heading = src.final_heading(i);
                gt.position = src.position(i);
                gt.last_move = src.last_move(i);
                gt.last_combat = src.last_combat(i);

                // Hierarchy — Battalion has parent brigade, Brigade has child battalions
                if (src.parent_id(i) != 0 || !src.element_ids(i).empty()) {
                    auto& hier = h.add<HierarchyComponent>();
                    hier.parent_id = src.parent_id(i);
                    hier.element_ids = src.element_ids(i);
                }
                break;
            }
            case UnitClass::Squadron: {
                auto& sq = h.add<SquadronComponent>();
                sq.airbase_id = src.airbase_id(i);
                sq.specialty = src.specialty(i);
                sq.aa_kills = src.aa_kills(i);
                sq.ag_kills = src.ag_kills(i);
                sq.as_kills = src.as_kills(i);
                sq.an_kills = src.an_kills(i);
                sq.missions_flown = src.missions_flown(i);
                sq.mission_score = src.mission_score(i);
                sq.total_losses = src.total_losses(i);
                sq.pilot_losses = src.pilot_losses(i);
                sq.squadron_patch = src.squadron_patch(i);
                sq.fuel = src.fuel(i);
                sq.pilots = src.pilots(i);

                // Resolve Squadron→airbase cross-reference
                if (src.airbase_id(i) != 0) {
                    auto it = obj_id_map.find(src.airbase_id(i));
                    if (it != obj_id_map.end()) {
                        sq.airbase = it->second;
                    }
                }
                break;
            }
            case UnitClass::Flight: {
                auto& fp = h.add<FlightPlanComponent>();
                fp.altitude = src.flight_altitude(i);
                fp.fuel_burnt = src.fuel_burnt(i);
                fp.time_on_target = src.time_on_target(i);
                fp.mission_over_time = src.mission_over_time(i);
                fp.mission_target = src.mission_target(i);
                fp.loadouts = src.loadouts(i);
                fp.mission = src.mission(i);
                fp.flight_priority = src.flight_priority(i);
                fp.mission_id = src.mission_id(i);
                fp.eval_flags = src.eval_flags(i);
                fp.callsign_id = src.callsign_id(i);
                fp.callsign_num = src.callsign_num(i);
                // VU_ID cross-references — resolved in second pass
                fp.package_id = src.package_id(i);
                fp.squadron_id = src.squadron_id(i);
                break;
            }
            case UnitClass::Package: {
                auto& ps = h.add<PackageSupportComponent>();
                ps.wait_cycles = src.wait_cycles(i);
                ps.interceptor_id = src.interceptor_id(i);
                ps.awacs_id = src.awacs_id(i);
                ps.jstar_id = src.jstar_id(i);
                ps.ecm_id = src.ecm_id(i);
                ps.tanker_id = src.tanker_id(i);
                break;
            }
            default:
                break;
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

        // --- PropertyBag for format residue ---
        {
            auto& pb = h.add<PropertyBag>();
            pb.ints["vu_id_creator"] = static_cast<int64_t>(src.id_creator(i));
            pb.ints["vu_id_num"]     = static_cast<int64_t>(src.id_num(i));
            pb.ints["entity_type"]   = static_cast<int64_t>(src.entity_type(i));
            pb.ints["camp_id"]       = static_cast<int64_t>(src.camp_id(i));
            pb.ints["name_id"]       = static_cast<int64_t>(src.name_id(i));
            pb.ints["reinforcement"] = static_cast<int64_t>(src.reinforcement(i));
            pb.ints["dest_x"]        = static_cast<int64_t>(src.dest_x(i));
            pb.ints["dest_y"]        = static_cast<int64_t>(src.dest_y(i));
            pb.ints["losses"]        = static_cast<int64_t>(src.losses(i));
            pb.ints["wp_count"]      = static_cast<int64_t>(src.wp_count(i));
            // Store elements count for Brigade/Package
            if (src.unit_class(i) == UnitClass::Brigade ||
                src.unit_class(i) == UnitClass::Package) {
                pb.ints["elements"] = static_cast<int64_t>(src.element_ids(i).size());
            }
            if (src.movement_type(i) != 0) {
                pb.ints["movement_type"] = static_cast<int64_t>(src.movement_type(i));
            }
            if (src.movement_speed(i) != 0) {
                pb.ints["movement_speed"] = static_cast<int64_t>(src.movement_speed(i));
            }
            if (src.max_range(i) != 0) {
                pb.ints["max_range"] = static_cast<int64_t>(src.max_range(i));
            }
            if (!src.movement_type_name(i).empty()) {
                pb.strings["movement_type_name"] = src.movement_type_name(i);
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
    for (int i = 0; i < src.unit_count(); ++i) {
        EntityHandle h(ids[static_cast<size_t>(i)], &world);

        switch (src.unit_class(i)) {
            case UnitClass::Battalion:
            case UnitClass::Brigade: {
                // Resolve hierarchy parent (battalion→brigade)
                auto* hier = h.get<HierarchyComponent>();
                if (hier && hier->parent_id != 0) {
                    auto it = unit_id_map.find(hier->parent_id);
                    if (it != unit_id_map.end()) {
                        hier->parent = it->second;
                    }
                }
                // Resolve children (brigade→battalions)
                if (hier && !hier->element_ids.empty()) {
                    hier->children.clear();
                    hier->children.reserve(hier->element_ids.size());
                    for (auto eid : hier->element_ids) {
                        auto it = unit_id_map.find(eid);
                        if (it != unit_id_map.end()) {
                            hier->children.push_back(it->second);
                        }
                    }
                }
                break;
            }
            case UnitClass::Flight: {
                // Resolve Flight→Package and Flight→Squadron
                auto* fp = h.get<FlightPlanComponent>();
                if (fp) {
                    if (fp->package_id != 0) {
                        auto it = unit_id_map.find(fp->package_id);
                        if (it != unit_id_map.end()) {
                            fp->package = it->second;
                        }
                    }
                    if (fp->squadron_id != 0) {
                        auto it = unit_id_map.find(fp->squadron_id);
                        if (it != unit_id_map.end()) {
                            fp->squadron = it->second;
                        }
                    }
                }
                break;
            }
            case UnitClass::Package: {
                // Resolve Package support flight references
                auto* ps = h.get<PackageSupportComponent>();
                if (ps) {
                    if (ps->interceptor_id != 0) {
                        auto it = unit_id_map.find(ps->interceptor_id);
                        if (it != unit_id_map.end()) ps->interceptor = it->second;
                    }
                    if (ps->awacs_id != 0) {
                        auto it = unit_id_map.find(ps->awacs_id);
                        if (it != unit_id_map.end()) ps->awacs = it->second;
                    }
                    if (ps->jstar_id != 0) {
                        auto it = unit_id_map.find(ps->jstar_id);
                        if (it != unit_id_map.end()) ps->jstar = it->second;
                    }
                    if (ps->ecm_id != 0) {
                        auto it = unit_id_map.find(ps->ecm_id);
                        if (it != unit_id_map.end()) ps->ecm = it->second;
                    }
                    if (ps->tanker_id != 0) {
                        auto it = unit_id_map.find(ps->tanker_id);
                        if (it != unit_id_map.end()) ps->tanker = it->second;
                    }
                }
                break;
            }
            default:
                break;
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
                          static_cast<const IUnitSource&>(adapter),
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
                              const IUnitSource& unit_src)
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
                          static_cast<const IUnitSource&>(adapters.units));
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
                          static_cast<const IUnitSource&>(adapters.units));
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
                          static_cast<const IUnitSource&>(adapters.units));
}

} // namespace f4::world
