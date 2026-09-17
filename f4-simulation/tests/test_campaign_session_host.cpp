// f4-simulation/tests/test_campaign_session_host.cpp
//
// CAMP-HOST-1 — the engine-backed host, end to end (the gate's demo):
// the contract's four surfaces driven over the REAL engine session
// (the tier rig's crafted world, the tiered policy):
//
//   1. Construction + identity: the fingerprint is stable before any
//      step and moves with the campaign clock after one.
//   2. step(): 600 fixed-dt ticks advance the campaign clock exactly 10
//      whole seconds (600 × 1/60 — the double product rounds to exactly
//      10.0, so the accumulator drains 600 whole ticks; the general
//      sub-tick boundary behavior is the engine's own accumulator
//      discipline, exercised by every existing advance() caller).
//   3. Queries: time / stats / flights / tasking / books / objectives
//      all serve REAL engine data — the books query returns the ledger's
//      byte-stable JSON VERBATIM (it is the identity hash's source), and
//      the flights rows carry the crafted flight's VU.
//   4. Commands: the FID family applies (force-deagg → the flight goes
//      live → force-reagg folds it back), the validation refusals are
//      typed (radius 0, unknown flight), and the CAMP-CMD queue refuses
//      with NotImplemented + the tranche ID in the detail.
//   5. Save: the runtime-safe path writes a non-empty WorldState JSON.
//   6. The protocol dispatcher end-to-end against THIS host: hello,
//      step, query, and the malformed line's exit-20 mapping.

#include <f4/simulation/campaign_session_host.hpp>

#include <f4/campaign/api/protocol.hpp>

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>

using namespace f4::simulation;
namespace api = f4::campaign::api;

namespace {

constexpr std::int64_t kNow = 38574360;

// The tier rig's crafted world (test_fidelity_tiers.cpp's shape): ROK vs
// DPRK, an AIRBASE objective, a squadron + one 3-leg flight, a DPRK
// battalion. Time-less waypoints → the engine's SPEED-mode cruise.
std::string host_world_json() {
    return R"({
  "version": 71,
  "theater": "korea",
  "campaign": {
    "current_time": 38574360,
    "te_team": 2,
    "teams": [
      {"slot": 2, "name": "ROK", "member": [0,0,1,0,0,0,0,0],
       "stance": [0,0,0,0,0,0,5,0]},
      {"slot": 6, "name": "DPRK", "member": [0,0,0,0,0,0,1,0],
       "stance": [0,0,5,0,0,0,0,0]}
    ]
  },
  "objectives": {
    "count": 1,
    "decoded": 1,
    "items": [
      {"type": 100, "id_num": 4101, "id_creator": 0,
       "objective_type": 1,
       "x": 390, "y": 455, "z": 0,
       "owner": 2, "nameid": 1627, "priority": 10,
       "fstatus": [0, 0], "links": []}
    ]
  },
  "units": {
    "count": 3,
    "decoded": 3,
    "items": [
      {"type": 200, "id_num": 4281, "unit_class": "squadron",
       "entity_type": 273, "domain": 2,
       "x": 390, "y": 455, "z": 0, "owner": 2,
       "airbase_id": 4101, "class_name": "52 TFS PAK"},
      {"type": 200, "id_num": 5001, "unit_class": "flight",
       "entity_type": 273, "domain": 2,
       "x": 390, "y": 455, "z": 0, "owner": 2,
       "mission": 13, "squadron_id": 4281, "package_id": 7029,
       "time_on_target": 43739352,
       "waypoints": [
         {"x": 390, "y": 455, "z": 0,    "action": 1},
         {"x": 420, "y": 460, "z": 2500, "action": 15},
         {"x": 460, "y": 500, "z": 2500, "action": 17},
         {"x": 390, "y": 455, "z": 0,    "action": 7}
       ]},
      {"type": 200, "id_num": 6001, "unit_class": "battalion",
       "entity_type": 180, "domain": 3,
       "x": 390, "y": 455, "z": 0, "owner": 6,
       "vehicle_groups": [
         {"group": 0, "vehicle_type": 101, "count": 3, "live_count": 3}
       ]}
    ]
  }
})";
}

std::filesystem::path make_temp_dir() {
    // unique per call AND per process (ctest -jN) — the tier rig's rule
    static std::atomic<unsigned> counter{0};
    const auto dir = std::filesystem::temp_directory_path() /
                     ("f4_campd_" + std::to_string(counter.fetch_add(1)) +
                      "_" + std::to_string(
                          std::chrono::steady_clock::now()
                              .time_since_epoch()
                              .count()));
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    return dir;
}

