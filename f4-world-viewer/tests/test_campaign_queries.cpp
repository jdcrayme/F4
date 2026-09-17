// f4-world-viewer/tests/test_campaign_queries.cpp
//
// CAMP-HOST-3 — the contract-plane query walks, pinned against the
// DTO encoders' byte-stable output shapes. campaign_queries.cpp is the
// viewer's ENTIRE campaign-state data access (the two-plane rule):
// these pins are the parity story for what the windows render.
//
// The walks must be ORDER-INDEPENDENT (the contract forbids relying on
// key order even though today's encoders are fixed) and ADDITIVE-
// TOLERANT (unknown keys skip; the DTO rule says new fields appear at
// the END without a protocol bump). Both are pinned here.

#include "campaign_queries.hpp"

#include <f4/campaign/api/session.hpp>
#include <gtest/gtest.h>

#include <map>
#include <string>

namespace f4::viewer {
namespace {

using f4::campaign::api::CampaignEvent;
using f4::campaign::api::CommandAck;
using f4::campaign::api::CommandIntent;
using f4::campaign::api::EventFilter;
using f4::campaign::api::ICampaignSession;
using f4::campaign::api::IdentityFingerprint;
using f4::campaign::api::QueryResult;
using f4::campaign::api::QuerySpec;
using f4::campaign::api::SaveResult;
using f4::campaign::api::StepResult;

// A canned session: every query name maps to a fixed payload; anything
// else answers !ok (the engine's unknown-name shape).
class CannedSession final : public ICampaignSession {
public:
    std::map<std::string, std::string> payloads;
    bool answer_ok = true;
    std::string last_query_name;

