// f4-simulation/tests/test_deagg_takeoff.cpp
//
// DEAGG-RWY regression — the "aircraft taxi to their target in the
// campaign and never take off" report, session-level (spawn → takeoff).
//
// History: a ground-deaggregated campaign flight entered the takeoff
// FSM's Taxi state and crawled along the parking → hold-short route —
// or, before the per-base ATC registration fix, taxied toward the
// theater-origin fallback field and never lined up at all (the bug
// report). DEAGG-RWY replaces the crawl outright: the flight holds
// brakes at parking for a set time (TakeoffModule::runway_wait_s),
// then the module publishes RunwayTeleportRequest and the HOST places
// the airframe on its field's runway threshold, lined up on the runway
// heading — FreeFalcon's imminent-slot placement (a flight whose
// takeoff slot is near is placed directly on the runway via
// ATCBrainClass::FindTakeoffPt, atcbrain.cpp; the reimplementation
// applies that placement after the wait instead of at deagg time).
// From the threshold the normal HoldShort → TakeoffClearance → lineup →
// roll → FlyOut chain flies it off.
//
// This test drives the WHOLE chain over a campaign world with one saved
// flight (the same shape a real save decodes): hold → teleport jump →
// TakeoffState::Done.

#include "f4/simulation/simulation.hpp"

#include <f4/ai/brain_component.hpp>
#include <f4/ai/modules/takeoff_module.hpp>
#include <f4/ai/atc/messages.hpp>
#include <f4/entities/entity.hpp>
#include <f4/flight/flight_model_component.hpp>

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>

using namespace f4::simulation;
using f4::entities::EntityHandle;
using f4::entities::EntityId;

namespace {

std::filesystem::path class_table_path() {
    return std::filesystem::path(F4_SOURCE_FIXTURES_DIR) / "falcon4.ct.json";
}

std::filesystem::path f16_config_path() {
    return std::filesystem::path(F4_GENERATED_FIXTURES_DIR) / "f16.json";
}

// A campaign world shaped like a save (the same JSON shape
// test_simulation_lifetime.cpp crafts): two teams at war, an AIRBASE
// objective with an empty ground layout (the synthetic-field path —
// runway 36, threshold 3500 ft south of the objective center, parking
// row on the ramp), a squadron, and one flight with a saved takeoff
// route. The flight is what the campaign-flights spawn path
// materializes as a GROUND aircraft (the deaggregated-flight shape).
const char* kWorldJson = R"({
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
    "count": 2,
    "decoded": 2,
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
         {"x": 390, "y": 455, "z": 0,    "action": 1},
         {"x": 420, "y": 460, "z": 2500, "action": 15},
         {"x": 460, "y": 500, "z": 2500, "action": 17},
         {"x": 390, "y": 455, "z": 0,    "action": 7}
       ]}
    ]
  }
})";

std::filesystem::path make_temp_dir() {
    // Unique per call (monotonic counter + steady-clock tag) — portable,
    // no getpid dependency.
    static std::atomic<unsigned> counter{0};
    const auto tag =
        std::to_string(counter.fetch_add(1)) + "_" +
        std::to_string(static_cast<std::uintptr_t>(
            std::chrono::steady_clock::now().time_since_epoch().count() %
            1000000));
    auto dir = std::filesystem::temp_directory_path() /
               ("f4_deagg_takeoff_" + tag);
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    return dir;
}

} // namespace

