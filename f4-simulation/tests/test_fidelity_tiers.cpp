// f4-simulation/tests/test_fidelity_tiers.cpp
//
// FID-1..4 — the tiered session end to end over the crafted world (the
// test_simulation_lifetime rig's shape: two teams at war, an airbase
// objective, a squadron, one FLIGHT with a 4-waypoint route, one
// garrison battalion):
//
//   1. FullFidelity (default) is TODAY's behavior: the flight spawns
//      one aircraft at initialize; no aggregate engine exists.
//   2. Tiered defers the fleet: the world populates, but no aircraft
//      spawn — the flight is an aggregate (tiered(), agg counts).
//   3. Aggregates advance without sim: advancing an hour moves the
//      flight's aggregate position (and its entity's transform mirror)
//      with ZERO aircraft on the roster (no observer, no ops window).
//   4. Force deagg spawns AIRBORNE at the aggregate: the aircraft is
//      in air, at the aggregate's position, with the handoff's fuel.
//   5. Force reagg folds: the aggregate takes the aircraft's state
//      (monotone fuel), the aircraft retires, and a re-deagg spawns a
//      FRESH aircraft (the spawner protection clears on reagg).
//   6. The camera bubble deaggregates and (with cooldown) the dropped
//      bubble reaggregates — the V-3DLIVE rule on the air side.
//   7. The ops window deaggregates a pre-takeoff flight as a GROUND
//      spawn (the ATC path) — the timed-world variant.
//
// Docs/FIDELITY_TIERS_PLAN.md §5 (FID-1/3/4 acceptance, session half).

#include <f4/simulation/campaign_session.hpp>
#include <f4/flight/flight_model_component.hpp>
#include <f4/entities/entity.hpp>
#include <f4/simulation/visual_model_component.hpp>

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
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

// The crafted world: the lifetime rig's shape. The flight's waypoints
// carry NO arrive/depart times → the engine's SPEED mode (cruise 12
// grid/min); the first waypoint is the flight's own position (a
// zero-length start leg — snap), then a real 3-leg route.
constexpr std::int64_t kNow = 38574360;

std::string tier_world_json(std::int64_t takeoff_depart = 0) {
    // takeoff_depart > 0 stamps the first waypoint's depart (the
    // ops-window variant); 0 keeps the time-less SPEED-mode world.
    std::string depart_block;
    if (takeoff_depart > 0) {
        depart_block = ", \"depart\": " + std::to_string(takeoff_depart);
    }
    std::string out = R"({
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
       "x": 390, "y": 455, "z": 0, "owner": 2,
       "mission": 13, "squadron_id": 4281, "package_id": 7029,
       "time_on_target": 43739352,
       "waypoints": [
         {"x": 390, "y": 455, "z": 0,    "action": 1)" +
        depart_block + R"(},
         {"x": 420, "y": 460, "z": 2500, "action": 15},
         {"x": 460, "y": 500, "z": 2500, "action": 17},
         {"x": 390, "y": 455, "z": 0,    "action": 7}
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
    return out;
}

std::filesystem::path make_temp_dir() {
    // Unique per call AND per process (the counter resets in every
    // gtest process, and ctest -jN runs the suite's filters
    // concurrently — two processes writing the same f4_tiers_N world
    // file raced and flaked the suite; the steady-clock tag makes the
    // name collision-free across processes too).
    static std::atomic<unsigned> counter{0};
    const auto dir = std::filesystem::temp_directory_path() /
                     ("f4_tiers_" +
                      std::to_string(counter.fetch_add(1)) + "_" +
                      std::to_string(
                          std::chrono::steady_clock::now()
                              .time_since_epoch()
                              .count()));
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    return dir;
}

struct TierRig {
    std::filesystem::path dir;
    std::filesystem::path world;
    std::unique_ptr<CampaignSession> session;

    static TierRig make(CampaignSessionOptions opts,
                        std::int64_t takeoff_depart = 0) {
        TierRig rig;
        rig.dir = make_temp_dir();
        rig.world = rig.dir / "tier.world.json";
        {
            std::ofstream f(rig.world);
            f << tier_world_json(takeoff_depart);
        }
        opts.world_json = rig.world;
        opts.class_table = class_table_path();
        opts.aircraft_config = f16_config_path();
        opts.mission_profiles = F4_MISSION_PROFILES_JSON;
        // No synthetic spawns in the window the tests advance: the
        // assertions count ONLY the tier machinery's aircraft.
        opts.tasking_cycle_sec = 1000000;
        opts.reinforce_period_sec = 0;
        opts.atm_pipeline = false;
        std::string err;
        rig.session = CampaignSession::create(opts, &err);
        EXPECT_NE(rig.session, nullptr) << err;
        return rig;
    }

    std::uint32_t flight_vu() const { return 5001; }
};

CampaignSessionOptions base_opts() {
    CampaignSessionOptions o;
    o.max_flights = 8;
    // Headless-budget runs (the QC's own shape): the tier tests advance
    // hours of campaign time, so the per-advance tick cap must cover it
    // (the default 240 is the INTERACTIVE cap — 4 sim-seconds).
    o.max_steps_per_advance = 400000;
    return o;
}

} // namespace

