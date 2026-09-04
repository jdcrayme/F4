// f4-simulation/tests/test_register_aircraft.cpp
//
// Simulation::register_aircraft — the late-spawn roster API.
//
// The scenario: a HOST spawns an aircraft into sim.world() AFTER
// initialize() (the campaign-spawner path — a MissionIntent materializes
// mid-run). update_all advances every behavioral component, so the
// late-comer's brain + flight model DO run — but the tick loop's
// per-aircraft sync (FM state -> TransformComponent, the thing every
// renderer and every missile-boresight consumer reads) walks the
// aircraft_entities_ roster, which initialize() filled. An unregistered
// late-comer therefore flies inside the FM while its transform sits at
// the spawn point forever: "materialized but not flying."
//
// register_aircraft(id) adds the entity to the roster. This test pins
// the whole contract:
//   1. a REGISTERED late-comer's transform follows its FM state (the
//      sync covers it, same-tick as its first update);
//   2. an UNREGISTERED twin's FM advances (update_all is global) while
//      its transform stays at spawn — the gap the API exists to close,
//      demonstrated as the control;
//   3. registration is idempotent (duplicate -> false, no double entry);
//   4. unknown/non-FM entities are rejected (false, roster untouched).
//
// The late-comers are built exactly the way a host builds them: entity
// in sim.world() + TransformComponent + FlightModelComponent::init(cfg,
// alt, vt, hdg, inAir, north, east) from the generated F-16 config
// fixture (the FM needs real aero tables).

#include <f4/simulation/simulation.hpp>
#include <f4/simulation/scenario.hpp>
#include <f4/entities/entity.hpp>
#include <f4/flight/flight_model_component.hpp>
#include <f4/data/aircraft_config.hpp>
#include <f4/data/config_loader.hpp>

#include <gtest/gtest.h>

#include <cstdlib>
#include <filesystem>
#include <string>

using namespace f4::simulation;
using f4::entities::EntityHandle;
using f4::entities::EntityId;
using f4::entities::TransformComponent;
using f4::flight::FlightModelComponent;

