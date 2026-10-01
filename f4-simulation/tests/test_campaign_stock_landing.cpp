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

int st_live(const std::unique_ptr<CampaignSession>& s) {
    int n = 0;
    for (const auto& f : s->flight_engine()->flights()) {
        if (f.suspended) ++n;
    }
    return n;
}
int st_deaggs(const std::unique_ptr<CampaignSession>& s) {
    return s->stats().tier_deaggs;
}

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

TEST(CampaignStockLanding, LiveWaveFlightFliesMissionAndTouchesDown) {
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

    for (int probe = 0; probe < 10; ++probe) {
        session->advance(60.0);
        const auto& rr = session->flight_engine()->flights();
        const auto& rt0 = session->flight_engine()->routes()[0];
        std::fprintf(stderr,
                     "[wave %3d s] susp %d deaggs %d row0 to_depart %d "
                     "gate %ld\n",
                     (probe + 1) * 60, st_live(session), st_deaggs(session),
                     session->flight_engine()->seconds_to_depart(0),
                     rt0.empty() ? -1L
                                 : static_cast<long>(rt0.front().depart));
    }
    {
        const auto& rr = session->flight_engine()->flights();
        int susp = 0, agg = 0, scrub = 0;
        long first_gate = -1, second_gate = -1;
        for (const auto& f : rr) {
            if (f.suspended) ++susp;
            else if (f.scrubbed) ++scrub;
            else ++agg;
        }
        for (std::size_t i = 0; i + 1 < rr.size() && second_gate < 0; ++i) {
            const auto& rt = session->flight_engine()->routes()[i];
            if (rt.empty()) continue;
            if (first_gate < 0) first_gate = rt.front().depart;
            else if (second_gate < 0) second_gate = rt.front().depart;
        }
        std::fprintf(stderr, "[wave probe] rows %d susp %d agg %d scrub %d "
                             "gate1 %ld gate2 %ld deaggs %d\n",
                     static_cast<int>(rr.size()), susp, agg, scrub,
                     first_gate, second_gate, session->stats().tier_deaggs);
    }
    const auto tiers = session->flight_tiers();
    const auto live_it = std::find_if(tiers.begin(), tiers.end(),
                                      [](const auto& t) { return t.live; });
    ASSERT_NE(live_it, tiers.end())
        << "no live aircraft after the initial wave window";
    const std::uint32_t vu = live_it->vu;

    // Keep the flight OBSERVED for its whole mission: the bubble rides
    // with it (the user's camera selection does the same), so the tier
    // machinery never folds it back to an aggregate. This is the LIVE
    // landing path the user watches in 3D.
    std::string last_state;
    bool touched_down = false;
    for (int i = 0; i < 540; ++i) {   // 90 min of campaign time
        double px = 0.0, py = 0.0;
        bool found = false;
        for (const auto& t : session->flight_tiers()) {
            if (t.vu == vu) {
                px = t.x_grid;
                py = t.y_grid;
                found = true;
                if (i % 30 == 29) {
                    std::fprintf(stderr,
                                 "[tier +%4d s] live %d arr %d dst %d abt %d"
                                 " at %.1f,%.1f\n",
                                 (i + 1) * 10, t.live ? 1 : 0,
                                 t.arrived ? 1 : 0, t.destroyed ? 1 : 0,
                                 t.aborted ? 1 : 0, px, py);
                }
                if (t.live) {
                    // follow only while live
                } else {
                    found = false;
                }
                break;
            }
        }
        if (found) {
            session->set_view_bubble(
                8192.0, f4::geo::WorldPosition(px * 1024.0, py * 1024.0,
                                               0.0));
        }
        session->advance(10.0);
        for (const auto eid : session->sim().aircraft_entities()) {
            f4::entities::EntityHandle h(eid, &session->sim().world());
            const auto* org = h.get<CampaignOriginComponent>();
            if (org == nullptr || org->flight_vu != (vu & 0xFFFFu))
                continue;
            const auto* brain = h.get<f4::ai::BrainComponent>();
            const auto* tfr = h.get<f4::entities::TransformComponent>();
            if (brain != nullptr) {
                const std::string st = brain->landing().state_name();
                if (i % 30 == 29) {
                    std::fprintf(stderr,
                                 "[mission +%4d s] phase %s landing %s "
                                 "alt %.0f at %.1f,%.1f (sample)
",
                                 (i + 1) * 10, brain->phase_name(),
                                 st.c_str(),
                                 tfr ? tfr->position.z : -1.0,
                                 tfr ? tfr->position.x / 1024.0 : -1.0,
                                 tfr ? tfr->position.y / 1024.0 : -1.0);
                }
                if (st != last_state) {
                    std::fprintf(stderr,
                                 "[mission +%4d s] phase %s landing %s "
                                 "alt %.0f at %.1f,%.1f\n",
                                 (i + 1) * 10, brain->phase_name(),
                                 st.c_str(),
                                 tfr ? tfr->position.z : -1.0,
                                 tfr ? tfr->position.x / 1024.0 : -1.0,
                                 tfr ? tfr->position.y / 1024.0 : -1.0);
                    last_state = st;
                }
                if (st == "Rollout" || st == "TaxiIn" || st == "Parked") {
                    touched_down = true;
                }
            }
            break;
        }
        if (touched_down) break;
    }
    EXPECT_TRUE(touched_down)
        << "the observed wave flight never touched down in 90 min (last "
           "landing state: " << last_state << ")";
}