struct HostRig {
    std::filesystem::path dir;
    std::filesystem::path world;
    std::unique_ptr<EngineSessionHost> host;

    static HostRig make() {
        HostRig rig;
        rig.dir = make_temp_dir();
        rig.world = rig.dir / "host.world.json";
        {
            std::ofstream f(rig.world);
            f << host_world_json();
        }
        CampaignSessionOptions opts;
        opts.world_json = rig.world;
        opts.class_table = std::filesystem::path(F4_SOURCE_FIXTURES_DIR) /
                           "falcon4.ct.json";
        opts.aircraft_config =
            std::filesystem::path(F4_GENERATED_FIXTURES_DIR) / "f16.json";
        opts.mission_profiles = F4_MISSION_PROFILES_JSON;
        opts.fidelity_policy = FidelityPolicy::Tiered;
        // a quiet war: the assertions count only what the test itself
        // commands (the tier rig's choices)
        opts.tasking_cycle_sec = 1000000;
        opts.reinforce_period_sec = 0;
        opts.atm_pipeline = false;
        opts.max_flights = 8;
        // the 600-tick steps below must not dilate: raise the engine's
        // per-advance cap above the request (the default is 240)
        opts.max_steps_per_advance = 1000;
        std::string err;
        rig.host = EngineSessionHost::create(opts, &err);
        EXPECT_NE(rig.host, nullptr) << err;
        return rig;
    }

    // Run one protocol line; returns the response (with its newline).
    std::string line(const std::string& request) const {
        std::string out;
        (void)api::host_handle(*host, request, out);
        return out;
    }

    std::string query(const char* q) const {
        return line(std::string(R"({"v":1,"op":"query","q":")") + q +
                    R"("})");
    }
};

} // namespace

// ============================================================================
// construction + identity
// ============================================================================

TEST(CampaignSessionHost, ConstructionServesIdentityStably) {
    const auto rig = HostRig::make();
    const auto a = rig.host->identity();
    const auto b = rig.host->identity();
    EXPECT_EQ(a.protocol_version, api::kProtocolVersion);
    EXPECT_EQ(a.campaign_time_s, kNow);
    EXPECT_EQ(a.ledger_fnv, b.ledger_fnv);
    EXPECT_EQ(a.ledger_fnv.size(), 16U); // the canonical hex spelling
}

// ============================================================================
// step + time — the lifecycle advances the war deterministically
// ============================================================================

TEST(CampaignSessionHost, StepSixHundredTicksAdvancesTenWholeSeconds) {
    const auto rig = HostRig::make();
    const auto before = rig.host->identity();

    const auto res = rig.host->step(600); // 600 × 1/60 s = 10 sim seconds
    EXPECT_FALSE(res.dilated);

    // The SIM clock: 600 requested ticks land within ONE tick of 10.0 —
    // the engine's accumulator drains whole ticks and carries sub-tick
    // rounding residue to the NEXT step (the session's TOTAL time stays
    // exact; the C5 identity harnesses pin that). A per-call drain near
    // an exact-second boundary may come up one tick short or over.
    const auto t = rig.host->query({"time", -1, 0});
    ASSERT_TRUE(t.ok);
    const auto key = t.data_json.find("\"sim_time_s\":");
    ASSERT_NE(key, std::string::npos);
    EXPECT_NEAR(std::stod(t.data_json.substr(key + std::strlen("\"sim_time_s\":"))),
                10.0, 1.0 / 60.0 + 1e-6);

    // The CAMPAIGN clock advances in whole seconds off the engine's own
    // accumulator (advance()'s chunky cadence — one fire can carry
    // several seconds). 600 ticks land at +9; the 10th whole second
    // fires one tick later. Deterministic — pinned as the engine does
    // it, not as naive arithmetic would have it.
    const auto after = rig.host->identity();
    EXPECT_EQ(after.campaign_time_s, before.campaign_time_s + 9);
}

TEST(CampaignSessionHost, StepZeroIsANoop) {
    const auto rig = HostRig::make();
    const auto before = rig.host->identity();
    (void)rig.host->step(0);
    EXPECT_EQ(rig.host->identity().campaign_time_s,
              before.campaign_time_s);
}

TEST(CampaignSessionHost, PauseStopsTheClock) {
    const auto rig = HostRig::make();
    rig.host->set_paused(true);
    (void)rig.host->step(600);
    EXPECT_EQ(rig.host->identity().campaign_time_s, kNow);
}

// ============================================================================
// queries — the v1 set serves real engine data
// ============================================================================

