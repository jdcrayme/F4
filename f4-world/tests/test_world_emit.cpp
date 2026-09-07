// f4-world/tests/test_world_emit.cpp
//
// SAVE_WRITE_PLAN §6.1 — the WorldState → JSON emitter (the runtime-
// mutated save path). The contract: to_json_string() is the exact inverse
// of load_from_string() over the typed structs —
//
//   WorldState ws1; ws1.load_from_string(doc);
//   WorldState ws2; ws2.load_from_string(ws1.to_json_string());
//   eq(ws1, ws2) — field for field, floats bit-exact.
//
// Pinned two ways:
//   1. The REAL campaign fixture: save1.cam → cam2json (build-time
//      fixture, the same one test_world_state loads) → round-trip
//      equality over 72 squadrons, 2659 objectives, the full team set.
//   2. A synthetic edge-case state: tea enrichment both ways, radar arcs,
//      ground layouts, features with names, waypoints, pilots, vehicle
//      groups, loadout stations, mis_request, atm schedules/requests,
//      Unknown unit class, and floats with ugly mantissas.
//
// The emitter is a PROJECTION (the world-JSON schema, not the decode
// structs' full wire fidelity) — the .cam save path diffs this document
// against the original and overwrites only the campaign loop's owned
// fields (f4-world-convert's derive_save_mutations). That side is pinned
// in f4-world-convert's test_save_writeback.cpp.

#include <f4/world/detail/world_state.hpp>

#include <gtest/gtest.h>

#include <cmath>
#include <string>

using namespace f4::world;

