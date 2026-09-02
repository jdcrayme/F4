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
// They now live in the OPT-IN <f4/world/world_adapters.hpp> (still excluded
// from the umbrella header, but public for consumers like f4-campaign that
// legitimately bind the IDataSource interfaces to a WorldState).

#include <f4/world/world_loader.hpp>
#include <f4/world/world_adapters.hpp>  // WorldState→I*Source adapters (moved out of this .cpp in B.2 — public opt-in header now, f4-campaign consumes the interfaces too)
#include <f4/world/detail/world_state.hpp>
#include <f4/geo/constants.hpp>

#include <cmath>
#include <cstdint>
#include <unordered_map>
#include <vector>

namespace f4::world {

// using namespace f4::entities — retained because 30+ entity types are used
// in this file. Explicit using-declarations for each would be ~30 lines of
// noise with no readability benefit. using-namespace is acceptable in .cpp
// implementation files (only headers are the real risk).
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

    // Phase A: NAME tag — campaign is a singleton, so a stable literal
    // is the most useful display value (the inspector and any future
    // entity-tree view can show "Campaign" without a component query).
    h.set_tag(tags::NAME, TagValue::from(std::string("Campaign")));

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
    cs.bullseye_x         = src.bullseye_x();
    cs.bullseye_y         = src.bullseye_y();
    cs.bullseye_name      = src.bullseye_name();

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

        // Phase A: NAME tag — promote the team callsign (e.g. "ROK",
        // "Japan", "PRC") so consumers can display/filter team names
        // without querying CampaignIdentityComponent.
        h.set_tag(tags::NAME, TagValue::from(std::string(src.name(i))));

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

        // Phase A: NAME + CLASS + ICON tags.
        //
        // NAME  ← ObjectiveTypeComponent::class_name (the per-instance label,
        //         e.g. "02_20 Airbase 2"). Promoted here so the canvas search
        //         filter and inspector can read it without a component query.
        //
        // CLASS ← objective_type (1..39). The raw input to
        //         f4::world_convert::objective_type_name(). Stored as int to
        //         avoid duplicating the name table (which lives in
        //         f4-world-convert, not depended on by f4-world).
        //
        // ICON  ← same objective_type value — it IS the dispatch input for
        //         f4::renderer::symbol_for_objective_type(). Storing it here
        //         lets the renderer skip the ObjectiveTypeComponent +
        //         PropertyBag lookup in the per-frame canvas loop.
        const std::string& obj_name = src.class_name(i);
        const int64_t obj_type = static_cast<int64_t>(src.objective_type(i));
        if (!obj_name.empty()) {
            h.set_tag(tags::NAME, TagValue::from(std::string(obj_name)));
        }
        if (obj_type > 0) {
            h.set_tag(tags::CLASS, TagValue::from(obj_type));
            h.set_tag(tags::ICON,  TagValue::from(obj_type));
        }

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
// Airbase positional index — for Squadron→airbase fallback resolution
// ============================================================================
//
// PROBLEM: The Squadron binary tail stores the home airbase as a raw VU_ID
// (8 bytes: creator + num). In save1.cam and many other campaign saves,
// this VU_ID is zero for every squadron — the airbase link is silently
// lost during populate_units(). The existing resolution code at line ~650
// leaves SquadronComponent::airbase = EntityId{} (invalid) in that case.
//
// FIX: Build a small lookup of "airbase-capable" objectives by grid
// coordinate. For each Squadron with airbase_id == 0:
//   1. Try an exact (x, y) grid match — recovers 66/72 (91.7%) of
//      squadrons in save1.cam, since squadrons sit on their airbase's grid.
//   2. If no exact match, do a radius search for the nearest airbase-
//      capable objective within ~5 grid units (5120 ft). Recovers most
//      of the remainder; squadrons with no nearby airbase stay unresolved
//      (their aircraft will fall back to the airfield threshold position
//      in spawn_aircraft_from_flights / spawn_aircraft_from_squadrons).
//
// "Airbase-capable" = ObjectiveType in {TYPE_AIRBASE=1, TYPE_AIRSTRIP=2,
// TYPE_ARMYBASE=3}. Army Base is included because squadrons are sometimes
// based there (especially helicopter / attack squadrons).
//
// The index is built once per populate_units() call from the already-
// populated ObjectiveTypeComponent entities. Cost is O(N_objectives),
// paid once at world load — negligible.
namespace {

/// One entry in the airbase positional index. Stored in a vector keyed by
/// grid (multiple airbases can share a grid in pathological cases — we
/// pick the first; the rest are tracked for the radius fallback).
struct AirbaseEntry {
    int16_t  grid_x;
    int16_t  grid_y;
    EntityId id;
};

/// Build the airbase positional index by walking every entity with an
/// ObjectiveTypeComponent. Filters to airbase-capable types only.
/// O(N_objectives) — called once per populate_units().
[[nodiscard]] std::vector<AirbaseEntry>
build_airbase_index(const EntityWorld& world) {
    std::vector<AirbaseEntry> out;
    const auto obj_ids = world.with_component<ObjectiveTypeComponent>();
    out.reserve(obj_ids.size());
    for (const auto eid : obj_ids) {
        EntityHandle h(eid, const_cast<EntityWorld*>(&world));
        const auto* ot = h.get<ObjectiveTypeComponent>();
        const auto* tf = h.get<TransformComponent>();
        if (!ot || !tf) continue;
        // ObjectiveTypeComponent::type is entity_type (100+). Convert to
        // the 1..39 objective_type via the same subtraction used in
        // entity_render.cpp's entity_icon_info().
        const int16_t et = ot->type;
        const int16_t obj_type = (et >= 100) ? static_cast<int16_t>(et - 100) : et;
        // 1=AIRBASE, 2=AIRSTRIP, 3=ARMYBASE — all parking-capable.
        if (obj_type != 1 && obj_type != 2 && obj_type != 3) continue;
        AirbaseEntry e;
        e.grid_x = static_cast<int16_t>(tf->position.x / FT_PER_GRID);
        e.grid_y = static_cast<int16_t>(tf->position.y / FT_PER_GRID);
        e.id = eid;
        out.push_back(e);
    }
    return out;
}

/// Resolve a Squadron's home airbase by positional fallback when the binary
/// airbase_id is zero. Returns EntityId{} (invalid) if no airbase is found.
///
/// Strategy:
///   1. Exact (x, y) grid match — common case (squadrons sit on their
///      airbase's grid in the campaign).
///   2. Nearest airbase within 5 grid units (5120 ft) — handles squadrons
///      that are offset from the airbase center.
[[nodiscard]] EntityId
resolve_airbase_by_position(const std::vector<AirbaseEntry>& index,
                              int16_t sq_grid_x, int16_t sq_grid_y) {
    if (index.empty()) return EntityId{};

    // 1. Exact grid match.
    for (const auto& e : index) {
        if (e.grid_x == sq_grid_x && e.grid_y == sq_grid_y) return e.id;
    }

    // 2. Nearest within 5 grid units (~5120 ft).
    constexpr int16_t SEARCH_RADIUS_GRIDS = 5;
    constexpr double SEARCH_RADIUS_GRIDS_D = static_cast<double>(SEARCH_RADIUS_GRIDS);
    double best_d2 = SEARCH_RADIUS_GRIDS_D * SEARCH_RADIUS_GRIDS_D;
    EntityId best{};
    for (const auto& e : index) {
        const double dx = static_cast<double>(e.grid_x - sq_grid_x);
        const double dy = static_cast<double>(e.grid_y - sq_grid_y);
        const double d2 = dx * dx + dy * dy;
        if (d2 < best_d2) {
            best_d2 = d2;
            best = e.id;
        }
    }
    return best;
}

} // namespace

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

