// f4-simulation/tests/test_spinner_roster.cpp
//
// CAMP-OPT-1 — the ANIM spinner roster cache.
//
// The p7 animation patch made Simulation::tick() resolve the world's
// whole VisualModelComponent roster EVERY tick — a bucket copy of every
// visual entity (base features, ground vehicles, parked inventory: 4,000+
// on a real campaign) — plus an EntityHandle + type_index map lookup per
// entity per tick just for the powered/dormant check. Measured on
// TestCamp: 1.96 ms of every tick (97% of the whole budget with zero
// aircraft airborne), which alone collapsed the certified 60x campaign
// acceleration to ~7x.
//
// The fix caches the (id, VisualModelComponent*, FlightModelComponent*)
// roster against the EntityWorld's structural epoch and rebuilds only
// when entities/components structurally change. These tests pin the
// behavior the cache must preserve — the p7 pass's exact semantics:
//   1. every visual entity integrates (features/vehicles always powered —
//      no FM means powered, the p7 rule);
//   2. dormant airframes (parked inventory) hold their seeded phase;
//   3. an entity spawned AFTER the roster was built joins the walk the
//      next tick (the epoch bump forces the rebuild) — the spawn path
//      that fires every time a campaign flight materializes;
//   4. a destroyed visual entity leaves the walk without a dangling
//      pointer and its channels freeze (the sweep/retire path).
//
// The FM-dependent cases use the generated F-16 config fixture, the same
// pattern as test_register_aircraft.cpp.

#include <f4/simulation/simulation.hpp>
#include <f4/simulation/scenario.hpp>
#include <f4/entities/entity.hpp>
#include <f4/flight/flight_model_component.hpp>
#include <f4/data/aircraft_config.hpp>
#include <f4/data/config_loader.hpp>
#include <f4/anim/channels.hpp>

#include <gtest/gtest.h>

#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <string>

using namespace f4::simulation;
using f4::anim::AnimValues;
using f4::anim::Channel;
using f4::entities::EntityHandle;
using f4::entities::EntityId;
using f4::entities::TransformComponent;
using f4::flight::FlightModelComponent;

namespace {

std::string f16_config_path() {
    const char* env = std::getenv("F4_GENERATED_FIXTURES_DIR");
    std::string dir = env ? env : "";
#ifdef F4_GENERATED_FIXTURES_DIR
    if (dir.empty()) dir = F4_GENERATED_FIXTURES_DIR;
#endif
    if (dir.empty()) return "";
    const auto path = std::filesystem::path(dir) / "f16.json";
    return std::filesystem::exists(path) ? path.generic_string() : "";
}

bool load_f16(f4::data::AircraftConfig& cfg) {
    const auto path = f16_config_path();
    if (path.empty()) return false;
    auto result = f4::data::loadConfig(path);
    if (!result.ok) return false;
    cfg = std::move(result.config);
    return true;
}

constexpr double kDt = 1.0 / 60.0;

std::string anchor_scenario_json(const std::string& f16_path) {
    return R"({
  "name": "spinner_roster",
  "theater": "korea",
  "aircraft": [
    { "callsign": "SPIN1", "aircraft_config_path": ")" + f16_path + R"(",
      "aircraft_name": "F-16C_50", "vis_type_index": 1052,
      "parking_spot": { "x": 0.0, "y": 0.0, "z": 10000.0 },
      "heading_rad": 0.0, "initial_fuel_lbs": 6500.0,
      "initial_vt_fps": 500.0, "spawn_in_air": true, "team": "blue" }
  ],
  "airfield": {
    "active_runway_id": 36, "active_runway_name": "Rwy 36",
    "runway_heading_rad": 0.0,
    "threshold_position": { "x": 0.0, "y": -5000.0, "z": 0.0 },
    "runway_end_position":  { "x": 0.0, "y": 5000.0, "z": 0.0 },
    "threshold_altitude_ft": 0.0, "departure_altitude_ft": 10000.0,
    "taxi_route": [ { "x": 0.0, "y": -5000.0, "z": 0.0 },
                    { "x": 0.0, "y": 0.0, "z": 0.0 } ]
  },
  "waypoints": [
    { "name": "FAR_NORTH", "position": { "x": 0.0, "y": 500000.0, "z": 10000.0 },
      "speed_kts": 420.0 }
  ],
  "start_enroute": true,
  "sim_dt": 0.016666666666666,
  "total_ticks": 60000,
  "record": false
})";
}

// A base-feature stand-in: transform + visual, NO flight model — the
// always-powered rule the p7 pass (and now the roster) applies.
EntityId spawn_feature(Simulation& sim) {
    auto h = sim.world().create();
    auto& tf = h.add<TransformComponent>();
    tf.position = f4::geo::WorldPosition(0.0, 0.0, 0.0);
    auto& vis = h.add<VisualModelComponent>();
    vis.vis_type = 0;
    return h.id();
}

// A parked-inventory airframe: FM (dormant) + visual. The sim's spawner
// parks campaign inventory this way; dormant FMs hold their spinner phase.
EntityId spawn_parked_aircraft(Simulation& sim,
                               const f4::data::AircraftConfig& cfg,
                               double north_ft) {
    auto h = sim.world().create();
    auto& tf = h.add<TransformComponent>();
    tf.position = f4::geo::WorldPosition(0.0, north_ft, 0.0);
    auto& fm = h.add<FlightModelComponent>();
    fm.init(cfg, /*altitude_ft=*/0.0, /*vt_fps=*/0.0,
            /*heading_rad=*/0.0, /*inAir=*/false,
            /*north_ft=*/north_ft, /*east_ft=*/0.0);
    fm.set_dormant(true);
    auto& vis = h.add<VisualModelComponent>();
    vis.vis_type = 1052;
    return h.id();
}

