// test_campaign_personnel.cpp — the CAMP-DOM-3 war gates (the session).
//
// The engine's mechanics are pinned in f4-campaign's own tests
// (test_campaign_personnel.cpp — the pick, the decay, the books, the
// write-back face); here we pin what the SESSION adds, over the kunsan
// fixture (its two squadrons carry the save's own 48-pilot rosters —
// the perfect signal):
//
//   1. THE CREWS RIDE — with the AssignPilots arm on, every filed
//      flight draws a crew (the pilot_assigned events publish
//      one-for-one with the ledger's assignment log), the published
//      MissionIntent carries the crew (the spawner's lead-skill seam),
//      and the run is deterministic (two sessions, one ledger).
//   2. THE SQUADRONS QUERY — the personnel face: one row per squadron,
//      the wire's identity with the run's deltas applied (drawn slots
//      unavailable, nobody dead in this rig).
//   3. THE GOLDEN IDENTITY — arms off: no pilot events, no personnel
//      rows, no crews on the intents (rosters ignored beyond the
//      SCALE-1 skill map).
//   4. THE WRITE-BACK GATE — a draws-only run (outs, no deaths, no
//      recoveries, no rating fires) writes NO personnel: the out is
//      transient, the save carries no phantom states.

#include <f4/campaign/api/events.hpp>
#include <f4/campaign/api/protocol.hpp>
#include <f4/campaign/campaign.hpp>
#include <f4/campaign/result_ledger.hpp>
#include <f4/campaign/world_writeback.hpp>
#include <f4/entities/entity.hpp>
#include <f4/simulation/campaign_session.hpp>
#include <f4/simulation/campaign_session_host.hpp>
#include <f4/world/detail/world_state.hpp>

#include <gtest/gtest.h>

#include <algorithm>
#include <filesystem>
#include <set>
#include <string>
#include <vector>

using namespace f4::simulation;
namespace api = f4::campaign::api;

namespace {

std::filesystem::path kunsan_world() {
    return std::filesystem::path(F4_SIMULATION_TEST_FIXTURES_DIR) /
           "kunsan_campaign.world.json";
}
std::filesystem::path class_table() {
    return std::filesystem::path(F4_SOURCE_FIXTURES_DIR) / "falcon4.ct.json";
}
std::filesystem::path f16_config() {
    return std::filesystem::path(F4_GENERATED_FIXTURES_DIR) / "f16.json";
}

// The ATM-pipeline rig (test_campaign_session's ATM test shape): the
// campaign cycle shortened to 5 s so a few advance() calls cross it.
CampaignSessionOptions make_opts(bool pilot_assignment,
                                 bool rating_decay = false) {
    CampaignSessionOptions o;
    o.world_json = kunsan_world();
    o.class_table = class_table();
    o.aircraft_config = f16_config();
    o.mission_profiles = F4_MISSION_PROFILES_JSON;
    o.tasking_cycle_sec = 5;
    o.reinforce_period_sec = 0;   // off: draws only, no deliveries
    o.max_flights = 8;
    o.atm_pipeline = true;
    o.pilot_assignment = pilot_assignment;
    o.rating_decay = rating_decay;
    return o;
}

/// The pilot families off the session's bus (the host's own
/// subscription shape) + the crews the intents carried.
struct PersonnelObserver {
    int assigned = 0;
    int lost = 0;
    int recovered = 0;
    std::vector<std::vector<std::uint8_t>> crews;   // per assignment
    std::size_t intent_sub = 0;
    std::size_t event_sub = 0;
    int intents_with_crew = 0;

    void attach(CampaignSession& s) {
        event_sub = s.sim().bus().subscribe<api::CampaignEvent>(
            [this](const api::CampaignEvent& e) {
                switch (e.kind) {
                    case api::CampaignEvent::Kind::PilotAssigned:
                        ++assigned;
                        crews.push_back(e.pilot_assigned.pilots);
                        break;
                    case api::CampaignEvent::Kind::PilotLost: ++lost; break;
                    case api::CampaignEvent::Kind::PilotRecovered:
                        ++recovered;
                        break;
                    default: break;
                }
            });
        intent_sub = s.sim().bus().subscribe<f4::campaign::MissionIntent>(
            [this](const f4::campaign::MissionIntent& in) {
                if (!in.crew.empty()) ++intents_with_crew;
            });
    }
};

std::string squadrons_query(EngineSessionHost& host) {
    std::string out;
    (void)api::host_handle(
        host, R"({"v":1,"op":"query","q":"squadrons"})", out);
    return out;
}

/// One row's object text from a squadrons array response (the row that
/// carries `"vu":<vu>` — the encoding is byte-pinned, one flat object
/// with no nested braces).
std::string squad_row(const std::string& json, std::uint32_t vu) {
    const std::string key = "\"vu\":" + std::to_string(vu);
    const auto start = json.find(key);
    if (start == std::string::npos) return {};
    const auto end = json.find("},", start);
    return json.substr(start, end == std::string::npos
                                   ? std::string::npos
                                   : end - start + 1);
}

} // namespace

// ── 1. The crews ride ──────────────────────────────────────────────────────

