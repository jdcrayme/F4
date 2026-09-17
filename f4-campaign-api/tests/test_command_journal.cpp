// f4-campaign-api/tests/test_command_journal.cpp
//
// CAMP-CMD-1 — the command journal's byte discipline, with NO engine in
// the link (the contract proves itself, the HOST-1 rule):
//
//   1. The intent body encoder pins byte-for-byte for every kind (the
//      canonical key order IS the contract; the parse side accepts it
//      back verbatim).
//   2. The journal's three line kinds pin byte-for-byte (header,
//      roe_set lines — one golden per scope kind — and the closing
//      identity).
//   3. Round trip: a written journal loads clean through the reader —
//      entries equal in order, the footer identity captured; header
//      drift, a malformed line, backwards ticks, and a missing footer
//      all fail loudly at the named line.

#include <f4/campaign/api/command_journal.hpp>
#include <f4/campaign/api/protocol.hpp>

#include <gtest/gtest.h>

#include <chrono>
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

IdentityFingerprint start_identity() {
    IdentityFingerprint id;
    id.protocol_version = 1;
    id.campaign_time_s = 38574360;
    id.ledger_fnv = "0123456789abcdef";
    return id;
}

IdentityFingerprint end_identity() {
    IdentityFingerprint id;
    id.protocol_version = 1;
    id.campaign_time_s = 38574500;
    id.ledger_fnv = "fedcba9876543210";
    return id;
}

CommandIntent roe_set(RoEScopeKind kind, std::uint8_t team,
                      std::uint8_t mission, std::uint32_t flight,
                      RoeLevel level) {
    CommandIntent cmd;
    cmd.kind = CommandIntent::Kind::RoeSet;
    cmd.scope.kind = kind;
    cmd.scope.team = team;
    cmd.scope.mission = mission;
    cmd.scope.flight = flight;
    cmd.roe = level;
    return cmd;
}

} // namespace

// ============================================================================
// 1. the intent body encoder — canonical bytes for every kind
// ============================================================================

TEST(CommandBody, GoldensPinEveryKind) {
    struct Case {
        const char* name;
        CommandIntent cmd;
        const char* want;
    };
    std::vector<Case> cases;

    CommandIntent roe = roe_set(RoEScopeKind::Team, 2, 0, 0, RoeLevel::Tight);
    cases.push_back({"roe_set_team",
                     roe,
                     R"("intent":"roe_set","scope":{"kind":"team","team":2,)"
                     R"("mission":0,"flight":0},"roe":1)"});

    CommandIntent retask;
    retask.kind = CommandIntent::Kind::FlightRetask;
    retask.flight = 5001;
    retask.mission_byte = 13;
    retask.target_objective_id = 4101;
    cases.push_back({"flight_retask",
                     retask,
                     R"("intent":"flight_retask","flight":5001,)"
                     R"("mission":13,"target":4101)"});

    CommandIntent abort;
    abort.kind = CommandIntent::Kind::FlightAbort;
    abort.flight = 5001;
    cases.push_back(
        {"flight_abort", abort,
         R"("intent":"flight_abort","flight":5001)"});

    CommandIntent prio;
    prio.kind = CommandIntent::Kind::ObjectivePriority;
    prio.objective_id = 4101;
    prio.weight = 25;
    cases.push_back(
        {"objective_priority", prio,
         R"("intent":"objective_priority","objective":4101,"weight":25)"});

    CommandIntent focus;
    focus.kind = CommandIntent::Kind::Focus;
    focus.x = 100.0;
    focus.y = 200.0;
    focus.z = 300.0;
    focus.radius_ft = 4800.0;
    cases.push_back({"focus", focus,
                     R"("intent":"focus","x":100,"y":200,"z":300,)"
                     R"("radius_ft":4800)"});

    CommandIntent clear;
    clear.kind = CommandIntent::Kind::ClearFocus;
    cases.push_back({"clear_focus", clear, R"("intent":"clear_focus")"});

    CommandIntent deagg;
    deagg.kind = CommandIntent::Kind::SelectDeagg;
    deagg.flight = 5001;
    cases.push_back(
        {"select_deagg", deagg,
         R"("intent":"select_deagg","flight":5001)"});

    CommandIntent reagg;
    reagg.kind = CommandIntent::Kind::SelectReagg;
    reagg.flight = 5002;
    cases.push_back(
        {"select_reagg", reagg,
         R"("intent":"select_reagg","flight":5002)"});

    for (const auto& c : cases) {
        f4::json::Writer w;
        encode_command_body(w, c.cmd);
        EXPECT_EQ(w.str(), c.want) << c.name;
    }
}

