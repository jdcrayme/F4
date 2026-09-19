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
#include <fstream>
#include <iterator>
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

// ── 5. CAMP-DOM-6 — the task-force movement (the naval sibling) ──────────
//
// The engine's mechanics are pinned in f4-campaign's own tests
// (test_naval_war.cpp — the snapshot, the walk, the arrival, the
// sync's activity gate); here we pin what the SESSION adds over the
// kunsan fixture (the same 2 task forces):
//
//   a. movement armed: the fleet walks (the frigate's 1.4-grid haul
//      ARRIVES — snap exact), the moved rows land in the session's
//      WorldState per update, and the `taskforces` query serves them
//      live (the wire-state rule — the sync IS the serving face);
//   b. movement off: every wire row stays byte-identical (the golden
//      identity, naval edition);
//   c. movement armed moves ONLY task-force rows (the activity gate
//      at session scale — every battalion and squadron row keeps the
//      fixture's own coordinates);
//   d. the save carries the moved rows (the host's save path);
//   e. two movement-armed runs answer the query identically.

namespace {

// The fixture's own rows, for the untouched-row diffs.
struct FixtureRow {
    int x = 0;
    int y = 0;
};

std::vector<FixtureRow> fixture_unit_rows() {
    std::ifstream in(kunsan_world(), std::ios::binary);
    if (!in) return {};
    std::string json((std::istreambuf_iterator<char>(in)),
                     std::istreambuf_iterator<char>());
    f4::world::WorldState ws;
    ws.load_from_string(json);
    std::vector<FixtureRow> rows;
    rows.reserve(ws.units.size());
    for (const auto& u : ws.units) {
        rows.push_back(FixtureRow{u.x, u.y});
    }
    return rows;
}

} // namespace

TEST(CampaignNavalSession, MovementArmedWalksTheFleetAndServesItLive) {
    if (!std::filesystem::exists(f16_config())) {
        GTEST_SKIP() << "f16.json fixture not generated";
    }
    std::string err;
    auto opts = make_opts(false);
    opts.naval_movement = true;   // the DOM-6 arm (the 60 s cadence)
    auto host = EngineSessionHost::create(opts, &err);
    ASSERT_NE(host, nullptr) << "create failed: " << err;

    // 121 s of campaign time: the naval engine fires updates at
    // t=0/60/120 — the frigate's 1.4-grid haul (362 fp at 128 fp per
    // update) SNAPS on the third.
    host->set_paused(false);
    for (int frame = 0; frame < 121; ++frame) {
        host->step(60);
    }
    auto& session = host->engine();

    // The engine ran and the fleet is home-bound.
    ASSERT_NE(session.naval_war(), nullptr);
    EXPECT_GE(session.naval_war()->stats().updates, 3);
    EXPECT_GE(session.naval_war()->stats().arrivals, 1);

    // The WorldState rows moved (the sync IS the serving face):
    int carrier_x = -1, carrier_y = -1, carrier_hdg = -1;
    int frigate_x = -1, frigate_y = -1, frigate_hdg = -1;
    for (const auto& u : session.world_state().units) {
        if (u.unit_class != f4::entities::UnitClass::TaskForce) continue;
        if (u.id_num == 4040) {
            carrier_x = u.x;
            carrier_y = u.y;
            carrier_hdg = u.heading;
        } else if (u.id_num == 4624) {
            frigate_x = u.x;
            frigate_y = u.y;
            frigate_hdg = u.heading;
        }
    }
    // The frigate arrived: snapped exactly onto the wire's own dest.
    EXPECT_EQ(frigate_x, 305);
    EXPECT_EQ(frigate_y, 469);
    // Its heading: the walk's last bearing (45 deg → 32 in the wire's
    // byte convention).
    EXPECT_EQ(frigate_hdg, 32);
    // The carrier walked ~1.2 grid south toward (743,583) — the
    // deterministic 3-update position (sub-grid truncation included).
    EXPECT_EQ(carrier_x, 752);
    EXPECT_EQ(carrier_y, 265);
    // Its heading: SSE (atan2(-10, +319) → −1.8 deg → 255).
    EXPECT_EQ(carrier_hdg, 255);

    // The query serves the moved rows LIVE.
    const auto json = run_query(*host, "\"taskforces\"");
    ASSERT_NE(json.find("\"status\":\"ok\""), std::string::npos) << json;
    EXPECT_NE(json.find("\"x\":305,\"y\":469"), std::string::npos)
        << "the frigate's arrived position is not served: " << json;
    EXPECT_NE(json.find("\"heading\":255"), std::string::npos)
        << "the carrier's movement heading is not served: " << json;
    EXPECT_NE(json.find("\"heading\":32"), std::string::npos);
    // The wire's own destination rode untouched (consumed, never
    // written).
    EXPECT_NE(json.find("\"dest_x\":743"), std::string::npos);
}

