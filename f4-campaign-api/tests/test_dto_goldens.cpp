// f4-campaign-api/tests/test_dto_goldens.cpp
//
// The DTO encoders pinned BYTE-FOR-BYTE (CAMP_HOST_PLAN.md §3.2): the
// canonical key order IS the contract — goldens catch any reordering,
// pretty-printing, or mid-sequence key addition. The event vocabulary is
// pinned here too, BEFORE HOST-2 makes the engine emit it (the plan's
// "versioned in v1 so the wire doesn't break later").

#include <f4/campaign/api/dto.hpp>
#include <f4/campaign/api/events.hpp>

#include <gtest/gtest.h>

#include <cstring>
#include <string>

using namespace f4::campaign::api;

namespace {

template <typename T>
std::string encode_json(const T& value) {
    f4::json::Writer w;
    encode(w, value);
    return w.str();
}

} // namespace

// ============================================================================
// time
// ============================================================================

TEST(DtoGoldens, TimeView) {
    TimeView t;
    t.tick_sec = 1.0 / 60.0;
    t.campaign_time_s = 38574360;
    t.sim_time_s = 120.5;
    t.paused = true;
    t.next_tasking_sec = 1679;
    t.time_scale = 4.0;
    // tick_sec encodes %.17g → 0.016666666666666666
    EXPECT_EQ(encode_json(t),
              R"({"tick_sec":0.016666666666666666,"campaign_time_s":38574360,)"
              R"("sim_time_s":120.5,"paused":1,"next_tasking_sec":1679,)"
              R"("time_scale":4})");
}

TEST(DtoGoldens, TimeViewDefaultIsDeterministic) {
    // the default view encodes identically every time (byte-stability)
    const auto a = encode_json(TimeView{});
    const auto b = encode_json(TimeView{});
    EXPECT_EQ(a, b);
    EXPECT_EQ(a,
              R"({"tick_sec":0,"campaign_time_s":0,"sim_time_s":0,"paused":0,)"
              R"("next_tasking_sec":0,"time_scale":1})");
}

// ============================================================================
// stats — the engine's counter vocabulary, fixed order
// ============================================================================

TEST(DtoGoldens, StatsViewGolden) {
    StatsView s;
    s.cycles = 3;
    s.next_tasking_sec = 1500;
    s.intents = 12;
    s.live_aircraft = 449;
    s.aa_kills = 7;
    s.ground_captures = 2;
    s.agg_live = 5;
    s.deferred_releases = 1;
    const auto json = encode_json(s);
    // spot-pin the GROUP ORDER: tasking → ledger → ground → tiers
    EXPECT_NE(json.find("\"cycles\":3,\"next_tasking_sec\":1500,\"intents\":12"),
              std::string::npos);
    EXPECT_NE(json.find("\"live_aircraft\":449"), std::string::npos);
    EXPECT_NE(json.find("\"aa_kills\":7"), std::string::npos);
    EXPECT_NE(json.find("\"ground_captures\":2"), std::string::npos);
    EXPECT_NE(json.find("\"agg_live\":5"), std::string::npos);
    // last key, no trailing comma
    EXPECT_EQ(json.substr(json.size() - std::strlen("\"deferred_releases\":1}")),
              "\"deferred_releases\":1}");
    EXPECT_EQ(json.find(",}"), std::string::npos);
}

// ============================================================================
// flights — the FID tier view
// ============================================================================

TEST(DtoGoldens, FlightViewArray) {
    FlightView a;
    a.vu = 118;
    a.team = 2;
    a.mission = 9;
    a.aircraft_count = 2;
    a.x_grid = 390.25;
    a.y_grid = 455.75;
    a.altitude_ft = 20000.0f;
    a.fuel_burnt = 1200;
    a.live = true;
    a.to_depart = -1;
    a.to_mission_over = 900;
    FlightView b; // the defaults (an aggregate that never departed)
    b.to_depart = 600;
    FlightView c; // CAMP-CMD-2: an aborted flight (the additive tail)
    c.vu = 55;
    c.aborted = true;

    std::vector<FlightView> flights{a, b, c};
    EXPECT_EQ(encode_json(flights),
              R"([{"vu":118,"team":2,"mission":9,"aircraft_count":2,)"
              R"("x_grid":390.25,"y_grid":455.75,"altitude_ft":20000,)"
              R"("fuel_burnt":1200,"live":1,"arrived":0,"destroyed":0,)"
              R"("to_depart":-1,"to_mission_over":900,"aborted":0},)"
              R"({"vu":0,"team":0,"mission":0,"aircraft_count":0,)"
              R"("x_grid":0,"y_grid":0,"altitude_ft":0,"fuel_burnt":0,)"
              R"("live":0,"arrived":0,"destroyed":0,"to_depart":600,)"
              R"("to_mission_over":-1,"aborted":0},)"
              R"({"vu":55,"team":0,"mission":0,"aircraft_count":0,)"
              R"("x_grid":0,"y_grid":0,"altitude_ft":0,"fuel_burnt":0,)"
              R"("live":0,"arrived":0,"destroyed":0,"to_depart":-1,)"
              R"("to_mission_over":-1,"aborted":1}])");
}