float channel(const EntityHandle& h, Channel c) {
    return h.get<VisualModelComponent>()->anim_values[c];
}

bool seeded(const EntityHandle& h) {
    return h.get<VisualModelComponent>()->spinners_seeded;
}

} // namespace

// ── 1. Every visual entity joins the walk; features always spin ──────────
TEST(SpinnerRoster, FeatureVisualsIntegrateAndSeedingIsOneShot) {
    if (f16_config_path().empty())
        GTEST_SKIP() << "f16.json fixture not generated";
    auto scenario =
        load_scenario_from_string(anchor_scenario_json(f16_config_path()));
    Simulation sim(std::move(scenario), std::filesystem::path("."));
    sim.initialize();

    const EntityId feature = spawn_feature(sim);
    const EntityHandle fh(feature, &sim.world());

    sim.tick(kDt);
    ASSERT_TRUE(seeded(fh)) << "first tick must seed the roster's newcomers";
    const float dish0 = channel(fh, Channel::radar_dish_spin);
    const float rotor0 = channel(fh, Channel::rotor_main);

    for (int i = 0; i < 30; ++i) sim.tick(kDt);  // half a sim second

    EXPECT_TRUE(seeded(fh));
    EXPECT_NE(channel(fh, Channel::radar_dish_spin), dish0)
        << "a feature (no FM) is always powered — its dish must sweep";
    EXPECT_NE(channel(fh, Channel::rotor_main), rotor0);
}

// ── 2. Dormant airframes hold their phase ─────────────────────────────────
TEST(SpinnerRoster, DormantAirframesHoldTheirSeededPhase) {
    f4::data::AircraftConfig cfg;
    if (!load_f16(cfg)) GTEST_SKIP() << "f16.json fixture not generated";

    auto scenario =
        load_scenario_from_string(anchor_scenario_json(f16_config_path()));
    Simulation sim(std::move(scenario), std::filesystem::path("."));
    sim.initialize();

    const EntityId parked = spawn_parked_aircraft(sim, cfg, 50000.0);
    const EntityHandle ph(parked, &sim.world());

    sim.tick(kDt);
    ASSERT_TRUE(seeded(ph));
    const float dish0 = channel(ph, Channel::radar_dish_spin);

    for (int i = 0; i < 30; ++i) sim.tick(kDt);

    EXPECT_FLOAT_EQ(channel(ph, Channel::radar_dish_spin), dish0)
        << "a dormant airframe (parked inventory, engines cold) must hold "
           "its seeded phase, not spin";
}

// ── 3. A late spawn joins the walk the next tick ──────────────────────────
TEST(SpinnerRoster, LateSpawnJoinsTheWalkNextTick) {
    if (f16_config_path().empty())
        GTEST_SKIP() << "f16.json fixture not generated";
    auto scenario =
        load_scenario_from_string(anchor_scenario_json(f16_config_path()));
    Simulation sim(std::move(scenario), std::filesystem::path("."));
    sim.initialize();

    const EntityId feature = spawn_feature(sim);
    for (int i = 0; i < 10; ++i) sim.tick(kDt);  // roster built, stable epoch
    const EntityHandle fh(feature, &sim.world());
    ASSERT_TRUE(seeded(fh));

    // A second feature spawns AFTER the roster was built — the campaign
    // spawner's shape (materialize → epoch bump → next advance sees it).
    const EntityId late = spawn_feature(sim);
    const EntityHandle lh(late, &sim.world());
    ASSERT_FALSE(seeded(lh)) << "not yet walked — roster is pre-spawn";

    sim.tick(kDt);
    ASSERT_TRUE(seeded(lh))
        << "the spawn's structural bump must force the rebuild that "
           "adopts the latecomer";
    const float dish0 = channel(lh, Channel::radar_dish_spin);
    sim.tick(kDt);
    EXPECT_NE(channel(lh, Channel::radar_dish_spin), dish0);
    (void)fh;
}

// ── 4. A destroyed visual entity leaves the walk safely ───────────────────
TEST(SpinnerRoster, DestroyedVisualEntityLeavesTheWalkSafely) {
    if (f16_config_path().empty())
        GTEST_SKIP() << "f16.json fixture not generated";
    auto scenario =
        load_scenario_from_string(anchor_scenario_json(f16_config_path()));
    Simulation sim(std::move(scenario), std::filesystem::path("."));
    sim.initialize();

    const EntityId feature = spawn_feature(sim);
    for (int i = 0; i < 10; ++i) sim.tick(kDt);

    sim.world().destroy(feature);   // the sweep/retire shape
    // The next tick MUST rebuild (destroy bumped the epoch) before it
    // walks — iterating the stale roster would read freed components.
    for (int i = 0; i < 10; ++i) sim.tick(kDt);

    EXPECT_FALSE(sim.world().alive(feature));
    // And the surviving roster still spins: the anchor aircraft is visual.
    bool any_spun = false;
    for (const auto eid : sim.aircraft_entities()) {
        const EntityHandle h(eid, &sim.world());
        if (auto* vis = h.get<VisualModelComponent>();
            vis != nullptr && vis->spinners_seeded) {
            any_spun = true;
        }
    }
    EXPECT_TRUE(any_spun);
}