// ── 1. FullFidelity is TODAY ────────────────────────────────────────────────

TEST(FidelityTiers, FullFidelitySpawnsAndHasNoEngine) {
    if (!std::filesystem::exists(f16_config_path())) {
        GTEST_SKIP() << "f16.json fixture not generated";
    }
    auto rig = TierRig::make(base_opts());
    ASSERT_NE(rig.session, nullptr);
    EXPECT_FALSE(rig.session->tiered());
    EXPECT_EQ(rig.session->stats().agg_flights, 0);
    // The pre-FID shape: the flight materialized at initialize.
    EXPECT_EQ(rig.session->sim().aircraft_entities().size(), 1u);
}

// ── 2. Tiered defers the fleet ──────────────────────────────────────────────

TEST(FidelityTiers, TieredDefersTheFleet) {
    if (!std::filesystem::exists(f16_config_path())) {
        GTEST_SKIP() << "f16.json fixture not generated";
    }
    auto opts = base_opts();
    opts.fidelity_policy = FidelityPolicy::Tiered;
    auto rig = TierRig::make(opts);
    ASSERT_NE(rig.session, nullptr);
    EXPECT_TRUE(rig.session->tiered());
    // The world populated; the aircraft did NOT.
    EXPECT_EQ(rig.session->sim().aircraft_entities().size(), 0u);
    EXPECT_EQ(rig.session->stats().agg_flights, 1);
    EXPECT_EQ(rig.session->stats().agg_live, 0);

    const auto tiers = rig.session->flight_tiers();
    ASSERT_EQ(tiers.size(), 1u);
    EXPECT_EQ(tiers[0].vu, rig.flight_vu());
    EXPECT_FALSE(tiers[0].live);
    EXPECT_DOUBLE_EQ(tiers[0].x_grid, 390.0);
}

// ── 3. Aggregates advance without sim ───────────────────────────────────────

TEST(FidelityTiers, AggregatesAdvanceWithZeroAircraft) {
    if (!std::filesystem::exists(f16_config_path())) {
        GTEST_SKIP() << "f16.json fixture not generated";
    }
    auto opts = base_opts();
    opts.fidelity_policy = FidelityPolicy::Tiered;
    auto rig = TierRig::make(opts);
    ASSERT_NE(rig.session, nullptr);

    // Ten minutes of campaign time: ~10 aggregate updates — the flight
    // is mid-route eastbound (the world's route is a ROUND TRIP: base →
    // (420,460) → (460,500) → base), with an EMPTY roster.
    rig.session->advance(600.0);
    EXPECT_GE(rig.session->stats().agg_updates, 9);
    EXPECT_EQ(rig.session->stats().live_aircraft, 0);

    const auto tiers = rig.session->flight_tiers();
    ASSERT_EQ(tiers.size(), 1u);
    EXPECT_FALSE(tiers[0].arrived);
    EXPECT_GT(tiers[0].x_grid, 390.0);   // eastbound on the route

    // The mirror: the flight ENTITY moved with the engine.
    const auto it = rig.session->unit_id_map().find(rig.flight_vu());
    ASSERT_NE(it, rig.session->unit_id_map().end());
    EntityHandle h(it->second, &rig.session->sim().world());
    auto* tf = h.get<f4::entities::TransformComponent>();
    ASSERT_NE(tf, nullptr);
    EXPECT_NEAR(tf->position.x / 1024.0, tiers[0].x_grid, 1e-6);
    EXPECT_NEAR(tf->position.y / 1024.0, tiers[0].y_grid, 1e-6);

    // A day later: the round trip is complete — the aggregate is home
    // (back at the save position) and arrived; updates stop costing.
    rig.session->advance(86400.0);
    const auto home = rig.session->flight_tiers();
    ASSERT_EQ(home.size(), 1u);
    EXPECT_TRUE(home[0].arrived);
    EXPECT_NEAR(home[0].x_grid, 390.0, 1e-9);
}

