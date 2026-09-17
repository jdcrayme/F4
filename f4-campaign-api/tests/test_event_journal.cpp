// f4-campaign-api/tests/test_event_journal.cpp
//
// CAMP-HOST-2 — the journal's byte discipline and the filter's
// semantics, with NO engine in the link (the contract proves itself,
// the HOST-1 rule):
//
//   1. The journal's three line kinds pin byte-for-byte (header, event
//      lines — one golden example per event family, the plan §8 gate —
//      and the closing identity).
//   2. Round trip: a written journal verifies clean; a single flipped
//      byte (header, any event line, the closing identity) is DRIFT at
//      the named line; a golden with lines left over is drift too.
//   3. The subscription filter: per-family team matching (the kill's
//      OR semantics), the kind gate, "all", and the teamless families.

#include <f4/campaign/api/journal.hpp>
#include <f4/campaign/api/protocol.hpp>

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

using namespace f4::campaign::api;

namespace {

std::filesystem::path temp_file(const char* stem) {
    static int counter = 0;
    const auto dir = std::filesystem::temp_directory_path();
    return dir / (std::string(stem) + "_" + std::to_string(counter++) +
                  "_" +
                  std::to_string(
                      std::chrono::steady_clock::now()
                          .time_since_epoch()
                          .count()) +
                  ".jsonl");
}

std::string slurp(const std::filesystem::path& p) {
    std::ifstream f(p, std::ios::binary);
    std::stringstream buf;
    buf << f.rdbuf();
    return buf.str();
}

IdentityFingerprint fingerprint(std::int64_t t, const char* ledger) {
    IdentityFingerprint id;
    id.protocol_version = kProtocolVersion;
    id.campaign_time_s = t;
    id.ledger_fnv = to_hex16(fnv1a64(ledger));
    return id;
}

// The eight families, one golden example each (deterministic payloads;
// the `roe_changed`/`weather_changed` payloads are the values their
// eventual emitters will carry — the vocabulary is pinned even where
// the engine site lands in CAMP-CMD-1 / a scenario session).
std::vector<CampaignEvent> one_event_per_family() {
    std::vector<CampaignEvent> events;

    CampaignEvent filed;
    filed.kind = CampaignEvent::Kind::MissionFiled;
    filed.mission_filed.t = 1800;
    filed.mission_filed.package_id = 42;
    filed.mission_filed.flight_id = 118;
    filed.mission_filed.team = 2;
    filed.mission_filed.mission_byte = 9;
    filed.mission_filed.mission_name = "OCA";
    filed.mission_filed.target_objective_id = 9001;
    filed.mission_filed.synthetic = true;
    events.push_back(filed);

    CampaignEvent kill;
    kill.kind = CampaignEvent::Kind::Kill;
    kill.kill.t = 357;
    kill.kill.killer_squadron = 214;
    kill.kill.killer_team = 0;
    kill.kill.victim_squadron = 317;
    kill.kill.victim_team = 1;
    kill.kill.weapon = "missile";
    events.push_back(kill);

    CampaignEvent damage;
    damage.kind = CampaignEvent::Kind::ObjectiveDamage;
    damage.objective_damage.t = 3660;
    damage.objective_damage.objective_id = 9001;
    damage.objective_damage.owner = 2;
    damage.objective_damage.features_damaged = 5;
    events.push_back(damage);

    CampaignEvent captured;
    captured.kind = CampaignEvent::Kind::ObjectiveCaptured;
    captured.objective_captured.t = 72000;
    captured.objective_captured.objective_id = 9001;
    captured.objective_captured.new_owner = 6;
    events.push_back(captured);

    CampaignEvent reinforced;
    reinforced.kind = CampaignEvent::Kind::ReinforcementDelivered;
    reinforced.reinforcement_delivered.t = 43200;
    reinforced.reinforcement_delivered.aircraft = 12;
    reinforced.reinforcement_delivered.squadrons_touched = 3;
    events.push_back(reinforced);

    CampaignEvent weather;
    weather.kind = CampaignEvent::Kind::WeatherChanged;
    weather.weather_changed.t = 43200;
    weather.weather_changed.condition = "inclement";
    events.push_back(weather);

    CampaignEvent roe;
    roe.kind = CampaignEvent::Kind::RoeChanged;
    roe.roe_changed.t = 90;
    roe.roe_changed.scope.kind = RoEScopeKind::Team;
    roe.roe_changed.scope.team = 1;
    roe.roe_changed.roe = RoeLevel::Tight;
    events.push_back(roe);

    CampaignEvent cycle;
    cycle.kind = CampaignEvent::Kind::TaskingCycle;
    cycle.tasking_cycle.t = 3600;
    cycle.tasking_cycle.cycles = 2;
    cycle.tasking_cycle.next_tasking_sec = 1800;
    cycle.tasking_cycle.intents = 96;
    events.push_back(cycle);

    return events;
}

} // namespace