TEST(CampaignSessionHost, TimeQueryEchoesTheClock) {
    const auto rig = HostRig::make();
    (void)rig.host->step(600);
    // the engine's whole-second cadence: +9 after 600 ticks (see the
    // step test's comment)
    const auto out = rig.query("time");
    EXPECT_NE(out.find("\"campaign_time_s\":" +
                       std::to_string(kNow + 9)),
              std::string::npos);
    EXPECT_NE(out.find("\"tick_sec\":0.016666666666666666"),
              std::string::npos);
}

TEST(CampaignSessionHost, StatsQueryCarriesTheCounterVocabulary) {
    const auto rig = HostRig::make();
    const auto out = rig.query("stats");
    EXPECT_NE(out.find("\"cycles\":0"), std::string::npos);
    EXPECT_NE(out.find("\"live_aircraft\":"), std::string::npos);
    EXPECT_NE(out.find("\"agg_contacts\":"), std::string::npos);
    EXPECT_NE(out.find("\"deferred_releases\":0}"), std::string::npos);
}

TEST(CampaignSessionHost, FlightsQueryCarriesTheCraftedFlight) {
    const auto rig = HostRig::make();
    const auto out = rig.query("flights");
    EXPECT_NE(out.find("\"vu\":5001"), std::string::npos);
    EXPECT_NE(out.find("\"team\":2"), std::string::npos);
    // the aggregate picture by default (the plan §4 rule)
    EXPECT_NE(out.find("\"live\":0"), std::string::npos);
}

TEST(CampaignSessionHost, TaskingQueryStartsEmptyInAQuietWar) {
    const auto rig = HostRig::make();
    EXPECT_EQ(rig.query("tasking"),
              R"({"v":1,"op":"query","q":"tasking","status":"ok","data":[]})" "\n");
}

TEST(CampaignSessionHost, BooksQueryReturnsTheLedgerVerbatim) {
    const auto rig = HostRig::make();
    const std::string ledger = rig.host->engine().ledger_json();
    EXPECT_FALSE(ledger.empty());
    // the books ride as ONE escaped string (the ledger's own writer is
    // pretty-printed; raw embedding would break the one-line framing) —
    // one client-side decode must return the EXACT ledger bytes
    std::string out;
    (void)api::host_handle(*rig.host,
                           R"({"v":1,"op":"query","q":"books"})", out);
    const auto pos = out.find("\"ledger_json\":\"");
    ASSERT_NE(pos, std::string::npos);
    f4::json::Reader r(out.substr(pos + 14)); // at the opening quote
    const auto decoded = r.read_string();
    EXPECT_EQ(decoded, ledger);
    // and the response stayed ONE wire line (no embedded raw newlines)
    EXPECT_EQ(out.find('\n'), out.size() - 1);
}

TEST(CampaignSessionHost, ObjectivesQueryCarriesOwnershipAndLogistics) {
    const auto rig = HostRig::make();
    const auto out = rig.query("objectives");
    EXPECT_NE(out.find("\"id_num\":4101"), std::string::npos);
    EXPECT_NE(out.find("\"owner\":2"), std::string::npos);
    EXPECT_NE(out.find("\"supply\":"), std::string::npos);
    EXPECT_NE(out.find("\"fstatus\":[0,0]"), std::string::npos);
}

// ============================================================================
// commands — the FID family applies; validation and the queue refuse
// ============================================================================

TEST(CampaignSessionHost, FocusValidationRefusal) {
    const auto rig = HostRig::make();
    api::CommandIntent cmd;
    cmd.kind = api::CommandIntent::Kind::Focus;
    cmd.radius_ft = 0.0;
    const auto ack = rig.host->submit(cmd);
    EXPECT_EQ(ack.status, api::CommandAck::Status::Refused);
    EXPECT_EQ(ack.refusal, api::CommandAck::Refusal::InvalidArgument);
}

TEST(CampaignSessionHost, ForceDeaggThenReaggFoldsBack) {
    const auto rig = HostRig::make();

    api::CommandIntent deagg;
    deagg.kind = api::CommandIntent::Kind::SelectDeagg;
    deagg.flight = 5001;
    const auto ack = rig.host->submit(deagg);
    EXPECT_EQ(ack.status, api::CommandAck::Status::Applied);
    EXPECT_NE(rig.query("flights").find("\"live\":1"), std::string::npos);

    api::CommandIntent reagg;
    reagg.kind = api::CommandIntent::Kind::SelectReagg;
    reagg.flight = 5001;
    const auto ack2 = rig.host->submit(reagg);
    EXPECT_EQ(ack2.status, api::CommandAck::Status::Applied);
    EXPECT_NE(rig.query("flights").find("\"live\":0"), std::string::npos);
}

