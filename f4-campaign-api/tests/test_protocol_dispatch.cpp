// f4-campaign-api/tests/test_protocol_dispatch.cpp
//
// The line protocol driven against a MOCK session (CAMP_HOST_PLAN.md
// §7): op routing, the strict envelope, the exit-code mapping, and
// refusal-as-data — with NO engine in the link. The engine-backed host
// gets its own test in f4-simulation (test_campaign_session_host); the
// point here is that the CONTRACT is provable on its own.

#include <f4/campaign/api/protocol.hpp>

#include <gtest/gtest.h>

#include <string>

using namespace f4::campaign::api;

namespace {

// The mock: records what it was asked, answers canned data.
class MockSession final : public ICampaignSession {
public:
    IdentityFingerprint identity() const override {
        IdentityFingerprint id;
        id.protocol_version = kProtocolVersion;
        id.campaign_time_s = identity_time;
        id.ledger_fnv = to_hex16(fnv1a64("mock-ledger"));
        return id;
    }

    StepResult step(std::uint32_t ticks) override {
        stepped += ticks;
        StepResult r;
        r.dilated = dilate_next;
        identity_time += static_cast<std::int64_t>(ticks);
        return r;
    }

    void set_time_scale(double scale) override { time_scale = scale; }
    void set_paused(bool on) override { paused = on; }

    SaveResult save(std::string_view path) override {
        SaveResult r;
        r.ok = save_ok;
        r.bytes = save_ok ? 4096 : 0;
        r.detail = save_ok ? std::string(path) : "disk full (mock)";
        return r;
    }

    QueryResult query(const QuerySpec& spec) override {
        last_query = spec.name;
        QueryResult r;
        if (fail_next) {
            fail_next = false;
            r.detail = "mock failure";
            return r;
        }
        r.ok = true;
        r.data_json = "{\"mock\":" + std::to_string(++query_count) + "}";
        return r;
    }

    CommandAck submit(const CommandIntent& intent) override {
        last_intent = intent;
        ++submits;
        return next_ack;
    }

    // --- events (HOST-2: the mock honors the same contract the engine
    // host does — the filter gates at buffering time, the drain clears)
    void set_event_filter(const EventFilter& f) override {
        filter = f;
        filter_set = true;
        ++subscriptions;
    }

    [[nodiscard]] std::vector<CampaignEvent> drain_events() override {
        std::vector<CampaignEvent> out;
        for (auto& e : queued) {
            if (matches(filter, e)) out.push_back(std::move(e));
        }
        queued.clear();
        return out;
    }

    // --- observation
    std::uint32_t stepped{0};
    std::int64_t identity_time{38574360};
    bool dilate_next{false};
    double time_scale{1.0};
    bool paused{false};
    bool save_ok{true};
    bool fail_next{false};
    std::string last_query;
    int query_count{0};
    int submits{0};
    CommandIntent last_intent{};
    CommandAck next_ack{};
    EventFilter filter{};
    bool filter_set{false};
    int subscriptions{0};
    std::vector<CampaignEvent> queued;
};

// Run one line through the dispatcher.
ProtocolOutcome handle(MockSession& s, const char* line, std::string& out) {
    return host_handle(s, line, out);
}

} // namespace

// ============================================================================
// hello — the identity fingerprint
// ============================================================================

TEST(ProtocolDispatch, HelloCarriesIdentity) {
    MockSession s;
    std::string out;
    const auto o = handle(s, R"({"v":1,"op":"hello"})", out);
    EXPECT_EQ(o.kind, ProtocolOutcome::Kind::Ok);
    EXPECT_EQ(o.exit_code, 0);
    const auto fnv = to_hex16(fnv1a64("mock-ledger"));
    EXPECT_EQ(out,
              "{\"v\":1,\"op\":\"hello\",\"status\":\"ok\",\"identity\":"
              "{\"protocol\":1,\"campaign_time_s\":38574360,"
              "\"ledger_fnv\":\"" + fnv + "\"}}\n");
}

