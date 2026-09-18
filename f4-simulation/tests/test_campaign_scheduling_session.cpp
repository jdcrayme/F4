// test_campaign_scheduling_session.cpp — the CAMP-DOM-4 war gates (the
// session).
//
// The engine's mechanics are pinned in f4-campaign's own tests
// (test_atm.cpp — the slide, the release, the overflow, the gate;
// test_campaign_scheduling.cpp — the Campaign's denial books); here we
// pin what the SESSION adds, over the kunsan fixture (its squadrons
// are base-less — the stock-save bridge assigns their airbases, which
// is exactly the shape the scheduling arm meets):
//
//   1. THE SLOTS RIDE — with the arm on, every filed flight's intent
//      carries its SCHEDULED takeoff (the phase-7 snap's output), and
//      the airfield-ops gate arms against the SLOT: the aggregate's
//      head departure is the slot itself (to_depart == takeoff − clock
//      while the flight still holds), not the TOT-derived guess — the
//      FIDELITY_TIERS §7 delivery-latency divergence closes.
//   2. THE AIRFIELDS QUERY — the scheduling face: one row per booked
//      base, the live grid as 64 hex chars, the anchor, the books.
//   3. THE GOLDEN IDENTITY — arm off: the intents carry no slot, the
//      TOT-anchored gate stands (to_depart == tot − 2×window − clock),
//      no denial books, the airfields query answers an empty set.
//   4. THE PARITY — every slot_denied event the stream carries is one
//      the ledger's log holds (the event IS the books' face), and the
//      armed run stays deterministic (two sessions, one ledger).

#include <f4/campaign/api/events.hpp>
#include <f4/campaign/api/protocol.hpp>
#include <f4/campaign/campaign.hpp>
#include <f4/campaign/result_ledger.hpp>
#include <f4/entities/entity.hpp>
#include <f4/simulation/campaign_session.hpp>
#include <f4/simulation/campaign_session_host.hpp>
#include <f4/world/detail/world_state.hpp>

#include <gtest/gtest.h>

#include <algorithm>
#include <filesystem>
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

// The ATM-pipeline rig (the personnel session test's shape) plus the
// stock-save bridge (kunsan's squadrons carry airbase 0 — without the
// bridge nothing bases, nothing slots) and the scheduling arm. The
// campaign cycle shortened to 5 s so a few advance() calls cross it.
CampaignSessionOptions make_opts(bool airbase_scheduling) {
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
    o.airbase_scheduling = airbase_scheduling;
    // Tiered: the synthetic intents register as AGGREGATES (the FID-5
    // lever) — the tiers view is where the gate's to_depart face lives.
    o.fidelity_policy = FidelityPolicy::Tiered;
    return o;
}

// The synthetic flight VU the session registers aggregates under
// (campaign_session.cpp's own kSyntheticVuBase — the find-the-flight
// key the tiers view answers with).
constexpr std::uint32_t kSyntheticVuBase = 0x53590000u;

/// The intents the bus carried (the slot + the TOT each rode on).
struct IntentObserver {
    struct Row {
        std::uint32_t flight_id = 0;
        std::int64_t takeoff = 0;
        std::int64_t tot = 0;
        std::int64_t issued = 0;     // the LADDER's clock at issue
        std::int64_t at_clock = 0;   // the engine clock at issue
    };
    std::vector<Row> rows;
    std::size_t sub = 0;

    void attach(CampaignSession& s) {
        sub = s.sim().bus().subscribe<f4::campaign::MissionIntent>(
            [this, &s](const f4::campaign::MissionIntent& in) {
                rows.push_back(Row{in.flight_id,
                                   static_cast<std::int64_t>(in.takeoff),
                                   static_cast<std::int64_t>(
                                       in.time_on_target),
                                   static_cast<std::int64_t>(in.issued_time),
                                   s.campaign_time()});
            });
    }
};

struct EventObserver {
    int slot_denied = 0;
    void attach(CampaignSession& s) {
        s.sim().bus().subscribe<api::CampaignEvent>(
            [this](const api::CampaignEvent& e) {
                if (e.kind == api::CampaignEvent::Kind::SlotDenied) {
                    ++slot_denied;
                }
            });
    }
};