namespace {

// ============================================================================
// Deep equality — every field the parser reads, compared exactly. Floats
// compare with == on purpose: the emitter's %.17g float→double contract
// must recover the original float bit-for-bit; a 1-ulp drift is a bug.
// ============================================================================

bool eq(const ObjectiveLink& a, const ObjectiveLink& b) {
    return a.neighbor_num == b.neighbor_num &&
           a.neighbor_creator == b.neighbor_creator &&
           a.is_road == b.is_road && a.is_rail == b.is_rail &&
           std::equal(std::begin(a.costs), std::end(a.costs),
                      std::begin(b.costs));
}

bool eq(const GroundLayoutPoint& a, const GroundLayoutPoint& b) {
    return a.x == b.x && a.y == b.y && a.type == b.type && a.flags == b.flags;
}

bool eq(const GroundLayoutList& a, const GroundLayoutList& b) {
    if (a.type != b.type || a.count != b.count ||
        a.runway_num != b.runway_num || a.ltrt != b.ltrt ||
        a.heading_deg != b.heading_deg ||
        a.points.size() != b.points.size()) {
        return false;
    }
    for (std::size_t i = 0; i < a.points.size(); ++i) {
        if (!eq(a.points[i], b.points[i])) return false;
    }
    return true;
}

bool eq(const FeatureEntryState& a, const FeatureEntryState& b) {
    return a.index == b.index && a.flags == b.flags && a.value == b.value &&
           a.offset_x == b.offset_x && a.offset_y == b.offset_y &&
           a.offset_z == b.offset_z && a.facing == b.facing &&
           a.name == b.name && a.hit_points == b.hit_points &&
           a.repair_time == b.repair_time && a.priority == b.priority &&
           a.feat_flags == b.feat_flags && a.radar_type == b.radar_type;
    // damage_state is NOT compared: it is DERIVED from the parent
    // objective's fstatus bitmap at parse time (the emitter does not
    // emit it), so a hand-built state and its round-trip legitimately
    // differ on this field until both come from a parse.
}

bool eq(const ObjectiveState& a, const ObjectiveState& b) {
    if (a.type != b.type || a.objective_type != b.objective_type ||
        a.id_creator != b.id_creator || a.id_num != b.id_num ||
        a.entity_type != b.entity_type || a.x != b.x || a.y != b.y ||
        a.z != b.z || a.owner != b.owner || a.camp_id != b.camp_id ||
        a.priority != b.priority || a.nameid != b.nameid ||
        a.obj_flags != b.obj_flags || a.supply != b.supply ||
        a.fuel != b.fuel || a.losses != b.losses ||
        a.last_repair != b.last_repair || a.first_owner != b.first_owner ||
        a.parent_id != b.parent_id || a.fstatus != b.fstatus ||
        a.has_radar != b.has_radar || a.radar_range_km != b.radar_range_km ||
        a.radar_name != b.radar_name ||
        a.radar_type_idx != b.radar_type_idx ||
        a.links.size() != b.links.size() ||
        a.class_name != b.class_name ||
        a.features_count != b.features_count ||
        a.radar_feature != b.radar_feature ||
        a.deag_distance != b.deag_distance ||
        a.pt_data_index != b.pt_data_index ||
        a.ground_layout.size() != b.ground_layout.size() ||
        a.features.size() != b.features.size()) {
        return false;
    }
    if (a.has_radar &&
        !std::equal(std::begin(a.detect_ratio), std::end(a.detect_ratio),
                    std::begin(b.detect_ratio))) {
        return false;
    }
    if (!std::equal(std::begin(a.objective_detection),
                    std::end(a.objective_detection),
                    std::begin(b.objective_detection))) {
        return false;
    }
    for (std::size_t i = 0; i < a.links.size(); ++i) {
        if (!eq(a.links[i], b.links[i])) return false;
    }
    for (std::size_t i = 0; i < a.ground_layout.size(); ++i) {
        if (!eq(a.ground_layout[i], b.ground_layout[i])) return false;
    }
    for (std::size_t i = 0; i < a.features.size(); ++i) {
        if (!eq(a.features[i], b.features[i])) return false;
    }
    return true;
}

bool eq(const WaypointState& a, const WaypointState& b) {
    return a.x == b.x && a.y == b.y && a.z == b.z && a.arrive == b.arrive &&
           a.action == b.action && a.route_action == b.route_action &&
           a.formation == b.formation && a.flags == b.flags &&
           a.target_num == b.target_num &&
           a.target_creator == b.target_creator &&
           a.target_building == b.target_building && a.depart == b.depart;
}

bool eq(const PilotState& a, const PilotState& b) {
    return a.pilot_id == b.pilot_id && a.skill == b.skill &&
           a.rating == b.rating && a.status == b.status &&
           a.aa_kills == b.aa_kills && a.ag_kills == b.ag_kills &&
           a.as_kills == b.as_kills && a.an_kills == b.an_kills &&
           a.missions_flown == b.missions_flown;
}

bool eq(const VehicleGroup& a, const VehicleGroup& b) {
    return a.group == b.group && a.vehicle_type == b.vehicle_type &&
           a.count == b.count && a.live_count == b.live_count &&
           a.vehicle_name == b.vehicle_name &&
           a.vehicle_nctr == b.vehicle_nctr &&
           a.hit_points == b.hit_points && a.max_speed == b.max_speed;
}

bool eq(const LoadoutStationState& a, const LoadoutStationState& b) {
    return a.weapon_id == b.weapon_id && a.count == b.count;
}

bool eq(const UnitState& a, const UnitState& b) {
    if (a.type != b.type || a.unit_class != b.unit_class ||
        a.unit_subtype != b.unit_subtype || a.domain != b.domain ||
        a.id_creator != b.id_creator || a.id_num != b.id_num ||
        a.entity_type != b.entity_type || a.x != b.x || a.y != b.y ||
        a.z != b.z || a.owner != b.owner || a.camp_id != b.camp_id ||
        a.dest_x != b.dest_x || a.dest_y != b.dest_y ||
        a.name_id != b.name_id || a.reinforcement != b.reinforcement ||
        a.wp_count != b.wp_count || a.losses != b.losses ||
        a.roster != b.roster || a.waypoints.size() != b.waypoints.size() ||
        a.supply != b.supply || a.morale != b.morale ||
        a.fatigue != b.fatigue || a.elements != b.elements ||
        a.fuel != b.fuel || a.parent_id != b.parent_id ||
        a.element_ids != b.element_ids || a.last_move != b.last_move ||
        a.last_combat != b.last_combat || a.heading != b.heading ||
        a.final_heading != b.final_heading || a.position != b.position ||
        a.airbase_id != b.airbase_id || a.specialty != b.specialty ||
        a.aa_kills != b.aa_kills || a.ag_kills != b.ag_kills ||
        a.as_kills != b.as_kills || a.an_kills != b.an_kills ||
        a.missions_flown != b.missions_flown ||
        a.mission_score != b.mission_score ||
        a.total_losses != b.total_losses ||
        a.pilot_losses != b.pilot_losses ||
        a.squadron_patch != b.squadron_patch ||
        a.pilots.size() != b.pilots.size() ||
        a.flight_altitude != b.flight_altitude ||
        a.fuel_burnt != b.fuel_burnt ||
        a.time_on_target != b.time_on_target ||
        a.mission_over_time != b.mission_over_time ||
        a.mission_target != b.mission_target ||
        a.loadouts != b.loadouts ||
        a.loadout_stations.size() != b.loadout_stations.size() ||
        a.mission != b.mission || a.flight_priority != b.flight_priority ||
        a.mission_id != b.mission_id || a.eval_flags != b.eval_flags ||
        a.package_id != b.package_id || a.squadron_id != b.squadron_id ||
        a.callsign_id != b.callsign_id || a.callsign_num != b.callsign_num ||
        a.wait_cycles != b.wait_cycles ||
        a.interceptor_id != b.interceptor_id || a.awacs_id != b.awacs_id ||
        a.jstar_id != b.jstar_id || a.ecm_id != b.ecm_id ||
        a.tanker_id != b.tanker_id ||
        a.request_present != b.request_present ||
        a.class_name != b.class_name ||
        a.movement_type != b.movement_type ||
        a.movement_type_name != b.movement_type_name ||
        a.movement_speed != b.movement_speed ||
        a.max_range != b.max_range ||
        a.vehicle_groups.size() != b.vehicle_groups.size() ||
        !std::equal(std::begin(a.unit_class_scores),
                    std::end(a.unit_class_scores),
                    std::begin(b.unit_class_scores)) ||
        !std::equal(std::begin(a.unit_hit_chance),
                    std::end(a.unit_hit_chance),
                    std::begin(b.unit_hit_chance)) ||
        !std::equal(std::begin(a.unit_weapon_range),
                    std::end(a.unit_weapon_range),
                    std::begin(b.unit_weapon_range))) {
        return false;
    }
    if (a.request_present) {
        if (a.request_mission != b.request_mission ||
            a.request_tot != b.request_tot ||
            a.request_priority != b.request_priority ||
            a.request_action_type != b.request_action_type ||
            a.request_target_num != b.request_target_num ||
            a.request_target_creator != b.request_target_creator ||
            a.request_requester_num != b.request_requester_num) {
            return false;
        }
    }
    for (std::size_t i = 0; i < a.waypoints.size(); ++i) {
        if (!eq(a.waypoints[i], b.waypoints[i])) return false;
    }
    for (std::size_t i = 0; i < a.pilots.size(); ++i) {
        if (!eq(a.pilots[i], b.pilots[i])) return false;
    }
    for (std::size_t i = 0; i < a.loadout_stations.size(); ++i) {
        if (!eq(a.loadout_stations[i], b.loadout_stations[i])) return false;
    }
    for (std::size_t i = 0; i < a.vehicle_groups.size(); ++i) {
        if (!eq(a.vehicle_groups[i], b.vehicle_groups[i])) return false;
    }
    return true;
}

bool eq(const AtmAirbaseState& a, const AtmAirbaseState& b) {
    return a.id_num == b.id_num && a.schedule == b.schedule;
}

bool eq(const AtmRequestState& a, const AtmRequestState& b) {
    return a.mission == b.mission && a.who == b.who &&
           a.aircraft == b.aircraft && a.tot == b.tot &&
           a.priority == b.priority && a.target_num == b.target_num &&
           a.requester_num == b.requester_num;
}

bool eq(const TeamState& a, const TeamState& b) {
    if (a.slot != b.slot || a.flags != b.flags || a.colour != b.colour ||
        a.name != b.name || a.motto != b.motto ||
        a.replacements_avail != b.replacements_avail ||
        a.tea_loaded != b.tea_loaded ||
        a.mission_priority != b.mission_priority ||
        a.objtype_priority != b.objtype_priority ||
        a.atm_airbases.size() != b.atm_airbases.size() ||
        a.atm_requests.size() != b.atm_requests.size() ||
        a.cteam != b.cteam || a.team_flags != b.team_flags ||
        a.member != b.member || a.stance != b.stance ||
        a.first_colonel != b.first_colonel ||
        a.first_commander != b.first_commander ||
        a.first_wingman != b.first_wingman ||
        a.last_wingman != b.last_wingman ||
        a.air_experience != b.air_experience ||
        a.air_defense_experience != b.air_defense_experience ||
        a.ground_experience != b.ground_experience ||
        a.naval_experience != b.naval_experience) {
        return false;
    }
    for (std::size_t i = 0; i < a.atm_airbases.size(); ++i) {
        if (!eq(a.atm_airbases[i], b.atm_airbases[i])) return false;
    }
    for (std::size_t i = 0; i < a.atm_requests.size(); ++i) {
        if (!eq(a.atm_requests[i], b.atm_requests[i])) return false;
    }
    return true;
}

bool eq(const CampaignState& a, const CampaignState& b) {
    return a.current_time == b.current_time &&
           a.te_start_time == b.te_start_time &&
           a.te_time_limit == b.te_time_limit &&
           a.te_victory_points == b.te_victory_points &&
           a.te_type == b.te_type &&
           a.te_number_teams == b.te_number_teams &&
           a.te_team == b.te_team && a.te_flags == b.te_flags &&
           a.te_number_aircraft == b.te_number_aircraft &&
           a.te_team_pts == b.te_team_pts &&
           a.bullseye_x == b.bullseye_x && a.bullseye_y == b.bullseye_y &&
           a.bullseye_name == b.bullseye_name &&
           a.last_resupply == b.last_resupply &&
           a.last_repair == b.last_repair &&
           a.last_reinforcement == b.last_reinforcement;
}

bool eq(const WorldState& a, const WorldState& b) {
    if (a.version != b.version || a.theater != b.theater ||
        a.terrain_file != b.terrain_file || !eq(a.campaign, b.campaign) ||
        a.teams.size() != b.teams.size() ||
        a.objectives.size() != b.objectives.size() ||
        a.units.size() != b.units.size()) {
        return false;
    }
    for (std::size_t i = 0; i < a.teams.size(); ++i) {
        if (!eq(a.teams[i], b.teams[i])) return false;
    }
    for (std::size_t i = 0; i < a.objectives.size(); ++i) {
        if (!eq(a.objectives[i], b.objectives[i])) return false;
    }
    for (std::size_t i = 0; i < a.units.size(); ++i) {
        if (!eq(a.units[i], b.units[i])) return false;
    }
    return true;
}

WorldState round_trip(const WorldState& ws) {
    WorldState out;
    out.load_from_string(ws.to_json_string());
    return out;
}

} // namespace

