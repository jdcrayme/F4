// f4-simulation/tests/test_campaign_airwar_qc.cpp
//
// CAMP-AIRWAR-QC — the generated war at the high level, over a REAL
// converted install save (F4_STOCK_WORLD-gated; CI never sets it).
//
// Are generated flights taking off, delivering at their TOTs, and
// recovering? One 6-hour run over the stock save (the viewer's own
// options), every flight tracked from registration to recovery:
//   * LAUNCH — the first live sample vs the row's takeoff gate;
//   * DELIVERY — the sample the row's cursor passed its route's
//     delivery waypoint vs the row's time-on-target;
//   * RECOVERY — the row folded home (live=false, arrived=true) or
//     its lead landed.
// The assertions are deliberately LOOSE (a measurement, not a unit
// pin): something must take off, deliveries must cluster near their
// TOTs, and recoveries must happen. The REPORT is the deliverable.

#include <f4/simulation/campaign_session.hpp>
#include <f4/ai/brain_component.hpp>
#include <f4/flight/flight_model_component.hpp>
#include <f4/simulation/campaign_origin.hpp>

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <unordered_map>
#include <vector>

using namespace f4::simulation;

namespace {

std::filesystem::path stock_world() {
    const char* env = std::getenv("F4_STOCK_WORLD");
    if (env == nullptr || *env == '\0') return {};
    std::filesystem::path p(env);
    return std::filesystem::exists(p) ? p : std::filesystem::path{};
}

std::filesystem::path stock_class_table(
    const std::filesystem::path& fallback) {
    const char* env = std::getenv("F4_STOCK_CLASS_TABLE");
    if (env == nullptr || *env == '\0') return fallback;
    std::filesystem::path p(env);
    return std::filesystem::exists(p) ? p : fallback;
}

} // namespace