TEST(DtoGoldens, EmptyFlightArray) {
    EXPECT_EQ(encode_json(std::vector<FlightView>{}), "[]");
}

// ============================================================================
// tasking — the ATO row
// ============================================================================

TEST(DtoGoldens, IntentView) {
    IntentView m;
    m.issued_time = 38574400;
    m.time_on_target = 38578000;
    m.team = 2;
    m.team_name = "ROK";
    m.mission_byte = 9;
    m.mission_name = "OCA";
    m.aircraft_count = 4;
    m.squadron_id = 214;
    m.squadron_name = "111th TFS";
    m.package_id = 42;
    m.flight_id = 118;
    m.target_objective_id = 9001;
    m.synthetic = true;
    // CAMP-HOST-3: the additive tail — the C3 route leg count and the
    // package role ride at the END (the DTO header's rule).
    m.route_waypoints = 7;
    m.flight_role = 2;
    // CAMP-DOM-4: the additive tail's newest key — the scheduled
    // takeoff slot (the phase-7 snap's output; 0 = never slotted —
    // this golden pins the never-slotted face, the always-present key).
    m.takeoff = 0;
    EXPECT_EQ(encode_json(m),
              R"({"issued_time":38574400,"time_on_target":38578000,"team":2,)"
              R"("team_name":"ROK","mission_byte":9,"mission_name":"OCA",)"
              R"("aircraft_count":4,"squadron_id":214,"squadron_name":"111th TFS",)"
              R"("package_id":42,"flight_id":118,"target_objective_id":9001,)"
              R"("synthetic":1,"route_waypoints":7,"flight_role":2,)"
              R"("takeoff":0})");
}

// ============================================================================
// objectives — ownership, logistics, the damage bitmap
// ============================================================================

TEST(DtoGoldens, ObjectiveViewWithFstatus) {
    ObjectiveView o;
    o.id_creator = 6;
    o.id_num = 9001;
    o.type = 105;
    o.objective_type = 3;
    o.entity_type = 105;
    o.x = 390;
    o.y = 455;
    o.z = 120.0f;
    o.owner = 2;
    o.first_owner = 2;
    o.priority = 5;
    o.nameid = 77;
    o.obj_flags = 0x400;
    o.parent_id = 0;
    o.supply = 80;
    o.fuel = 65;
    o.losses = 3;
    o.last_repair = 38570000;
    o.has_radar = true;
    o.radar_range_km = 185.0f;
    o.fstatus = {0x00, 0x15, 0xFF};
    EXPECT_EQ(encode_json(o),
              R"({"id_creator":6,"id_num":9001,"type":105,"objective_type":3,)"
              R"("entity_type":105,"x":390,"y":455,"z":120,"owner":2,)"
              R"("first_owner":2,"priority":5,"nameid":77,"obj_flags":1024,)"
              R"("parent_id":0,"supply":80,"fuel":65,"losses":3,)"
              R"("last_repair":38570000,"has_radar":1,"radar_range_km":185,)"
              R"("fstatus":[0,21,255]})");
}

// ============================================================================
// the event vocabulary — pinned in v1, emitted in HOST-2
// ============================================================================

TEST(EventGoldens, KillEvent) {
    KillEvent e;
    e.t = 357;
    e.killer_squadron = 214;
    e.killer_team = 0;
    e.victim_squadron = 317;
    e.victim_team = 1;
    e.weapon = "aim7";
    EXPECT_EQ(encode_json(e),
              R"({"ev":"kill","t":357,"killer":{"sq":214,"team":0},)"
              R"("victim":{"sq":317,"team":1},"weapon":"aim7"})");
}

TEST(EventGoldens, MissionFiledEvent) {
    MissionFiledEvent e;
    e.t = 1800;
    e.package_id = 42;
    e.flight_id = 118;
    e.team = 2;
    e.mission_byte = 9;
    e.mission_name = "OCA";
    e.target_objective_id = 9001;
    e.synthetic = true;
    EXPECT_EQ(encode_json(e),
              R"({"ev":"mission_filed","t":1800,"package_id":42,"flight_id":118,)"
              R"("team":2,"mission_byte":9,"mission_name":"OCA",)"
              R"("target_objective_id":9001,"synthetic":1})");
}