namespace {

// Locate the generated F-16 aircraft config fixture (same pattern as
// test_combat_integration.cpp). Empty string = not found; the test skips.
std::string f16_config_path() {
    const char* env = std::getenv("F4_GENERATED_FIXTURES_DIR");
    std::string dir = env ? env : "";
#ifdef F4_GENERATED_FIXTURES_DIR
    if (dir.empty()) dir = F4_GENERATED_FIXTURES_DIR;
#endif
    if (dir.empty()) return "";
    const auto path = std::filesystem::path(dir) / "f16.json";
    // generic_string: the path is embedded in scenario JSON documents
    // (anchor_scenario_json) — backslashes would JSON-escape ("\f" =
    // form feed); forward slashes are valid on Windows filesystems too.
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

// One enroute aircraft (the initialize()-spawned anchor of the roster)
// so the sim has a live flight model + a real tick workload.
std::string anchor_scenario_json(const std::string& f16_path) {
    return R"({
  "name": "register_aircraft",
  "theater": "korea",
  "aircraft": [
    { "callsign": "ANCHOR1", "aircraft_config_path": ")" + f16_path + R"(",
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

// A late-spawned aircraft: TransformComponent at (0,0,0) (the renderer's
// view of it before anything syncs) + an FM initialized 200,000 ft north
// at 8,000 ft MSL heading north at 400 ft/s — flying, immediately.
EntityId spawn_late_aircraft(Simulation& sim,
                             const f4::data::AircraftConfig& cfg,
                             double north_ft) {
    auto h = sim.world().create();
    auto& tf = h.add<TransformComponent>();
    tf.position = f4::geo::WorldPosition(0.0, 0.0, 0.0);
    auto& fm = h.add<FlightModelComponent>();
    fm.init(cfg, /*altitude_ft=*/8000.0, /*vt_fps=*/400.0,
            /*heading_rad=*/0.0, /*inAir=*/true,
            /*north_ft=*/north_ft, /*east_ft=*/0.0);
    return h.id();
}

} // namespace

// ── 1. The contract: sync covers registered late-comers only ──────────────
TEST(RegisterAircraft, RegisteredLateComerSyncsUnregisteredTwinDoesNot) {
    f4::data::AircraftConfig cfg;
    if (!load_f16(cfg)) GTEST_SKIP() << "f16.json fixture not generated";

    auto scenario =
        load_scenario_from_string(anchor_scenario_json(f16_config_path()));
    Simulation sim(std::move(scenario), std::filesystem::path("."));
    sim.initialize();
    ASSERT_EQ(sim.aircraft_entities().size(), 1u);

    // Two late-comers, same shape, 100,000 ft apart.
    const EntityId reg = spawn_late_aircraft(sim, cfg, 200000.0);
    const EntityId unreg = spawn_late_aircraft(sim, cfg, 300000.0);
    ASSERT_TRUE(reg.valid());
    ASSERT_TRUE(unreg.valid());

    // Register exactly ONE of them (the API under test).
    EXPECT_TRUE(sim.register_aircraft(reg));
    EXPECT_FALSE(sim.register_aircraft(reg));  // idempotent

    // 5 seconds of sim.
    for (int i = 0; i < 5 * 60; ++i) sim.tick(kDt);

    // Registered: the FM advanced AND the transform follows it (the
    // sync writes ENU: tf.x = east, tf.y = north).
    {
        auto h = EntityHandle(reg, &sim.world());
        auto* fm = h.get<FlightModelComponent>();
        auto* tf = h.get<TransformComponent>();
        ASSERT_NE(fm, nullptr);
        ASSERT_NE(tf, nullptr);
        EXPECT_GT(fm->state().kin.x, 200000.0)
            << "FM never advanced — update_all did not run the late-comer";
        // The transform must sit ON the FM's position (not the spawn
        // origin, not a stale value): |tf.y - kin.x| < 1 ft.
        EXPECT_NEAR(tf->position.y, fm->state().kin.x, 1.0)
            << "transform did not follow the FM state (sync gap)";
        EXPECT_NEAR(tf->position.x, fm->state().kin.y, 1.0);
    }

    // Unregistered twin: the FM advanced (update_all is global) but the
    // transform stayed at the spawn point — the demonstrated gap.
    {
        auto h = EntityHandle(unreg, &sim.world());
        auto* fm = h.get<FlightModelComponent>();
        auto* tf = h.get<TransformComponent>();
        ASSERT_NE(fm, nullptr);
        ASSERT_NE(tf, nullptr);
        EXPECT_GT(fm->state().kin.x, 300000.0);
        EXPECT_NEAR(tf->position.x, 0.0, 0.001);
        EXPECT_NEAR(tf->position.y, 0.0, 0.001);
        EXPECT_NEAR(tf->position.z, 0.0, 0.001);
    }
}

// ── 2. The roster: rejection cases leave it untouched ─────────────────────
TEST(RegisterAircraft, RejectsUnknownAndNonAircraftEntities) {
    f4::data::AircraftConfig cfg;
    if (!load_f16(cfg)) GTEST_SKIP() << "f16.json fixture not generated";

    auto scenario =
        load_scenario_from_string(anchor_scenario_json(f16_config_path()));
    Simulation sim(std::move(scenario), std::filesystem::path("."));
    sim.initialize();
    const auto roster_size = sim.aircraft_entities().size();

    // Default-constructed (invalid) id.
    EXPECT_FALSE(sim.register_aircraft(EntityId{}));

    // A live entity WITHOUT a FlightModelComponent (not an aircraft).
    const EntityId not_aircraft = [&]() {
        auto h = sim.world().create();
        h.add<TransformComponent>();
        return h.id();
    }();
    EXPECT_FALSE(sim.register_aircraft(not_aircraft));

    // Nothing was added by the rejected calls.
    EXPECT_EQ(sim.aircraft_entities().size(), roster_size);
}
