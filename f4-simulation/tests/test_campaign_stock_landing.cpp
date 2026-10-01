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

TEST(CampaignStockLanding, AbortedWaveFlightConvergesAndLands) {
    auto world = stock_world();
    if (world.empty()) {
        GTEST_SKIP() << "set F4_STOCK_WORLD to a converted stock save "
                        "(a .world.json with zero flight entities)";
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
    g_epoch = session->campaign_time();

    // The wave launches inside the first ops window: ~10 min of
    // campaign time puts live aircraft over the map.
    for (int probe = 0; probe < 20; ++probe) {
        session->advance(30.0);
        const auto& st = session->stats();
        const auto& rr = session->flight_engine()->flights();
        int susp = 0, scrubbed = 0, agg = 0;
        for (const auto& f : rr) {
            if (f.suspended) ++susp;
            else if (f.scrubbed) ++scrubbed;
            else ++agg;
        }
        std::fprintf(stderr,
                     "[probe %3d s] rows %d susp %d agg %d scrub %d "
                     "aircraft %d deaggs %d reaggs %d\n",
                     (probe + 1) * 30, static_cast<int>(rr.size()), susp,
                     agg, scrubbed, st.live_aircraft, st.tier_deaggs,
                     st.tier_reaggs);
    }
    {
        const auto& st = session->stats();
        std::fprintf(stderr, "[stock] cycles %d intents %d routes %d/%d "
                             "drawn %d rows %d live %d aircraft %d\n",
                     st.cycles, st.intents, st.routes_built, st.routes_failed,
                     st.drawn_aircraft,
                     static_cast<int>(session->flight_engine()->flights().size()),
                     st.agg_live, st.live_aircraft);
    }
    const auto tiers = session->flight_tiers();
    const auto live_it = std::find_if(tiers.begin(), tiers.end(),
                                      [](const auto& t) { return t.live; });
    ASSERT_NE(live_it, tiers.end())
        << "no live aircraft after the initial wave window";
    const std::uint32_t vu = live_it->vu;

    // CAMP-LAND pin: the ATC must answer a LandingRequest for THIS
    // flight's home base with THAT base's field. Before the fix the
    // campaign never registered its per-base fields with the ATC, and
    // every clearance answered with the stub's empty default (threshold
    // at the theater origin) — the recovering aircraft chased approach
    // data "off into infinity".
    // A COPY per aircraft: the bus message dies at the end of its
    // publish (a stored pointer reads freed memory), and 48 live
    // aircraft means the FIRST clearance is usually someone else's.
    std::unordered_map<std::uint64_t, f4::ai::atc::LandingClearance>
        clearances;
    // The go-around reasons: WHERE the procedure breaks. MASKED by the
    // tracked aircraft (an unmasked capture mixes the whole wave's
    // approaches into one misleading pile).
    std::vector<std::pair<int, std::string>> go_arounds;
    std::uint64_t tracked_eid = 0;   // resolved in Phase A
    auto ga_sub = session->sim().bus().subscribe<
        f4::ai::atc::GoAroundMessage>(
        [&](const f4::ai::atc::GoAroundMessage& m) {
            if (tracked_eid != 0 && m.aircraft_id != tracked_eid) return;
            go_arounds.emplace_back(
                static_cast<int>(session->campaign_time() - g_epoch),
                m.reason);
        });
    // Did the ATC ever grant cleared-to-land for the tracked aircraft?
    int cleared_count = 0;
    auto ctl_sub = session->sim().bus().subscribe<
        f4::ai::atc::ClearedToLand>(
        [&](const f4::ai::atc::ClearedToLand& m) {
            if (tracked_eid != 0 && m.aircraft_id == tracked_eid) {
                ++cleared_count;
            }
        });
    int approach_reqs = 0;
    auto ar_sub = session->sim().bus().subscribe<
        f4::ai::atc::ApproachClearance>(
        [&](const f4::ai::atc::ApproachClearance& m) {
            if (tracked_eid != 0 && m.aircraft_id == tracked_eid) {
                ++approach_reqs;
            }
        });
    auto clear_sub = session->sim().bus().subscribe<
        f4::ai::atc::LandingClearance>(
        [&clearances](const f4::ai::atc::LandingClearance& c) {
            clearances[c.aircraft_id] = c;
        });

    // RTB the flight. The abort writes the aggregate row a
    // [current position -> home landing waypoint] route; the tail is
    // the point the approach must chase.
    const auto ack = session->apply_abort_command(vu);
    ASSERT_EQ(ack.status, CampaignSession::CommandWrite::Applied) << ack.detail;

    std::size_t idx = session->flight_engine()->index_of(vu);
    ASSERT_NE(idx, std::size_t(-1));
    const auto& route = session->flight_engine()->routes()[idx];
    ASSERT_FALSE(route.empty());
    const auto& land = route.back();
    // The landing waypoint is a THEATER point: a (0,0) or continent-
    // scale tail is the unit bug (the aircraft would chase the map
    // corner "into infinity").
    EXPECT_GT(land.x, 1.0);
    EXPECT_GT(land.y, 1.0);
    EXPECT_LT(land.x, 2048.0);
    EXPECT_LT(land.y, 2048.0);

    // Keep the flight OBSERVED (the user's selection anchors the
    // camera bubble on the flight and drags it along): an unobserved
    // flight folds back to an aggregate within its cooldown, and the
    // aggregate path home already works — the bug lives in the LIVE
    // approach. The bubble keeps it live all the way in.
    auto follow_bubble = [&]() {
        for (const auto& t : session->flight_tiers()) {
            if (t.vu == vu && t.live) {
                session->set_view_bubble(
                    8192.0,
                    f4::geo::WorldPosition(t.x_grid * 1024.0,
                                           t.y_grid * 1024.0, 0.0));
                return true;
            }
        }
        return false;
    };

    // The recovery, per shape: a LIVE flight converges to the landing
    // waypoint and lands; a TIERED flight folds back to an aggregate
    // that flies the route home and arrives (the row stays listed,
    // live=false, arrived=true — that IS the recovery).
    auto dist_to_land = [&]() {
        for (const auto& t : session->flight_tiers()) {
            if (t.vu == vu) {
                if (!t.live && !t.destroyed) return -1.0;   // folded home
                const double dx = t.x_grid - land.x;
                const double dy = t.y_grid - land.y;
                return std::sqrt(dx * dx + dy * dy);
            }
        }
        return -1.0;   // row gone — recovered
    };

    // Phase A — fly home: follow with the bubble until the tracked
    // aircraft resolves, never DIVERGING from the landing waypoint.
    std::uint64_t aircraft_id = 0;
    int diverged = 0;
    double prev = -1.0;
    for (int i = 0; i < 240 && aircraft_id == 0; ++i) {
        follow_bubble();
        session->advance(10.0);
        if (prev < 0.0) {
            prev = dist_to_land();
            continue;   // wait for a live sample before pacing counts
        }
        const double d = dist_to_land();
        if (d < 0.0) break;   // folded (bubble dropped) — aggregate path
        if (d > prev + 5.0) ++diverged;
        prev = d;
        for (const auto eid : session->sim().aircraft_entities()) {
            f4::entities::EntityHandle h(eid, &session->sim().world());
            const auto* org = h.get<CampaignOriginComponent>();
            if (org != nullptr && org->flight_vu == (vu & 0xFFFFu)) {
                aircraft_id = eid.value;
                tracked_eid = eid.value;
                break;
            }
        }
    }
    ASSERT_NE(aircraft_id, 0u) << "the tracked aircraft never resolved";
    EXPECT_LE(diverged, 2)
        << "the RTB flight diverged " << diverged
        << " samples (the approach fix chased it off the map)";

    // Its OWN clearance: the ATC answers with the flight's OWN base's
    // field — a theater point near the landing waypoint, not the
    // origin default (the pre-fix flyaway).
    for (int i = 0;
         i < 300 && clearances.find(aircraft_id) == clearances.end(); ++i) {
        follow_bubble();
        session->advance(10.0);
        if (i % 30 == 29) {
            for (const auto eid : session->sim().aircraft_entities()) {
                f4::entities::EntityHandle h(eid, &session->sim().world());
                const auto* org = h.get<CampaignOriginComponent>();
                if (org == nullptr ||
                    org->flight_vu != (vu & 0xFFFFu))
                    continue;
                const auto* brain = h.get<f4::ai::BrainComponent>();
                if (brain != nullptr) {
                    std::fprintf(stderr, "[rtb +%4d s] phase %s landing "
                                         "%s\n",
                                 (i + 1) * 10,
                                 brain->phase_name(),
                                 brain->landing().state_name().c_str());
                }
                break;
            }
        }
    }
    ASSERT_TRUE(clearances.count(aircraft_id))
        << "no landing clearance for the tracked aircraft within its "
           "approach";
    {
        const double cdx =
            clearances[aircraft_id].threshold_position.x / 1024.0 - land.x;
        const double cdy =
            clearances[aircraft_id].threshold_position.y / 1024.0 - land.y;
        EXPECT_LT(std::sqrt(cdx * cdx + cdy * cdy), 25.0)
            << "the landing clearance's threshold is "
            << std::sqrt(cdx * cdx + cdy * cdy)
            << " grid from the flight's landing waypoint — the ATC "
               "answered with the wrong field (the flyaway)";
    }

    // Phase B — the approach FLYS TO TOUCHDOWN: the landing FSM leaves
    // RequestApproach, positions behind the FAF (CAMP-FAF), and the
    // run must reach the runway (Rollout/TaxiIn/Parked). The old shape
    // overflew the field and go-arounded forever.
    std::string last_state;
    int stuck = 0;
    bool touched_down = false;
    for (int i = 0; i < 360; ++i) {   // 60 min of campaign time
        follow_bubble();
        session->advance(10.0);
        for (const auto eid : session->sim().aircraft_entities()) {
            f4::entities::EntityHandle h(eid, &session->sim().world());
            const auto* org = h.get<CampaignOriginComponent>();
            if (org == nullptr || org->flight_vu != (vu & 0xFFFFu))
                continue;
            const auto* brain = h.get<f4::ai::BrainComponent>();
            if (brain == nullptr) break;
            const std::string st = brain->landing().state_name();
            if (st == "RequestApproach") ++stuck;
            if (st == "Rollout" || st == "TaxiIn" || st == "Parked") {
                touched_down = true;
            }
            if (st != last_state) {
                std::fprintf(stderr, "[approach +%4d s] %s\n",
                             (i + 1) * 10, st.c_str());
                last_state = st;
            }
            break;
        }
        if (touched_down) break;
    }
    EXPECT_NE(last_state, "")
        << "the landing FSM never engaged (no Approach phase transition)";
    for (const auto& [t, reason] : go_arounds) {
        std::fprintf(stderr, "[go-around t+%d s] reason: %s\n", t,
                     reason.c_str());
    }
    EXPECT_TRUE(touched_down)
        << "the aircraft never reached the runway (last landing state: "
        << last_state << ")";
    std::fprintf(stderr, "[ATC] approach requests %d cleared-to-land %d "
                         "go-arounds %zu\n",
                 approach_reqs, cleared_count, go_arounds.size());
}

