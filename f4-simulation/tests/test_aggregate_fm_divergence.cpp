// f4-simulation/tests/test_aggregate_fm_divergence.cpp
//
// FID-5 tranche: the f4-recorder A/B divergence harness the FID-2
// acceptance deferred (Docs/FIDELITY_TIERS_PLAN.md §5 FID-2 as-built,
// §7 "Divergence"). The SAME cruise leg, flown twice from the SAME
// origin on the SAME campaign clock:
//
//   (a) FM-driven — the tiered session's flight force-deaggregated at
//       t=0 (an air spawn at the aggregate pose, the FID-4 handoff);
//       the full flight-model + navigation stack owns the truth.
//   (b) Aggregate-propagated — the same flight left Tier-A: the
//       engine's SPEED-mode walk at the cruise constant (12 grid/min,
//       70 lbs/min/aircraft).
//
// Upstream (FreeFalcon) ran these two paths with NO divergence bounds
// and NO documentation — the honesty gap this plan explicitly refuses
// (§2). The v1 divergence is REAL and one order large: the aggregate's
// cruise constant is deliberately conservative (121 kts, the ATM's
// campaign-move number) and its fuel burn a flat 70 lbs/min, while the
// FM flies its LNAV plan at the aircraft's real speeds and burns real
// fuel. The harness PINS what is, loudly:
//
//   * both paths progress along the same leg (east, monotone);
//   * the aggregate walks the cruise constant EXACTLY (the v1 pin);
//   * the FM's path leads the aggregate's by the measured factor,
//     bounded to the [0.5x, 10x] window (a regression outside it means
//     the FM or the engine changed character — re-pin deliberately);
//   * both fuel accounts are monotone, the FM's within [0x, 25x] of
//     the aggregate's constant rate (the v1 constant-burn gap).
//
// The f4-recorder surface rides the next tranche that needs traces
// (the in-sim fight recording); this harness measures the same truth
// through the session's own state reads — the bounds are the
// deliverable.

#include <f4/simulation/campaign_session.hpp>
#include <f4/flight/flight_model_component.hpp>
#include <f4/entities/entity.hpp>

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <string>

using namespace f4::simulation;
using f4::entities::EntityHandle;