// ── The DEAGG-RWY chain: hold at parking → teleport to the runway →
//    TakeoffState::Done ─────────────────────────────────────────────────
TEST(DeaggTakeoff, GroundFlightWaitsThenTeleportsToRunwayAndTakesOff) {
    if (!std::filesystem::exists(f16_config_path())) {
        GTEST_SKIP() << "f16.json fixture not generated";
    }
    const auto dir = make_temp_dir();

    // 1. The world JSON (absolute — the loader resolves the scenario's
    //    relative paths against the scenario file's directory).
    const auto world = dir / "deagg_takeoff.world.json";
    {
        std::ofstream f(world);
        f << kWorldJson;
        ASSERT_TRUE(f.good());
    }

    // 2. The scenario (campaign_flights; every path absolute —
    //    generic_string so Windows backslashes never JSON-escape).
    const auto scenario_path = dir / "deagg_takeoff.scenario.json";
    {
        std::ofstream f(scenario_path);
        f << "{\n"
          << "  \"name\": \"deagg_takeoff_regression\",\n"
          << "  \"theater\": \"korea\",\n"
          << "  \"spawn_mode\": \"campaign_flights\",\n"
          << "  \"world_json_path\": \"" << world.generic_string() << "\",\n"
          << "  \"class_table_path\": \"" << class_table_path().generic_string()
          << "\",\n"
          << "  \"aircraft\": [{\n"
          << "    \"callsign\": \"DEAGG\",\n"
          << "    \"aircraft_config_path\": \""
          << f16_config_path().generic_string() << "\",\n"
          << "    \"aircraft_name\": \"F-16C_50\",\n"
          << "    \"vis_type_index\": 1052,\n"
          << "    \"parking_spot\": {\"x\": 0.0, \"y\": 0.0, \"z\": 0.0},\n"
          << "    \"heading_rad\": 0.0\n"
          << "  }],\n"
          << "  \"campaign_flight_filter\": {\"team\": -1, \"mission\": -1,"
          << " \"max_flights\": 1},\n"
          << "  \"sim_dt\": 0.016666666666666666,\n"
          << "  \"total_ticks\": 1000000000,\n"
          << "  \"record\": false\n"
          << "}\n";
        ASSERT_TRUE(f.good());
    }

    // 3. The simulation — the flight materializes as a GROUND aircraft
    //    (the deaggregated-flight spawn shape) with the DEAGG-RWY
    //    wait-then-teleport brain.
    Simulation sim(load_scenario(scenario_path), dir);
    ASSERT_NO_THROW(sim.initialize());
    ASSERT_GE(sim.aircraft_entities().size(), std::size_t{1})
        << "the saved flight did not materialize";

    const auto eid = sim.aircraft_entities().front();
    EntityHandle h(eid, &sim.world());
    auto* brain = h.get<f4::ai::BrainComponent>();
    auto* fm = h.get<f4::flight::FlightModelComponent>();
    auto* tf = h.get<f4::entities::TransformComponent>();
    ASSERT_NE(brain, nullptr);
    ASSERT_NE(fm, nullptr);
    ASSERT_NE(tf, nullptr);

    // The DEAGG-RWY mode must be armed by the ground spawn path itself —
    // that IS the feature (no test-side flag flips).
    EXPECT_TRUE(brain->module().wait_then_teleport);

    // Shorten the hold: the production default is 45 s; 2 s keeps this
    // test's tick budget lean while pinning the same mechanism.
    brain->module().runway_wait_s = 2.0;

    constexpr double kDt = 1.0 / 60.0;

    // 4. First tick: the brain initializes (TaxiRequest → TaxiClearance)
    //    and the FSM reaches Taxi; the aircraft sits at its parking spot.
    sim.tick(kDt);
    ASSERT_EQ(brain->module().state(), f4::ai::modules::TakeoffState::Taxi)
        << "expected the FSM in Taxi after the first tick (got "
        << brain->module().state_name() << ")";
    const auto spawn_pos = tf->position;

    // 5. The hold: for the dwell the aircraft must NOT move (brakes at
    //    parking — the taxi route is never flown in this mode).
    for (int i = 0; i < 100; ++i) {   // 1.67 s of the 2.0 s dwell
        sim.tick(kDt);
        ASSERT_EQ(brain->module().state(), f4::ai::modules::TakeoffState::Taxi)
            << "left Taxi after " << i << " hold ticks (wait_s="
            << brain->module().runway_wait_s << ")";
        const double dx = tf->position.x - spawn_pos.x;
        const double dy = tf->position.y - spawn_pos.y;
        ASSERT_LT(std::sqrt(dx * dx + dy * dy), 25.0)
            << "the flight drifted/taxied during its hold at parking";
    }

    // 6. The teleport: within a few ticks of the dwell expiring the FSM
    //    leaves Taxi (RunwayAssigned) and the aircraft appears ON the
    //    runway threshold — a JUMP of thousands of feet (the synthesized
    //    field's threshold sits 3500 ft south of the objective center),
    //    not a gradual crawl.
    bool jumped = false;
    double max_displacement = 0.0;
    for (int i = 0; i < 60 && !jumped; ++i) {
        sim.tick(kDt);
        const double dx = tf->position.x - spawn_pos.x;
        const double dy = tf->position.y - spawn_pos.y;
        max_displacement = std::sqrt(dx * dx + dy * dy);
        if (max_displacement > 1000.0) jumped = true;
    }
    ASSERT_TRUE(jumped)
        << "the flight never teleported to the runway (max displacement "
        << max_displacement << " ft)";

    // 7. Takeoff: the FSM must reach Done — departure altitude reached,
    //    the whole point of the fix. Generous cap (~3.3 sim minutes):
    //    roll + climb in well under two on every prior QC run.
    bool done = false;
    for (int i = 0; i < 12000 && !done; ++i) {
        sim.tick(kDt);
        if (brain->module().is_complete()) done = true;
    }
    EXPECT_TRUE(done) << "final state: " << brain->module().state_name()
                      << " (alt MSL " << tf->position.z << " ft)";
}