// Every body this encoder writes parses back through the wire parser —
// the round trip is the "one vocabulary, two directions" proof.
TEST(CommandBody, ParsesBackVerbatim) {
    std::vector<CommandIntent> sources;
    sources.push_back(roe_set(RoEScopeKind::Team, 2, 0, 0, RoeLevel::Hold));
    sources.push_back(
        roe_set(RoEScopeKind::Mission, 2, 13, 0, RoeLevel::Tight));
    sources.push_back(
        roe_set(RoEScopeKind::Flight, 0, 0, 5001, RoeLevel::Free));

    CommandIntent retask;
    retask.kind = CommandIntent::Kind::FlightRetask;
    retask.flight = 5001;
    retask.mission_byte = 21;
    retask.target_objective_id = 4102;
    sources.push_back(retask);

    for (const auto& src : sources) {
        f4::json::Writer w;
        encode_command_body(w, src);
        // wrap the body in the wire envelope the protocol ships
        const std::string line =
            "{\"v\":1,\"op\":\"command\"," + w.str() + "}";
        f4::json::Reader r(line);
        r.expect('{');
        (void)r.read_string();  // "v"
        r.expect(':');
        (void)r.read_int();
        r.expect(',');
        (void)r.read_string();  // "op"
        r.expect(':');
        (void)r.read_string();
        r.expect(',');
        const auto intent_key = r.read_string();
        ASSERT_EQ(intent_key, "intent");
        r.expect(':');
        const auto intent_name = r.read_string();
        const auto back = parse_command_body(intent_name, r);
        EXPECT_EQ(back.kind, src.kind);
        EXPECT_EQ(back.scope.kind, src.scope.kind);
        EXPECT_EQ(back.scope.team, src.scope.team);
        EXPECT_EQ(back.scope.mission, src.scope.mission);
        EXPECT_EQ(back.scope.flight, src.scope.flight);
        EXPECT_EQ(back.roe, src.roe);
        EXPECT_EQ(back.mission_byte, src.mission_byte);
        EXPECT_EQ(back.target_objective_id, src.target_objective_id);
    }
}

// ============================================================================
// 2. the journal's lines — byte goldens
// ============================================================================

TEST(CommandJournalLines, GoldenHeaderAndFooter) {
    f4::json::Writer h;
    encode_command_journal_header(h, start_identity());
    EXPECT_EQ(h.str(),
              "{\"v\":1,\"cmdjournal\":1,\"identity\":{\"protocol\":1,"
              "\"campaign_time_s\":38574360,"
              "\"ledger_fnv\":\"0123456789abcdef\"}}");

    f4::json::Writer f;
    encode_command_journal_end(f, end_identity());
    EXPECT_EQ(f.str(),
              "{\"journal_end\":{\"protocol\":1,"
              "\"campaign_time_s\":38574500,"
              "\"ledger_fnv\":\"fedcba9876543210\"}}");
}

TEST(CommandJournalLines, GoldenRoeSetPerScopeKind) {
    CommandJournalEntry team;
    team.apply_tick = 120;
    team.campaign_time_s = 38574480;
    team.intent = roe_set(RoEScopeKind::Team, 1, 0, 0, RoeLevel::Hold);
    f4::json::Writer w1;
    encode_command_journal_line(w1, team);
    EXPECT_EQ(
        w1.str(),
        "{\"apply_tick\":120,\"t\":38574480,\"intent\":\"roe_set\","
        "\"scope\":{\"kind\":\"team\",\"team\":1,\"mission\":0,"
        "\"flight\":0},\"roe\":2}");

    CommandJournalEntry mission;
    mission.apply_tick = 121;
    mission.campaign_time_s = 38574480;
    mission.intent =
        roe_set(RoEScopeKind::Mission, 2, 13, 0, RoeLevel::Tight);
    f4::json::Writer w2;
    encode_command_journal_line(w2, mission);
    EXPECT_EQ(
        w2.str(),
        "{\"apply_tick\":121,\"t\":38574480,\"intent\":\"roe_set\","
        "\"scope\":{\"kind\":\"mission\",\"team\":2,\"mission\":13,"
        "\"flight\":0},\"roe\":1}");

    CommandJournalEntry flight;
    flight.apply_tick = 122;
    flight.campaign_time_s = 38574480;
    flight.intent =
        roe_set(RoEScopeKind::Flight, 0, 0, 5001, RoeLevel::Free);
    f4::json::Writer w3;
    encode_command_journal_line(w3, flight);
    EXPECT_EQ(
        w3.str(),
        "{\"apply_tick\":122,\"t\":38574480,\"intent\":\"roe_set\","
        "\"scope\":{\"kind\":\"flight\",\"team\":0,\"mission\":0,"
        "\"flight\":5001},\"roe\":0}");
}

// ============================================================================
// 3. the writer + the reader — round trip and the loud failures
// ============================================================================