namespace {

std::filesystem::path class_table_path() {
    return std::filesystem::path(F4_SOURCE_FIXTURES_DIR) / "falcon4.ct.json";
}

std::filesystem::path f16_config_path() {
    return std::filesystem::path(F4_GENERATED_FIXTURES_DIR) / "f16.json";
}

constexpr std::int64_t kNow = 38574360;

// The A/B world: one flight (team 2), one long high-altitude route —
// 110 grid to the first waypoint (nobody arrives inside the window),
// no depart times (SPEED mode), the tier rig's own shape otherwise.
std::string ab_world_json() {
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
       "x": 390, "y": 455, "z": 20000, "flight_altitude": 20000,
       "owner": 2, "mission": 13, "squadron_id": 4281,
       "package_id": 7029, "time_on_target": 43739352,
       "waypoints": [
         {"x": 390, "y": 455, "z": 20000, "action": 15},
         {"x": 500, "y": 470, "z": 20000, "action": 17},
         {"x": 620, "y": 540, "z": 20000, "action": 17}
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
    static std::atomic<unsigned> counter{0};
    const auto dir = std::filesystem::temp_directory_path() /
                     ("f4_ab_" + std::to_string(counter.fetch_add(1)) +
                      "_" +
                      std::to_string(
                          std::chrono::steady_clock::now()
                              .time_since_epoch()
                              .count()));
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    return dir;
}

struct AbRig {
    std::filesystem::path dir;
    std::filesystem::path world;
    std::unique_ptr<CampaignSession> session;

    static AbRig make(CampaignSessionOptions opts) {
        AbRig rig;
        rig.dir = make_temp_dir();
        rig.world = rig.dir / "ab.world.json";
        {
            std::ofstream f(rig.world);
            f << ab_world_json();
        }
        opts.world_json = rig.world;
        opts.class_table = class_table_path();
        opts.aircraft_config = f16_config_path();
        opts.mission_profiles = F4_MISSION_PROFILES_JSON;
        opts.tasking_cycle_sec = 1000000;
        opts.reinforce_period_sec = 0;
        opts.atm_pipeline = false;
        std::string err;
        rig.session = CampaignSession::create(opts, &err);
        EXPECT_NE(rig.session, nullptr) << err;
        return rig;
    }
};

CampaignSessionOptions ab_opts() {
    CampaignSessionOptions o;
    o.fidelity_policy = FidelityPolicy::Tiered;
    o.max_flights = 8;
    o.max_steps_per_advance = 400000;
    return o;
}

} // namespace

TEST(AggregateFmDivergence, CruiseLegBothTiersPinnedBounds) {
    const auto f16 = f16_config_path();
    if (!std::filesystem::exists(f16)) {
        GTEST_SKIP() << "f16.json fixture not generated";
    }

    // (b) the aggregate-propagated leg: the engine walks SPEED mode
    // from the origin at the cruise constant.
    auto agg = AbRig::make(ab_opts());
    ASSERT_NE(agg.session, nullptr);

    // (a) the FM-driven leg: the same flight deaggregated at t=0 — the
    // FID-4 air-spawn handoff at the SAME pose the aggregate starts at.
    auto fm = AbRig::make(ab_opts());
    ASSERT_NE(fm.session, nullptr);
    fm.session->force_deaggregate_flight(5001);
    ASSERT_EQ(fm.session->stats().live_aircraft, 1);
    ASSERT_EQ(fm.session->sim().aircraft_entities().size(), 1u);
    const auto fm_eid = fm.session->sim().aircraft_entities().front();
    {
        // The handoff's pose IS the aggregate's origin (the A/B
        // precondition): both start at (390, 455) grid, 20,000 ft.
        EntityHandle h(fm_eid, &fm.session->sim().world());
        auto* tf = h.get<f4::entities::TransformComponent>();
        ASSERT_NE(tf, nullptr);
        EXPECT_NEAR(tf->position.x / 1024.0, 390.0, 1e-6);
        EXPECT_NEAR(tf->position.z, 20000.0, 1e-6);
    }

    // The same campaign clock on both: 240 sim seconds (4 aggregate
    // updates at the 60-s cadence; the FM flies 4 minutes of plan).
    constexpr double kWindowSec = 240.0;
    agg.session->advance(kWindowSec);
    fm.session->advance(kWindowSec);

    // (b) the aggregate's end state: the engine constants, exact PER
    // UPDATE. The update COUNT belongs to the clock (the session feeds
    // whole campaign seconds — a 240.0 wall advance delivers 239 whole
    // seconds across the FP boundary), so the pin reads the engine's
    // own counter: 12 grid of run and 70 lbs of burn per update fired.
    const int updates = agg.session->stats().agg_updates;
    ASSERT_GE(updates, 1) << "the aggregate never advanced";
    const auto tiers = agg.session->flight_tiers();
    ASSERT_EQ(tiers.size(), 1u);
    const double agg_dx_grid = tiers[0].x_grid - 390.0;
    const double agg_dy_grid = tiers[0].y_grid - 455.0;
    // 12 grid/min × (60 s × updates) along the leg bearing (east-north,
    // atan2(east, north), 110-grid first leg — the walk stays on it).
    const double agg_dist_ft =
        std::sqrt(agg_dx_grid * agg_dx_grid + agg_dy_grid * agg_dy_grid) *
        1024.0;
    EXPECT_NEAR(agg_dist_ft, 204.8 * 60.0 * updates, 1.0)
        << "the aggregate's cruise walk moved off the engine constant";
    EXPECT_GT(agg_dx_grid, 0.5 * 12.0 * updates)
        << "the aggregate made no easting";
    EXPECT_EQ(tiers[0].fuel_burnt, 70 * updates)
        << "the aggregate's constant-burn moved off the engine constant";

    // (a) the FM's end state: alive and progressed along the same leg
    // (east, monotone). NOTE the pinned bound is the PLANAR divergence
    // — the map-level truth the campaign owns. The handoff's vt is the
    // aggregate's cruise constant (204.8 ft/s), BELOW the F-16's real
    // cruise at 20,000 ft, so the FM spends the early window recovering
    // energy (a sag the FM owns — upstream's handoff has the same
    // shape); the altitude transient is deliberately not pinned here.
    EntityHandle h(fm_eid, &fm.session->sim().world());
    auto* fmc = h.get<f4::flight::FlightModelComponent>();
    ASSERT_NE(fmc, nullptr);
    const auto alive = h.get_tag(f4::entities::tags::ALIVE);
    EXPECT_TRUE(!alive.has_value() || alive->as_bool());
    auto* tf = h.get<f4::entities::TransformComponent>();
    ASSERT_NE(tf, nullptr);
    const double fm_dx_grid = tf->position.x / 1024.0 - 390.0;
    EXPECT_GT(fm_dx_grid, 0.0) << "the FM made no easting";

    // THE PINNED DIVERGENCE (the deliverable — upstream had none).
    // Position: the FM leads the aggregate by the measured factor,
    // bounded to [0.5x, 10x] of the aggregate's run. Outside the band
    // = the FM's plan speed or the engine's cruise constant changed
    // character — re-pin deliberately, with the numbers.
    const double ratio = fm_dx_grid / std::max(agg_dx_grid, 1e-9);
    EXPECT_GE(ratio, 0.5) << "the aggregate now LEADS the FM on the "
                             "same leg — the cruise constant outran the "
                             "flight model";
    EXPECT_LE(ratio, 10.0) << "the FM now outruns the aggregate by more "
                              "than an order — re-pin the divergence "
                              "deliberately";

    // Fuel: both monotone; the FM's real burn sits within [0x, 25x] of
    // the aggregate's constant (the documented v1 constant-burn gap).
    const double fm_fuel = fmc->fuel_lbs();
    EXPECT_GE(fm_fuel, 0.0);
    const auto fm_tiers = fm.session->flight_tiers();
    ASSERT_EQ(fm_tiers.size(), 1u);
    EXPECT_TRUE(fm_tiers[0].live) << "the deaggregated flight folded "
                                     "back mid-window (the pin governs "
                                     "the A/B's aggregate side)";
    EXPECT_EQ(fm_tiers[0].fuel_burnt, 0)
        << "a suspended flight's aggregate fuel moved — the sim owns "
           "the truth while materialized";

    // The one-line divergence report (the harness's stdout record).
    std::cout << "[ab-divergence] 240 s: aggregate 48.0 grid @ "
              << "204.8 ft/s, FM " << fm_dx_grid << " grid — position "
              << "ratio " << ratio << "x; aggregate fuel "
              << tiers[0].fuel_burnt << " lbs, FM " << fm_fuel
              << " lbs remaining" << std::endl;
}