// ============================================================================
// step / pause — the lifecycle
// ============================================================================

TEST(ProtocolDispatch, StepAdvancesAndReportsDilation) {
    MockSession s;
    std::string out;
    const auto o = handle(s, R"({"v":1,"op":"step","ticks":600})", out);
    EXPECT_EQ(o.kind, ProtocolOutcome::Kind::Ok);
    EXPECT_EQ(s.stepped, 600U);
    // "events":0 — the HOST-2 framing key; an un-subscribed client's
    // response shape differs from HOST-1's by exactly this key
    EXPECT_EQ(out,
              R"({"v":1,"op":"step","status":"ok","ticks":600,"dilated":0,"events":0})" "\n");

    s.dilate_next = true;
    out.clear();
    (void)handle(s, R"({"v":1,"op":"step","ticks":1})", out);
    EXPECT_EQ(out,
              R"({"v":1,"op":"step","status":"ok","ticks":1,"dilated":1,"events":0})" "\n");
}

TEST(ProtocolDispatch, NegativeTicksIsMalformed) {
    MockSession s;
    std::string out;
    const auto o = handle(s, R"({"v":1,"op":"step","ticks":-5})", out);
    EXPECT_EQ(o.kind, ProtocolOutcome::Kind::ProtocolError);
    EXPECT_EQ(o.exit_code, 20);
    EXPECT_NE(out.find("\"code\":\"malformed\""), std::string::npos);
}

TEST(ProtocolDispatch, PauseRoundTrip) {
    MockSession s;
    std::string out;
    (void)handle(s, R"({"v":1,"op":"pause","on":true})", out);
    EXPECT_TRUE(s.paused);
    EXPECT_EQ(out, R"({"v":1,"op":"pause","status":"ok","on":1})" "\n");
}

// ============================================================================
// query — the v1 whitelist and the exit-21 miss
// ============================================================================

TEST(ProtocolDispatch, QueryDispatchesAndEmbedsData) {
    MockSession s;
    std::string out;
    const auto o = handle(
        s, R"({"v":1,"op":"query","q":"flights","team":2,"limit":10})", out);
    EXPECT_EQ(o.kind, ProtocolOutcome::Kind::Ok);
    EXPECT_EQ(s.last_query, "flights");
    EXPECT_EQ(out,
              R"({"v":1,"op":"query","q":"flights","status":"ok","data":{"mock":1}})" "\n");
}

TEST(ProtocolDispatch, UnknownQueryIsExit21) {
    MockSession s;
    std::string out;
    const auto o = handle(s, R"({"v":1,"op":"query","q":"weather"})", out);
    EXPECT_EQ(o.kind, ProtocolOutcome::Kind::ProtocolError);
    EXPECT_EQ(o.exit_code, 21);
    EXPECT_NE(out.find("\"code\":\"unknown_query\""), std::string::npos);
}

TEST(ProtocolDispatch, ThreatQueryIsWhitelisted) {
    // CAMP-HOST-3: `threat` joined the v1 whitelist additively (the DTO
    // landed at the END of dto.hpp) — it dispatches like any served
    // query, while routes/weather stay future-tranche names.
    MockSession s;
    std::string out;
    const auto o = handle(s, R"({"v":1,"op":"query","q":"threat"})", out);
    EXPECT_EQ(o.kind, ProtocolOutcome::Kind::Ok);
    EXPECT_EQ(s.last_query, "threat");
    EXPECT_EQ(out,
              R"({"v":1,"op":"query","q":"threat","status":"ok","data":{"mock":1}})" "\n");
}

TEST(ProtocolDispatch, VerdictQueryIsWhitelisted) {
    // CAMP-DOM-1: `verdict` joined the v1 whitelist additively (the DTO
    // landed at the END of dto.hpp) — it dispatches like any served
    // query, while the TE threshold/terminal semantics stay future
    // vocabulary.
    MockSession s;
    std::string out;
    const auto o = handle(s, R"({"v":1,"op":"query","q":"verdict"})", out);
    EXPECT_EQ(o.kind, ProtocolOutcome::Kind::Ok);
    EXPECT_EQ(s.last_query, "verdict");
    EXPECT_EQ(out,
              R"({"v":1,"op":"query","q":"verdict","status":"ok","data":{"mock":1}})" "\n");
}