TEST(CommandJournalRoundTrip, WriterReaderAgree) {
    const auto path = temp_file("f4_cmdjournal");
    std::vector<CommandJournalEntry> src;
    CommandJournalEntry e1;
    e1.apply_tick = 40;
    e1.campaign_time_s = 38574400;
    e1.intent = roe_set(RoEScopeKind::Team, 2, 0, 0, RoeLevel::Hold);
    src.push_back(e1);
    CommandJournalEntry e2;
    e2.apply_tick = 40;  // same tick: same-boundary commands, file order
    e2.campaign_time_s = 38574400;
    e2.intent =
        roe_set(RoEScopeKind::Flight, 0, 0, 5002, RoeLevel::Tight);
    src.push_back(e2);
    CommandJournalEntry e3;
    e3.apply_tick = 100;
    e3.campaign_time_s = 38574460;
    e3.intent = roe_set(RoEScopeKind::Team, 2, 0, 0, RoeLevel::Free);
    src.push_back(e3);

    {
        CommandJournalWriter w;
        std::string err;
        ASSERT_TRUE(w.open(path.string(), start_identity(), &err)) << err;
        for (const auto& e : src) w.append(e);
        ASSERT_TRUE(w.close(end_identity(), &err)) << err;
    }

    // the file's bytes are exactly header + lines + footer
    const std::string bytes = slurp(path);
    const std::string want =
        "{\"v\":1,\"cmdjournal\":1,\"identity\":{\"protocol\":1,"
        "\"campaign_time_s\":38574360,"
        "\"ledger_fnv\":\"0123456789abcdef\"}}\n"
        "{\"apply_tick\":40,\"t\":38574400,\"intent\":\"roe_set\","
        "\"scope\":{\"kind\":\"team\",\"team\":2,\"mission\":0,"
        "\"flight\":0},\"roe\":2}\n"
        "{\"apply_tick\":40,\"t\":38574400,\"intent\":\"roe_set\","
        "\"scope\":{\"kind\":\"flight\",\"team\":0,\"mission\":0,"
        "\"flight\":5002},\"roe\":1}\n"
        "{\"apply_tick\":100,\"t\":38574460,\"intent\":\"roe_set\","
        "\"scope\":{\"kind\":\"team\",\"team\":2,\"mission\":0,"
        "\"flight\":0},\"roe\":0}\n"
        "{\"journal_end\":{\"protocol\":1,\"campaign_time_s\":38574500,"
        "\"ledger_fnv\":\"fedcba9876543210\"}}\n";
    EXPECT_EQ(bytes, want);

    // and the reader loads them back identically
    CommandJournalReader r;
    std::string err;
    ASSERT_TRUE(r.open(path.string(), start_identity(), &err)) << err;
    std::vector<CommandJournalEntry> back;
    CommandJournalEntry e;
    while (r.next(e, &err)) back.push_back(e);
    ASSERT_EQ(r.state(), CommandJournalReader::State::Ended) << err;
    ASSERT_EQ(back.size(), src.size());
    for (std::size_t i = 0; i < src.size(); ++i) {
        EXPECT_EQ(back[i].apply_tick, src[i].apply_tick) << i;
        EXPECT_EQ(back[i].campaign_time_s, src[i].campaign_time_s) << i;
        EXPECT_EQ(back[i].intent.kind, src[i].intent.kind) << i;
        EXPECT_EQ(back[i].intent.scope.kind, src[i].intent.scope.kind) << i;
        EXPECT_EQ(back[i].intent.scope.team, src[i].intent.scope.team) << i;
        EXPECT_EQ(back[i].intent.roe, src[i].intent.roe) << i;
    }
    EXPECT_TRUE(r.footer_seen());
    EXPECT_EQ(r.end_identity().ledger_fnv, "fedcba9876543210");
    EXPECT_EQ(r.end_identity().campaign_time_s, 38574500);

    // the one-call loader agrees
    std::vector<CommandJournalEntry> loaded;
    IdentityFingerprint loaded_end;
    ASSERT_TRUE(CommandJournalReader::load(path.string(), start_identity(),
                                           loaded, loaded_end, &err))
        << err;
    EXPECT_EQ(loaded.size(), src.size());
    EXPECT_EQ(loaded_end.ledger_fnv, "fedcba9876543210");
}

TEST(CommandJournalFailures, HeaderDriftNamesLineOne) {
    const auto path = temp_file("f4_cmdjournal_hdr");
    {
        CommandJournalWriter w;
        std::string err;
        ASSERT_TRUE(w.open(path.string(), start_identity(), &err));
        w.append(CommandJournalEntry{40, 38574400,
                                     roe_set(RoEScopeKind::Team, 2, 0, 0,
                                             RoeLevel::Hold)});
        ASSERT_TRUE(w.close(end_identity(), &err));
    }
    CommandJournalReader r;
    std::string err;
    IdentityFingerprint other = start_identity();
    other.ledger_fnv = "deadbeefdeadbeef";  // a different war
    EXPECT_FALSE(r.open(path.string(), other, &err));
    EXPECT_NE(err.find("line 1"), std::string::npos) << err;
    EXPECT_EQ(r.state(), CommandJournalReader::State::Failed);
}