// ── 4. Force deagg: airborne at the aggregate ───────────────────────────────

TEST(FidelityTiers, ForceDeaggSpawnsAirborneAtTheAggregate) {
    if (!std::filesystem::exists(f16_config_path())) {
        GTEST_SKIP() << "f16.json fixture not generated";
    }
    auto opts = base_opts();
    opts.fidelity_policy = FidelityPolicy::Tiered;
    auto rig = TierRig::make(opts);
    ASSERT_NE(rig.session, nullptr);

    rig.session->advance(600.0);   // mid-route on the round trip
    const auto tiers = rig.session->flight_tiers();
    ASSERT_EQ(tiers.size(), 1u);
    const double agg_x = tiers[0].x_grid;
    const float agg_alt = tiers[0].altitude_ft;
    const std::int32_t burnt = tiers[0].fuel_burnt;
    EXPECT_GT(burnt, 0);
    EXPECT_FALSE(tiers[0].arrived);

    rig.session->force_deaggregate_flight(rig.flight_vu());
    EXPECT_EQ(rig.session->stats().agg_live, 1);
    EXPECT_EQ(rig.session->sim().aircraft_entities().size(), 1u);

    EntityHandle h(rig.session->sim().aircraft_entities().front(),
                   &rig.session->sim().world());
    auto* fm = h.get<f4::flight::FlightModelComponent>();
    ASSERT_NE(fm, nullptr);
    EXPECT_TRUE(fm->state().gear.inAir);   // the AIR spawn, not the ramp
    auto* tf = h.get<f4::entities::TransformComponent>();
    ASSERT_NE(tf, nullptr);
    EXPECT_NEAR(tf->position.x / 1024.0, agg_x, 0.5);      // at the aggregate
    EXPECT_NEAR(tf->position.z, static_cast<double>(agg_alt), 1.0);
    // The handoff's fuel: capacity − the aggregate's burnt (strictly
    // below full tanks — f16.json's internalFuel is 7162 lbs — because
    // the aggregate burned for the whole hour).
    EXPECT_GT(fm->fuel_lbs(), 0.0);
    EXPECT_LT(fm->fuel_lbs(), 7162.0);
}

// ── 5. Force reagg folds + a re-deagg spawns fresh ─────────────────────────

TEST(FidelityTiers, ForceReaggFoldsAndRedeaggSpawnsFresh) {
    if (!std::filesystem::exists(f16_config_path())) {
        GTEST_SKIP() << "f16.json fixture not generated";
    }
    auto opts = base_opts();
    opts.fidelity_policy = FidelityPolicy::Tiered;
    auto rig = TierRig::make(opts);
    ASSERT_NE(rig.session, nullptr);

    rig.session->advance(3600.0);
    const auto pre = rig.session->flight_tiers()[0].fuel_burnt;

    rig.session->force_deaggregate_flight(rig.flight_vu());
    ASSERT_EQ(rig.session->stats().agg_live, 1);

    // Let the aircraft FLY a little before folding (the sim advances
    // the materialized aircraft at 60 Hz).
    rig.session->advance(5.0);

    rig.session->force_reaggregate_flight(rig.flight_vu());
    EXPECT_EQ(rig.session->stats().agg_live, 0);
    EXPECT_EQ(rig.session->stats().tier_reaggs, 1);
    EXPECT_EQ(rig.session->sim().aircraft_entities().size(), 0u);

    // The fold: aggregate fuel monotone (≥ the pre-deagg burn — the
    // aircraft burned more in-sim), position inside the theater.
    const auto post = rig.session->flight_tiers()[0];
    EXPECT_FALSE(post.live);
    EXPECT_GE(post.fuel_burnt, pre);
    EXPECT_GT(post.x_grid, 0.0);

    // The re-deagg: the spawner protection cleared with the fold — a
    // FRESH aircraft materializes at the folded position.
    rig.session->force_deaggregate_flight(rig.flight_vu());
    EXPECT_EQ(rig.session->stats().agg_live, 1);
    EXPECT_EQ(rig.session->stats().tier_deaggs, 2);
    ASSERT_EQ(rig.session->sim().aircraft_entities().size(), 1u);
    EntityHandle h(rig.session->sim().aircraft_entities().front(),
                   &rig.session->sim().world());
    auto* tf = h.get<f4::entities::TransformComponent>();
    ASSERT_NE(tf, nullptr);
    EXPECT_NEAR(tf->position.x / 1024.0, post.x_grid, 0.5);
}