TEST(ProtocolDispatch, SquadronsQueryIsWhitelisted) {
    // CAMP-DOM-3: `squadrons` joined the v1 whitelist additively (the
    // personnel face — dto.hpp's tail) — it dispatches like any served
    // query, while per-pilot detail rows stay future vocabulary.
    MockSession s;
    std::string out;
    const auto o = handle(s, R"({"v":1,"op":"query","q":"squadrons"})", out);
    EXPECT_EQ(o.kind, ProtocolOutcome::Kind::Ok);
    EXPECT_EQ(s.last_query, "squadrons");
    EXPECT_EQ(out,
              R"({"v":1,"op":"query","q":"squadrons","status":"ok","data":{"mock":1}})" "\n");
}

TEST(ProtocolDispatch, EngineSideQueryFailureIsExit24) {
    MockSession s;
    s.fail_next = true; // a whitelisted query the ENGINE side fails
    std::string out;
    const auto o = handle(s, R"({"v":1,"op":"query","q":"objectives"})", out);
    EXPECT_EQ(o.kind, ProtocolOutcome::Kind::ProtocolError);
    EXPECT_EQ(o.exit_code, 24);
    EXPECT_NE(out.find("\"code\":\"query_failed\""), std::string::npos);
}

// ============================================================================
// command — applied and refused (refusal is DATA, exit 22 for scripts)
// ============================================================================

TEST(ProtocolDispatch, CommandApplied) {
    MockSession s;
    s.next_ack.status = CommandAck::Status::Applied;
    s.next_ack.detail = "view bubble set";
    std::string out;
    const auto o = handle(
        s,
        R"({"v":1,"op":"command","intent":"focus","x":1.0,"y":2.0,"z":3.0,"radius_ft":120000.0})",
        out);
    EXPECT_EQ(o.kind, ProtocolOutcome::Kind::Ok);
    EXPECT_EQ(s.submits, 1);
    EXPECT_EQ(s.last_intent.kind, CommandIntent::Kind::Focus);
    EXPECT_DOUBLE_EQ(s.last_intent.radius_ft, 120000.0);
    EXPECT_NE(out.find(R"("status":"ok","ack":{"status":"applied")"),
              std::string::npos);
}

TEST(ProtocolDispatch, CommandRefusedIsDataPlusExit22) {
    MockSession s;
    s.next_ack.status = CommandAck::Status::Refused;
    s.next_ack.refusal = CommandAck::Refusal::NotImplemented;
    s.next_ack.detail = "CAMP-CMD-1 lands roe_set";
    std::string out;
    const auto o = handle(
        s,
        R"({"v":1,"op":"command","intent":"roe_set","scope":{"kind":"team","team":1},"roe":2})",
        out);
    EXPECT_EQ(o.kind, ProtocolOutcome::Kind::Refused);
    EXPECT_EQ(o.exit_code, 22);
    // AND the refusal still rode the wire as data — a UI renders it
    EXPECT_NE(out.find(R"("status":"refused")"), std::string::npos);
    EXPECT_NE(out.find(R"("refusal":"not_implemented")"), std::string::npos);
    EXPECT_EQ(s.last_intent.roe, RoeLevel::Hold);
}

// ============================================================================
// save — the runtime-safe path (exit 24 on engine-side failure)
// ============================================================================

TEST(ProtocolDispatch, SaveReportsBytesAndPath) {
    MockSession s;
    std::string out;
    const auto o = handle(s, R"({"v":1,"op":"save","path":"/tmp/war.json"})", out);
    EXPECT_EQ(o.kind, ProtocolOutcome::Kind::Ok);
    EXPECT_NE(out.find(R"("bytes":4096)"), std::string::npos);
    EXPECT_NE(out.find(R"("path":"/tmp/war.json")"), std::string::npos);
}