TEST(CampaignStockAirWar, LaunchesDeliveriesAndRecoveries) {
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

    struct Track {
        std::int64_t tot_abs = 0;        // absolute campaign second
        std::int64_t gate_abs = 0;       // first waypoint's depart
        std::int64_t launch_s = -1;      // first materialized sample
        std::int64_t airborne_s = -1;    // first inAir sample (the lead)
        std::int64_t at_target_s = -1;   // cursor passed the delivery wp
        std::int64_t recovered_s = -1;   // folded home / arrived
        std::int64_t destroyed_s = -1;
        std::int64_t aborted_s = -1;
        double target_x = 0.0, target_y = 0.0;
        int delivery_index = -1;
        std::uint8_t mission = 0;
    };
    std::unordered_map<std::uint32_t, Track> tracks;

    constexpr double kAtTargetGrid = 8.0;
    constexpr int kHorizonMin = 360;    // 6 h of campaign time
    constexpr int kSampleSec = 30;

    std::int64_t t = 0;
    for (int sample = 0; sample < (kHorizonMin * 60) / kSampleSec; ++sample) {
        session->advance(static_cast<double>(kSampleSec));
        t += kSampleSec;
        const std::int64_t now = session->campaign_time();

        const auto& rows = session->flight_engine()->flights();
        for (std::size_t i = 0; i < rows.size(); ++i) {
            const auto& f = rows[i];
            auto it = tracks.find(f.vu);
            if (it == tracks.end()) {
                Track tr;
                tr.tot_abs = f.time_on_target;
                tr.gate_abs = session->flight_engine()->routes()[i].empty()
                                  ? 0
                                  : session->flight_engine()
                                        ->routes()[i]
                                        .front()
                                        .depart;
                tr.mission = f.mission;
                const auto& route =
                    session->flight_engine()->routes()[i];
                // The delivery waypoint: the LAST route waypoint with an
                // A-G delivery action (17 STRIKE, 18 BOMB, 14 GNDSTRIKE,
                // 15 NAVSTRIKE, 19 SEAD).
                for (int k = static_cast<int>(route.size()) - 1; k >= 0;
                     --k) {
                    const std::uint8_t a = route[k].action;
                    if (a == 17 || a == 18 || a == 14 || a == 15 ||
                        a == 19) {
                        tr.delivery_index = k;
                        tr.target_x = route[k].x;
                        tr.target_y = route[k].y;
                        break;
                    }
                }
                it = tracks.emplace(f.vu, tr).first;
            }
            Track& tr = it->second;

            if (tr.launch_s < 0 && f.suspended) tr.launch_s = now;
            if (tr.destroyed_s < 0 && f.destroyed) tr.destroyed_s = now;
            if (tr.aborted_s < 0 && f.scrubbed) tr.aborted_s = now;
            if (tr.recovered_s < 0 && f.arrived && !f.suspended) {
                tr.recovered_s = now;
            }
            // The delivery: the row's cursor passed the delivery
            // waypoint (SPEED-mode rows walk wp_index toward the route
            // end) and the row sits within range of the target.
            if (tr.at_target_s < 0 && tr.delivery_index >= 0 &&
                !f.suspended &&
                f.wp_index > static_cast<std::size_t>(tr.delivery_index)) {
                const double dx = f.fx - tr.target_x;
                const double dy = f.fy - tr.target_y;
                if (dx * dx + dy * dy <= kAtTargetGrid * kAtTargetGrid) {
                    tr.at_target_s = now;
                }
            }
        }

        // The LIVE aircraft's airborne flag per flight (the lead's gear).
        for (const auto eid : session->sim().aircraft_entities()) {
            f4::entities::EntityHandle h(eid, &session->sim().world());
            const auto* org = h.get<CampaignOriginComponent>();
            if (org == nullptr || org->flight_vu == 0) continue;
            // The origin carries the intent's RAW flight id; the engine
            // rows key on the synthetic-namespace composite.
            std::uint32_t composite = 0;
            for (const auto& [tvu, tr] : tracks) {
                (void)tr;
                if ((tvu & 0xFFFFu) == org->flight_vu) {
                    composite = tvu;
                    break;
                }
            }
            if (composite == 0) continue;
            auto it = tracks.find(composite);
            const auto* fm = h.get<f4::flight::FlightModelComponent>();
            if (fm != nullptr && fm->state().gear.inAir &&
                it->second.airborne_s < 0) {
                it->second.airborne_s = now;
            }
        }
    }

    // ── the report ─────────────────────────────────────────────────────
    int registered = 0, launched = 0, airborne = 0, recovered = 0,
        destroyed = 0, aborted = 0, with_target = 0, delivered = 0,
        no_show = 0, pending = 0;
    std::vector<double> launch_lateness;    // launch − gate (s)
    std::vector<double> tot_error;          // at_target − TOT (s signed)
    for (const auto& [vu, tr] : tracks) {
        (void)vu;
        ++registered;
        if (tr.launch_s >= 0) {
            ++launched;
            if (tr.gate_abs > 0) {
                launch_lateness.push_back(
                    static_cast<double>(tr.launch_s - tr.gate_abs));
            }
        }
        if (tr.airborne_s >= 0) ++airborne;
        if (tr.recovered_s >= 0) ++recovered;
        if (tr.destroyed_s >= 0) ++destroyed;
        if (tr.aborted_s >= 0) ++aborted;
        if (tr.delivery_index < 0) continue;
        ++with_target;
        if (tr.at_target_s >= 0) {
            ++delivered;
            tot_error.push_back(
                static_cast<double>(tr.at_target_s - tr.tot_abs));
        } else if (tr.tot_abs <= t) {
            ++no_show;   // its TOT passed; never in range
        } else {
            ++pending;   // the horizon ends before its TOT
        }
    }
    auto median = [](std::vector<double> v) -> double {
        if (v.empty()) return -1;
        std::sort(v.begin(), v.end());
        return v[v.size() / 2];
    };
    auto within = [](const std::vector<double>& v, double win) {
        std::size_t n = 0;
        for (const auto& e : v) {
            if (std::abs(e) <= win) ++n;
        }
        return n;
    };

    std::fprintf(stderr,
                 "\n[AIRWAR] horizon %d min\n"
                 "  flights registered %d | launched %d | airborne %d | "
                 "recovered %d | destroyed %d | aborted %d\n"
                 "  delivery missions %d | delivered %d | no-show %d | "
                 "pending %d\n"
                 "  launch vs gate: median %+.0f s (%zu samples)\n"
                 "  delivery vs TOT: median %+.0f s | within +-5 min %d/%zu"
                 " | +-15 min %d/%zu\n\n",
                 kHorizonMin, registered, launched, airborne, recovered,
                 destroyed, aborted, with_target, delivered, no_show,
                 pending, median(launch_lateness), launch_lateness.size(),
                 median(tot_error), within(tot_error, 300),
                 tot_error.size(), within(tot_error, 900),
                 tot_error.size());

    // The loose pins: the war flew, and deliveries cluster near TOT.
    EXPECT_GT(launched, 0) << "nothing ever launched";
    EXPECT_GT(delivered, 0)
        << "no delivery mission ever reached its target";
    ASSERT_FALSE(tot_error.empty());
    EXPECT_LT(std::abs(median(tot_error)), 600.0)
        << "the median delivery missed its TOT by 10+ minutes";
}
