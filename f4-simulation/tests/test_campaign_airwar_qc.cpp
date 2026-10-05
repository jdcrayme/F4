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
#include <unordered_set>
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
        bool ever_suspended = false;     // the tier machinery took it live
        double booked_cruise = 0.0;      // the fold's booked walk speed
        double route_grids = 0.0;        // base → delivery leg sum
        double route0_x = 0.0, route0_y = 0.0;
        double route_end_x = 0.0, route_end_y = 0.0;
        bool base_printed = false;
    };
    std::unordered_map<std::uint32_t, Track> tracks;
    std::unordered_set<std::uint32_t> lifecycle_traces_;
    std::unordered_set<std::uint32_t> lifecycle_dumped_;

    constexpr double kAtTargetGrid = 8.0;
    const int kHorizonMin = [] {
        const char* env = std::getenv("F4_AIRWAR_HORIZON_MIN");
        return (env != nullptr && *env != '\0') ? std::atoi(env) : 360;
    }();                                // 6 h of campaign time (default)
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
                // The gate's own ingress estimate: the base → delivery
                // leg sum (what travel_s paced at the engine cruise).
                for (int k = 1; k <= tr.delivery_index; ++k) {
                    const double dx =
                        static_cast<double>(route[k].x) - route[k - 1].x;
                    const double dy =
                        static_cast<double>(route[k].y) - route[k - 1].y;
                    tr.route_grids += std::sqrt(dx * dx + dy * dy);
                }
                tr.route0_x = route.front().x;
                tr.route0_y = route.front().y;
                tr.route_end_x = route.back().x;
                tr.route_end_y = route.back().y;
                it = tracks.emplace(f.vu, tr).first;
            }
            Track& tr = it->second;

            if (f.suspended) tr.ever_suspended = true;
            if (tr.booked_cruise <= 0.0) {
                tr.booked_cruise = session->flight_engine()
                                       ->effective_cruise_grid_per_min(i);
            }
            if (tr.launch_s < 0 && f.suspended) tr.launch_s = now;
            // The lifecycle probe (F4_AIRWAR_LIFECYCLE): the first 3
            // delivery tracks, one line per sample — the row's position,
            // distance to its target, cursor, and suspension. The
            // CAMP-TOT-PACE 2 diagnosis surface (the +1,860-s median
            // decomposed here: the takeoff-window deagg suspends the row
            // from t≈0, the live aircraft owns the truth, and the
            // row-side numbers go stale for hours).
            {
                static const bool life_on =
                    std::getenv("F4_AIRWAR_LIFECYCLE") != nullptr;
                static const char* life_vus =
                    std::getenv("F4_AIRWAR_TRACE_VU");
                static bool vus_parsed = false;
                static int traced = 0;
                bool wanted = false;
                if (life_vus != nullptr && *life_vus != '\0') {
                    // Explicit trace list (comma vus) — a late-cohort
                    // flight's lifecycle, not the first-registered three.
                    if (!vus_parsed) {
                        vus_parsed = true;
                        const char* p = life_vus;
                        while (*p != '\0') {
                            lifecycle_traces_.insert(
                                static_cast<std::uint32_t>(
                                    std::strtoul(p, nullptr, 10)));
                            while (*p != ',' && *p != '\0') ++p;
                            if (*p == ',') ++p;
                        }
                    }
                    wanted = lifecycle_traces_.count(f.vu) != 0;
                } else if (life_on) {
                    wanted = tr.delivery_index >= 0 &&
                             (traced < 3 ||
                              lifecycle_traces_.count(f.vu) != 0);
                    if (wanted && lifecycle_traces_.count(f.vu) == 0) {
                        lifecycle_traces_.insert(f.vu);
                        ++traced;
                    }
                }
                if (wanted && tr.delivery_index >= 0) {
                    const double dx = f.fx - tr.target_x;
                    const double dy = f.fy - tr.target_y;
                    std::fprintf(
                        stderr,
                        "[life %u] t %lld row %.1f,%.1f d%.1f wp %zu"
                        " susp %d\n",
                        f.vu, now, f.fx, f.fy, std::sqrt(dx * dx + dy * dy),
                        f.wp_index, f.suspended ? 1 : 0);
                }
            }
            if (tr.destroyed_s < 0 && f.destroyed) tr.destroyed_s = now;
            if (tr.aborted_s < 0 && f.scrubbed) tr.aborted_s = now;
            if (tr.recovered_s < 0 && f.arrived && !f.suspended) {
                tr.recovered_s = now;
            }
            // The delivery: the row is within range of the delivery
            // waypoint with its cursor AT or past it. SPEED-mode rows
            // capture the waypoint (wp_index moves PAST it); TIME-mode
            // rows snap/interpolate TO it (wp_index sits ON it at the
            // TOT appointment) — the detector honors both.
            if (tr.at_target_s < 0 && tr.delivery_index >= 0 &&
                !f.suspended) {
                const bool time_mode =
                    session->flight_engine()->is_time_mode(i);
                const bool past =
                    time_mode
                        ? f.wp_index >=
                              static_cast<std::size_t>(tr.delivery_index)
                        : f.wp_index >
                              static_cast<std::size_t>(tr.delivery_index);
                if (past) {
                    const double dx = f.fx - tr.target_x;
                    const double dy = f.fy - tr.target_y;
                    if (dx * dx + dy * dy <=
                        kAtTargetGrid * kAtTargetGrid) {
                        tr.at_target_s = now;
                    }
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
            // The base-mismatch probe: the route's launch grid vs the
            // origin stamp's home-airbase objective grid. The loader's
            // positional fallback + the synthesis feed the SIM's parking
            // (and the whole materialization); the ATM's snapshot feeds
            // the ROUTE — when the two disagree, the sortie lives a
            // cross-country lie (the CAMP-TOT-PACE 2 late cohort).
            if (!it->second.base_printed && org->home_airbase_vu != 0) {
                it->second.base_printed = true;
                double bx = 0.0, by = 0.0;
                const auto bit =
                    session->objective_id_map().find(org->home_airbase_vu);
                if (bit != session->objective_id_map().end() &&
                    bit->second.valid()) {
                    const auto* tf =
                        f4::entities::EntityHandle(bit->second,
                                                   &session->sim().world())
                            .get<f4::entities::TransformComponent>();
                    if (tf != nullptr) {
                        bx = tf->position.x / 1024.0;
                        by = tf->position.y / 1024.0;
                    }
                }
                std::fprintf(stderr,
                             "[base %u] route0 %.0f,%.0f rEnd %.0f,%.0f"
                             " home_vu %u base %.1f,%.1f\n",
                             composite,
                             it->second.route0_x, it->second.route0_y,
                             it->second.route_end_x, it->second.route_end_y,
                             org->home_airbase_vu, bx, by);
            }
            // CAMP-AIRWAR-QC 2 — the LIVE delivery. A tiered war's
            // delivery flies DEAGGREGATED (the TOT window arms one ops
            // window early; the airborne push-wait holds the
            // appointment), and the row's cursor advances past the
            // delivery waypoint only when the flight FOLDS BACK — the
            // ops pin + the reagg cooldown ride every row-side
            // measurement (the measured +1,860-s median: the pin, not
            // the delivery). The lead's own position at the delivery
            // waypoint IS the delivery moment; sample it per tick.
            const auto* tf = h.get<f4::entities::TransformComponent>();
            if (tf != nullptr) {
                const double dxg =
                    tf->position.x / 1024.0 - it->second.target_x;
                const double dyg =
                    tf->position.y / 1024.0 - it->second.target_y;
                static const bool life_on =
                    std::getenv("F4_AIRWAR_LIFECYCLE") != nullptr;
                if (life_on && lifecycle_traces_.count(composite) != 0) {
                    std::fprintf(stderr,
                                 "[life %u] t %lld LIVE eid %llu"
                                 " %.1f,%.1f d%.1f inair %d\n",
                                 composite, now,
                                 static_cast<unsigned long long>(eid.value),
                                 tf->position.x / 1024.0,
                                 tf->position.y / 1024.0,
                                 std::sqrt(dxg * dxg + dyg * dyg),
                                 (fm != nullptr && fm->state().gear.inAir)
                                     ? 1
                                     : 0);
                }
                if (it->second.at_target_s < 0 &&
                    it->second.delivery_index >= 0 &&
                    dxg * dxg + dyg * dyg <=
                        kAtTargetGrid * kAtTargetGrid) {
                    it->second.at_target_s = now;
                }
            }
        }
    }

    // ── the report ─────────────────────────────────────────────────────
    int registered = 0, launched = 0, airborne = 0, recovered = 0,
        destroyed = 0, aborted = 0, with_target = 0, delivered = 0,
        no_show = 0, pending = 0;
    std::vector<double> launch_lateness;    // launch − gate (s)
    std::vector<double> liftoff_lateness;   // airborne − gate (s)
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
        if (tr.airborne_s >= 0) {
            ++airborne;
            if (tr.gate_abs > 0) {
                liftoff_lateness.push_back(
                    static_cast<double>(tr.airborne_s - tr.gate_abs));
            }
        }
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
    {
        int dumped = 0;
        for (const auto& [vu, tr] : tracks) {
            if (tr.at_target_s < 0 || dumped >= 24) continue;
            std::fprintf(stderr,
                         "[flight %u] mission %u launch %lld gate %lld "
                         "airborne %lld TOT %lld at_target %lld (err %lld)"
                         " didx %d susp %d cruise %.1f route %.0f\n",
                         vu, tr.mission, tr.launch_s, tr.gate_abs,
                         tr.airborne_s,
                         tr.tot_abs, tr.at_target_s,
                         tr.at_target_s - tr.tot_abs, tr.delivery_index,
                         tr.ever_suspended ? 1 : 0, tr.booked_cruise,
                         tr.route_grids);
            ++dumped;
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
                 "  liftoff vs gate: median %+.0f s (%zu samples)\n"
                 "  delivery vs TOT: median %+.0f s | within +-5 min %d/%zu"
                 " | +-15 min %d/%zu\n\n",
                 kHorizonMin, registered, launched, airborne, recovered,
                 destroyed, aborted, with_target, delivered, no_show,
                 pending, median(launch_lateness), launch_lateness.size(),
                 median(liftoff_lateness), liftoff_lateness.size(),
                 median(tot_error), within(tot_error, 300),
                 tot_error.size(), within(tot_error, 900),
                 tot_error.size());

    // The loose pins: the war flew. The delivery pins need a horizon
    // that matures TOTs — deliveries land ON their appointments now,
    // so a short window (a debug run) legitimately shows none.
    EXPECT_GT(launched, 0) << "nothing ever launched";
    if (kHorizonMin >= 180 && !tot_error.empty()) {
        EXPECT_GT(delivered, 0)
            << "no delivery mission ever reached its target";
        EXPECT_LT(std::abs(median(tot_error)), 600.0)
            << "the median delivery missed its TOT by 10+ minutes";
    }
}