TEST(ProtocolDispatch, SaveFailureIsExit24) {
    MockSession s;
    s.save_ok = false;
    std::string out;
    const auto o = handle(s, R"({"v":1,"op":"save","path":"/tmp/war.json"})", out);
    EXPECT_EQ(o.kind, ProtocolOutcome::Kind::ProtocolError);
    EXPECT_EQ(o.exit_code, 24);
    EXPECT_NE(out.find("\"code\":\"save_failed\""), std::string::npos);
}

// ============================================================================
// the strict envelope — loud rejections (the P5/P6 discipline)
// ============================================================================

TEST(ProtocolDispatch, MalformedLineIsExit20) {
    MockSession s;
    std::string out;
    const auto o = handle(s, R"({"v":1,"op":)", out);
    EXPECT_EQ(o.kind, ProtocolOutcome::Kind::ProtocolError);
    EXPECT_EQ(o.exit_code, 20);
    EXPECT_NE(out.find("\"code\":\"malformed\""), std::string::npos);
}

TEST(ProtocolDispatch, BadVersionIsExit20) {
    MockSession s;
    std::string out;
    const auto o = handle(s, R"({"v":2,"op":"hello"})", out);
    EXPECT_EQ(o.exit_code, 20);
    EXPECT_NE(out.find("\"code\":\"bad_version\""), std::string::npos);
}

TEST(ProtocolDispatch, UnknownOpIsExit20) {
    MockSession s;
    std::string out;
    const auto o = handle(s, R"({"v":1,"op":"teleport"})", out);
    EXPECT_EQ(o.exit_code, 20);
    EXPECT_NE(out.find("\"code\":\"unknown_op\""), std::string::npos);
}

TEST(ProtocolDispatch, UnknownKeyIsMalformed) {
    MockSession s;
    std::string out;
    const auto o = handle(s, R"({"v":1,"op":"hello","cheat":1})", out);
    EXPECT_EQ(o.exit_code, 20);
    EXPECT_NE(out.find("\"code\":\"malformed\""), std::string::npos);
}

TEST(ProtocolDispatch, MissingVersionIsBadVersion) {
    MockSession s;
    std::string out;
    const auto o = handle(s, R"({"op":"hello"})", out);
    EXPECT_EQ(o.exit_code, 20);
    EXPECT_NE(out.find("\"code\":\"bad_version\""), std::string::npos);
}

// ============================================================================
// subscribe + the event framing (HOST-2 — plan §3.4's delivery rules)
// ============================================================================

TEST(ProtocolDispatch, SubscribeArmsAndEchoesTheFilter) {
    MockSession s;
    std::string out;
    const auto o = handle(
        s,
        R"({"v":1,"op":"subscribe","kinds":["kill","tasking_cycle"],"teams":[0,6]})",
        out);
    EXPECT_EQ(o.kind, ProtocolOutcome::Kind::Ok);
    EXPECT_TRUE(s.filter_set);
    EXPECT_EQ(s.subscriptions, 1);
    EXPECT_TRUE(s.filter.all == false);
    ASSERT_EQ(s.filter.kinds.size(), 2U);
    EXPECT_EQ(s.filter.kinds[0], CampaignEvent::Kind::Kill);
    EXPECT_EQ(s.filter.kinds[1], CampaignEvent::Kind::TaskingCycle);
    ASSERT_EQ(s.filter.teams.size(), 2U);
    EXPECT_EQ(s.filter.teams[0], 0);
    EXPECT_EQ(s.filter.teams[1], 6);
    // the echo is the filter's canonical spelling
    EXPECT_EQ(out,
              R"({"v":1,"op":"subscribe","status":"ok","kinds":["kill",)"
              R"("tasking_cycle"],"teams":[0,6]})" "\n");
}