TEST(CampaignNavalSession, MovementOffLeavesEveryWireRowUntouched) {
    if (!std::filesystem::exists(f16_config())) {
        GTEST_SKIP() << "f16.json fixture not generated";
    }
    const auto fixture = fixture_unit_rows();
    ASSERT_FALSE(fixture.empty());

    std::string err;
    auto host = EngineSessionHost::create(make_opts(false), &err);
    ASSERT_NE(host, nullptr) << "create failed: " << err;
    ASSERT_EQ(host->engine().naval_war(), nullptr)
        << "the engine constructed with the arm off";

    host->set_paused(false);
    for (int frame = 0; frame < 20; ++frame) {
        host->step(60);
    }

    const auto& ws = host->engine().world_state();
    ASSERT_EQ(ws.units.size(), fixture.size());
    for (std::size_t i = 0; i < ws.units.size(); ++i) {
        EXPECT_EQ(ws.units[i].x, fixture[i].x) << "unit index " << i;
        EXPECT_EQ(ws.units[i].y, fixture[i].y) << "unit index " << i;
    }
}

TEST(CampaignNavalSession, MovementArmedMovesOnlyTaskForceRows) {
    if (!std::filesystem::exists(f16_config())) {
        GTEST_SKIP() << "f16.json fixture not generated";
    }
    const auto fixture = fixture_unit_rows();
    ASSERT_FALSE(fixture.empty());

    std::string err;
    auto opts = make_opts(false);
    opts.naval_movement = true;
    auto host = EngineSessionHost::create(opts, &err);
    ASSERT_NE(host, nullptr) << "create failed: " << err;
    host->set_paused(false);
    for (int frame = 0; frame < 121; ++frame) {
        host->step(60);
    }

    const auto& ws = host->engine().world_state();
    ASSERT_EQ(ws.units.size(), fixture.size());
    for (std::size_t i = 0; i < ws.units.size(); ++i) {
        if (ws.units[i].unit_class == f4::entities::UnitClass::TaskForce) {
            continue;   // the naval face may move
        }
        EXPECT_EQ(ws.units[i].x, fixture[i].x) << "unit index " << i;
        EXPECT_EQ(ws.units[i].y, fixture[i].y) << "unit index " << i;
    }
}

TEST(CampaignNavalSession, MovementArmedSaveCarriesTheMovedRows) {
    if (!std::filesystem::exists(f16_config())) {
        GTEST_SKIP() << "f16.json fixture not generated";
    }
    std::string err;
    auto opts = make_opts(false);
    opts.naval_movement = true;
    auto host = EngineSessionHost::create(opts, &err);
    ASSERT_NE(host, nullptr) << "create failed: " << err;
    host->set_paused(false);
    for (int frame = 0; frame < 121; ++frame) {
        host->step(60);
    }

    const auto path = std::filesystem::temp_directory_path() /
                      "f4_dom6_naval_save.json";
    const auto res = host->save(path.string());
    ASSERT_TRUE(res.ok) << res.detail;

    std::FILE* f = std::fopen(path.string().c_str(), "rb");
    ASSERT_NE(f, nullptr);
    std::string json;
    char buf[65536];
    std::size_t n = 0;
    while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) {
        json.append(buf, n);
    }
    std::fclose(f);
    std::error_code ec;
    std::filesystem::remove(path, ec);

    // The frigate's arrived row: the emitter's own key order puts
    // x/y a few fields after id_num (pretty-printed, "key": value) —
    // the arrived pair must sit in the frigate's own row.
    const auto row_at = json.find("\"id_num\": 4624,");
    ASSERT_NE(row_at, std::string::npos);
    const auto x_at = json.find("\"x\": 305,", row_at);
    ASSERT_NE(x_at, std::string::npos);
    EXPECT_LT(x_at, json.find("\"unit_subtype\"", row_at))
        << "the frigate's arrived x did not land in its own row";
    EXPECT_NE(json.find("\"y\": 469,", x_at), std::string::npos);
}

TEST(CampaignNavalSession, TwoMovementArmedRunsAnswerIdentically) {
    if (!std::filesystem::exists(f16_config())) {
        GTEST_SKIP() << "f16.json fixture not generated";
    }
    auto run = [&]() {
        std::string err;
        auto opts = make_opts(false);
        opts.naval_movement = true;
        auto host = EngineSessionHost::create(opts, &err);
        EXPECT_NE(host, nullptr) << "create failed: " << err;
        host->set_paused(false);
        for (int frame = 0; frame < 25; ++frame) {
            host->step(60);
        }
        return run_query(*host, "\"taskforces\"");
    };
    const auto a = run();
    const auto b = run();
    EXPECT_EQ(a, b);
}