    // Build the airbase positional index once for this populate_units()
    // call. Used as a fallback when a Squadron's binary airbase_id is zero
    // (which is the case for all 72 squadrons in save1.cam). See comment
    // block above build_airbase_index() for the full rationale.
    const auto airbase_index = build_airbase_index(world);

    // --- First pass: create all unit entities ---
    for (int i = 0; i < src.unit_count(); ++i) {
        auto h = world.create();

        // --- Tags ---
        h.set_tag(tags::ROLE, TagValue::from(std::string(unit_class_name(src.unit_class(i)))));
        h.set_tag(tags::TEAM, TagValue::from(static_cast<int64_t>(src.owner(i))));
        h.set_tag(tags::OPDOMAIN, TagValue::from(std::string(domain_name(src.domain(i)))));
        h.set_tag(tags::ALIVE, TagValue::from(true));

        // Phase A: NAME + CLASS + ICON tags.
        //
        // NAME  ← UnitCoreComponent::class_name (e.g. "Armor Battalion",
        //         "Patrol", "52 TFS PAK"). Promoted so the canvas label
        //         pass and inspector can read it without querying
        //         UnitCoreComponent for non-selected entities.
        //
        // CLASS ← unit_subtype (STYPE_UNIT_*). The raw input to
        //         f4::world_convert::unit_subtype_name(domain, subtype).
        //         domain is already available via tags::OPDOMAIN, so the
        //         consumer has everything needed to produce the display
        //         string ("Fighter", "Armor", "Infantry", ...).
        //
        // ICON  ← (unit_class << 8) | unit_subtype — the packed dispatch
        //         input for f4::renderer::symbol_for_unit(). The consumer
        //         unpacks: cls = (icon >> 8) & 0xFF, sub = icon & 0xFF,
        //         then calls symbol_for_unit(cls, sub). This eliminates the
        //         UnitCoreComponent query in the per-frame canvas loop.
        const std::string& unit_name = src.class_name(i);
        const uint8_t u_class = static_cast<uint8_t>(src.unit_class(i));
        const uint8_t u_subtype = src.unit_subtype(i);
        if (!unit_name.empty()) {
            h.set_tag(tags::NAME, TagValue::from(std::string(unit_name)));
        }
        h.set_tag(tags::CLASS, TagValue::from(static_cast<int64_t>(u_subtype)));
        h.set_tag(tags::ICON,  TagValue::from(
            static_cast<int64_t>((static_cast<uint16_t>(u_class) << 8) | u_subtype)));

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

            // Resolve Squadron→airbase cross-reference.
            //
            // Primary: binary airbase_id (a VU_ID packed into uint32) →
            // EntityId via obj_id_map. Works when the campaign save has a
            // valid VU_ID in the Squadron tail.
            //
            // Fallback (positional): when airbase_id is zero (the case for
            // all 72 squadrons in save1.cam — the binary VU_ID is zero),
            // search for an airbase-capable objective at the squadron's
            // grid. Recovers 66/72 (91.7%) via exact grid match in
            // save1.cam; the remainder via radius search. See
            // build_airbase_index() / resolve_airbase_by_position() above.
            const uint32_t ab_id = sq_src->airbase_id(i);
            if (ab_id != 0) {
                auto it = obj_id_map.find(ab_id);
                if (it != obj_id_map.end()) {
                    sq.airbase = it->second;
                }
            } else {
                // Positional fallback: try exact grid match, then radius.
                const int16_t sq_gx = src.x(i);
                const int16_t sq_gy = src.y(i);
                const EntityId resolved = resolve_airbase_by_position(
                    airbase_index, sq_gx, sq_gy);
                if (resolved.value != 0) {
                    sq.airbase = resolved;
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
            fp.loadout_stations = fl->loadout_stations(i);
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
            // B.3 tranche — carry the ATM mission request raw fields onto
            // the component now; the target/requester EntityIds are
            // resolved in the second pass (both maps must be complete).
            ps.request.present = pk->request_present(i);
            ps.request.mission = pk->request_mission(i);
            ps.request.tot = pk->request_tot(i);
            ps.request.priority = pk->request_priority(i);
            ps.request.action_type = pk->request_action_type(i);
            ps.request.target_num = pk->request_target_num(i);
            ps.request.target_creator = pk->request_target_creator(i);
            ps.request.requester_num = pk->request_requester_num(i);
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
                // B.3 tranche — resolve the flight's mission target. The
                // wire value is the target objective's (or, for unit-
                // targeted missions, the target unit's) VU_ID.num. Objectives
                // are tried first: the overwhelmingly common case
                // (strike/SEAD/CAP waypoints reference objectives), and VU
                // numbering spaces can overlap, so a unit VU_ID that
                // collides with an objective id would mis-resolve — prefer
                // the objective reading and only fall back to units when no
                // objective matches.
                const uint32_t tgt_num =
                    static_cast<uint32_t>(fl->mission_target(i));
                if (tgt_num != 0) {
                    auto it = obj_id_map.find(tgt_num);
                    if (it != obj_id_map.end()) {
                        fp->target = it->second;
                    } else {
                        auto uit = unit_id_map.find(tgt_num);
                        if (uit != unit_id_map.end()) {
                            fp->target = uit->second;
                        }
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

                // B.3 tranche — package element flights (same resolution
                // path as Brigade→children, but for air packages).
                const auto& elems = pk->element_ids(i);
                if (!elems.empty()) {
                    ps->elements.clear();
                    ps->elements.reserve(elems.size());
                    for (auto eid : elems) {
                        auto it = unit_id_map.find(eid);
                        if (it != unit_id_map.end()) {
                            ps->elements.push_back(it->second);
                        }
                    }
                }

                // B.3 tranche — ATM mission request target + requester.
                // Targets are usually objectives (the thing to strike);
                // requesters are usually units (the front-line battalion
                // that called for CAS) — try both maps both ways, with
                // the statistically-likely order first.
                if (ps->request.present) {
                    if (ps->request.target_num != 0) {
                        auto it = obj_id_map.find(ps->request.target_num);
                        if (it != obj_id_map.end()) {
                            ps->request.target = it->second;
                        } else {
                            auto uit = unit_id_map.find(ps->request.target_num);
                            if (uit != unit_id_map.end()) {
                                ps->request.target = uit->second;
                            }
                        }
                    }
                    if (ps->request.requester_num != 0) {
                        auto it = unit_id_map.find(ps->request.requester_num);
                        if (it != unit_id_map.end()) {
                            ps->request.requester = it->second;
                        } else {
                            auto oit = obj_id_map.find(ps->request.requester_num);
                            if (oit != obj_id_map.end()) {
                                ps->request.requester = oit->second;
                            }
                        }
                    }
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

    // Phase B: the per-kind EntityId vectors are no longer stored on
    // PopulatedWorld — they're tag-derivable from EntityWorld via
    // with_tag(tags::ROLE, ...) at any time. We still call the four
    // populate_* functions (they create the entities and set tags), we
    // just discard their return values. The two VU_ID maps ARE stored
    // on PopulatedWorld because they're not tag-derivable (VU_IDs are
    // external binary-format identifiers, not ECS tags) and downstream
    // consumers (inspector) need them to resolve raw VU_IDs in
    // format-residue fields like ObjectivePriorityComponent::parent_id.
    (void)populate_campaign  (world, camp_src);
    (void)populate_teams     (world, team_src);
    (void)populate_objectives(world, obj_src,  pw.objective_id_map);
    (void)populate_units     (world, unit_src, pw.objective_id_map, pw.unit_id_map);

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