TEST(ProtocolDispatch, SubscribeAllEchoesAll) {
    MockSession s;
    std::string out;
    (void)handle(s, R"({"v":1,"op":"subscribe","kinds":["all"]})", out);
    EXPECT_TRUE(s.filter.all);
    EXPECT_EQ(out,
              R"({"v":1,"op":"subscribe","status":"ok","kinds":["all"],)"
              R"("teams":[]})" "\n");
}

TEST(ProtocolDispatch, SubscribePilotFamiliesByName) {
    // CAMP-DOM-3: the personnel trio joined the subscribe vocabulary —
    // the wire names parse, the echo carries them back in filter order.
    MockSession s;
    std::string out;
    const auto o = handle(
        s,
        R"({"v":1,"op":"subscribe","kinds":["pilot_assigned","pilot_lost","pilot_recovered"],"teams":[2]})",
        out);
    EXPECT_EQ(o.kind, ProtocolOutcome::Kind::Ok);
    ASSERT_EQ(s.filter.kinds.size(), 3U);
    EXPECT_EQ(s.filter.kinds[0], CampaignEvent::Kind::PilotAssigned);
    EXPECT_EQ(s.filter.kinds[1], CampaignEvent::Kind::PilotLost);
    EXPECT_EQ(s.filter.kinds[2], CampaignEvent::Kind::PilotRecovered);
    EXPECT_EQ(out,
              R"({"v":1,"op":"subscribe","status":"ok","kinds":["pilot_assigned",)"
              R"("pilot_lost","pilot_recovered"],"teams":[2]})" "\n");
}

TEST(ProtocolDispatch, SubscribeWithoutKindsIsMalformed) {
    MockSession s;
    std::string out;
    const auto o = handle(s, R"({"v":1,"op":"subscribe"})", out);
    EXPECT_EQ(o.exit_code, 20);
    EXPECT_NE(out.find("subscribe needs kinds"), std::string::npos);
}

TEST(ProtocolDispatch, SubscribeUnknownKindIsMalformed) {
    MockSession s;
    std::string out;
    const auto o = handle(
        s, R"({"v":1,"op":"subscribe","kinds":["meteor_shower"]})", out);
    EXPECT_EQ(o.exit_code, 20);
    EXPECT_NE(out.find("unknown event kind: meteor_shower"),
              std::string::npos);
}

TEST(ProtocolDispatch, StepDeliversSubscribedEventsAfterTheResponse) {
    MockSession s;
    // the war fired: one kill, one tasking cycle
    CampaignEvent kill;
    kill.kind = CampaignEvent::Kind::Kill;
    kill.kill.t = 357;
    kill.kill.killer_squadron = 214;
    kill.kill.killer_team = 0;
    kill.kill.victim_squadron = 317;
    kill.kill.victim_team = 1;
    kill.kill.weapon = "missile";
    CampaignEvent cycle;
    cycle.kind = CampaignEvent::Kind::TaskingCycle;
    cycle.tasking_cycle.t = 360;
    cycle.tasking_cycle.cycles = 1;
    cycle.tasking_cycle.next_tasking_sec = 1800;
    cycle.tasking_cycle.intents = 3;
    s.queued = {kill, cycle};

    std::string out;
    (void)handle(s, R"({"v":1,"op":"subscribe","kinds":["all"]})", out);
    out.clear();
    const auto o = handle(s, R"({"v":1,"op":"step","ticks":60})", out);
    EXPECT_EQ(o.kind, ProtocolOutcome::Kind::Ok);
    // the response line announces 2; the 2 event lines follow in order
    EXPECT_EQ(
        out,
        R"({"v":1,"op":"step","status":"ok","ticks":60,"dilated":0,"events":2})" "\n"
        R"({"ev":"kill","t":357,"killer":{"sq":214,"team":0},)"
        R"("victim":{"sq":317,"team":1},"weapon":"missile"})" "\n"
        R"({"ev":"tasking_cycle","t":360,"cycles":1,)"
        R"("next_tasking_sec":1800,"intents":3})" "\n");
}

