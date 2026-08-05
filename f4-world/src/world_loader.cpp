// f4-world/src/world_loader.cpp — populate f4-entities from WorldState.
//
// Phase 1: populate_teams adds TeamComponent (with .tea enrichment data) and
// a narrowed CampaignIdentityComponent (team_id + callsign only).
//
// Phase 3: populate_objectives, populate_units, populate_campaign, populate_world
// bridge all entity types. This is the ONLY place where WorldState's format-
// derived fields (VU_ID, nameid, obj_flags) are resolved into domain
// components. Consumers work through EntityWorld, never WorldState directly.

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
// populate_campaign
// ============================================================================
EntityId populate_campaign(EntityWorld& world, const WorldState& ws) {
    auto h = world.create();
    h.set_tag(tags::ROLE, TagValue::from(std::string("campaign")));
    h.set_tag(tags::ALIVE, TagValue::from(true));

    auto& cs = h.add<CampaignStateComponent>();
    cs.current_time       = ws.campaign.current_time;
    cs.te_start_time      = ws.campaign.te_start_time;
    cs.te_time_limit      = ws.campaign.te_time_limit;
    cs.te_victory_points  = ws.campaign.te_victory_points;
    cs.te_type            = ws.campaign.te_type;
    cs.te_number_teams    = ws.campaign.te_number_teams;
    cs.te_team            = ws.campaign.te_team;
    cs.te_flags           = ws.campaign.te_flags;
    cs.te_number_aircraft = ws.campaign.te_number_aircraft;
    cs.te_team_pts        = ws.campaign.te_team_pts;

    return h.id();
}

// ============================================================================
// populate_teams (Phase 1 — unchanged, just reformatted)
// ============================================================================
std::vector<EntityId> populate_teams(EntityWorld& world, const WorldState& ws) {
    std::vector<EntityId> ids;
    for (const auto& t : ws.teams) {
        // Slot 0 with an empty or placeholder name is the neutral/unused slot.
        if (t.name.empty()) continue;

        auto h = world.create();
        h.set_tag(tags::ROLE, TagValue::from(std::string("team")));
        h.set_tag(tags::TEAM, TagValue::from(t.name));
        h.set_tag(tags::ALIVE, TagValue::from(true));

        // Narrowed CampaignIdentityComponent — only team_id + callsign.
        auto& cid = h.add<CampaignIdentityComponent>();
        cid.team_id = t.slot;
        cid.callsign = t.name;

        // TeamComponent — carries all team-specific data including .tea
        // enrichment (stance, member, experience, pilot slots).
        auto& tc = h.add<TeamComponent>();
        tc.slot = t.slot;
        tc.flags = t.flags;
        tc.colour = t.colour;
        tc.motto = t.motto;
        tc.stance = t.stance;
        tc.member = t.member;
        tc.air_experience = t.air_experience;
        tc.ground_experience = t.ground_experience;
        tc.naval_experience = t.naval_experience;
        tc.air_defense_experience = t.air_defense_experience;
        tc.first_colonel = t.first_colonel;
        tc.first_commander = t.first_commander;
        tc.first_wingman = t.first_wingman;
        tc.last_wingman = t.last_wingman;

        ids.push_back(h.id());
    }
    return ids;
}