TEST(CampaignPersonnel, CrewsRideIntentsBooksAndEvents) {
    if (!std::filesystem::exists(f16_config())) {
        GTEST_SKIP() << "f16.json fixture not generated";
    }
    std::string err;
    auto session = CampaignSession::create(make_opts(true), &err);
    ASSERT_NE(session, nullptr) << "create failed: " << err;

    PersonnelObserver obs;
    obs.attach(*session);

    session->set_paused(false);
    for (int frame = 0; frame < 60; ++frame) {   // ~60 s: 12 cycles
        session->advance(1.0);
    }

    // The engine drew crews: the books carry the assignments, the
    // events publish one-for-one, and the intents carried the crews.
    const auto& log = session->ledger().pilot_assignment_log();
    ASSERT_FALSE(log.empty()) << "no crew was ever assigned";
    EXPECT_EQ(obs.assigned, static_cast<int>(log.size()));
    EXPECT_EQ(obs.lost, 0);       // no combat in this rig
    EXPECT_EQ(obs.recovered, 0);  // mission-over is minutes out
    EXPECT_GT(obs.intents_with_crew, 0);

    // Every pick: non-empty, distinct slots, the lead from the
    // roster's front third (48 pilots → slots 0..15; kunsan's rosters
    // are all-available, so the first pick IS slot 0).
    for (const auto& crew : obs.crews) {
        ASSERT_FALSE(crew.empty());
        std::set<int> distinct(crew.begin(), crew.end());
        EXPECT_EQ(distinct.size(), crew.size()) << "a slot was double-picked";
        EXPECT_LT(crew[0], 16) << "the lead escaped the front third";
    }

    // Determinism: a second session, identically driven, lands on the
    // same bytes (the C5 contract, personnel edition).
    auto b = CampaignSession::create(make_opts(true), &err);
    ASSERT_NE(b, nullptr) << err;
    b->set_paused(false);
    for (int frame = 0; frame < 60; ++frame) b->advance(1.0);
    EXPECT_EQ(session->ledger_json(), b->ledger_json());
}

// ── 2. The squadrons query ────────────────────────────────────────────────

TEST(CampaignPersonnel, SquadronsQueryServesThePersonnelFace) {
    if (!std::filesystem::exists(f16_config())) {
        GTEST_SKIP() << "f16.json fixture not generated";
    }
    std::string err;
    auto host = EngineSessionHost::create(make_opts(true), &err);
    ASSERT_NE(host, nullptr) << err;
    host->set_paused(false);
    for (int frame = 0; frame < 30; ++frame) {
        host->step(60);   // one sim second per step call
    }

    const std::string json = squadrons_query(*host);
    // Kunsan's two rostered squadrons ride with their wire identity…
    const std::string a = squad_row(json, 4041);
    const std::string b = squad_row(json, 4048);
    ASSERT_FALSE(a.empty()) << json;
    ASSERT_FALSE(b.empty()) << json;
    EXPECT_NE(a.find("\"pilots_total\":48"), std::string::npos);
    EXPECT_NE(b.find("\"pilots_total\":48"), std::string::npos);
    // …and the run's deltas: the ATM filed heavily enough that BOTH
    // wings drew crews — their rosters thinned by the outs (drawn
    // slots are unavailable until their flights recover), and nobody
    // is dead in this rig.
    EXPECT_EQ(a.find("\"pilots_available\":48"), std::string::npos)
        << a << "\nthe drawn squadron's roster never thinned";
    EXPECT_EQ(b.find("\"pilots_available\":48"), std::string::npos)
        << b << "\nthe drawn squadron's roster never thinned";
    EXPECT_EQ(a.find("\"pilots_dead\":1"), std::string::npos);
    EXPECT_EQ(b.find("\"pilots_dead\":1"), std::string::npos);
}

// ── 3. The golden identity ────────────────────────────────────────────────

TEST(CampaignPersonnel, ArmsOffPublishesNothing) {
    if (!std::filesystem::exists(f16_config())) {
        GTEST_SKIP() << "f16.json fixture not generated";
    }
    std::string err;
    auto session = CampaignSession::create(make_opts(false, false), &err);
    ASSERT_NE(session, nullptr) << "create failed: " << err;

    PersonnelObserver obs;
    obs.attach(*session);
    session->set_paused(false);
    for (int frame = 0; frame < 60; ++frame) session->advance(1.0);

    // The war still tasking (the rig flies), but the personnel layer
    // never moved: no crews on the intents, no logs, no events.
    EXPECT_GT(session->stats().intents, 0);
    EXPECT_EQ(obs.intents_with_crew, 0);
    EXPECT_TRUE(session->ledger().pilot_assignment_log().empty());
    EXPECT_TRUE(session->ledger().pilot_loss_log().empty());
    EXPECT_TRUE(session->ledger().pilot_recovery_log().empty());
    EXPECT_EQ(obs.assigned, 0);
    EXPECT_EQ(obs.lost, 0);
    EXPECT_EQ(obs.recovered, 0);
}

// ── 4. The write-back gate ────────────────────────────────────────────────

TEST(CampaignPersonnel, DrawsOnlyRunWritesNoPersonnel) {
    if (!std::filesystem::exists(f16_config())) {
        GTEST_SKIP() << "f16.json fixture not generated";
    }
    std::string err;
    auto session = CampaignSession::create(make_opts(true), &err);
    ASSERT_NE(session, nullptr) << "create failed: " << err;
    session->set_paused(false);
    for (int frame = 0; frame < 30; ++frame) session->advance(1.0);

    // Crews drew (the books moved) but nothing died, recovered, or
    // decayed: the roster face stays the save's own.
    EXPECT_FALSE(session->ledger().pilot_assignment_log().empty());
    const auto wv = session->apply_writeback();
    EXPECT_EQ(wv.personnel_written, 0);
}