std::string run_query(EngineSessionHost& host, const char* q) {
    std::string out;
    (void)api::host_handle(host, std::string(R"({"v":1,"op":"query","q":)") +
                                     q + "}",
                           out);
    return out;
}

/// One row's object text from a JSON array response (the encoding is
/// byte-pinned, one flat object with no nested braces).
std::string row_for(const std::string& json, const std::string& key) {
    const auto start = json.find(key);
    if (start == std::string::npos) return {};
    const auto end = json.find("},", start);
    return json.substr(start, end == std::string::npos
                                   ? std::string::npos
                                   : end - start + 1);
}

} // namespace

// ── 1. The slots ride, the gate arms on them ──────────────────────────────

TEST(CampaignSchedulingSession, SlotsRideIntentsAndTheGateArmsOnThem) {
    if (!std::filesystem::exists(f16_config())) {
        GTEST_SKIP() << "f16.json fixture not generated";
    }
    std::string err;
    auto session = CampaignSession::create(make_opts(true), &err);
    ASSERT_NE(session, nullptr) << "create failed: " << err;

    IntentObserver obs;
    obs.attach(*session);

    session->set_paused(false);
    for (int frame = 0; frame < 20; ++frame) {   // ~20 s: 4 cycles
        session->advance(1.0);
    }

    // The engine slotted the filings: the intents carry the scheduled
    // takeoff (the grid's snapped minute).
    ASSERT_FALSE(obs.rows.empty()) << "the war never filed";
    std::size_t slotted = 0;
    for (const auto& r : obs.rows) {
        if (r.takeoff > 0) ++slotted;
    }
    EXPECT_GT(slotted, 0u) << "no intent ever carried a slot";

    // The gate arms on the SLOT: for a flight still holding at its
    // base, to_depart == takeoff − clock exactly (the aggregate's head
    // departure IS the scheduled minute — not TOT − 2×window).
    const std::int64_t clock =
        static_cast<std::int64_t>(session->campaign_time());
    const auto tiers = session->flight_tiers();
    // The engine clock carries the save's base: the session-relative
    // second is campaign_time() − epoch, and the epoch is the constant
    // behind any intent's (engine clock at issue − ladder clock at
    // issue) pair — the two clocks advance together.
    ASSERT_FALSE(obs.rows.empty());
    const std::int64_t epoch =
        obs.rows.front().at_clock - obs.rows.front().issued;
    const std::int64_t sess = clock - epoch;
    int checked = 0;
    for (const auto& r : obs.rows) {
        if (r.takeoff <= 0 || r.takeoff <= sess) continue;
        const std::uint32_t vu =
            kSyntheticVuBase | (r.flight_id & 0xFFFFu);
        const auto it = std::find_if(
            tiers.begin(), tiers.end(),
            [vu](const auto& t) { return t.vu == vu; });
        if (it == tiers.end() || it->to_depart < 0) continue;
        EXPECT_EQ(it->to_depart, r.takeoff - sess)
            << "the gate did not arm on the slot";
        if (++checked >= 3) break;
    }
    EXPECT_GT(checked, 0) << "no slotted flight was still holding";
}

// ── 2. The airfields query ────────────────────────────────────────────────