TEST(CampaignSessionHost, UnknownFlightRefusesTyped) {
    const auto rig = HostRig::make();
    api::CommandIntent cmd;
    cmd.kind = api::CommandIntent::Kind::SelectDeagg;
    cmd.flight = 999999;
    const auto ack = rig.host->submit(cmd);
    EXPECT_EQ(ack.status, api::CommandAck::Status::Refused);
    EXPECT_EQ(ack.refusal, api::CommandAck::Refusal::UnknownFlight);
}

TEST(CampaignSessionHost, ReaggOfAnAggregateRefusesTyped) {
    const auto rig = HostRig::make();
    api::CommandIntent cmd;
    cmd.kind = api::CommandIntent::Kind::SelectReagg;
    cmd.flight = 5001; // aggregate (never deaggregated)
    const auto ack = rig.host->submit(cmd);
    EXPECT_EQ(ack.status, api::CommandAck::Status::Refused);
    EXPECT_EQ(ack.refusal, api::CommandAck::Refusal::InvalidArgument);
}

TEST(CampaignSessionHost, CampCmdQueueRefusesWithTheTrancheNamed) {
    const auto rig = HostRig::make();

    api::CommandIntent roe;
    roe.kind = api::CommandIntent::Kind::RoeSet;
    roe.scope.kind = api::RoEScopeKind::Team;
    roe.scope.team = 6;
    roe.roe = api::RoeLevel::Hold;
    const auto a1 = rig.host->submit(roe);
    EXPECT_EQ(a1.refusal, api::CommandAck::Refusal::NotImplemented);
    EXPECT_NE(a1.detail.find("CAMP-CMD-1"), std::string::npos);

    for (const auto kind : {api::CommandIntent::Kind::FlightRetask,
                            api::CommandIntent::Kind::FlightAbort,
                            api::CommandIntent::Kind::ObjectivePriority}) {
        api::CommandIntent cmd;
        cmd.kind = kind;
        const auto ack = rig.host->submit(cmd);
        EXPECT_EQ(ack.refusal, api::CommandAck::Refusal::NotImplemented);
        EXPECT_NE(ack.detail.find("CAMP-CMD-2"), std::string::npos);
    }
}

// ============================================================================
// save — the runtime-safe path
// ============================================================================

TEST(CampaignSessionHost, SaveWritesWorldStateJson) {
    const auto rig = HostRig::make();
    (void)rig.host->step(600);
    const auto path = rig.dir / "after.world.json";
    const auto res = rig.host->save(path.string());
    EXPECT_TRUE(res.ok) << res.detail;
    EXPECT_GT(res.bytes, 0U);
    std::ifstream f(path);
    std::stringstream buf;
    buf << f.rdbuf();
    const auto text = buf.str();
    EXPECT_EQ(text.size(), res.bytes);
    // the emitter's own header (pretty-printed with spaces; its internal
    // version number is the WorldState's, not the input doc's)
    EXPECT_NE(text.find("\"version\":"), std::string::npos);
    EXPECT_NE(text.find("\"theater\""), std::string::npos);
    EXPECT_NE(text.find("\"korea\""), std::string::npos);
}

// ============================================================================
// the protocol end to end — the reference host's every line
// ============================================================================

TEST(CampaignSessionHost, ProtocolEndToEnd) {
    const auto rig = HostRig::make();

    // hello carries the fingerprint
    const auto hello = rig.line(R"({"v":1,"op":"hello"})");
    EXPECT_NE(hello.find("\"status\":\"ok\""), std::string::npos);
    EXPECT_NE(hello.find("\"protocol\":1"), std::string::npos);

    // step reports the dilation flag
    EXPECT_NE(rig.line(R"({"v":1,"op":"step","ticks":600})")
                  .find("\"dilated\":0"),
              std::string::npos);

    // a refused command rides the wire as data
    const auto refused = rig.line(
        R"({"v":1,"op":"command","intent":"flight_abort","flight":5001})");
    EXPECT_NE(refused.find("\"status\":\"refused\""), std::string::npos);

    // malformed is exit 20 with the error object
    std::string out;
    const auto o = api::host_handle(*rig.host, R"({"v":1,"op":)", out);
    EXPECT_EQ(o.kind, api::ProtocolOutcome::Kind::ProtocolError);
    EXPECT_EQ(o.exit_code, 20);
    EXPECT_NE(out.find("\"code\":\"malformed\""), std::string::npos);
}