// ============================================================================
// the journal's bytes — header, per-family event lines, closing identity
// ============================================================================

TEST(EventJournal, HeaderAndEndBytes) {
    const auto start = fingerprint(38574360, "start-ledger");
    const auto end = fingerprint(38659920, "end-ledger");

    f4::json::Writer h;
    encode_journal_header(h, start);
    EXPECT_EQ(h.str(),
              "{\"v\":1,\"journal\":1,\"identity\":{\"protocol\":1,"
              "\"campaign_time_s\":38574360,\"ledger_fnv\":\"" +
                  start.ledger_fnv + "\"}}");

    f4::json::Writer e;
    encode_journal_end(e, end);
    EXPECT_EQ(e.str(),
              "{\"journal_end\":{\"protocol\":1,"
              "\"campaign_time_s\":38659920,\"ledger_fnv\":\"" +
                  end.ledger_fnv + "\"}}");
}

TEST(EventJournal, OneGoldenLinePerFamily) {
    const auto events = one_event_per_family();
    ASSERT_EQ(events.size(), 8U);

    f4::json::Writer w;
    encode(w, events[0]);
    EXPECT_EQ(w.str(),
              R"({"ev":"mission_filed","t":1800,"package_id":42,"flight_id":118,)"
              R"("team":2,"mission_byte":9,"mission_name":"OCA",)"
              R"("target_objective_id":9001,"synthetic":1})");

    w = f4::json::Writer{};
    encode(w, events[1]);
    EXPECT_EQ(w.str(),
              R"({"ev":"kill","t":357,"killer":{"sq":214,"team":0},)"
              R"("victim":{"sq":317,"team":1},"weapon":"missile"})");

    // the objective_damage family's golden (the one DTO-goldens gap)
    w = f4::json::Writer{};
    encode(w, events[2]);
    EXPECT_EQ(w.str(),
              R"({"ev":"objective_damage","t":3660,"objective_id":9001,)"
              R"("owner":2,"features_damaged":5})");

    w = f4::json::Writer{};
    encode(w, events[3]);
    EXPECT_EQ(w.str(),
              R"({"ev":"objective_captured","t":72000,"objective_id":9001,)"
              R"("new_owner":6})");

    w = f4::json::Writer{};
    encode(w, events[4]);
    EXPECT_EQ(w.str(),
              R"({"ev":"reinforcement_delivered","t":43200,"aircraft":12,)"
              R"("squadrons_touched":3})");

    w = f4::json::Writer{};
    encode(w, events[5]);
    EXPECT_EQ(w.str(),
              R"({"ev":"weather_changed","t":43200,"condition":"inclement"})");

    w = f4::json::Writer{};
    encode(w, events[6]);
    EXPECT_EQ(w.str(),
              R"({"ev":"roe_changed","t":90,"scope":{"kind":"team","team":1,)"
              R"("mission":0,"flight":0},"roe":1})");

    w = f4::json::Writer{};
    encode(w, events[7]);
    EXPECT_EQ(w.str(),
              R"({"ev":"tasking_cycle","t":3600,"cycles":2,)"
              R"("next_tasking_sec":1800,"intents":96})");
}

TEST(EventJournal, WrittenFileShape) {
    const auto path = temp_file("f4_journal");
    const auto start = fingerprint(1000, "war-a");
    const auto end = fingerprint(2000, "war-b");

    EventJournalWriter j;
    ASSERT_TRUE(j.open(path.string(), start));
    for (const auto& e : one_event_per_family()) {
        j.append(e);
    }
    ASSERT_TRUE(j.close(end));
    EXPECT_EQ(j.detail(), path.string());

    const auto text = slurp(path);
    // 1 header + 8 events + 1 end = 10 lines, every line ending \n
    EXPECT_EQ(std::count(text.begin(), text.end(), '\n'), 10);
    EXPECT_EQ(text.find("{\"v\":1,\"journal\":1,\"identity\":{"), 0U);
    EXPECT_NE(text.find("\n{\"ev\":\"kill\""), std::string::npos);
    EXPECT_NE(text.find("{\"journal_end\":{\"protocol\":1,"),
              std::string::npos);
    std::filesystem::remove(path);
}