// ── The DEFAULT-FIELD follow-up: a scenario_list config over a world,
//    with NO hand-authored airfield, must resolve the ATC's DEFAULT
//    airfield from the world (the derived field nearest the scenario
//    aircraft) — not the empty theater-origin fallback. This is the
//    population the per-base registration fix couldn't reach: aircraft
//    whose TakeoffModule carries airbase_id = 0 (scenario-list spawns
//    never tag a home base). Before the fix their taxi route came back
//    anchored at the origin; now the clearance references the real
//    field the aircraft is parked at. ──────────────────────────────────
TEST(DeaggTakeoff, WorldDerivesTheDefaultAirfieldForScenarioList) {
    if (!std::filesystem::exists(f16_config_path())) {
        GTEST_SKIP() << "f16.json fixture not generated";
    }
    const auto dir = make_temp_dir();

    const auto world = dir / "default_field.world.json";
    {
        std::ofstream f(world);
        f << kWorldJson;
        ASSERT_TRUE(f.good());
    }

    // The scenario is built PROGRAMMATICALLY: the JSON loader demands a
    // hand-authored taxi route (>= 2 waypoints) unless airbase_source is
    // set, and this regression pins exactly the shape that skips both —
    // a world-backed scenario_list config whose default airfield used to
    // stay empty (the theater origin).
    Scenario s;
    s.name = "default_field_regression";
    s.spawn_mode = SpawnMode::ScenarioList;
    s.world_json_path = world;
    s.class_table_path = class_table_path();
    s.sim_dt = 1.0 / 60.0;
    s.total_ticks = 10;

    ScenarioAircraft tpl;
    tpl.callsign = "DEFAULTFIELD";
    tpl.aircraft_config_path = f16_config_path().string();
    tpl.aircraft_name = "F-16C_50";
    tpl.vis_type_index = 1052;
    // Park at the world's airbase: grid (390, 455) — the same constant
    // grid_to_enu applies (1 grid unit = 1024 ft, z already in feet).
    constexpr double kFtPerGrid = 1024.0;
    tpl.parking_spot = f4::geo::WorldPosition(390.0 * kFtPerGrid,
                                              455.0 * kFtPerGrid, 0.0);
    s.aircraft.push_back(tpl);

    // Capture the TaxiClearance the ATC answers the first tick with.
    std::optional<f4::ai::atc::TaxiClearance> clearance;
    Simulation sim(std::move(s), dir);
    sim.bus().subscribe<f4::ai::atc::TaxiClearance>(
        [&clearance](const f4::ai::atc::TaxiClearance& c) {
            if (!clearance.has_value()) clearance = c;
        });

    sim.initialize();
    sim.tick(1.0 / 60.0);

    ASSERT_TRUE(clearance.has_value())
        << "no TaxiClearance arrived — the ATC never answered the request";
    ASSERT_FALSE(clearance->taxi_route.empty())
        << "the default airfield carries no taxi route";

    // The route must live at the aircraft's own base (within 10 NM),
    // not at the theater origin the old fallback anchored.
    constexpr double kFtPerNm = 6076.12;
    const auto& p = clearance->taxi_route.front();
    const double dx = p.x - tpl.parking_spot.x;
    const double dy = p.y - tpl.parking_spot.y;
    const double dist_ft = std::sqrt(dx * dx + dy * dy);
    EXPECT_LT(dist_ft, 10.0 * kFtPerNm)
        << "the default airfield is not the world's airbase (route starts "
        << dist_ft / kFtPerNm << " NM from the aircraft, at ("
        << p.x << ", " << p.y << "))";
}