    IdentityFingerprint identity() const override { return {}; }
    StepResult step(std::uint32_t) override { return {}; }
    void set_time_scale(double) override {}
    void set_paused(bool) override {}
    SaveResult save(std::string_view path) override {
        SaveResult r;
        r.ok = true;
        r.detail = std::string(path);
        return r;
    }
    QueryResult query(const QuerySpec& spec) override {
        last_query_name = spec.name;
        QueryResult r;
        const auto it = payloads.find(spec.name);
        r.ok = answer_ok && it != payloads.end();
        if (r.ok) r.data_json = it->second;
        if (!r.ok) r.detail = "canned: no payload for " + spec.name;
        return r;
    }
    CommandAck submit(const CommandIntent&) override { return {}; }
    void set_event_filter(const EventFilter&) override {}
    std::vector<CampaignEvent> drain_events() override { return {}; }
};

// ---------------------------------------------------------------------------
// time
// ---------------------------------------------------------------------------

TEST(QueryWalks, TimeFields) {
    CannedSession s;
    s.payloads["time"] =
        "{\"tick_sec\":0.0166666666666667,\"campaign_time_s\":518400,"
        "\"sim_time_s\":120.5,\"paused\":1,\"next_tasking_sec\":1679,"
        "\"time_scale\":10}";
    const auto t = fetch_time(s);
    ASSERT_TRUE(t.ok);
    EXPECT_DOUBLE_EQ(t.tick_sec, 0.0166666666666667);
    EXPECT_EQ(t.campaign_time_s, 518400);
    EXPECT_DOUBLE_EQ(t.sim_time_s, 120.5);
    EXPECT_TRUE(t.paused);
    EXPECT_EQ(t.next_tasking_sec, 1679);
    EXPECT_DOUBLE_EQ(t.time_scale, 10.0);
}

TEST(QueryWalks, TimeUnknownKeyIsSkipped) {
    // The additive-field rule: a future wire adds keys at the END; a
    // client that hard-fails on them ossifies the wire.
    CannedSession s;
    s.payloads["time"] =
        "{\"tick_sec\":0.016,\"campaign_time_s\":7,\"sim_time_s\":0,"
        "\"paused\":0,\"next_tasking_sec\":0,\"time_scale\":1,"
        "\"future_thing\":123,\"another\":{\"nested\":true}}";
    const auto t = fetch_time(s);
    ASSERT_TRUE(t.ok);
    EXPECT_EQ(t.campaign_time_s, 7);
    EXPECT_FALSE(t.paused);
}

TEST(QueryWalks, QueryFailureReadsAsDefaults) {
    CannedSession s;  // no payloads at all
    const auto t = fetch_time(s);
    EXPECT_FALSE(t.ok);
    EXPECT_EQ(t.campaign_time_s, 0);
    EXPECT_FALSE(t.paused);
}

// ---------------------------------------------------------------------------
// stats
// ---------------------------------------------------------------------------

TEST(QueryWalks, StatsFields) {
    CannedSession s;
    s.payloads["stats"] =
        "{\"cycles\":3,\"next_tasking_sec\":1500,\"intents\":12,"
        "\"routes_built\":10,\"routes_failed\":2,\"route_waypoints\":96,"
        "\"drawn_aircraft\":30,\"air_losses\":1,\"reinforce_fires\":2,"
        "\"reinforced\":8,\"synthetic_spawned\":6,\"live_aircraft\":24,"
        "\"airborne\":18,\"sim_time_s\":3600.25,\"retired\":2,"
        "\"packages\":4,\"escorts\":2,\"recovered\":1,"
        "\"armed_aircraft\":20,\"armed_fighters\":8,\"armed_defensive\":12,"
        "\"aa_kills\":1,\"ground_updates\":99,\"ground_battalions\":12,"
        "\"ground_mobile\":7,\"ground_losses\":3,\"ground_losses_air\":1,"
        "\"ground_destroyed\":2,\"ground_captures\":1,\"ground_engaged\":5,"
        "\"ground_front_columns\":3,\"agg_updates\":11,\"agg_flights\":9,"
        "\"agg_live\":2,\"agg_arrived\":1,\"agg_destroyed\":1,"
        "\"tier_deaggs\":4,\"tier_reaggs\":3,\"combat_deaggs\":1,"
        "\"synthetic_aggregates\":2,\"agg_contacts\":1,"
        "\"deferred_releases\":0}";
    const auto v = fetch_stats(s);
    ASSERT_TRUE(v.ok);
    EXPECT_EQ(v.cycles, 3);
    EXPECT_EQ(v.intents, 12);
    EXPECT_EQ(v.routes_built, 10);
    EXPECT_EQ(v.routes_failed, 2);
    EXPECT_EQ(v.route_waypoints, 96);
    EXPECT_EQ(v.live_aircraft, 24);
    EXPECT_EQ(v.airborne, 18);
    EXPECT_DOUBLE_EQ(v.sim_time_s, 3600.25);
    EXPECT_EQ(v.agg_flights, 9);
    EXPECT_EQ(v.tier_deaggs, 4);
    EXPECT_EQ(v.deferred_releases, 0);
}

// ---------------------------------------------------------------------------
// flights
// ---------------------------------------------------------------------------

TEST(QueryWalks, FlightsRows) {
    CannedSession s;
    s.payloads["flights"] =
        "[{\"vu\":118,\"team\":1,\"mission\":9,\"aircraft_count\":4,"
        "\"x_grid\":55.5,\"y_grid\":66.25,\"altitude_ft\":25000,"
        "\"fuel_burnt\":1200,\"live\":0,\"arrived\":0,\"destroyed\":0,"
        "\"to_depart\":90,\"to_mission_over\":-1},"
        "{\"vu\":119,\"team\":2,\"mission\":5,\"aircraft_count\":2,"
        "\"x_grid\":10,\"y_grid\":20,\"altitude_ft\":1500,"
        "\"fuel_burnt\":0,\"live\":1,\"arrived\":0,\"destroyed\":0,"
        "\"to_depart\":-1,\"to_mission_over\":600}]";
    const auto rows = fetch_flights(s);
    ASSERT_EQ(rows.size(), 2u);
    EXPECT_EQ(rows[0].vu, 118u);
    EXPECT_EQ(rows[0].team, 1);
    EXPECT_EQ(rows[0].mission, 9);
    EXPECT_EQ(rows[0].aircraft_count, 4);
    EXPECT_DOUBLE_EQ(rows[0].x_grid, 55.5);
    EXPECT_DOUBLE_EQ(rows[0].y_grid, 66.25);
    EXPECT_FLOAT_EQ(rows[0].altitude_ft, 25000.0f);
    EXPECT_EQ(rows[0].fuel_burnt, 1200);
    EXPECT_FALSE(rows[0].live);
    EXPECT_EQ(rows[0].to_depart, 90);
    EXPECT_EQ(rows[0].to_mission_over, -1);
    EXPECT_TRUE(rows[1].live);
    EXPECT_EQ(rows[1].to_mission_over, 600);
}

TEST(QueryWalks, FlightsEmptyArray) {
    CannedSession s;
    s.payloads["flights"] = "[]";
    EXPECT_TRUE(fetch_flights(s).empty());
}

// ---------------------------------------------------------------------------
// tasking
// ---------------------------------------------------------------------------

TEST(QueryWalks, TaskingRowsWithAdditiveTail) {
    CannedSession s;
    s.payloads["tasking"] =
        "[{\"issued_time\":3600,\"time_on_target\":7200,\"team\":1,"
        "\"team_name\":\"Blue\",\"mission_byte\":9,"
        "\"mission_name\":\"OCA strike\",\"aircraft_count\":4,"
        "\"squadron_id\":214,\"squadron_name\":\"111th TFS\","
        "\"package_id\":42,\"flight_id\":118,\"target_objective_id\":9001,"
        "\"synthetic\":1,\"route_waypoints\":7,\"flight_role\":1}]";
    const auto rows = fetch_tasking(s);
    ASSERT_EQ(rows.size(), 1u);
    EXPECT_EQ(rows[0].issued_time, 3600);
    EXPECT_EQ(rows[0].time_on_target, 7200);
    EXPECT_EQ(rows[0].team_name, "Blue");
    EXPECT_EQ(rows[0].mission_name, "OCA strike");
    EXPECT_EQ(rows[0].squadron_name, "111th TFS");
    EXPECT_EQ(rows[0].target_objective_id, 9001u);
    EXPECT_TRUE(rows[0].synthetic);
    EXPECT_EQ(rows[0].route_waypoints, 7);
    EXPECT_EQ(rows[0].flight_role, 1);
}

TEST(QueryWalks, TaskingEscapedStringsDecode) {
    CannedSession s;
    // The wire escapes embedded quotes/backslashes; read_string()
    // unescapes — the window renders the RAW name.
    s.payloads["tasking"] =
        "[{\"issued_time\":0,\"time_on_target\":0,\"team\":0,"
        "\"team_name\":\"team \\\"A\\\"\\\\2\",\"mission_byte\":0,"
        "\"mission_name\":\"\",\"aircraft_count\":0,\"squadron_id\":0,"
        "\"squadron_name\":\"\",\"package_id\":0,\"flight_id\":0,"
        "\"target_objective_id\":0,\"synthetic\":0,"
        "\"route_waypoints\":0,\"flight_role\":0}]";
    const auto rows = fetch_tasking(s);
    ASSERT_EQ(rows.size(), 1u);
    EXPECT_EQ(rows[0].team_name, "team \"A\"\\2");
}

// ---------------------------------------------------------------------------
// threat
// ---------------------------------------------------------------------------

TEST(QueryWalks, ThreatGridAndDensity) {
    CannedSession s;
    s.payloads["threat"] =
        "{\"viewer_team\":1,\"cell_grid\":6,\"cells_x\":2,\"cells_y\":2,"
        "\"low\":[0,3,1,0],\"high\":[2,0,0,4]}";
    const auto t = fetch_threat(s);
    ASSERT_TRUE(t.ok);
    EXPECT_EQ(t.viewer_team, 1);
    EXPECT_EQ(t.cell_grid, 6);
    EXPECT_EQ(t.cells_x, 2);
    EXPECT_EQ(t.cells_y, 2);
    // Row-major: (0,0)=0+2=2, (1,0)=3+0=3, (0,1)=1+0=1, (1,1)=0+4=4.
    EXPECT_EQ(t.density(0, 0), 2);
    EXPECT_EQ(t.density(1, 0), 3);
    EXPECT_EQ(t.density(0, 1), 1);
    EXPECT_EQ(t.density(1, 1), 4);
    // Out of range reads 0 (the painter culls, it never crashes).
    EXPECT_EQ(t.density(5, 5), 0);
    EXPECT_EQ(t.density(-1, 0), 0);
}

TEST(QueryWalks, ThreatEmpty) {
    CannedSession s;
    s.payloads["threat"] =
        "{\"viewer_team\":0,\"cell_grid\":0,\"cells_x\":0,\"cells_y\":0,"
        "\"low\":[],\"high\":[]}";
    const auto t = fetch_threat(s);
    ASSERT_TRUE(t.ok);
    EXPECT_EQ(t.cells_x, 0);
    EXPECT_EQ(t.density(0, 0), 0);
}

// ---------------------------------------------------------------------------
// books
// ---------------------------------------------------------------------------

TEST(QueryWalks, BooksLedgerUnescapesToExactBytes) {
    CannedSession s;
    s.payloads["books"] =
        "{\"ledger_json\":\"{\\\"campaign\\\":{\\\"time\\\":1}}\\n\\t"
        "second \\\"line\\\"\"}";
    const auto ledger = fetch_books_ledger(s);
    EXPECT_EQ(ledger, "{\"campaign\":{\"time\":1}}\n\tsecond \"line\"");
}

TEST(QueryWalks, BooksFailureReadsEmpty) {
    CannedSession s;
    EXPECT_TRUE(fetch_books_ledger(s).empty());
}

// ---------------------------------------------------------------------------
// the snapshot
// ---------------------------------------------------------------------------

TEST(QueryWalks, SnapshotGatesTheThreatWalk) {
    CannedSession s;
    s.payloads["time"] =
        "{\"tick_sec\":0.016,\"campaign_time_s\":0,\"sim_time_s\":0,"
        "\"paused\":1,\"next_tasking_sec\":0,\"time_scale\":1}";
    s.payloads["stats"] = "{\"cycles\":0}";
    s.payloads["flights"] = "[]";
    s.payloads["tasking"] = "[]";
    s.payloads["threat"] =
        "{\"viewer_team\":0,\"cell_grid\":6,\"cells_x\":1,\"cells_y\":1,"
        "\"low\":[1],\"high\":[0]}";

    const auto without = fetch_snapshot(s, false);
    EXPECT_TRUE(without.time.ok);
    EXPECT_TRUE(without.stats.ok);
    EXPECT_FALSE(without.threat.ok);  // the walk was gated off

    const auto with = fetch_snapshot(s, true);
    EXPECT_TRUE(with.threat.ok);
    EXPECT_EQ(with.threat.density(0, 0), 1);
}

TEST(QueryWalks, MalformedPayloadReadsAsDefaults) {
    // Loud failure discipline: a malformed body never yields partial
    // garbage rows — the fetch answers defaults and the caller's !ok /
    // empty-set path handles it.
    CannedSession s;
    s.answer_ok = true;
    s.payloads["flights"] = "[{\"vu\":118,\"team\":1,\"bogus_block\":";
    const auto rows = fetch_flights(s);
    EXPECT_TRUE(rows.empty());  // the Reader threw; the fetch caught it
}

} // namespace
} // namespace f4::viewer