// ── 6. The camera bubble ────────────────────────────────────────────────────

TEST(FidelityTiers, CameraBubbleDeaggsAndDroppedBubbleReaggs) {
    if (!std::filesystem::exists(f16_config_path())) {
        GTEST_SKIP() << "f16.json fixture not generated";
    }
    auto opts = base_opts();
    opts.fidelity_policy = FidelityPolicy::Tiered;
    opts.deagg_cooldown_sec = 5.0;   // a short guard the test can cross
    auto rig = TierRig::make(opts);
    ASSERT_NE(rig.session, nullptr);

    rig.session->advance(600.0);   // the flight moved off-base
    const auto tiers = rig.session->flight_tiers();
    ASSERT_EQ(tiers.size(), 1u);

    // Zoom in: the camera bubble deaggregates the flight (immediately —
    // the V-3DLIVE rule; the session is not even advancing).
    const f4::geo::WorldPosition center(tiers[0].x_grid * 1024.0,
                                        tiers[0].y_grid * 1024.0, 0.0);
    rig.session->set_view_bubble(5120.0, center);
    EXPECT_EQ(rig.session->stats().agg_live, 1);
    EXPECT_EQ(rig.session->sim().aircraft_entities().size(), 1u);

    // Zoom back out: the dropped bubble reaggregates on the next tier
    // pass — after the cooldown. (Advances of 1.0 s fire NO whole-second
    // pass — 60 ticks of 1/60 sum to 0.999… — so the first advance
    // crosses 2 whole seconds.)
    rig.session->clear_view_bubble();
    rig.session->advance(2.0);
    EXPECT_EQ(rig.session->stats().agg_live, 1);      // inside the cooldown
    rig.session->advance(10.0);
    EXPECT_EQ(rig.session->stats().agg_live, 0);      // folded
    EXPECT_EQ(rig.session->sim().aircraft_entities().size(), 0u);
}

// ── 7. The ops window: a pre-takeoff GROUND spawn ──────────────────────────

TEST(FidelityTiers, OpsWindowDeaggsGroundSpawnForTakeoff) {
    if (!std::filesystem::exists(f16_config_path())) {
        GTEST_SKIP() << "f16.json fixture not generated";
    }
    auto opts = base_opts();
    opts.fidelity_policy = FidelityPolicy::Tiered;
    // Takeoff in 2 minutes — inside the default 600 s window.
    auto rig = TierRig::make(opts, /*takeoff_depart=*/kNow + 120);
    ASSERT_NE(rig.session, nullptr);

    // The first advance's tier pass fires the ops deagg: a GROUND
    // spawn (the flight has not departed) at its airbase — the ATC
    // path, not the AirSpawnPose. (2 s: 1.0 s fires no whole-second
    // pass — 60 ticks of 1/60 sum to 0.999…)
    rig.session->advance(2.0);
    EXPECT_EQ(rig.session->stats().agg_live, 1);
    ASSERT_EQ(rig.session->sim().aircraft_entities().size(), 1u);
    EntityHandle h(rig.session->sim().aircraft_entities().front(),
                   &rig.session->sim().world());
    auto* fm = h.get<f4::flight::FlightModelComponent>();
    ASSERT_NE(fm, nullptr);
    EXPECT_FALSE(fm->state().gear.inAir);   // on the ramp, waiting for ATC
}