TEST(ProtocolDispatch, DrainClearsSoTheNextStepAnnouncesZero) {
    MockSession s;
    CampaignEvent cycle;
    cycle.kind = CampaignEvent::Kind::TaskingCycle;
    cycle.tasking_cycle.t = 60;
    cycle.tasking_cycle.cycles = 1;
    s.queued = {cycle};
    std::string out;
    (void)handle(s, R"({"v":1,"op":"subscribe","kinds":["all"]})", out);
    const auto first = handle(s, R"({"v":1,"op":"step","ticks":60})", out);
    EXPECT_EQ(s.stepped, 60U);
    EXPECT_NE(first.kind, ProtocolOutcome::Kind::ProtocolError);
    EXPECT_NE(out.find("\"events\":1"), std::string::npos);

    out.clear();
    (void)handle(s, R"({"v":1,"op":"step","ticks":60})", out);
    EXPECT_NE(out.find("\"events\":0"), std::string::npos);
    EXPECT_EQ(out.find("\"ev\":"), std::string::npos);
}

TEST(ProtocolDispatch, TeamFilterGatesDelivery) {
    MockSession s;
    CampaignEvent kill;
    kill.kind = CampaignEvent::Kind::Kill;
    kill.kill.t = 10;
    kill.kill.killer_team = 2;   // ROK shoots down...
    kill.kill.victim_team = 6;   // ...a DPRK aircraft — both sides see it
    CampaignEvent filed;
    filed.kind = CampaignEvent::Kind::MissionFiled;
    filed.mission_filed.t = 10;
    filed.mission_filed.team = 6;
    s.queued = {kill, filed};

    // team 2 only: the kill rides (killer side), the DPRK filing does not
    std::string out;
    (void)handle(
        s,
        R"({"v":1,"op":"subscribe","kinds":["kill","mission_filed"],"teams":[2]})",
        out);
    out.clear();
    (void)handle(s, R"({"v":1,"op":"step","ticks":60})", out);
    EXPECT_NE(out.find("\"events\":1"), std::string::npos);
    EXPECT_NE(out.find("\"ev\":\"kill\""), std::string::npos);
    EXPECT_EQ(out.find("\"ev\":\"mission_filed\""), std::string::npos);
}

TEST(ProtocolDispatch, ActionFiledKindSubscribesAndRidesTheStepLine) {
    // CAMP-ATM-1 — the ninth family on the wire: the ACTION tables'
    // filing subscribes by name, echoes canonically, and its step line
    // is byte-pinned.
    MockSession s;
    CampaignEvent action;
    action.kind = CampaignEvent::Kind::ActionFiled;
    action.action_filed.t = 4000;
    action.action_filed.team = 2;
    action.action_filed.mission_byte = 20;
    action.action_filed.mission_name = "CAS";
    action.action_filed.action_type = 1;
    action.action_filed.context = 4;
    action.action_filed.objective_id = 4281;
    action.action_filed.damage_pct = 12;
    s.queued = {action};

    std::string out;
    (void)handle(s, R"({"v":1,"op":"subscribe","kinds":["action_filed"],"teams":[2]})",
                 out);
    EXPECT_EQ(out,
              R"({"v":1,"op":"subscribe","status":"ok","kinds":["action_filed"],)"
              R"("teams":[2]})" "\n");
    out.clear();
    const auto o = handle(s, R"({"v":1,"op":"step","ticks":60})", out);
    EXPECT_EQ(o.kind, ProtocolOutcome::Kind::Ok);
    EXPECT_EQ(
        out,
        R"({"v":1,"op":"step","status":"ok","ticks":60,"dilated":0,"events":1})" "\n"
        R"({"ev":"action_filed","t":4000,"team":2,"mission_byte":20,)"
        R"("mission_name":"CAS","action_type":1,"context":4,)"
        R"("objective_id":4281,"damage_pct":12})" "\n");
}