// ============================================================================
// populate_objectives (Phase 3)
// ============================================================================
std::vector<EntityId> populate_objectives(
    EntityWorld& world,
    const WorldState& ws,
    std::unordered_map<uint32_t, EntityId>& obj_id_map)
{
    std::vector<EntityId> ids;
    ids.reserve(ws.objectives.size());

    for (const auto& o : ws.objectives) {
        auto h = world.create();

        // --- Tags ---
        h.set_tag(tags::ROLE, TagValue::from(std::string("objective")));
        // Owner as a team tag: 0=neutral, 1=enemy, 2=friendly, etc.
        h.set_tag(tags::TEAM, TagValue::from(static_cast<int64_t>(o.owner)));
        h.set_tag(tags::ALIVE, TagValue::from(true));

        // --- Transform (grid → feet) ---
        auto& tf = h.add<TransformComponent>();
        tf.position = grid_to_feet(o.x, o.y, o.z);

        // --- Always-present objective components ---
        auto& ot = h.add<ObjectiveTypeComponent>();
        ot.type = o.type;
        ot.class_table_index = static_cast<int16_t>(o.entity_type);
        ot.class_name = o.class_name;

        auto& own = h.add<OwnershipComponent>();
        own.team = o.owner;
        own.first_owner = o.first_owner;

        auto& pri = h.add<ObjectivePriorityComponent>();
        pri.priority = o.priority;
        pri.nameid = o.nameid;
        pri.obj_flags = o.obj_flags;
        pri.parent_id = o.parent_id;

        // --- Conditional components ---

        // Supply/fuel/losses — add when any supply data is non-default
        if (o.supply != 0 || o.fuel != 0 || o.losses != 0 || o.last_repair != 0) {
            auto& sup = h.add<SupplyStateComponent>();
            sup.supply = o.supply;
            sup.fuel = o.fuel;
            sup.losses = o.losses;
            sup.last_repair = o.last_repair;
        }

        // Damage bitmap — add when features have damage data
        if (!o.fstatus.empty()) {
            auto& dmg = h.add<DamageBitmapComponent>();
            dmg.fstatus = o.fstatus;
        }

        // Radar — only when the objective has radar
        if (o.has_radar) {
            auto& rad = h.add<RadarComponent>();
            for (int i = 0; i < 8; ++i) rad.detect_ratio[i] = o.detect_ratio[i];
            rad.range_km = o.radar_range_km;
            rad.name = o.radar_name;
            rad.radar_type_idx = o.radar_type_idx;
        }

        // Network links — when the objective has road/rail connections
        if (!o.links.empty()) {
            auto& nl = h.add<NetworkLinksComponent>();
            nl.links = o.links;
        }

        // Ground layout — when the objective is an airbase with layout data
        if (!o.ground_layout.empty()) {
            auto& gl = h.add<GroundLayoutComponent>();
            gl.layouts = o.ground_layout;
        }

        // Feature set — when the objective has features (buildings, structures)
        if (o.features_count > 0 || !o.features.empty()) {
            auto& fs = h.add<FeatureSetComponent>();
            fs.features_count = o.features_count;
            fs.radar_feature = o.radar_feature;
            fs.deag_distance = o.deag_distance;
            fs.pt_data_index = o.pt_data_index;
            fs.objective_detection = o.objective_detection;
            fs.features = o.features;
        }

        // --- PropertyBag for format residue ---
        // VU_ID, entity_type, obj_flags are format concepts that don't belong
        // in domain components. They go into PropertyBag so systems that need
        // them (e.g. save-file round-trip) can access them, but they don't
        // pollute the domain model.
        {
            auto& pb = h.add<PropertyBag>();
            pb.ints["vu_id_creator"] = static_cast<int64_t>(o.id_creator);
            pb.ints["vu_id_num"]     = static_cast<int64_t>(o.id_num);
            pb.ints["entity_type"]   = static_cast<int64_t>(o.entity_type);
            pb.ints["camp_id"]       = static_cast<int64_t>(o.camp_id);
        }

        // Build VU_ID→EntityId map for cross-reference resolution
        if (o.id_num != 0) {
            obj_id_map[o.id_num] = h.id();
        }

        ids.push_back(h.id());
    }
    return ids;
}