// ============================================================================
// the verifier — replay identity and the drift it guards (exit 23's root)
// ============================================================================

TEST(EventJournal, VerifiesAReplayByteForByte) {
    const auto path = temp_file("f4_journal_replay");
    const auto start = fingerprint(1000, "war-a");
    const auto end = fingerprint(2000, "war-b");
    const auto events = one_event_per_family();

    EventJournalWriter j;
    ASSERT_TRUE(j.open(path.string(), start));
    for (const auto& e : events) j.append(e);
    ASSERT_TRUE(j.close(end));

    EventJournalVerifier v;
    ASSERT_TRUE(v.open(path.string(), start)) << v.detail();
    for (const auto& e : events) {
        ASSERT_TRUE(v.expect(e)) << v.detail();
    }
    ASSERT_TRUE(v.close(end)) << v.detail();
    std::filesystem::remove(path);
}

TEST(EventJournal, HeaderDriftIsNamed) {
    const auto path = temp_file("f4_journal_hdr");
    EventJournalWriter j;
    ASSERT_TRUE(j.open(path.string(), fingerprint(1000, "war-a")));
    ASSERT_TRUE(j.close(fingerprint(2000, "war-b")));

    EventJournalVerifier v;
    std::string err;
    // a DIFFERENT save/seed → a different start identity → drift at open
    EXPECT_FALSE(v.open(path.string(), fingerprint(1000, "war-other")));
    EXPECT_NE(v.detail().find("line 1"), std::string::npos);
    (void)err;
    std::filesystem::remove(path);
}

TEST(EventJournal, EventLineDriftIsNamed) {
    const auto path = temp_file("f4_journal_evt");
    const auto start = fingerprint(1000, "war-a");
    const auto end = fingerprint(2000, "war-b");

    EventJournalWriter j;
    ASSERT_TRUE(j.open(path.string(), start));
    const auto events = one_event_per_family();
    for (const auto& e : events) j.append(e);
    ASSERT_TRUE(j.close(end));

    EventJournalVerifier v;
    ASSERT_TRUE(v.open(path.string(), start));
    // replay emits the same families but ONE payload differs (the kill's
    // victim squadron) — the books would have moved, the identity too
    auto drifted = events;
    drifted[1].kill.victim_squadron = 318;
    std::string err;
    ASSERT_TRUE(v.expect(drifted[0], &err)) << v.detail();
    EXPECT_FALSE(v.expect(drifted[1], &err));
    EXPECT_EQ(v.line(), 3U); // header + mission_filed + the kill line
    EXPECT_NE(v.detail().find("line 3 diverges"), std::string::npos);
    (void)err;
    std::filesystem::remove(path);
}

TEST(EventJournal, EndDriftIsNamed) {
    const auto path = temp_file("f4_journal_end");
    const auto start = fingerprint(1000, "war-a");

    EventJournalWriter j;
    ASSERT_TRUE(j.open(path.string(), start));
    ASSERT_TRUE(j.close(fingerprint(2000, "war-b")));

    EventJournalVerifier v;
    ASSERT_TRUE(v.open(path.string(), start));
    std::string err;
    // the replay's ledger moved (a different identity at EOF)
    EXPECT_FALSE(v.close(fingerprint(2001, "war-b"), &err));
    EXPECT_NE(v.detail().find("line 2 diverges"), std::string::npos);
    (void)err;
    std::filesystem::remove(path);
}

TEST(EventJournal, GoldenWithLinesLeftOverIsDrift) {
    const auto path = temp_file("f4_journal_extra");
    const auto start = fingerprint(1000, "war-a");
    const auto end = fingerprint(2000, "war-b");

    EventJournalWriter j;
    ASSERT_TRUE(j.open(path.string(), start));
    j.append(one_event_per_family()[0]); // the golden saw a mission_filed
    ASSERT_TRUE(j.close(end));

    // the live run produced NOTHING (fewer events than the record)
    EventJournalVerifier v;
    ASSERT_TRUE(v.open(path.string(), start));
    std::string err;
    EXPECT_FALSE(v.close(end, &err));
    EXPECT_NE(v.detail().find("diverges"), std::string::npos);
    (void)err;
    std::filesystem::remove(path);
}