TEST(CampaignSchedulingSession, AirfieldsQueryServesTheScheduleBooks) {
    if (!std::filesystem::exists(f16_config())) {
        GTEST_SKIP() << "f16.json fixture not generated";
    }
    std::string err;
    auto host = EngineSessionHost::create(make_opts(true), &err);
    ASSERT_NE(host, nullptr) << err;
    host->set_paused(false);
    for (int frame = 0; frame < 20; ++frame) {
        host->step(60);   // one sim second per step call
    }

    const std::string json = run_query(*host, "\"airfields\"");
    ASSERT_NE(json.find("\"status\":\"ok\""), std::string::npos) << json;
    // Rows exist (the filed flights booked slots on their bases) and
    // each carries the 64-hex grid + the books.
    const auto row_at = [](const std::string& s, std::size_t from) {
        const auto start = s.find("{\"vu\":", from);
        if (start == std::string::npos) return std::string{};
        const auto end = s.find("}", start);
        return s.substr(start, end - start + 1);
    };
    std::size_t pos = json.find('[');
    ASSERT_NE(pos, std::string::npos) << json;
    int rows = 0;
    int booked_sum = 0;
    for (std::string row = row_at(json, pos); !row.empty();
         row = row_at(json, pos)) {
        ++rows;
        const auto sk = row.find("\"schedule\":\"");
        ASSERT_NE(sk, std::string::npos) << row;
        const auto send = row.find('"', sk + 12);
        ASSERT_NE(send, std::string::npos) << row;
        EXPECT_EQ(send - (sk + 12), 64u) << row;
        const auto bk = row.find("\"booked\":");
        ASSERT_NE(bk, std::string::npos) << row;
        booked_sum += std::atoi(row.c_str() + bk + 9);
        pos += 2;   // walk forward; the row_at rescan finds the next
    }
    EXPECT_GT(rows, 0) << json;
    EXPECT_GT(booked_sum, 0) << "no slot was ever booked";
}

// ── 3. The golden identity ────────────────────────────────────────────────

TEST(CampaignSchedulingSession, ArmOffKeepsTheTotAnchoredGate) {
    if (!std::filesystem::exists(f16_config())) {
        GTEST_SKIP() << "f16.json fixture not generated";
    }
    std::string err;
    auto session = CampaignSession::create(make_opts(false), &err);
    ASSERT_NE(session, nullptr) << "create failed: " << err;

    IntentObserver obs;
    EventObserver ev;
    obs.attach(*session);
    ev.attach(*session);

    session->set_paused(false);
    for (int frame = 0; frame < 20; ++frame) session->advance(1.0);

    // The gate stays TOT-anchored (the intents may carry the engine's
    // own snap — the session ignores it disarmed), and the books never
    // moved.
    ASSERT_FALSE(obs.rows.empty());
    const std::int64_t clock =
        static_cast<std::int64_t>(session->campaign_time());
    const std::int64_t epoch =
        obs.rows.front().at_clock - obs.rows.front().issued;
    const std::int64_t sess = clock - epoch;
    const auto tiers = session->flight_tiers();
    int checked = 0;
    for (const auto& r : obs.rows) {
        if (r.tot - 1200 <= sess) continue;   // the TOT-anchored depart
        const std::uint32_t vu =
            kSyntheticVuBase | (r.flight_id & 0xFFFFu);
        const auto it = std::find_if(
            tiers.begin(), tiers.end(),
            [vu](const auto& t) { return t.vu == vu; });
        if (it == tiers.end() || it->to_depart < 0) continue;
        EXPECT_EQ(it->to_depart, r.tot - 1200 - sess)
            << "the gate moved off the TOT anchor";
        if (++checked >= 3) break;
    }
    EXPECT_GT(checked, 0);
    EXPECT_TRUE(session->ledger().slot_denial_log().empty());
    EXPECT_EQ(ev.slot_denied, 0);
}

// ── 4. The parity + determinism ───────────────────────────────────────────

TEST(CampaignSchedulingSession, EventsMirrorTheBooksAndTheRunIsStable) {
    if (!std::filesystem::exists(f16_config())) {
        GTEST_SKIP() << "f16.json fixture not generated";
    }
    std::string err;
    auto session = CampaignSession::create(make_opts(true), &err);
    ASSERT_NE(session, nullptr) << "create failed: " << err;

    EventObserver ev;
    ev.attach(*session);

    session->set_paused(false);
    for (int frame = 0; frame < 20; ++frame) session->advance(1.0);

    // The event IS the books' face: the stream carries exactly the
    // ledger's log (the honest parity, whatever the war produced).
    const auto& log = session->ledger().slot_denial_log();
    EXPECT_EQ(ev.slot_denied, static_cast<int>(log.size()));

    // Determinism: a second session, identically driven, lands on the
    // same bytes (the C5 contract, scheduling edition).
    auto b = CampaignSession::create(make_opts(true), &err);
    ASSERT_NE(b, nullptr) << err;
    b->set_paused(false);
    for (int frame = 0; frame < 20; ++frame) b->advance(1.0);
    EXPECT_EQ(session->ledger_json(), b->ledger_json());
}