TEST(EventGoldens, ObjectiveCapturedEvent) {
    ObjectiveCapturedEvent e;
    e.t = 72000;
    e.objective_id = 9001;
    e.new_owner = 6;
    EXPECT_EQ(encode_json(e),
              R"({"ev":"objective_captured","t":72000,"objective_id":9001,"new_owner":6})");
}

TEST(EventGoldens, RoeChangedEvent) {
    RoeChangedEvent e;
    e.t = 90;
    e.scope.kind = RoEScopeKind::Team;
    e.scope.team = 1;
    e.roe = RoeLevel::Tight;
    EXPECT_EQ(encode_json(e),
              R"({"ev":"roe_changed","t":90,"scope":{"kind":"team","team":1,)"
              R"("mission":0,"flight":0},"roe":1})");
}

TEST(EventGoldens, TaskingCycleEvent) {
    TaskingCycleEvent e;
    e.t = 3600;
    e.cycles = 2;
    e.next_tasking_sec = 1800;
    e.intents = 96;
    EXPECT_EQ(encode_json(e),
              R"({"ev":"tasking_cycle","t":3600,"cycles":2,)"
              R"("next_tasking_sec":1800,"intents":96})");
}

TEST(EventGoldens, WeatherAndReinforcement) {
    WeatherChangedEvent w;
    w.t = 43200;
    w.condition = "overcast";
    EXPECT_EQ(encode_json(w),
              R"({"ev":"weather_changed","t":43200,"condition":"overcast"})");

    ReinforcementDeliveredEvent r;
    r.t = 43200;
    r.aircraft = 232;
    r.squadrons_touched = 22;
    EXPECT_EQ(encode_json(r),
              R"({"ev":"reinforcement_delivered","t":43200,"aircraft":232,)"
              R"("squadrons_touched":22})");
}

TEST(EventGoldens, VerdictEvent) {
    // CAMP-DOM-1 — the tenth family: the books' projection moved (the
    // leader's slot + swing ride the wake-up; the rows stay on the
    // query).
    VerdictEvent v;
    v.t = 7260;
    v.band = "decisive";
    v.leader = 6;
    v.swing = 130;
    EXPECT_EQ(encode_json(v),
              R"({"ev":"verdict","t":7260,"band":"decisive",)"
              R"("leader":6,"swing":130})");

    VerdictEvent none;  // a DEFAULT payload — the DTO never invents
                        // vocabulary; the producer always writes the
                        // band word, so the unset value rides empty
    none.t = 9000;
    EXPECT_EQ(encode_json(none),
              R"({"ev":"verdict","t":9000,"band":"",)"
              R"("leader":-1,"swing":0})");
}

// ============================================================================
// threat — the C3 SAM-ring grid (CAMP-HOST-3)
// ============================================================================

TEST(DtoGoldens, ThreatViewGolden) {
    ThreatView t;
    t.viewer_team = 1;
    t.cell_grid = 6;
    t.cells_x = 2;
    t.cells_y = 2;
    t.low = {0, 3, 1, 0};
    t.high = {2, 0, 0, 4};
    EXPECT_EQ(encode_json(t),
              R"({"viewer_team":1,"cell_grid":6,"cells_x":2,"cells_y":2,)"
              R"("low":[0,3,1,0],"high":[2,0,0,4]})");
}

TEST(DtoGoldens, ThreatViewEmptyGrid) {
    ThreatView t;  // no map built yet: zeros + empty bands
    EXPECT_EQ(encode_json(t),
              R"({"viewer_team":0,"cell_grid":0,"cells_x":0,"cells_y":0,)"
              R"("low":[],"high":[]})");
}

// ============================================================================
// verdict — the books' projection (CAMP-DOM-1)
// ============================================================================

TEST(DtoGoldens, VerdictViewGolden) {
    VerdictView v;
    v.t = 7260;
    v.threshold = 0;
    v.band = "advantage";
    v.leader = 2;
    v.leader_swing = 30;
    VerdictTeamRow rok;
    rok.slot = 2;
    rok.name = "ROK";
    rok.owned = 2;
    rok.gained = 1;
    rok.gained_value = 30;
    rok.swing = 30;
    rok.captures = 1;
    rok.aircraft_remaining = 38;
    VerdictTeamRow dprk;
    dprk.slot = 6;
    dprk.name = "DPRK";
    dprk.lost = 1;
    dprk.lost_value = 30;
    dprk.swing = -30;
    dprk.ground_losses = 4;
    v.teams = {rok, dprk};
    EXPECT_EQ(encode_json(v),
              R"({"t":7260,"threshold":0,"band":"advantage","leader":2,)"
              R"("leader_swing":30,"teams":[)"
              R"({"slot":2,"name":"ROK","owned":2,"gained":1,"lost":0,)"
              R"("gained_value":30,"lost_value":0,"swing":30,"captures":1,)"
              R"("air_losses":0,"ground_losses":0,"battalions_destroyed":0,)"
              R"("aircraft_remaining":38},)"
              R"({"slot":6,"name":"DPRK","owned":0,"gained":0,"lost":1,)"
              R"("gained_value":0,"lost_value":30,"swing":-30,"captures":0,)"
              R"("air_losses":0,"ground_losses":4,"battalions_destroyed":0,)"
              R"("aircraft_remaining":0}]})");
}