// ============================================================================
// the subscription filter — per-family team semantics (plan §3.4)
// ============================================================================

TEST(EventFilter, KindGateAndAll) {
    EventFilter f;
    f.kinds = {CampaignEvent::Kind::Kill};
    const auto events = one_event_per_family();

    EXPECT_TRUE(matches(f, events[1]));  // kill
    EXPECT_FALSE(matches(f, events[0])); // mission_filed
    EXPECT_FALSE(matches(f, events[7])); // tasking_cycle

    EventFilter all;
    all.all = true;
    for (const auto& e : events) {
        EXPECT_TRUE(matches(all, e));
    }

    EventFilter none; // no kinds named, not all → nothing rides
    for (const auto& e : events) {
        EXPECT_FALSE(matches(none, e));
    }
}

TEST(EventFilter, KillMatchesEitherSide) {
    EventFilter rok;
    rok.all = true;
    rok.teams = {2}; // ROK
    EventFilter dprk = rok;
    dprk.teams = {6}; // DPRK

    CampaignEvent kill;
    kill.kind = CampaignEvent::Kind::Kill;
    kill.kill.t = 10;
    kill.kill.killer_team = 2;
    kill.kill.victim_team = 6;

    // a war-room on EITHER side of the fight sees the kill
    EXPECT_TRUE(matches(rok, kill));
    EXPECT_TRUE(matches(dprk, kill));

    EventFilter neutral;
    neutral.all = true;
    neutral.teams = {0};
    EXPECT_FALSE(matches(neutral, kill));
}

TEST(EventFilter, TeamlessFamiliesMatchAnyTeamGate) {
    EventFilter f;
    f.all = true;
    f.teams = {2};

    const auto events = one_event_per_family();
    EXPECT_TRUE(matches(f, events[4])); // reinforcement_delivered
    EXPECT_TRUE(matches(f, events[5])); // weather_changed
    EXPECT_TRUE(matches(f, events[7])); // tasking_cycle
}

TEST(EventFilter, OwnedFamiliesMatchTheOwningSide) {
    EventFilter rok;
    rok.all = true;
    rok.teams = {2};

    CampaignEvent damage;
    damage.kind = CampaignEvent::Kind::ObjectiveDamage;
    damage.objective_damage.t = 10;
    damage.objective_damage.owner = 6;
    EXPECT_FALSE(matches(rok, damage));
    damage.objective_damage.owner = 2;
    EXPECT_TRUE(matches(rok, damage));

    CampaignEvent captured;
    captured.kind = CampaignEvent::Kind::ObjectiveCaptured;
    captured.objective_captured.t = 10;
    captured.objective_captured.new_owner = 6;
    EXPECT_FALSE(matches(rok, captured));

    CampaignEvent filed;
    filed.kind = CampaignEvent::Kind::MissionFiled;
    filed.mission_filed.t = 10;
    filed.mission_filed.team = 6;
    EXPECT_FALSE(matches(rok, filed));

    CampaignEvent roe;
    roe.kind = CampaignEvent::Kind::RoeChanged;
    roe.roe_changed.t = 10;
    roe.roe_changed.scope.kind = RoEScopeKind::Team;
    roe.roe_changed.scope.team = 6;
    EXPECT_FALSE(matches(rok, roe));
    // a mission/flight scope has no team slot — it matches any gate
    roe.roe_changed.scope.kind = RoEScopeKind::Flight;
    EXPECT_TRUE(matches(rok, roe));
}

TEST(EventFilter, KindNamesRoundTrip) {
    for (const auto k : {
             CampaignEvent::Kind::MissionFiled,
             CampaignEvent::Kind::Kill,
             CampaignEvent::Kind::ObjectiveDamage,
             CampaignEvent::Kind::ObjectiveCaptured,
             CampaignEvent::Kind::ReinforcementDelivered,
             CampaignEvent::Kind::WeatherChanged,
             CampaignEvent::Kind::RoeChanged,
             CampaignEvent::Kind::TaskingCycle,
         }) {
        CampaignEvent::Kind parsed{};
        ASSERT_TRUE(parse_event_kind(event_kind_name(k), parsed));
        EXPECT_EQ(parsed, k);
    }
    CampaignEvent::Kind parsed{};
    EXPECT_FALSE(parse_event_kind("meteor_shower", parsed));
    EXPECT_FALSE(parse_event_kind("all", parsed)); // "all" is the op's
}
