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
//
// CAMP-HOST-2 adds:
//   7. The event stream: silent until subscribed, the kunsan rig's
//      tasking/mission events in engine order, the wire filter's
//      kind/team gates, the ARMED WAR journaled and replayed (the C6
//      gate: the replay reproduces the identity, drift = exit 23), and
//      the empty-journal save's byte identity.
//
// CAMP-CMD-1 adds:
//   8. roe_set — the P7 fire-control path behind the command: team
//      scope gates + the roe_changed event + the ack's context, the
//      mission/flight scopes, the typed refusals (team 0, unknown
//      flight), and the LOWERING the old tighten-only ratchet could
//      not express. The outcome gate: a war held at t=2.5 s kills
//      nobody at t=13 where the un-commanded rig journals the kill
//      pair. And the identity statement's replay half: the command
//      journal re-applied at its recorded ticks reproduces the
//      record's fingerprint regardless of how the replay's steps are
//      chunked.

#include <f4/simulation/campaign_session_host.hpp>

#include <f4/campaign/api/journal.hpp>
#include <f4/campaign/api/protocol.hpp>
#include <f4/json/reader.hpp>  // CAMP-HOST-3: the parity probes walk the rows

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

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

// The FID-5 combat rig's world (test_fidelity_combat.cpp's shape):
// teams 2 and 6 at war, one airbase, two squadrons, flight 5001 (team
// 2) eastbound at 20,000 ft, flight 5002 (team 6) westbound at 22,000
// ft — head-on, closing inside the combat envelope. Flight 5003 is the
// control (no convergence).
std::string combat_world_json() {
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
    "count": 6,
    "decoded": 6,
    "items": [
      {"type": 200, "id_num": 4281, "unit_class": "squadron",
       "entity_type": 273, "domain": 2,
       "x": 390, "y": 455, "z": 0, "owner": 2,
       "airbase_id": 4101, "class_name": "52 TFS PAK"},
      {"type": 200, "id_num": 4282, "unit_class": "squadron",
       "entity_type": 273, "domain": 2,
       "x": 390, "y": 455, "z": 0, "owner": 6,
       "airbase_id": 4101, "class_name": "105 FES"},
      {"type": 200, "id_num": 5001, "unit_class": "flight",
       "entity_type": 273, "domain": 2,
       "x": 400, "y": 455, "z": 20000, "flight_altitude": 20000,
       "owner": 2, "mission": 7, "squadron_id": 4281,
       "package_id": 7029, "time_on_target": 43739352,
       "waypoints": [
         {"x": 400, "y": 455, "z": 20000, "action": 15},
         {"x": 470, "y": 455, "z": 20000, "action": 17}
       ]},
      {"type": 200, "id_num": 5002, "unit_class": "flight",
       "entity_type": 273, "domain": 2,
       "x": 424, "y": 457, "z": 22000, "flight_altitude": 22000,
       "owner": 6, "mission": 7, "squadron_id": 4282,
       "package_id": 7030, "time_on_target": 43739352,
       "waypoints": [
         {"x": 424, "y": 457, "z": 22000, "action": 15},
         {"x": 354, "y": 457, "z": 22000, "action": 17}
       ]},
      {"type": 200, "id_num": 5003, "unit_class": "flight",
       "entity_type": 273, "domain": 2,
       "x": 300, "y": 400, "z": 18000, "flight_altitude": 18000,
       "owner": 2, "mission": 7, "squadron_id": 4281,
       "package_id": 7031, "time_on_target": 43739352,
       "waypoints": [
         {"x": 300, "y": 400, "z": 18000, "action": 15},
         {"x": 330, "y": 400, "z": 18000, "action": 17}
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
        // A failed rig must fail the TEST, not segfault it: a factory
        // cannot ASSERT (non-void return), and EXPECT lets the test
        // walk on into a null host — the parallel-suite SEGFAULT. gtest
        // catches the throw and reports the session's own error.
        if (rig.host == nullptr) {
            throw std::runtime_error(
                "HostRig: session create failed: " + err);
        }
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

TEST(CampaignSessionHost, VerdictQuerySumsTheBooksInAQuietWar) {
    // CAMP-DOM-1: the books' projection. The quiet rig holds one
    // objective (owner 2 == opening owner 2), so the census counts
    // ROK's holding and moves nothing — stalemate, no lead, empty
    // books, the exact bytes pinned.
    const auto rig = HostRig::make();
    EXPECT_EQ(
        rig.query("verdict"),
        R"({"v":1,"op":"query","q":"verdict","status":"ok","data":)"
        R"({"t":0,"threshold":0,"band":"stalemate","leader":-1,)"
        R"("leader_swing":0,"teams":[)"
        R"({"slot":2,"name":"ROK","owned":1,"gained":0,"lost":0,)"
        R"("gained_value":0,"lost_value":0,"swing":0,"captures":0,)"
        R"("air_losses":0,"ground_losses":0,"battalions_destroyed":0,)"
        R"("aircraft_remaining":0},)"
        R"({"slot":6,"name":"DPRK","owned":0,"gained":0,"lost":0,)"
        R"("gained_value":0,"lost_value":0,"swing":0,"captures":0,)"
        R"("air_losses":0,"ground_losses":0,"battalions_destroyed":0,)"
        R"("aircraft_remaining":0}]}})" "\n");
}

TEST(CampaignSessionHost, VerdictFamilyStaysSilentInAQuietWar) {
    // The diff's opening discipline: a fresh session starts
    // stalemate/no-lead and never publishes a synthetic "here is your
    // state" event — a war at rest is verdict-silent.
    const auto rig = HostRig::make();
    api::EventFilter f;
    f.kinds = {api::CampaignEvent::Kind::Verdict};
    rig.host->set_event_filter(f);
    (void)rig.host->step(600);
    EXPECT_TRUE(rig.host->drain_events().empty());
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

TEST(CampaignSessionHost, CampCmdQueueRefusesTyped) {
    const auto rig = HostRig::make();

    // CAMP-CMD-2 landed the last of the v1 command set — the queue's
    // refusals are now the ENGINE's typed semantics, not the tranche
    // stub. Kind-only intents hit every guard:
    //   flight_retask — mission byte 0 is not a taskable mission.
    //   flight_abort  — flight VU 0 is in nobody's war.
    //   objective_priority — objective 0 is in no world.
    api::CommandIntent retask;
    retask.kind = api::CommandIntent::Kind::FlightRetask;
    auto ack = rig.host->submit(retask);
    EXPECT_EQ(ack.status, api::CommandAck::Status::Refused);
    EXPECT_EQ(ack.refusal, api::CommandAck::Refusal::InvalidArgument);
    EXPECT_NE(ack.detail.find("taskable"), std::string::npos);

    api::CommandIntent abort;
    abort.kind = api::CommandIntent::Kind::FlightAbort;
    ack = rig.host->submit(abort);
    EXPECT_EQ(ack.status, api::CommandAck::Status::Refused);
    EXPECT_EQ(ack.refusal, api::CommandAck::Refusal::UnknownFlight);

    api::CommandIntent priority;
    priority.kind = api::CommandIntent::Kind::ObjectivePriority;
    ack = rig.host->submit(priority);
    EXPECT_EQ(ack.status, api::CommandAck::Status::Refused);
    EXPECT_EQ(ack.refusal, api::CommandAck::Refusal::UnknownObjective);

    // The weight scale is the save's own objective-priority byte.
    priority.weight = 101;
    priority.objective_id = 4101;
    ack = rig.host->submit(priority);
    EXPECT_EQ(ack.refusal, api::CommandAck::Refusal::InvalidArgument);

    // And the write itself: the first runtime write of the objective
    // priority — the `objectives` query echoes it from the same row
    // every tasking score reads.
    priority.weight = 80;
    ack = rig.host->submit(priority);
    EXPECT_EQ(ack.status, api::CommandAck::Status::Applied);
    EXPECT_EQ(ack.refusal, api::CommandAck::Refusal::None);
    EXPECT_NE(ack.detail.find("4101"), std::string::npos);
    EXPECT_NE(ack.detail.find("80"), std::string::npos);
    const auto objectives = rig.query("objectives");
    EXPECT_NE(objectives.find("\"priority\":80"), std::string::npos);
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

    // step reports the dilation flag (and the HOST-2 "events":0 key)
    EXPECT_NE(rig.line(R"({"v":1,"op":"step","ticks":600})")
                  .find("\"dilated\":0"),
              std::string::npos);

    // a refused command rides the wire as data (CAMP-CMD-2: the abort
    // of a REAL flight applies now — the refusal is the unknown VU's)
    const auto refused = rig.line(
        R"({"v":1,"op":"command","intent":"flight_abort","flight":777})");
    EXPECT_NE(refused.find("\"status\":\"refused\""), std::string::npos);
    EXPECT_NE(refused.find("\"refusal\":\"unknown_flight\""),
              std::string::npos);

    // malformed is exit 20 with the error object
    std::string out;
    const auto o = api::host_handle(*rig.host, R"({"v":1,"op":)", out);
    EXPECT_EQ(o.kind, api::ProtocolOutcome::Kind::ProtocolError);
    EXPECT_EQ(o.exit_code, 20);
    EXPECT_NE(out.find("\"code\":\"malformed\""), std::string::npos);
}

// ============================================================================
// CAMP-HOST-2 — the event stream, the journal, the replay identity
// ============================================================================

namespace {

// The kunsan rig (the C5/C6 acceptance fixture) through the HOST — the
// event-stream tests' war. ATM on: the ladder files missions every
// cycle; `armed` mirrors the C6 ArmedWar options (the save flights are
// synthetic, spawned by the legacy ladder, and armed for A/A).
struct WarRig {
    std::filesystem::path dir;
    std::filesystem::path world;
    std::unique_ptr<EngineSessionHost> host;

    static WarRig make(bool atm, bool armed) {
        const auto f16 =
            std::filesystem::path(F4_GENERATED_FIXTURES_DIR) / "f16.json";
        EXPECT_FALSE(f16.empty()) << "f16.json fixture not generated";

        WarRig rig;
        rig.dir = make_temp_dir();
        CampaignSessionOptions opts;
        opts.world_json =
            std::filesystem::path(F4_SIMULATION_TEST_FIXTURES_DIR) /
            "kunsan_session.world.json";
        opts.class_table =
            std::filesystem::path(F4_SOURCE_FIXTURES_DIR) /
            "falcon4.ct.json";
        opts.aircraft_config = f16;
        opts.mission_profiles = F4_MISSION_PROFILES_JSON;
        opts.tasking_cycle_sec = 5;
        opts.reinforce_period_sec = 0;
        opts.atm_pipeline = atm;
        opts.max_flights = 8;
        opts.aa_combat = armed;
        opts.max_steps_per_advance = 4000;
        std::string err;
        rig.host = EngineSessionHost::create(opts, &err);
        // Same discipline as HostRig::make: fail the test, never the
        // process (the factory cannot ASSERT — throw instead).
        if (rig.host == nullptr) {
            throw std::runtime_error(
                "WarRig: session create failed: " + err);
        }
        return rig;
    }

    /// The FID-5 combat rig's head-on merge through the HOST: two save
    /// flights (teams 2 and 6) converging inside the combat envelope,
    /// armed (aa_combat), tiered. The C6 fight — both sides shoot at
    /// t=13 and the journal carries the kill pair.
    static WarRig make_combat() {
        const auto f16 =
            std::filesystem::path(F4_GENERATED_FIXTURES_DIR) / "f16.json";
        EXPECT_FALSE(f16.empty()) << "f16.json fixture not generated";

        WarRig rig;
        rig.dir = make_temp_dir();
        rig.world = rig.dir / "combat.world.json";
        {
            std::ofstream f(rig.world);
            f << combat_world_json();
        }
        CampaignSessionOptions opts;
        opts.world_json = rig.world;
        opts.class_table =
            std::filesystem::path(F4_SOURCE_FIXTURES_DIR) /
            "falcon4.ct.json";
        opts.aircraft_config = f16;
        opts.mission_profiles = F4_MISSION_PROFILES_JSON;
        opts.fidelity_policy = FidelityPolicy::Tiered;
        opts.tasking_cycle_sec = 1000000;
        opts.reinforce_period_sec = 0;
        opts.atm_pipeline = false;
        opts.aa_combat = true;
        opts.max_steps_per_advance = 4000;
        std::string err;
        rig.host = EngineSessionHost::create(opts, &err);
        // Same discipline as HostRig::make: fail the test, never the
        // process (the factory cannot ASSERT — throw instead).
        if (rig.host == nullptr) {
            throw std::runtime_error(
                "WarRig::make_combat: session create failed: " + err);
        }
        return rig;
    }

    // Count occurrences of a wire fragment across a protocol output.
    static int count(const std::string& text, const std::string& needle) {
        int n = 0;
        for (auto pos = text.find(needle); pos != std::string::npos;
             pos = text.find(needle, pos + needle.size())) {
            ++n;
        }
        return n;
    }
};

// A subscription + step over the protocol; returns the FULL output
// (the step response line + any event lines).
std::string subscribe_step(EngineSessionHost& host, const char* kinds,
                           const char* teams, std::uint32_t ticks) {
    std::string sub_req =
        std::string(R"({"v":1,"op":"subscribe","kinds":)") + kinds;
    if (teams != nullptr) {
        sub_req += std::string(R"(,"teams":)") + teams;
    }
    sub_req += "}";
    std::string out;
    (void)api::host_handle(host, sub_req, out);
    EXPECT_NE(out.find("\"status\":\"ok\""), std::string::npos) << out;
    out.clear();
    (void)api::host_handle(
        host,
        std::string(R"({"v":1,"op":"step","ticks":)") +
            std::to_string(ticks) + "}",
        out);
    return out;
}

} // namespace

// ============================================================================
// CAMP-CMD-1 — roe_set: the P7 fire-control path behind the command
// ============================================================================

namespace {

// The first campaign aircraft of a team (the combat rig materializes
// one flight per side — via the FID-5 deagg in the tiered policy), for
// the brain-gate assertions.
f4::entities::EntityId find_team_aircraft(EngineSessionHost& host,
                                          std::uint8_t team) {
    for (const auto id : host.engine().campaign_aircraft()) {
        f4::entities::EntityHandle h(id, &host.engine().sim().world());
        auto* origin = h.get<CampaignOriginComponent>();
        if (origin != nullptr && origin->team_slot == team) return id;
    }
    return {};
}

api::CommandIntent roe_team(std::uint8_t team, api::RoeLevel level) {
    api::CommandIntent cmd;
    cmd.kind = api::CommandIntent::Kind::RoeSet;
    cmd.scope.kind = api::RoEScopeKind::Team;
    cmd.scope.team = team;
    cmd.roe = level;
    return cmd;
}

} // namespace

TEST(CommandRoe, TeamScopeAppliesGatesAndPublishes) {
    auto rig = WarRig::make_combat();
    rig.host->step(10);  // the war's clock is at t = 2.5 s
    // Tiered policy: the flights stay VIRTUAL aggregates until FID
    // materializes them — force the deagg (the FID-4 commands) so the
    // aircraft exist to command. The fresh aircraft spawn free (the
    // session's doctrine store is still empty).
    rig.host->engine().force_deaggregate_flight(5001);
    rig.host->engine().force_deaggregate_flight(5002);
    const auto blue = find_team_aircraft(*rig.host, 2);
    const auto red = find_team_aircraft(*rig.host, 6);
    ASSERT_TRUE(blue.valid());
    ASSERT_TRUE(red.valid());

    // the wire's filter: roe_changed only
    api::EventFilter filter;
    filter.kinds.push_back(api::CampaignEvent::Kind::RoeChanged);
    rig.host->set_event_filter(filter);

    // HOLD team 2 — applied at the CURRENT boundary, red untouched
    const auto ack = rig.host->submit(roe_team(2, api::RoeLevel::Hold));
    EXPECT_EQ(ack.status, api::CommandAck::Status::Applied);
    EXPECT_EQ(ack.refusal, api::CommandAck::Refusal::None);
    EXPECT_EQ(ack.apply_tick, rig.host->engine().campaign_time());
    EXPECT_NE(ack.detail.find("team 2 = hold"), std::string::npos);
    EXPECT_NE(ack.detail.find("1 live aircraft matched"), std::string::npos);

    {
        f4::entities::EntityHandle hb(blue, &rig.host->engine().sim().world());
        auto* brain = hb.get<f4::ai::BrainComponent>();
        ASSERT_NE(brain, nullptr);
        EXPECT_TRUE(brain->hold_fire());
        EXPECT_TRUE(brain->bvr_hold());
        EXPECT_TRUE(brain->bvr().fire().config().hold_fire);
        EXPECT_TRUE(brain->wvr().fire().config().hold_fire);
        EXPECT_TRUE(brain->wvr().guns().config().hold_fire);
    }
    {
        f4::entities::EntityHandle hr(red, &rig.host->engine().sim().world());
        auto* brain = hr.get<f4::ai::BrainComponent>();
        ASSERT_NE(brain, nullptr);
        EXPECT_FALSE(brain->hold_fire());
        EXPECT_FALSE(brain->bvr().fire().config().hold_fire);
    }

    // roe_changed published — the HOST-2 pinned encoder's bytes, the
    // scope echoing the command's verbatim
    auto events = rig.host->drain_events();
    ASSERT_EQ(events.size(), 1u);
    EXPECT_EQ(events[0].kind, api::CampaignEvent::Kind::RoeChanged);
    f4::json::Writer w;
    api::encode(w, events[0]);
    EXPECT_NE(w.str().find("\"ev\":\"roe_changed\",\"t\":"), std::string::npos)
        << w.str();
    EXPECT_NE(w.str().find(",\"roe\":2}"), std::string::npos) << w.str();
    EXPECT_NE(w.str().find(
                  "\"scope\":{\"kind\":\"team\",\"team\":2,\"mission\":0,"
                  "\"flight\":0}"),
              std::string::npos)
        << w.str();

    // The LOWERING apply_flight_roe could never express — the full
    // recompute restores what the level no longer holds (the combat
    // session's doctrine baseline is all-free).
    const auto ack2 = rig.host->submit(roe_team(2, api::RoeLevel::Tight));
    EXPECT_EQ(ack2.status, api::CommandAck::Status::Applied);
    {
        f4::entities::EntityHandle hb(blue, &rig.host->engine().sim().world());
        auto* brain = hb.get<f4::ai::BrainComponent>();
        ASSERT_NE(brain, nullptr);
        EXPECT_FALSE(brain->hold_fire());      // the brain-level hold CLEARED
        EXPECT_TRUE(brain->bvr_hold());        // tight: BVR still suppressed
        EXPECT_TRUE(brain->bvr().fire().config().hold_fire);
        EXPECT_FALSE(brain->wvr().fire().config().hold_fire);
        EXPECT_FALSE(brain->wvr().guns().config().hold_fire);
    }
    const auto ack3 = rig.host->submit(roe_team(2, api::RoeLevel::Free));
    EXPECT_EQ(ack3.status, api::CommandAck::Status::Applied);
    {
        f4::entities::EntityHandle hb(blue, &rig.host->engine().sim().world());
        auto* brain = hb.get<f4::ai::BrainComponent>();
        ASSERT_NE(brain, nullptr);
        EXPECT_FALSE(brain->hold_fire());
        EXPECT_FALSE(brain->bvr_hold());
        EXPECT_FALSE(brain->bvr().fire().config().hold_fire);
        EXPECT_FALSE(brain->wvr().fire().config().hold_fire);
        EXPECT_FALSE(brain->wvr().guns().config().hold_fire);
    }
    EXPECT_EQ(rig.host->drain_events().size(), 2u);  // two more changes
}

TEST(CommandRoe, ScopesRefuseLoudlyAndMatchByOrigin) {
    auto rig = WarRig::make_combat();
    rig.host->step(10);
    // tiered policy: materialize the flights first (see the sibling test)
    rig.host->engine().force_deaggregate_flight(5001);
    rig.host->engine().force_deaggregate_flight(5002);
    const auto blue = find_team_aircraft(*rig.host, 2);
    ASSERT_TRUE(blue.valid());
    std::uint8_t blue_mission = 0;
    {
        f4::entities::EntityHandle hb(blue, &rig.host->engine().sim().world());
        auto* origin = hb.get<CampaignOriginComponent>();
        ASSERT_NE(origin, nullptr);
        blue_mission = origin->mission_byte;
    }

    // zero-values are non-targets (refusal is data)
    {
        const auto ack = rig.host->submit(roe_team(0, api::RoeLevel::Hold));
        EXPECT_EQ(ack.status, api::CommandAck::Status::Refused);
        EXPECT_EQ(ack.refusal, api::CommandAck::Refusal::InvalidArgument);
        EXPECT_NE(ack.detail.find("team 0"), std::string::npos);
    }
    {
        api::CommandIntent cmd = roe_team(2, api::RoeLevel::Hold);
        cmd.scope.kind = api::RoEScopeKind::Mission;
        cmd.scope.mission = 0;  // untasked is not a doctrine target
        const auto ack = rig.host->submit(cmd);
        EXPECT_EQ(ack.status, api::CommandAck::Status::Refused);
        EXPECT_EQ(ack.refusal, api::CommandAck::Refusal::InvalidArgument);
    }
    {
        api::CommandIntent cmd = roe_team(2, api::RoeLevel::Hold);
        cmd.scope.kind = api::RoEScopeKind::Flight;
        cmd.scope.flight = 0;
        const auto ack = rig.host->submit(cmd);
        EXPECT_EQ(ack.status, api::CommandAck::Status::Refused);
        EXPECT_EQ(ack.refusal, api::CommandAck::Refusal::UnknownFlight);
    }
    {
        api::CommandIntent cmd = roe_team(2, api::RoeLevel::Hold);
        cmd.scope.kind = api::RoEScopeKind::Flight;
        cmd.scope.flight = 999999;  // not in the session's roster
        const auto ack = rig.host->submit(cmd);
        EXPECT_EQ(ack.status, api::CommandAck::Status::Refused);
        EXPECT_EQ(ack.refusal, api::CommandAck::Refusal::UnknownFlight);
        EXPECT_NE(ack.detail.find("roster"), std::string::npos);
    }

    // the FLIGHT scope matches by the origin's flight VU and leaves the
    // rest of the war alone
    {
        api::CommandIntent cmd = roe_team(0, api::RoeLevel::Hold);
        cmd.scope.kind = api::RoEScopeKind::Flight;
        cmd.scope.flight = 5001;  // the combat rig's team-2 flight
        const auto ack = rig.host->submit(cmd);
        EXPECT_EQ(ack.status, api::CommandAck::Status::Applied);
        EXPECT_NE(ack.detail.find("flight VU 5001 = hold"),
                  std::string::npos);
        EXPECT_NE(ack.detail.find("1 live aircraft matched"),
                  std::string::npos);
    }

    // the MISSION scope matches (team, mission_byte) off the same origin
    {
        api::CommandIntent cmd = roe_team(0, api::RoeLevel::Free);
        cmd.scope.kind = api::RoEScopeKind::Mission;
        cmd.scope.team = 2;
        cmd.scope.mission = blue_mission;
        ASSERT_NE(blue_mission, 0) << "the rig's flight must be tasked";
        const auto ack = rig.host->submit(cmd);
        EXPECT_EQ(ack.status, api::CommandAck::Status::Applied);
        EXPECT_NE(ack.detail.find("team 2 mission " +
                                  std::to_string(blue_mission)),
                  std::string::npos);
        EXPECT_NE(ack.detail.find("1 live aircraft matched"),
                  std::string::npos);
    }

    // a doctrine for nobody matches zero and still applies (future
    // flights spawn under it)
    {
        const auto ack =
            rig.host->submit(roe_team(5, api::RoeLevel::Hold));
        EXPECT_EQ(ack.status, api::CommandAck::Status::Applied);
        EXPECT_NE(ack.detail.find("0 live aircraft matched"),
                  std::string::npos);
    }
}

TEST(CommandRoe, HoldStopsTheWarInTheBooks) {
    // The outcome gate (plan §8): the un-commanded combat rig journals
    // the kill pair at t=13; the same war with BOTH teams held at
    // t=2.5 s kills nobody — the fire-control gates carried the
    // command all the way into the books.
    auto rig = WarRig::make_combat();
    rig.host->step(10);  // t = 2.5 s — before the merge

    ASSERT_EQ(rig.host->submit(roe_team(2, api::RoeLevel::Hold)).status,
              api::CommandAck::Status::Applied);
    ASSERT_EQ(rig.host->submit(roe_team(6, api::RoeLevel::Hold)).status,
              api::CommandAck::Status::Applied);

    int kills = 0;
    (void)rig.host->add_event_sink(
        [&](const api::CampaignEvent& e) {
            if (e.kind == api::CampaignEvent::Kind::Kill) ++kills;
        });
    rig.host->step(600);  // through the merge window and well past it
    EXPECT_EQ(kills, 0);
    EXPECT_EQ(rig.host->engine().stats().aa_kills, 0);
}

TEST(CommandReplay, JournalReproducesTheIdentity) {
    // The identity statement's replay half (plan §2.3/§5): the same
    // (save, seed, command journal) reproduces the record's
    // fingerprint — and the replay's step CHUNKING is irrelevant, the
    // host segments around the journal's ticks.
    std::vector<api::CommandJournalEntry> recorded;
    api::IdentityFingerprint final_a;
    {
        auto rig = WarRig::make_combat();
        rig.host->set_command_journal_sink(
            [&](std::uint64_t tick, std::int64_t t_s,
                const api::CommandIntent& intent) {
                recorded.push_back(api::CommandJournalEntry{tick, t_s, intent});
            });
        rig.host->step(10);
        ASSERT_EQ(rig.host->submit(roe_team(2, api::RoeLevel::Hold)).status,
                  api::CommandAck::Status::Applied);
        rig.host->step(20);
        ASSERT_EQ(rig.host->submit(roe_team(6, api::RoeLevel::Tight)).status,
                  api::CommandAck::Status::Applied);
        rig.host->step(30);
        ASSERT_EQ(rig.host->submit(roe_team(2, api::RoeLevel::Free)).status,
                  api::CommandAck::Status::Applied);
        rig.host->step(40);
        final_a = rig.host->identity();
    }
    ASSERT_EQ(recorded.size(), 3u);
    EXPECT_EQ(recorded[0].apply_tick, 10u);
    EXPECT_EQ(recorded[1].apply_tick, 30u);
    EXPECT_EQ(recorded[2].apply_tick, 60u);
    EXPECT_EQ(recorded[0].intent.roe, api::RoeLevel::Hold);

    // B — the whole journal through ONE step call
    {
        auto rig = WarRig::make_combat();
        ASSERT_TRUE(rig.host->start_command_replay(recorded));
        EXPECT_EQ(rig.host->pending_replay_commands(), 3u);
        rig.host->step(100);
        EXPECT_EQ(rig.host->pending_replay_commands(), 0u);
        const auto id = rig.host->identity();
        EXPECT_EQ(id.ledger_fnv, final_a.ledger_fnv);
        EXPECT_EQ(id.campaign_time_s, final_a.campaign_time_s);
    }

    // C — the record's own step pattern
    {
        auto rig = WarRig::make_combat();
        ASSERT_TRUE(rig.host->start_command_replay(recorded));
        rig.host->step(10);
        rig.host->step(20);
        rig.host->step(30);
        rig.host->step(40);
        const auto id = rig.host->identity();
        EXPECT_EQ(id.ledger_fnv, final_a.ledger_fnv);
        EXPECT_EQ(id.campaign_time_s, final_a.campaign_time_s);
    }

    // D — an odd chunking (25 × step(4)) — same war
    {
        auto rig = WarRig::make_combat();
        ASSERT_TRUE(rig.host->start_command_replay(recorded));
        for (int i = 0; i < 25; ++i) rig.host->step(4);
        EXPECT_EQ(rig.host->pending_replay_commands(), 0u);
        const auto id = rig.host->identity();
        EXPECT_EQ(id.ledger_fnv, final_a.ledger_fnv);
    }

    // E — a replay cut short: the commands past its last tick stay
    // pending (the reference host exits 23 on this)
    {
        auto rig = WarRig::make_combat();
        ASSERT_TRUE(rig.host->start_command_replay(recorded));
        rig.host->step(20);
        EXPECT_EQ(rig.host->pending_replay_commands(), 2u);
    }

    // F — a wire command during replay is refused: the journal is the
    // command source
    {
        auto rig = WarRig::make_combat();
        ASSERT_TRUE(rig.host->start_command_replay(recorded));
        const auto ack = rig.host->submit(roe_team(2, api::RoeLevel::Hold));
        EXPECT_EQ(ack.status, api::CommandAck::Status::Refused);
        EXPECT_EQ(ack.refusal, api::CommandAck::Refusal::InvalidArgument);
        EXPECT_NE(ack.detail.find("replay"), std::string::npos);
    }
}


// The golden-identity rule on the WIRE: an un-subscribed client's step
// announces zero events (and receives none) — the HOST-1 line shape.
TEST(EventStream, SilentUntilSubscribed) {
    auto rig = WarRig::make(/*atm=*/true, /*armed=*/false);
    std::string out;
    (void)api::host_handle(
        *rig.host, R"({"v":1,"op":"step","ticks":600})", out);
    EXPECT_NE(out.find("\"events\":0}"), std::string::npos);
    EXPECT_EQ(out.find("\"ev\":"), std::string::npos);

    // ...and the session-side surface agrees (nothing buffered, ever)
    EXPECT_TRUE(rig.host->drain_events().empty());
}

// The kunsan war's first tasking cycle, on the wire. The engine's own
// order, as the bus carries it: the cycle's FILINGS ride the intent
// publish (inside Campaign::tick), the cycle SUMMARY follows the tick
// that fired it. 600 ticks = 9 whole campaign seconds (the engine's
// accumulator discipline, pinned by the HOST-1 step test) — exactly
// one 5-second cycle, at t=5.
TEST(EventStream, TheKunsanWarFilesEventsInEngineOrder) {
    auto rig = WarRig::make(/*atm=*/true, /*armed=*/false);
    const auto out =
        subscribe_step(*rig.host, R"(["all"])", nullptr, 600);

    // one cycle fired in the 9-second window, and it filed missions
    EXPECT_EQ(WarRig::count(out, "\"ev\":\"tasking_cycle\""), 1);
    EXPECT_GE(WarRig::count(out, "\"ev\":\"mission_filed\""), 1);
    // the summary's clock is the ladder's own: the 5-second mark
    EXPECT_NE(out.find("\"ev\":\"tasking_cycle\",\"t\":5,"), std::string::npos)
        << out.substr(0, 400);

    // ordering: the filings precede their cycle's summary line
    const auto first_cycle = out.find("\"ev\":\"tasking_cycle\"");
    const auto first_filed = out.find("\"ev\":\"mission_filed\"");
    ASSERT_NE(first_cycle, std::string::npos);
    ASSERT_NE(first_filed, std::string::npos);
    EXPECT_LT(first_filed, first_cycle);
}

// The wire filter: kinds select families, teams select sides (the kill
// family's OR semantics ride the same matcher the mock tests pin).
TEST(EventStream, TheFilterGatesTheWire) {
    auto rig = WarRig::make(/*atm=*/true, /*armed=*/false);
    // kill-only subscription: a quiet A/A sky files missions but kills
    // nobody — the subscription delivers nothing even as events fire
    const auto kills_only =
        subscribe_step(*rig.host, R"(["kill"])", nullptr, 600);
    EXPECT_NE(kills_only.find("\"events\":0}"), std::string::npos);
    EXPECT_EQ(kills_only.find("\"ev\":"), std::string::npos);

    // DPRK-side filings only: every mission_filed line carries team 6
    auto rig2 = WarRig::make(/*atm=*/true, /*armed=*/false);
    const auto dprk =
        subscribe_step(*rig2.host, R"(["mission_filed"])", R"([6])", 600);
    const auto filed = WarRig::count(dprk, "\"ev\":\"mission_filed\"");
    if (filed > 0) {
        EXPECT_EQ(WarRig::count(dprk, "\"ev\":\"tasking_cycle\""), 0);
        // every delivered filing names team 6 (the filter's team gate)
        for (auto pos = dprk.find("\"ev\":\"mission_filed\"");
             pos != std::string::npos;
             pos = dprk.find("\"ev\":\"mission_filed\"", pos + 1)) {
            const auto row_end = dprk.find('\n', pos);
            ASSERT_NE(row_end, std::string::npos);
            EXPECT_NE(dprk.substr(pos, row_end - pos).find("\"team\":6"),
                      std::string::npos)
                << dprk.substr(pos, row_end - pos);
        }
    }
}

// THE C6 GATE (plan §8 CAMP-HOST-2): journal the armed war, replay it,
// assert the identity reproduces — the same (save, seed, commands)
// regenerates the ENTIRE stream byte-for-byte, closing with the same
// ledger fingerprint the C5 harness MD5s. The fight is the FID-5
// combat rig's head-on merge (two save flights, armed, converging at
// ~410 ft/s): both sides shoot at t=13, both books move, the journal
// carries the kill pair, and the replay regenerates it to the byte.
TEST(EventStream, TheArmedWarJournalsAndReplays) {
    // Run 1: journal the fight (40 s of the merge — the kills land at 13).
    const auto war_path = make_temp_dir() / "war.jsonl";
    api::IdentityFingerprint start1;
    {
        auto rig = WarRig::make_combat();
        start1 = rig.host->identity();
        api::EventJournalWriter journal;
        ASSERT_TRUE(journal.open(war_path.string(), start1));
        (void)rig.host->add_event_sink(
            [&journal](const api::CampaignEvent& e) { journal.append(e); });
        for (int i = 0; i < 12; ++i) {
            (void)rig.host->step(200);   // 12 × 200 = 2400 ticks = 40 s
        }
        const auto end1 = rig.host->identity();
        ASSERT_TRUE(journal.close(end1));
        // the fight actually FOUGHT: the stream carries the kill pair
        const auto text = [&war_path] {
            std::ifstream f(war_path);
            std::stringstream buf;
            buf << f.rdbuf();
            return buf.str();
        }();
        EXPECT_EQ(WarRig::count(text, "\"ev\":\"kill\""), 2)
            << "the armed war's journal carries no kill pair — the C6 "
               "fight did not close";
        // and the first kill's line is the pinned bytes (squadron 4282
        // of the DPRK draws first blood at t=13, a missile's credit)
        EXPECT_NE(text.find(
                      "{\"ev\":\"kill\",\"t\":13,\"killer\":{\"sq\":4282,"
                      "\"team\":6},\"victim\":{\"sq\":4281,\"team\":2},"
                      "\"weapon\":\"missile\"}"),
                  std::string::npos)
            << text.substr(0, 600);
    }

    // Run 2: the replay — same save, same seed, same stepping; every
    // line verified against run 1's journal, footer identity included.
    {
        auto rig = WarRig::make_combat();
        api::EventJournalVerifier verifier;
        ASSERT_TRUE(verifier.open(war_path.string(), start1))
            << verifier.detail();
        (void)rig.host->add_event_sink(
            [&verifier](const api::CampaignEvent& e) {
                std::string err;
                if (!verifier.expect(e, &err)) {
                    FAIL() << "replay drift: " << err;
                }
            });
        for (int i = 0; i < 12; ++i) {
            (void)rig.host->step(200);
        }
        const auto end2 = rig.host->identity();
        std::string err;
        ASSERT_TRUE(verifier.close(end2, &err)) << err;
    }
    std::filesystem::remove(war_path);
}

// The journal arms by use and never touches the war: a journaled
// session's save is byte-identical to an un-journaled one, and an
// EMPTY journal (nothing armed the stream) is exactly two lines.
TEST(EventStream, EmptyJournalSaveByteIdentical) {
    const auto quiet = [] {
        // the tier rig's quiet war: no cycles, no combat, no events
        return HostRig::make();
    };

    auto baseline = quiet();
    (void)baseline.host->step(600);
    const auto base_path = baseline.dir / "base.world.json";
    const auto base_res = baseline.host->save(base_path.string());
    ASSERT_TRUE(base_res.ok) << base_res.detail;

    auto journaled = quiet();
    const auto war_path = journaled.dir / "war.jsonl";
    api::EventJournalWriter journal;
    ASSERT_TRUE(journal.open(war_path.string(), journaled.host->identity()));
    (void)journaled.host->add_event_sink(
        [&journal](const api::CampaignEvent& e) { journal.append(e); });
    (void)journaled.host->step(600);
    ASSERT_TRUE(journal.close(journaled.host->identity()));

    const auto war_res =
        journaled.host->save((journaled.dir / "journaled.world.json").string());
    ASSERT_TRUE(war_res.ok) << war_res.detail;

    // the war never noticed the journal
    EXPECT_EQ(base_res.bytes, war_res.bytes);
    EXPECT_EQ(baseline.host->identity().ledger_fnv,
              journaled.host->identity().ledger_fnv);
    std::ifstream a(base_path, std::ios::binary);
    std::ifstream b(journaled.dir / "journaled.world.json",
                    std::ios::binary);
    std::stringstream ba, bb;
    ba << a.rdbuf();
    bb << b.rdbuf();
    ASSERT_EQ(ba.str(), bb.str());

    // and the journal recorded the silence: header + footer, nothing else
    std::ifstream j(war_path);
    std::stringstream jb;
    jb << j.rdbuf();
    const auto text = jb.str();
    EXPECT_EQ(std::count(text.begin(), text.end(), '\n'), 2);
    EXPECT_EQ(text.find("{\"v\":1,\"journal\":1,\"identity\":{"), 0U);
    EXPECT_NE(text.find("{\"journal_end\":{\"protocol\":1,"),
              std::string::npos);
}

// ============================================================================
// CAMP-HOST-3 — the contract↔engine PARITY pin
//
// The viewer's contract plane renders ONLY the query rows; the gate is
// that those rows agree with the ENGINE'S OWN views (flight_tiers, the
// intents, stats, the threat map) field for field. The adapter is a
// MAP, not a policy — this test is the proof, on the same kunsan war
// the event stream rides.
// ============================================================================

namespace {

// A minimal row walker for the flights rows (vu/team/live/count) —
// the viewer's own parser is pinned separately (test_campaign_queries);
// here we only need enough to diff against the engine truth.
struct FlightRowProbe {
    std::uint32_t vu = 0;
    int team = 0;
    int aircraft_count = 0;
    bool live = false;
};

std::vector<FlightRowProbe> probe_flight_rows(const std::string& json) {
    std::vector<FlightRowProbe> rows;
    f4::json::Reader r(json);
    r.expect('[');
    if (!r.consume(']')) {
        while (true) {
            FlightRowProbe p;
            r.expect('{');
            while (true) {
                if (r.consume('}')) break;
                const auto key = r.read_string();
                r.expect(':');
                if (key == "vu") p.vu = static_cast<std::uint32_t>(r.read_int());
                else if (key == "team") p.team = static_cast<int>(r.read_int());
                else if (key == "aircraft_count")
                    p.aircraft_count = static_cast<int>(r.read_int());
                else if (key == "live") p.live = r.read_int() != 0;
                else r.skip_value();
                if (r.consume(',')) continue;
                r.expect('}');
                break;
            }
            rows.push_back(p);
            if (r.consume(',')) continue;
            r.expect(']');
            break;
        }
    }
    return rows;
}

struct ThreatProbe {
    int viewer_team = -1;
    int cell_grid = 0;
    int cells_x = 0;
    int cells_y = 0;
    std::vector<int> low, high;
};

ThreatProbe probe_threat(const std::string& json) {
    ThreatProbe t;
    f4::json::Reader r(json);
    r.expect('{');
    while (true) {
        if (r.consume('}')) break;
        const auto key = r.read_string();
        r.expect(':');
        if (key == "viewer_team")
            t.viewer_team = static_cast<int>(r.read_int());
        else if (key == "cell_grid")
            t.cell_grid = static_cast<int>(r.read_int());
        else if (key == "cells_x")
            t.cells_x = static_cast<int>(r.read_int());
        else if (key == "cells_y")
            t.cells_y = static_cast<int>(r.read_int());
        else if (key == "low" || key == "high") {
            auto& out = key == "low" ? t.low : t.high;
            r.expect('[');
            if (!r.consume(']')) {
                while (true) {
                    out.push_back(static_cast<int>(r.read_int()));
                    if (r.consume(',')) continue;
                    r.expect(']');
                    break;
                }
            }
        } else r.skip_value();
        if (r.consume(',')) continue;
        r.expect('}');
        break;
    }
    return t;
}

} // namespace

TEST(CampaignHostParity, FlightsRowsMatchTheEngineTiers) {
    WarRig rig = WarRig::make(/*atm=*/true, /*armed=*/false);
    ASSERT_NE(rig.host, nullptr);
    rig.host->step(60 * 8);  // past the rig's first 5 s tasking cycle

    const auto res = rig.host->query({"flights", -1, 0});
    ASSERT_TRUE(res.ok) << res.detail;
    const auto rows = probe_flight_rows(res.data_json);
    const auto& tiers = rig.host->engine().flight_tiers();
    ASSERT_EQ(rows.size(), tiers.size());
    for (std::size_t i = 0; i < rows.size(); ++i) {
        EXPECT_EQ(rows[i].vu, tiers[i].vu);
        EXPECT_EQ(rows[i].team, static_cast<int>(tiers[i].team));
        EXPECT_EQ(rows[i].aircraft_count, tiers[i].aircraft_count);
        EXPECT_EQ(rows[i].live, tiers[i].live);
    }

    // The team filter gates the SAME rows the engine's own view would.
    if (!tiers.empty()) {
        const auto team = static_cast<int>(tiers[0].team);
        const auto filtered =
            rig.host->query({"flights", team, 0});
        ASSERT_TRUE(filtered.ok);
        const auto frows = probe_flight_rows(filtered.data_json);
        const auto expected = std::count_if(
            tiers.begin(), tiers.end(),
            [&](const auto& t) {
                return static_cast<int>(t.team) == team;
            });
        EXPECT_EQ(frows.size(), static_cast<std::size_t>(expected));
    }
}

TEST(CampaignHostParity, TaskingRowsCarryRouteAndRole) {
    WarRig rig = WarRig::make(/*atm=*/true, /*armed=*/false);
    ASSERT_NE(rig.host, nullptr);
    rig.host->step(60 * 8);

    const auto res = rig.host->query({"tasking", -1, 0});
    ASSERT_TRUE(res.ok) << res.detail;
    const auto& intents = rig.host->engine().intents();

    // Row count parity (one row object per intent — the payload is a
    // bare array, so every '{' is one row).
    const int n = static_cast<int>(std::count(res.data_json.begin(),
                                              res.data_json.end(), '{'));
    EXPECT_EQ(n, static_cast<int>(intents.size()));

    // The additive tail carries the C3 route leg count + the package
    // role of the FIRST intent, byte-exact against the engine.
    if (!intents.empty()) {
        const std::string expect_wps =
            "\"route_waypoints\":" +
            std::to_string(intents[0].route.size());
        EXPECT_NE(res.data_json.find(expect_wps), std::string::npos)
            << res.data_json;
        const std::string expect_role =
            "\"flight_role\":" +
            std::to_string(static_cast<int>(intents[0].flight_role));
        EXPECT_NE(res.data_json.find(expect_role), std::string::npos)
            << res.data_json;
    }
}

TEST(CampaignHostParity, TimeAndStatsEchoTheEngine) {
    WarRig rig = WarRig::make(/*atm=*/true, /*armed=*/false);
    ASSERT_NE(rig.host, nullptr);
    rig.host->step(60 * 8);

    const auto t = rig.host->query({"time", -1, 0});
    ASSERT_TRUE(t.ok);
    EXPECT_NE(t.data_json.find(
                  "\"campaign_time_s\":" +
                  std::to_string(rig.host->engine().campaign_time())),
              std::string::npos);
    EXPECT_NE(t.data_json.find("\"paused\":" +
                               std::string(rig.host->engine().paused()
                                               ? "1"
                                               : "0")),
              std::string::npos);

    const auto s = rig.host->query({"stats", -1, 0});
    ASSERT_TRUE(s.ok);
    const auto& st = rig.host->engine().stats();
    EXPECT_NE(s.data_json.find("\"cycles\":" + std::to_string(st.cycles)),
              std::string::npos);
    EXPECT_NE(s.data_json.find("\"intents\":" + std::to_string(st.intents)),
              std::string::npos);
    EXPECT_NE(s.data_json.find("\"live_aircraft\":" +
                               std::to_string(st.live_aircraft)),
              std::string::npos);
}

TEST(CampaignHostParity, ThreatGridMatchesTheMap) {
    WarRig rig = WarRig::make(/*atm=*/true, /*armed=*/false);
    ASSERT_NE(rig.host, nullptr);
    rig.host->step(60 * 8);

    const auto res = rig.host->query({"threat", -1, 0});
    ASSERT_TRUE(res.ok) << res.detail;
    const auto t = probe_threat(res.data_json);
    const auto& map = rig.host->engine().route_builder().threat_map();
    EXPECT_EQ(t.viewer_team,
              static_cast<int>(rig.host->engine().threat_viewer_team()));
    EXPECT_EQ(t.cells_x, map.cells_x());
    EXPECT_EQ(t.cells_y, map.cells_y());
    ASSERT_EQ(t.low.size(), t.high.size());
    ASSERT_EQ(t.low.size(),
              static_cast<std::size_t>(t.cells_x) *
                  static_cast<std::size_t>(t.cells_y));
    // Sample cells: the wire carries the SAME densities the engine's
    // map answers, band by band.
    for (int cy = 0; cy < t.cells_y; cy += 7) {
        for (int cx = 0; cx < t.cells_x; cx += 7) {
            const auto i = static_cast<std::size_t>(cy) * t.cells_x + cx;
            EXPECT_EQ(t.low[i],
                      map.low_band_density(
                          cx, cy, static_cast<std::uint8_t>(t.viewer_team)));
            EXPECT_EQ(t.high[i],
                      map.high_band_density(
                          cx, cy, static_cast<std::uint8_t>(t.viewer_team)));
        }
    }
}