// ============================================================================
// 1. The real campaign fixture: save1.cam → cam2json → WorldState →
//    emitter → WorldState. Field-for-field equality over the full war.
// ============================================================================
TEST(WorldEmit, RealFixtureRoundTripsFieldForField) {
    WorldState ws;
    ws.load(WORLD_JSON_FIXTURE);
    ASSERT_GT(ws.objectives.size(), 0u);
    ASSERT_GT(ws.units.size(), 0u);
    ASSERT_GT(ws.teams.size(), 0u);

    WorldState ws2 = round_trip(ws);
    EXPECT_TRUE(eq(ws, ws2));

    // A THIRD pass — the emitter's own output re-emitted — must be
    // stable (the round-trip is a fixed point).
    WorldState ws3 = round_trip(ws2);
    EXPECT_TRUE(eq(ws2, ws3));
}

// ============================================================================
// 2. Synthetic edge cases: every optional block, both tea states, radar
//    arcs, layouts, features, the package request — and floats with ugly
//    mantissas (the %.17g contract).
// ============================================================================
TEST(WorldEmit, SyntheticEdgeCasesRoundTrip) {
    WorldState ws;
    ws.version = 71;
    ws.theater = "korea";
    ws.terrain_file = "@asset:theater:korea";

    ws.campaign.current_time = 12345678;
    ws.campaign.te_start_time = 1000;
    ws.campaign.te_time_limit = 86400 * 30;
    ws.campaign.te_victory_points = 5000;
    ws.campaign.te_type = 1;
    ws.campaign.te_number_teams = 2;
    ws.campaign.te_team = 2;
    ws.campaign.te_flags = 0x7;
    ws.campaign.te_number_aircraft = {100, 200, 300, 400, 500, 600, 700, 800};
    ws.campaign.te_team_pts = {1, 2, 3, 4, 5, 6, 7, 8};
    ws.campaign.bullseye_x = 500;
    ws.campaign.bullseye_y = 480;
    ws.campaign.bullseye_name = 3;
    ws.campaign.last_resupply = 100;
    ws.campaign.last_repair = 200;
    ws.campaign.last_reinforcement = 300;

    // Team 0: no .tea enrichment (base fields only).
    TeamState plain;
    plain.slot = 0;
    plain.flags = 1;
    plain.colour = 2;
    plain.name = "ROK";
    plain.motto = "Liberty, not free";
    plain.replacements_avail = 42;
    ws.teams.push_back(plain);

    // Team 1: FULL .tea enrichment — schedules, requests, priorities,
    // memberships, stances.
    TeamState rich;
    rich.slot = 1;
    rich.name = "DPRK";
    rich.motto = "";
    rich.tea_loaded = true;
    rich.cteam = 1;
    rich.team_flags = 0x33;
    rich.first_colonel = 7;
    rich.first_commander = 11;
    rich.first_wingman = 13;
    rich.last_wingman = 17;
    rich.air_experience = 90;
    rich.air_defense_experience = 80;
    rich.ground_experience = 70;
    rich.naval_experience = 60;
    rich.member = {0, 1, 1, 0};
    rich.stance = {0, -1, 5, 0};
    rich.mission_priority = {0, 10, 20, 30};
    rich.objtype_priority = {1, 2, 3, 4, 5, 6};
    AtmAirbaseState ab;
    ab.id_num = 4242;
    for (int i = 0; i < 32; ++i) ab.schedule[static_cast<std::size_t>(i)] =
        static_cast<uint8_t>(i & 0xFF);
    rich.atm_airbases.push_back(ab);
    AtmRequestState rq;
    rq.mission = 7;
    rq.who = 1;
    rq.aircraft = 2;
    rq.tot = 987654;
    rq.priority = 55;
    rq.target_num = 121212;
    rq.requester_num = 343434;
    rich.atm_requests.push_back(rq);
    ws.teams.push_back(rich);

    // Objective: radar site + ground layout + features + links + ugly z.
    ObjectiveState o;
    o.type = 3;
    o.objective_type = 1;  // airbase
    o.id_creator = 4;
    o.id_num = 8801;
    o.entity_type = 110;
    o.x = 500;
    o.y = 480;
    o.z = 867554.625f;  // the SAVE_WRITE_PLAN §2b float-precision fixture
    o.owner = 1;
    o.camp_id = 2;
    o.priority = 9;
    o.nameid = 44;
    o.obj_flags = 3958048256u;  // > INT32_MAX — the read_int() 64-bit case
    o.supply = 66;
    o.fuel = 77;
    o.losses = 5;
    o.last_repair = 321321;
    o.first_owner = 2;
    o.parent_id = 999;
    o.fstatus = {0x1B, 0x2E, 0x7F, 0x00, 0xAA};
    o.has_radar = true;
    for (int i = 0; i < 8; ++i) {
        o.detect_ratio[i] = 0.125f * static_cast<float>(i) + 0.001f;
    }
    o.radar_range_km = 185.5f;
    o.radar_name = "APG-68";
    o.radar_type_idx = 12;
    ObjectiveLink l;
    l.neighbor_num = 8802;
    l.neighbor_creator = 4;
    l.is_road = true;
    l.is_rail = false;
    for (int i = 0; i < 8; ++i) l.costs[i] = static_cast<uint8_t>(i * 31);
    o.links.push_back(l);
    o.class_name = "02_20 Airbase 2";
    o.features_count = 3;
    o.radar_feature = 1;
    o.deag_distance = 3;
    o.pt_data_index = 17;
    for (int i = 0; i < 8; ++i) {
        o.objective_detection[static_cast<std::size_t>(i)] =
            static_cast<uint8_t>(i * 11);
    }
    GroundLayoutList gll;
    gll.type = 1;
    gll.count = 2;
    gll.runway_num = 1;
    gll.ltrt = -1;
    gll.heading_deg = 23.5f;
    GroundLayoutPoint p0;
    p0.x = -4219.0f;
    p0.y = 88.5f;
    p0.type = 1;
    p0.flags = 2;
    GroundLayoutPoint p1;
    p1.x = 4219.5f;
    p1.y = -88.25f;
    p1.type = 2;
    p1.flags = 0;
    gll.points = {p0, p1};
    o.ground_layout.push_back(gll);
    FeatureEntryState f;
    f.index = 2200;
    f.flags = 0x1010;
    f.value = 100;
    f.offset_x = -250.0f;
    f.offset_y = 0.5f;
    f.offset_z = 12.25f;
    f.facing = 90;
    f.name = "Control Tower";
    f.hit_points = 50;
    f.repair_time = 3600;
    f.priority = 2;
    f.feat_flags = 0x00FF;
    f.radar_type = 12;
    o.features.push_back(f);
    ws.objectives.push_back(o);

    // Unit: squadron with pilots + loadout + a package with mis_request
    // + a battalion with waypoints + an Unknown-class unit.
    UnitState sq;
    sq.type = 305;
    sq.unit_class = UnitClass::Squadron;
    sq.unit_subtype = 5;
    sq.domain = 2;
    sq.id_creator = 4;
    sq.id_num = 9901;
    sq.entity_type = 305;
    sq.x = 500;
    sq.y = 480;
    sq.z = 0.1f;
    sq.owner = 2;
    sq.camp_id = 2;
    sq.name_id = 77;
    sq.fuel = 999999;
    sq.airbase_id = 8801;
    sq.specialty = 3;
    sq.aa_kills = 12;
    sq.ag_kills = 34;
    sq.as_kills = 5;
    sq.an_kills = 6;
    sq.missions_flown = 321;
    sq.mission_score = 1234;
    sq.total_losses = 7;
    sq.pilot_losses = 3;
    sq.squadron_patch = 9;
    PilotState p;
    p.pilot_id = 1;
    p.skill = 5;
    p.rating = 3;
    p.status = 0;
    p.aa_kills = 4;
    p.ag_kills = 2;
    p.as_kills = 1;
    p.an_kills = 0;
    p.missions_flown = 45;
    sq.pilots.push_back(p);
    LoadoutStationState st;
    st.weapon_id = 33;
    st.count = 2;
    sq.loadout_stations.push_back(st);
    sq.class_name = "F-16C Squadron";
    sq.movement_type = 5;
    sq.movement_type_name = "Air";
    sq.movement_speed = 800;
    sq.max_range = 500;
    for (int i = 0; i < 16; ++i) {
        sq.unit_class_scores[static_cast<std::size_t>(i)] =
            static_cast<uint8_t>(i * 15);
    }
    for (int i = 0; i < 8; ++i) {
        sq.unit_hit_chance[static_cast<std::size_t>(i)] =
            static_cast<uint8_t>(i * 7);
        sq.unit_weapon_range[static_cast<std::size_t>(i)] =
            static_cast<uint8_t>(i * 9);
    }
    ws.units.push_back(sq);

    UnitState pkg;
    pkg.type = 310;
    pkg.unit_class = UnitClass::Package;
    pkg.domain = 2;
    pkg.id_num = 9902;
    pkg.wait_cycles = 2;
    pkg.interceptor_id = 1;
    pkg.awacs_id = 2;
    pkg.jstar_id = 3;
    pkg.ecm_id = 4;
    pkg.tanker_id = 5;
    pkg.request_present = true;
    pkg.request_mission = 9;
    pkg.request_tot = 555555;
    pkg.request_priority = 88;
    pkg.request_action_type = 202;
    pkg.request_target_num = 8802;
    pkg.request_target_creator = 4;
    pkg.request_requester_num = 9903;
    pkg.element_ids = {9904, 9905, 9906};
    ws.units.push_back(pkg);

    UnitState bn;
    bn.type = 201;
    bn.unit_class = UnitClass::Battalion;
    bn.domain = 3;
    bn.id_num = 9907;
    bn.x = 512;
    bn.y = 480;
    bn.dest_x = 520;
    bn.dest_y = 490;
    bn.roster = 0xDEADBEEF;
    bn.losses = 3;
    bn.supply = 80;
    bn.morale = 90;
    bn.fatigue = 25;
    bn.heading = 128;
    bn.final_heading = 200;
    bn.position = 1;
    bn.last_move = 777777;
    bn.last_combat = 888888;
    bn.parent_id = 9908;
    WaypointState wp;
    wp.x = 511;
    wp.y = 481;
    wp.z = 100;
    wp.arrive = 11111;
    wp.action = 2;
    wp.route_action = 3;
    wp.formation = 4;
    wp.flags = -5;
    wp.target_num = 8803;
    wp.target_creator = 4;
    wp.target_building = 2;
    wp.depart = 22222;
    bn.waypoints.push_back(wp);
    bn.vehicle_groups.push_back(
        {0, 2100, 3, 3, "M-1A1", "ARMOR", 60, 72});
    ws.units.push_back(bn);

    UnitState unk;
    unk.unit_class = UnitClass::Unknown;
    unk.id_num = 9999;
    ws.units.push_back(unk);

    WorldState ws2 = round_trip(ws);
    EXPECT_EQ(ws.version, ws2.version);
    EXPECT_EQ(ws.theater, ws2.theater);
    EXPECT_EQ(ws.terrain_file, ws2.terrain_file);
    ASSERT_EQ(ws.teams.size(), ws2.teams.size());
    for (std::size_t i = 0; i < ws.teams.size(); ++i) {
        EXPECT_TRUE(eq(ws.teams[i], ws2.teams[i])) << "team " << i;
    }
    ASSERT_EQ(ws.objectives.size(), ws2.objectives.size());
    for (std::size_t i = 0; i < ws.objectives.size(); ++i) {
        const auto& a = ws.objectives[i];
        const auto& b = ws2.objectives[i];
        EXPECT_EQ(a.type, b.type) << i;
        EXPECT_EQ(a.objective_type, b.objective_type) << i;
        EXPECT_EQ(a.id_num, b.id_num) << i;
        EXPECT_EQ(a.id_creator, b.id_creator) << i;
        EXPECT_EQ(a.entity_type, b.entity_type) << i;
        EXPECT_EQ(a.x, b.x) << i;
        EXPECT_EQ(a.y, b.y) << i;
        EXPECT_EQ(a.z, b.z) << i;
        EXPECT_EQ(a.owner, b.owner) << i;
        EXPECT_EQ(a.priority, b.priority) << i;
        EXPECT_EQ(a.nameid, b.nameid) << i;
        EXPECT_EQ(a.camp_id, b.camp_id) << i;
        EXPECT_EQ(a.obj_flags, b.obj_flags) << i;
        EXPECT_EQ(a.supply, b.supply) << i;
        EXPECT_EQ(a.fuel, b.fuel) << i;
        EXPECT_EQ(a.losses, b.losses) << i;
        EXPECT_EQ(a.last_repair, b.last_repair) << i;
        EXPECT_EQ(a.first_owner, b.first_owner) << i;
        EXPECT_EQ(a.parent_id, b.parent_id) << i;
        EXPECT_EQ(a.fstatus, b.fstatus) << i;
        EXPECT_EQ(a.has_radar, b.has_radar) << i;
        EXPECT_EQ(a.radar_range_km, b.radar_range_km) << i;
        EXPECT_EQ(a.radar_name, b.radar_name) << i;
        EXPECT_EQ(a.radar_type_idx, b.radar_type_idx) << i;
        EXPECT_EQ(a.links.size(), b.links.size()) << i;
        EXPECT_EQ(a.class_name, b.class_name) << i;
        EXPECT_EQ(a.features_count, b.features_count) << i;
        EXPECT_EQ(a.radar_feature, b.radar_feature) << i;
        EXPECT_EQ(a.deag_distance, b.deag_distance) << i;
        EXPECT_EQ(a.pt_data_index, b.pt_data_index) << i;
        EXPECT_EQ(a.ground_layout.size(), b.ground_layout.size()) << i;
        for (int d = 0; d < 8; ++d) {
            EXPECT_EQ(a.detect_ratio[d], b.detect_ratio[d])
                << i << " detect_ratio[" << d << "]";
            EXPECT_EQ(a.objective_detection[d], b.objective_detection[d])
                << i << " objective_detection[" << d << "]";
        }
        ASSERT_EQ(a.links.size(), b.links.size()) << i;
        for (std::size_t k = 0; k < a.links.size(); ++k) {
            EXPECT_TRUE(eq(a.links[k], b.links[k])) << i << " link " << k;
        }
        for (std::size_t k = 0; k < a.ground_layout.size(); ++k) {
            EXPECT_TRUE(eq(a.ground_layout[k], b.ground_layout[k]))
                << i << " gll " << k;
        }
        ASSERT_EQ(a.features.size(), b.features.size()) << i;
        for (std::size_t k = 0; k < a.features.size(); ++k) {
            EXPECT_EQ(a.features[k].name, b.features[k].name) << i << "/" << k;
            EXPECT_EQ(a.features[k].index, b.features[k].index) << i << "/" << k;
        }
    }
    ASSERT_EQ(ws.units.size(), ws2.units.size());
    for (std::size_t i = 0; i < ws.units.size(); ++i) {
        EXPECT_TRUE(eq(ws.units[i], ws2.units[i])) << "unit " << i;
    }

    WorldState ws3 = round_trip(ws2);
    EXPECT_TRUE(eq(ws2, ws3));
}