// ============================================================================
// populate_units (Phase 3)
// ============================================================================
std::vector<EntityId> populate_units(
    EntityWorld& world,
    const WorldState& ws,
    const std::unordered_map<uint32_t, EntityId>& obj_id_map,
    std::unordered_map<uint32_t, EntityId>& unit_id_map)
{
    std::vector<EntityId> ids;
    ids.reserve(ws.units.size());

    for (const auto& u : ws.units) {
        auto h = world.create();

        // --- Tags ---
        h.set_tag(tags::ROLE, TagValue::from(std::string(unit_class_name(u.unit_class))));
        h.set_tag(tags::TEAM, TagValue::from(static_cast<int64_t>(u.owner)));
        h.set_tag(tags::OPDOMAIN, TagValue::from(std::string(domain_name(u.domain))));
        h.set_tag(tags::ALIVE, TagValue::from(true));

        // --- Transform (grid → feet) ---
        auto& tf = h.add<TransformComponent>();
        tf.position = grid_to_feet(u.x, u.y, u.z);

        // --- UnitCoreComponent (all units) ---
        auto& uc = h.add<UnitCoreComponent>();
        uc.unit_class = u.unit_class;
        uc.domain = u.domain;
        uc.unit_subtype = u.unit_subtype;
        uc.class_table_index = static_cast<int16_t>(u.entity_type);
        uc.roster = u.roster;
        uc.class_name = u.class_name;

        // --- Subclass-specific components ---
        switch (u.unit_class) {
            case UnitClass::Battalion:
            case UnitClass::Brigade:
            case UnitClass::TaskForce: {
                // Ground tactical state
                auto& gt = h.add<GroundTacticalComponent>();
                gt.supply = u.supply;
                gt.morale = u.morale;
                gt.fatigue = u.fatigue;
                gt.heading = u.heading;
                gt.final_heading = u.final_heading;
                gt.position = u.position;
                gt.last_move = u.last_move;
                gt.last_combat = u.last_combat;

                // Hierarchy — Battalion has parent brigade, Brigade has child battalions
                if (u.parent_id != 0 || !u.element_ids.empty()) {
                    auto& hier = h.add<HierarchyComponent>();
                    hier.parent_id = u.parent_id;
                    hier.element_ids = u.element_ids;
                }
                break;
            }
            case UnitClass::Squadron: {
                auto& sq = h.add<SquadronComponent>();
                sq.airbase_id = u.airbase_id;
                sq.specialty = u.specialty;
                sq.aa_kills = u.aa_kills;
                sq.ag_kills = u.ag_kills;
                sq.as_kills = u.as_kills;
                sq.an_kills = u.an_kills;
                sq.missions_flown = u.missions_flown;
                sq.mission_score = u.mission_score;
                sq.total_losses = u.total_losses;
                sq.pilot_losses = u.pilot_losses;
                sq.squadron_patch = u.squadron_patch;
                sq.fuel = u.fuel;
                sq.pilots = u.pilots;

                // Resolve Squadron→airbase cross-reference
                if (u.airbase_id != 0) {
                    auto it = obj_id_map.find(u.airbase_id);
                    if (it != obj_id_map.end()) {
                        sq.airbase = it->second;
                    }
                }
                break;
            }
            case UnitClass::Flight: {
                auto& fp = h.add<FlightPlanComponent>();
                fp.altitude = u.flight_altitude;
                fp.fuel_burnt = u.fuel_burnt;
                fp.time_on_target = u.time_on_target;
                fp.mission_over_time = u.mission_over_time;
                fp.mission_target = u.mission_target;
                fp.loadouts = u.loadouts;
                fp.mission = u.mission;
                fp.flight_priority = u.flight_priority;
                fp.mission_id = u.mission_id;
                fp.eval_flags = u.eval_flags;
                fp.callsign_id = u.callsign_id;
                fp.callsign_num = u.callsign_num;
                // VU_ID cross-references — resolved in second pass
                fp.package_id = u.package_id;
                fp.squadron_id = u.squadron_id;
                break;
            }
            case UnitClass::Package: {
                auto& ps = h.add<PackageSupportComponent>();
                ps.wait_cycles = u.wait_cycles;
                ps.interceptor_id = u.interceptor_id;
                ps.awacs_id = u.awacs_id;
                ps.jstar_id = u.jstar_id;
                ps.ecm_id = u.ecm_id;
                ps.tanker_id = u.tanker_id;
                break;
            }
            default:
                break;
        }

        // --- Conditional components ---

        // Waypoint plan — when the unit has waypoints
        if (!u.waypoints.empty()) {
            auto& wp = h.add<WaypointPlanComponent>();
            wp.waypoints = u.waypoints;
        }

        // Vehicle composition — when the unit has vehicle groups
        if (!u.vehicle_groups.empty()) {
            auto& vc = h.add<VehicleCompositionComponent>();
            vc.groups = u.vehicle_groups;
        }

        // Unit class scores — when any score is non-zero
        {
            bool has_scores = false;
            for (auto s : u.unit_class_scores) {
                if (s != 0) { has_scores = true; break; }
            }
            if (has_scores) {
                auto& ucs = h.add<UnitClassScoreComponent>();
                ucs.scores = u.unit_class_scores;
            }
        }

        // --- PropertyBag for format residue ---
        {
            auto& pb = h.add<PropertyBag>();
            pb.ints["vu_id_creator"] = static_cast<int64_t>(u.id_creator);
            pb.ints["vu_id_num"]     = static_cast<int64_t>(u.id_num);
            pb.ints["entity_type"]   = static_cast<int64_t>(u.entity_type);
            pb.ints["camp_id"]       = static_cast<int64_t>(u.camp_id);
            pb.ints["name_id"]       = static_cast<int64_t>(u.name_id);
            pb.ints["reinforcement"] = static_cast<int64_t>(u.reinforcement);
            pb.ints["dest_x"]        = static_cast<int64_t>(u.dest_x);
            pb.ints["dest_y"]        = static_cast<int64_t>(u.dest_y);
            if (u.movement_type != 0) {
                pb.ints["movement_type"] = static_cast<int64_t>(u.movement_type);
            }
            if (u.movement_speed != 0) {
                pb.ints["movement_speed"] = static_cast<int64_t>(u.movement_speed);
            }
            if (u.max_range != 0) {
                pb.ints["max_range"] = static_cast<int64_t>(u.max_range);
            }
            if (!u.movement_type_name.empty()) {
                pb.strings["movement_type_name"] = u.movement_type_name;
            }
        }

        // Build VU_ID→EntityId map
        if (u.id_num != 0) {
            unit_id_map[u.id_num] = h.id();
        }

        ids.push_back(h.id());
    }

    // --- Second pass: resolve unit→unit cross-references ---
    // Now that all units have EntityIds, resolve Flight→Package, Flight→Squadron,
    // Battalion→Brigade, and Package support flights.
    for (size_t i = 0; i < ws.units.size(); ++i) {
        const auto& u = ws.units[i];
        EntityHandle h(ids[i], &world);

        switch (u.unit_class) {
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
// populate_world (Phase 3 — orchestrator)
// ============================================================================
PopulatedWorld populate_world(EntityWorld& world, const WorldState& ws) {
    PopulatedWorld pw;

    pw.campaign = populate_campaign(world, ws);
    pw.teams    = populate_teams(world, ws);
    pw.objectives = populate_objectives(world, ws, pw.objective_id_map);
    pw.units    = populate_units(world, ws, pw.objective_id_map, pw.unit_id_map);

    return pw;
}

} // namespace f4::world
