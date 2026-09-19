// test_campaign_naval_session.cpp — the CAMP-DOM-5 war gates (the
// session).
//
// The engine's mechanics are pinned in f4-campaign's own tests
// (test_naval_tasking.cpp — the ranker, the family split, the ATM
// arm, the Campaign's per-target books); here we pin what the SESSION
// adds, over the kunsan fixture (its 2 task forces — the team-1
// carrier and the team-6 frigate — are the raw material):
//
//   1. THE TASKFORCES QUERY — the naval face: the wire's own rows
//      (identity, subtype name, position, destination, supply),
//      whether or not the arm is on, with the armed run's filing
//      books overlaid (the carrier/frigate get filed at).
//   2. THE FILINGS RIDE mission_filed — the anti-ship packages
//      publish on the SAME event every other package rides, the
//      target id in the event matching the books.
//   3. THE GOLDEN IDENTITY — arm off: the query still serves the same
//      2 rows with filings=0 (the wire owns the facts, the run adds
//      nothing), and no mission_filed event ever carries a task-force
//      target.
//   4. THE DETERMINISM — two armed runs answer the query byte-for-
//      byte identically.

#include <f4/campaign/api/dto.hpp>
#include <f4/campaign/api/events.hpp>
#include <f4/campaign/api/protocol.hpp>
#include <f4/campaign/campaign.hpp>
#include <f4/simulation/campaign_session.hpp>
#include <f4/simulation/campaign_session_host.hpp>
#include <f4/world/detail/world_state.hpp>

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdlib>
#include <cstring>
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

// The ATM-pipeline rig (the scheduling session test's shape) plus the
// stock-save bridge (kunsan's squadrons carry airbase 0) and the naval
// arm. The campaign cycle shortened to 5 s so a few steps cross it.
CampaignSessionOptions make_opts(bool naval_tasking) {
    CampaignSessionOptions o;
    o.world_json = kunsan_world();
    o.class_table = class_table();
    o.aircraft_config = f16_config();
    o.mission_profiles = F4_MISSION_PROFILES_JSON;
    o.tasking_cycle_sec = 5;
    o.reinforce_period_sec = 0;   // off: draws only, no deliveries
    o.max_flights = 8;
    o.atm_pipeline = true;
    o.synthesize_airbases = true;
    o.naval_tasking = naval_tasking;
    o.fidelity_policy = FidelityPolicy::Tiered;
    return o;
}

std::string run_query(EngineSessionHost& host, const char* q) {
    std::string out;
    (void)api::host_handle(host,
                           std::string(R"({"v":1,"op":"query","q":)") + q +
                               "}",
                           out);
    return out;
}

// The wire's task forces (the save's own truth, read off the session's
// WorldState).
std::set<std::uint32_t> session_taskforces(CampaignSession& s) {
    std::set<std::uint32_t> out;
    for (const auto& u : s.world_state().units) {
        if (u.unit_class == f4::entities::UnitClass::TaskForce) {
            out.insert(u.id_num);
        }
    }
    return out;
}

/// The mission_filed events (the anti-ship ones carry the target).
struct FiledObserver {
    struct Row {
        std::uint8_t mission = 0;
        std::uint32_t target = 0;
    };
    std::vector<Row> rows;
    void attach(CampaignSession& s) {
        s.sim().bus().subscribe<api::CampaignEvent>(
            [this](const api::CampaignEvent& e) {
                if (e.kind == api::CampaignEvent::Kind::MissionFiled) {
                    rows.push_back(Row{e.mission_filed.mission_byte,
                                       e.mission_filed.target_objective_id});
                }
            });
    }
};

// The response's "filings" values, summed (the DTO's per-row book).
int filings_sum(const std::string& json) {
    int sum = 0;
    std::size_t pos = 0;
    while ((pos = json.find("\"filings\":", pos)) != std::string::npos) {
        const auto val_start = pos + std::strlen("\"filings\":");
        sum += std::atoi(json.c_str() + val_start);
        pos = val_start;
    }
    return sum;
}

} // namespace

// ── 1 + 2. The query serves the wire's rows, the filings ride ────────────

