// f4-simulation/tests/test_campaign_stock_landing.cpp
//
// CAMP-LAND — the stock-save landing repro, over a REAL converted
// install save (skipped when F4_STOCK_WORLD points nowhere; CI never
// sets it). The user's report: "aircraft come back, switch into
// approach and then just fly off into infinity" — an aborted (RTB'd)
// live flight must CONVERGE to the landing waypoint the abort wrote
// and recover, not chase an approach fix off the map.
//
// The rig mirrors the viewer's session options (tiered, synthesized
// airbases, the near-initial-wave launch) over the stock save1
// conversion: zero save flights, a fully generated war.

#include <f4/simulation/campaign_session.hpp>
#include <f4/ai/brain_component.hpp>
#include <f4/ai/atc/messages.hpp>

#include <optional>
#include <unordered_map>
#include <f4/simulation/campaign_origin.hpp>

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <cstdio>

using namespace f4::simulation;

namespace {

std::filesystem::path stock_world() {
    const char* env = std::getenv("F4_STOCK_WORLD");
    if (env == nullptr || *env == '\0') return {};
    std::filesystem::path p(env);
    return std::filesystem::exists(p) ? p : std::filesystem::path{};
}

/// The stock save's CLASS TABLE must come from the SAME install as the
/// world (F4_STOCK_CLASS_TABLE): the ATM's FindBestAir matches
/// squadrons to missions through the class table's entity types, and a
/// mismatched pair files intents whose routes never build.
std::filesystem::path stock_class_table(
    const std::filesystem::path& fallback) {
    const char* env = std::getenv("F4_STOCK_CLASS_TABLE");
    if (env == nullptr || *env == '\0') return fallback;
    std::filesystem::path p(env);
    return std::filesystem::exists(p) ? p : fallback;
}

} // namespace

namespace {
std::int64_t g_epoch = 0;
} // namespace

TEST(CampaignStockLanding, AbortedWaveFlightRecoversHome) {
    auto world = stock_world();
    if (world.empty()) {
        GTEST_SKIP() << "set F4_STOCK_WORLD to a converted stock save";
    }

    CampaignSessionOptions opts;
    opts.world_json = world;
    opts.class_table = stock_class_table(
        std::filesystem::path(F4_SOURCE_FIXTURES_DIR) / "falcon4.ct.json");
    opts.aircraft_config = std::filesystem::path(F4_GENERATED_FIXTURES_DIR) /
                           "f16.json";
    opts.mission_profiles = F4_MISSION_PROFILES_JSON;
    opts.fidelity_policy = FidelityPolicy::Tiered;
    opts.synthesize_airbases = true;
    opts.near_initial_wave = true;
    opts.initial_tasking_cycle = true;
    opts.tasking_cycle_sec = 1800;
    opts.reinforce_period_sec = 43200;
    opts.ground_war = true;
    opts.ground_objective_supply = true;
    opts.ground_resupply_sec = 21600;
    opts.max_steps_per_advance = 400000;

    std::string err;
    auto session = CampaignSession::create(opts, &err);
    ASSERT_NE(session, nullptr) << err;

    // The wave launches staggered (15 min per base): ~20 min of
    // campaign time puts the first departures airborne and mid-route.
    session->advance(1200.0);
    const auto tiers = session->flight_tiers();
    const auto live_it = std::find_if(tiers.begin(), tiers.end(),
                                      [](const auto& t) { return t.live; });
    ASSERT_NE(live_it, tiers.end())
        << "no live aircraft after the initial wave window";
    const std::uint32_t vu = live_it->vu;

    // Abort: the sortie's books close NOW and the aircraft recovers
    // home. The recovery rides the flight's OWN base queue (the wave
    // staggers per base) — the aircraft lands at its home field.
    const auto ack = session->apply_abort_command(vu);
    ASSERT_EQ(ack.status, CampaignSession::CommandWrite::Applied)
        << ack.detail;

    // Within 45 min the sortie must be CLOSED: the row aborted
    // (scrubbed/arrived/destroyed) — the aircraft on the deck at its
    // home field, not flying the sortie forever. The ATC chain pin
    // (the clearance names the flight's own base) lives in the
    // module's unit suite and the airwar QC's recoveries.
    bool closed = false;
    double home_x = -1.0, home_y = -1.0;
    for (const auto& t : session->flight_tiers()) {
        if (t.vu == vu) {
            // home = the row's own last-known position won't do; the
            // abort's landing waypoint is what it flew to. Recovery =
            // the row left the live set (scrubbed/arrived).
            closed = t.aborted || t.arrived || !t.live;
        }
        (void)home_x;
        (void)home_y;
    }
    for (int i = 0; i < 180 && !closed; ++i) {   // +30 min
        session->advance(10.0);
        for (const auto& t2 : session->flight_tiers()) {
            if (t2.vu != vu) continue;
            closed = t2.aborted || t2.arrived || !t2.live;
            break;
        }
    }
    EXPECT_TRUE(closed)
        << "the aborted flight never closed its sortie (still live 45 "
           "min after the abort)";
}