// ============================================================================
// 3. The float contract specifically: bit-exact recovery of nasty float
//    mantissas through the %.17g wire form.
// ============================================================================
TEST(WorldEmit, FloatsRoundTripBitExact) {
    WorldState ws;
    const float ugly[] = {
        0.1f,
        867554.625f,
        1e-30f,
        1.0000001f,
        3.3554432e7f,   // 2^25 — the exact-float/double boundary
        123456.789f,
        -0.0f,
    };
    for (const float v : ugly) {
        ws.objectives.clear();
        ObjectiveState o;
        o.id_num = 1;
        o.z = v;
        ws.objectives.push_back(o);
        WorldState ws2 = round_trip(ws);
        ASSERT_EQ(ws2.objectives.size(), 1u);
        EXPECT_EQ(ws2.objectives[0].z, v)
            << "float z did not survive the JSON round-trip: " << v;
    }
}

// ============================================================================
// 4. Empty world: the degenerate document (no teams, no objectives, no
//    units) parses back to an empty state — and the emitter never emits
//    a trailing comma the Reader would choke on.
// ============================================================================
TEST(WorldEmit, EmptyWorldRoundTrips) {
    WorldState ws;
    ws.version = 63;
    ws.theater = "korea";
    ws.terrain_file = "korea.terrain.json";

    WorldState ws2 = round_trip(ws);
    EXPECT_TRUE(ws2.teams.empty());
    EXPECT_TRUE(ws2.objectives.empty());
    EXPECT_TRUE(ws2.units.empty());
    EXPECT_EQ(ws2.version, 63);
    EXPECT_EQ(ws2.theater, "korea");
    EXPECT_EQ(ws2.terrain_file, "korea.terrain.json");
}