TEST(CampaignNavalSession, TaskforcesQueryServesTheWireAndTheFilingsBook) {
    if (!std::filesystem::exists(f16_config())) {
        GTEST_SKIP() << "f16.json fixture not generated";
    }
    std::string err;
    auto host = EngineSessionHost::create(make_opts(true), &err);
    ASSERT_NE(host, nullptr) << "create failed: " << err;

    FiledObserver obs;
    obs.attach(host->engine());

    host->set_paused(false);
    for (int frame = 0; frame < 20; ++frame) {   // ~20 s: 4 cycles
        host->step(60);
    }
    auto& session = host->engine();

    // The wire's own truth: exactly the carrier + the frigate.
    const auto tfs = session_taskforces(session);
    ASSERT_EQ(tfs.size(), 2u);

    // The query: the wire's rows, the armed run's books overlaid.
    const auto json = run_query(*host, "\"taskforces\"");
    ASSERT_NE(json.find("\"status\":\"ok\""), std::string::npos) << json;
    EXPECT_NE(json.find("\"id_num\":4040"), std::string::npos)
        << "the carrier row is missing: " << json;
    EXPECT_NE(json.find("\"id_num\":4624"), std::string::npos)
        << "the frigate row is missing: " << json;
    EXPECT_NE(json.find("\"subtype_name\":\"Carrier\""), std::string::npos);
    EXPECT_NE(json.find("\"subtype_name\":\"Frigate\""), std::string::npos);

    // The filings booked one-for-one with the mission_filed stream:
    // every anti-ship filing the stream carries targets a task force,
    // and the books' per-target counts sum to the stream's filings.
    std::size_t aship = 0;
    for (const auto& r : obs.rows) {
        if (r.mission != 35) continue;   // AMIS_ASHIP
        ++aship;
        EXPECT_NE(tfs.count(r.target), 0u)
            << "a mission_filed anti-ship event at " << r.target
            << " is not a task force";
    }
    EXPECT_GT(aship, 0) << "the war never filed anti-ship";
    EXPECT_EQ(filings_sum(json), static_cast<int>(aship))
        << "the query's filing books and the event stream disagree";

    // The wire facts ride untouched: the carrier's destination and the
    // frigate's supply (the save's own bytes) answer through the row.
    EXPECT_NE(json.find("\"dest_x\":743"), std::string::npos)
        << "the carrier row's wire destination is missing";
    EXPECT_NE(json.find("\"supply\":100"), std::string::npos)
        << "the frigate row's wire supply is missing";
}

// ── 3. The golden identity ───────────────────────────────────────────────

TEST(CampaignNavalSession, ArmOffServesTheSameRowsWithHonestZeros) {
    if (!std::filesystem::exists(f16_config())) {
        GTEST_SKIP() << "f16.json fixture not generated";
    }
    std::string err;
    auto host = EngineSessionHost::create(make_opts(false), &err);
    ASSERT_NE(host, nullptr) << "create failed: " << err;

    FiledObserver obs;
    obs.attach(host->engine());

    host->set_paused(false);
    for (int frame = 0; frame < 20; ++frame) {
        host->step(60);
    }
    auto& session = host->engine();

    const auto json = run_query(*host, "\"taskforces\"");
    // The wire's rows stand (the save's naval truth is not the arm's).
    EXPECT_NE(json.find("\"id_num\":4040"), std::string::npos);
    EXPECT_NE(json.find("\"id_num\":4624"), std::string::npos);
    // The books: the honest zeros — the arm was off, nothing filed.
    EXPECT_EQ(filings_sum(json), 0);

    // No mission_filed event ever carried a task-force target.
    const auto tfs = session_taskforces(session);
    for (const auto& r : obs.rows) {
        if (r.mission == 35) {
            EXPECT_EQ(tfs.count(r.target), 0u)
                << "the disarmed run targeted a task force";
        }
    }
}

// ── 4. The determinism ───────────────────────────────────────────────────

TEST(CampaignNavalSession, TwoArmedRunsAnswerTheQueryIdentically) {
    if (!std::filesystem::exists(f16_config())) {
        GTEST_SKIP() << "f16.json fixture not generated";
    }
    auto run = [&]() {
        std::string err;
        auto host = EngineSessionHost::create(make_opts(true), &err);
        EXPECT_NE(host, nullptr) << "create failed: " << err;
        host->set_paused(false);
        for (int frame = 0; frame < 20; ++frame) {
            host->step(60);
        }
        return run_query(*host, "\"taskforces\"");
    };
    const auto a = run();
    const auto b = run();
    EXPECT_EQ(a, b);
}