TEST(DtoGoldens, VerdictViewQuietWar) {
    VerdictView v;  // a DEFAULT view — the band word is the producer's
                    // (the session always writes band_name); unset rides
                    // empty, the no-lead numeric defaults still hold
    VerdictTeamRow row;
    row.slot = 2;
    row.name = "ROK";
    row.owned = 1;
    v.teams = {row};
    EXPECT_EQ(encode_json(v),
              R"({"t":0,"threshold":0,"band":"","leader":-1,)"
              R"("leader_swing":0,"teams":[{"slot":2,"name":"ROK",)"
              R"("owned":1,"gained":0,"lost":0,"gained_value":0,)"
              R"("lost_value":0,"swing":0,"captures":0,"air_losses":0,)"
              R"("ground_losses":0,"battalions_destroyed":0,)"
              R"("aircraft_remaining":0}]})");
}

TEST(DtoGoldens, AirfieldView) {
    // CAMP-DOM-4 — the scheduling face: the grid rides as 64 lowercase
    // hex chars (block 0's byte first), the anchor is the campaign-
    // minute block 0 maps to, and the denial books ride the row.
    AirfieldView a;
    a.vu = 4281;
    a.epoch_min = 75;
    a.schedule = "04000000000000000000000000000000"
                 "00000000000000000000000000000000";
    a.booked = 2;
    a.denied = 1;
    a.overflowed = 0;
    EXPECT_EQ(encode_json(a),
              R"({"vu":4281,"epoch_min":75,"schedule":)"
              R"("04000000000000000000000000000000)"
              R"(00000000000000000000000000000000","booked":2,)"
              R"("denied":1,"overflowed":0})");
}

TEST(DtoGoldens, AirfieldViewQuietGrid) {
    // The unset face: a DEFAULT view — the schedule hex is the
    // producer's (the session always fills the 64 chars from the grid;
    // the verdict-band rule — an unset DTO rides empty), the anchor
    // and the honest books stay 0.
    AirfieldView a;
    a.vu = 9001;
    EXPECT_EQ(encode_json(a),
              R"({"vu":9001,"epoch_min":0,"schedule":"",)"
              R"("booked":0,"denied":0,"overflowed":0})");
}

TEST(DtoGoldens, TaskForceView) {
    // CAMP-DOM-5 — the naval face: the wire's identity (the VU pair),
    // the owner slot, the sea subtype + the class table's name, the
    // position AND the wire's own destination (CAMP-DOM-6's naval
    // movement engine consumes it as the order), the TaskForce tail's
    // supply byte, this run's filing book, and the movement face's
    // heading byte (the additive tail, always present — the DTO rule).
    TaskForceView t;
    t.id_creator = 4040;
    t.id_num = 4040;
    t.team = 1;
    t.unit_subtype = 3;
    t.subtype_name = "Carrier";
    t.x = 753;
    t.y = 264;
    t.dest_x = 743;
    t.dest_y = 583;
    t.supply = 0;
    t.filings = 4;
    t.heading = 190;
    EXPECT_EQ(encode_json(t),
              R"({"id_creator":4040,"id_num":4040,"team":1,)"
              R"("unit_subtype":3,"subtype_name":"Carrier",)"
              R"("x":753,"y":264,"dest_x":743,"dest_y":583,)"
              R"("supply":0,"filings":4,"heading":190})");
}

TEST(DtoGoldens, TaskForceViewQuietRow) {
    // The unset face: a DEFAULT view — every wire fact 0, the
    // subtype name empty (the producer's word — the session always
    // fills it from the class table), the honest book at 0, the
    // heading byte at the wire's 0 (a force that never moved in this
    // run reports the wire's own byte).
    TaskForceView t;
    EXPECT_EQ(encode_json(t),
              R"({"id_creator":0,"id_num":0,"team":0,)"
              R"("unit_subtype":0,"subtype_name":"",)"
              R"("x":0,"y":0,"dest_x":0,"dest_y":0,)"
              R"("supply":0,"filings":0,"heading":0})");
}