TEST(CommandJournalFailures, MalformedLineFailsLoudly) {
    const auto path = temp_file("f4_cmdjournal_bad");
    {
        std::ofstream f(path, std::ios::binary);
        f4::json::Writer h;
        encode_command_journal_header(h, start_identity());
        f << h.str() << "\n";
        f << "{\"apply_tick\":40,\"t\":38574400,\"intent\":\"nope\"}\n";
    }
    CommandJournalReader r;
    std::string err;
    ASSERT_TRUE(r.open(path.string(), start_identity(), &err));
    CommandJournalEntry e;
    EXPECT_FALSE(r.next(e, &err));
    EXPECT_EQ(r.state(), CommandJournalReader::State::Failed);
    EXPECT_NE(err.find("line 2"), std::string::npos) << err;
}

TEST(CommandJournalFailures, BackwardsTicksRefuse) {
    const auto path = temp_file("f4_cmdjournal_back");
    {
        std::ofstream f(path, std::ios::binary);
        f4::json::Writer h;
        encode_command_journal_header(h, start_identity());
        f << h.str() << "\n";
        f << "{\"apply_tick\":100,\"t\":38574460,\"intent\":\"roe_set\","
             "\"scope\":{\"kind\":\"team\",\"team\":2,\"mission\":0,"
             "\"flight\":0},\"roe\":2}\n";
        f << "{\"apply_tick\":40,\"t\":38574400,\"intent\":\"roe_set\","
             "\"scope\":{\"kind\":\"team\",\"team\":2,\"mission\":0,"
             "\"flight\":0},\"roe\":0}\n";
    }
    CommandJournalReader r;
    std::string err;
    ASSERT_TRUE(r.open(path.string(), start_identity(), &err));
    CommandJournalEntry e;
    ASSERT_TRUE(r.next(e, &err));  // the first line is fine
    EXPECT_FALSE(r.next(e, &err));
    EXPECT_EQ(r.state(), CommandJournalReader::State::Failed);
    EXPECT_NE(err.find("ticks backwards"), std::string::npos) << err;
}

TEST(CommandJournalFailures, MissingFooterIsATruncation) {
    const auto path = temp_file("f4_cmdjournal_trunc");
    {
        std::ofstream f(path, std::ios::binary);
        f4::json::Writer h;
        encode_command_journal_header(h, start_identity());
        f << h.str() << "\n";
        f << "{\"apply_tick\":40,\"t\":38574400,\"intent\":\"roe_set\","
             "\"scope\":{\"kind\":\"team\",\"team\":2,\"mission\":0,"
             "\"flight\":0},\"roe\":2}\n";
        // no journal_end — the record was cut short
    }
    CommandJournalReader r;
    std::string err;
    ASSERT_TRUE(r.open(path.string(), start_identity(), &err));
    CommandJournalEntry e;
    ASSERT_TRUE(r.next(e, &err));
    EXPECT_FALSE(r.next(e, &err));
    EXPECT_EQ(r.state(), CommandJournalReader::State::Failed);
    EXPECT_NE(err.find("without journal_end"), std::string::npos) << err;
}

TEST(CommandJournalFailures, EnvelopeKeysMayPrecedeIntent) {
    // the reader's one leniency: the envelope keys (apply_tick, t) may
    // ride in any order BEFORE the intent — the intent's body closes
    // the line either way.
    const auto path = temp_file("f4_cmdjournal_order");
    {
        std::ofstream f(path, std::ios::binary);
        f4::json::Writer h;
        encode_command_journal_header(h, start_identity());
        f << h.str() << "\n";
        f << "{\"t\":38574400,\"apply_tick\":40,\"intent\":\"roe_set\","
             "\"scope\":{\"kind\":\"team\",\"team\":2,\"mission\":0,"
             "\"flight\":0},\"roe\":2}\n";
        f4::json::Writer ftr;
        encode_command_journal_end(ftr, end_identity());
        f << ftr.str() << "\n";
    }
    CommandJournalReader r;
    std::string err;
    ASSERT_TRUE(r.open(path.string(), start_identity(), &err));
    CommandJournalEntry e;
    ASSERT_TRUE(r.next(e, &err)) << err;
    EXPECT_EQ(e.apply_tick, 40u);
    EXPECT_EQ(e.campaign_time_s, 38574400);
    EXPECT_EQ(e.intent.roe, RoeLevel::Hold);
    EXPECT_FALSE(r.next(e, &err));
    EXPECT_EQ(r.state(), CommandJournalReader::State::Ended);
}