TEST(ProtocolDispatch, VerdictKindSubscribesAndRidesTheStepLine) {
    // CAMP-DOM-1 — the tenth family on the wire: the verdict subscribes
    // by name (teamless — the war's outcome is theater-wide) and its
    // step line is byte-pinned.
    MockSession s;
    CampaignEvent verdict;
    verdict.kind = CampaignEvent::Kind::Verdict;
    verdict.verdict.t = 7260;
    verdict.verdict.band = "advantage";
    verdict.verdict.leader = 2;
    verdict.verdict.swing = 30;
    s.queued = {verdict};

    std::string out;
    (void)handle(s, R"({"v":1,"op":"subscribe","kinds":["verdict"]})", out);
    EXPECT_EQ(out,
              R"({"v":1,"op":"subscribe","status":"ok","kinds":["verdict"],)"
              R"("teams":[]})" "\n");
    out.clear();
    const auto o = handle(s, R"({"v":1,"op":"step","ticks":60})", out);
    EXPECT_EQ(o.kind, ProtocolOutcome::Kind::Ok);
    EXPECT_EQ(
        out,
        R"({"v":1,"op":"step","status":"ok","ticks":60,"dilated":0,"events":1})" "\n"
        R"({"ev":"verdict","t":7260,"band":"advantage",)"
        R"("leader":2,"swing":30})" "\n");
}

TEST(ProtocolDispatch, ObjectiveRepairedKindSubscribesAndRidesTheStepLine) {
    // CAMP-DOM-2 — the eleventh family on the wire: the repair
    // subscribes by name (team-owned — the holding side's gate) and
    // its step line is byte-pinned.
    MockSession s;
    CampaignEvent repaired;
    repaired.kind = CampaignEvent::Kind::ObjectiveRepaired;
    repaired.objective_repaired.t = 8000;
    repaired.objective_repaired.objective_id = 101;
    repaired.objective_repaired.owner = 2;
    repaired.objective_repaired.features_repaired = 1;
    repaired.objective_repaired.features_destroyed = 0;
    repaired.objective_repaired.supply = 45;
    s.queued = {repaired};

    std::string out;
    (void)handle(s, R"({"v":1,"op":"subscribe","kinds":["objective_repaired"]})", out);
    EXPECT_EQ(out,
              R"({"v":1,"op":"subscribe","status":"ok","kinds":["objective_repaired"],)"
              R"("teams":[]})" "\n");
    out.clear();
    const auto o = handle(s, R"({"v":1,"op":"step","ticks":60})", out);
    EXPECT_EQ(o.kind, ProtocolOutcome::Kind::Ok);
    EXPECT_EQ(
        out,
        R"({"v":1,"op":"step","status":"ok","ticks":60,"dilated":0,"events":1})" "\n"
        R"({"ev":"objective_repaired","t":8000,"objective_id":101,)"
        R"("owner":2,"features_repaired":1,"features_destroyed":0,)"
        R"("supply":45})" "\n");

    // The team gate: the holding side sees it, the other side does not.
    out.clear();
    (void)handle(s, R"({"v":1,"op":"subscribe","kinds":["objective_repaired"],"teams":[6]})", out);
    out.clear();
    (void)handle(s, R"({"v":1,"op":"step","ticks":60})", out);
    EXPECT_EQ(out,
        R"({"v":1,"op":"step","status":"ok","ticks":60,"dilated":0,"events":0})" "\n");
}

TEST(ProtocolDispatch, EmptyKindListDeliversNothing) {
    MockSession s;
    CampaignEvent cycle;
    cycle.kind = CampaignEvent::Kind::TaskingCycle;
    s.queued = {cycle};
    std::string out;
    (void)handle(s, R"({"v":1,"op":"subscribe","kinds":[]})", out);
    out.clear();
    (void)handle(s, R"({"v":1,"op":"step","ticks":60})", out);
    EXPECT_NE(out.find("\"events\":0"), std::string::npos);
    EXPECT_EQ(out.find("\"ev\":"), std::string::npos);
}
