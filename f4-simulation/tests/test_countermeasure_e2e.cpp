// test_countermeasure_e2e.cpp — the countermeasure tranche's host-layer
// E2E: through the Simulation's own tick, the defending jet DISPENSES
// (the brain's MissileDefeat intents ride execute_brain_combat_intents
// into deploy_countermeasure), the launched missile CARRIES the decoy-
// aware seeker (the intents pass attaches it to every AI release), and
// the IR seeker cards flow from the scenario's ir_seeker_data_path.
//
// Fixture-independent pieces pin the data seams:
//   find_ir_seeker_flare_chance — the card matching rules (exact stem,
//     family bridge aim9*->aim9p, no-card default) against an in-memory
//     library AND against the shipped Data/SimData/irstdata.json;
//   resolve_ir_seeker_data — the empty-path identity + the loud-failure
//     discipline on a configured-but-unloadable path.

#include <gtest/gtest.h>

#include "f4/simulation/combat_bridge.hpp"
#include "f4/simulation/simulation.hpp"
#include "f4/simulation/scenario.hpp"

#include <f4/entities/entity.hpp>
#include <f4/messaging/bus.hpp>
#include <f4/sensors/f4_sensors.hpp>
#include <f4/weapons/countermeasures.hpp>
#include <f4/weapons/f4_weapons.hpp>

#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

using namespace f4::simulation;
namespace entities = f4::entities;
namespace messaging = f4::messaging;
namespace sensors = f4::sensors;
namespace weapons = f4::weapons;

namespace {

constexpr double kDt = 1.0 / 60.0;

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

std::string shipped_irstdata_path() {
#ifdef F4_SOURCE_DATA_DIR
    return std::string(F4_SOURCE_DATA_DIR) + "/SimData/irstdata.json";
#else
    return "";
#endif
}

// The M3 stern chase (test_combat_integration's geometry): EAGLE1 blue
// at the origin, BANDIT1 red ~13.2 NM north, both flying north.
std::string combat_scenario_json(const std::string& f16_path,
                                 const std::string& irst_block = {}) {
    return R"({
  "name": "countermeasure_e2e",
  "theater": "korea",
  "aircraft": [
    { "callsign": "EAGLE1", "aircraft_config_path": ")" + f16_path + R"(",
      "aircraft_name": "F-16C_50", "vis_type_index": 1052,
      "parking_spot": { "x": 0.0, "y": 0.0, "z": 10000.0 },
      "heading_rad": 0.0, "initial_fuel_lbs": 6500.0,
      "initial_vt_fps": 506.0, "spawn_in_air": true, "team": "blue" },
    { "callsign": "BANDIT1", "aircraft_config_path": ")" + f16_path + R"(",
      "aircraft_name": "F-16C_50", "vis_type_index": 1052,
      "parking_spot": { "x": 0.0, "y": 80000.0, "z": 10000.0 },
      "heading_rad": 0.0, "initial_fuel_lbs": 6500.0,
      "initial_vt_fps": 420.0, "spawn_in_air": true, "team": "red" }
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
  "total_ticks": 30000,
  "record": false,
  "combat": { "enabled": true, "radar_rng_seed": 777, "countermeasures": true)" + irst_block + R"( }
})";
}

struct EventLog {
    std::vector<weapons::MissileLaunchedMessage> launched;
    std::vector<weapons::CountermeasureDeployedMessage> deployed;
    std::vector<weapons::EntityKilledMessage> killed;

    void attach(messaging::MessageBus& bus) {
        bus.subscribe<weapons::MissileLaunchedMessage>(
            [this](const weapons::MissileLaunchedMessage& m) {
                launched.push_back(m);
            });
        bus.subscribe<weapons::CountermeasureDeployedMessage>(
            [this](const weapons::CountermeasureDeployedMessage& m) {
                deployed.push_back(m);
            });
        bus.subscribe<weapons::EntityKilledMessage>(
            [this](const weapons::EntityKilledMessage& m) {
                killed.push_back(m);
            });
    }
};

/// An in-memory stand-in for the shipped card library.
f4::data::IrstSensorData small_card_library() {
    f4::data::IrstSensorData lib;
    lib.sensors.push_back({"generic",
                           {120.0, 60.0, 10.0, 1.0, 0.0}});
    lib.sensors.push_back({"aim9p", {60.0, 60.0, 2.0, 0.001, 0.4}});
    lib.sensors.push_back({"sa7", {45.0, 45.0, 2.0, 0.001, 0.5}});
    return lib;
}

} // namespace

// ============================================================================
// The data seams (fixture-independent)
// ============================================================================
TEST(IrSeekerCards, ExactStemAndFamilyBridgesMatch) {
    const auto lib = small_card_library();

    // Family bridge: the WCD's AIM-9M -> the aim9p card (0.4).
    EXPECT_DOUBLE_EQ(find_ir_seeker_flare_chance(&lib, "AIM-9M"), 0.4);
    // Exact stem (normalization strips the dash): SA-7 -> sa7 (0.5).
    EXPECT_DOUBLE_EQ(find_ir_seeker_flare_chance(&lib, "SA-7"), 0.5);
    // No IR card for the radar classes: the default.
    EXPECT_DOUBLE_EQ(find_ir_seeker_flare_chance(&lib, "AIM-120C"),
                     weapons::kDefaultIrFlareChance);
    EXPECT_DOUBLE_EQ(find_ir_seeker_flare_chance(&lib, "M61A1"),
                     weapons::kDefaultIrFlareChance);
    // No library at all: the default.
    EXPECT_DOUBLE_EQ(find_ir_seeker_flare_chance(nullptr, "AIM-9M"),
                     weapons::kDefaultIrFlareChance);
}

TEST(IrSeekerCards, ShippedDataCardDrivesTheChance) {
    const auto path = shipped_irstdata_path();
    if (path.empty() || !std::filesystem::exists(path)) {
        GTEST_SKIP() << "shipped Data/SimData/irstdata.json not found";
    }
    const auto lib = resolve_ir_seeker_data(path);
    EXPECT_EQ(lib.sensors.size(), 8u);   // the shipped card set

    // AIM-9M flies the aim9p card's 0.4 through the family bridge;
    // the radar classes never consult an IR card.
    EXPECT_DOUBLE_EQ(find_ir_seeker_flare_chance(&lib, "AIM-9M"), 0.4);
    EXPECT_DOUBLE_EQ(find_ir_seeker_flare_chance(&lib, "AIM-120C"),
                     weapons::kDefaultIrFlareChance);
}

TEST(IrSeekerCards, EmptyPathIsTheIdentityBadPathIsLoud) {
    EXPECT_TRUE(resolve_ir_seeker_data("").sensors.empty());
    EXPECT_THROW(resolve_ir_seeker_data("/no/such/irstdata.json"),
                 std::runtime_error);
}

// ============================================================================
// The host E2E (fixture-gated like the sibling combat tests)
// ============================================================================
TEST(CountermeasureE2E, VictimDeploysAndAiMissileCarriesSeductionSeeker) {
    const auto f16 = f16_config_path();
    if (f16.empty()) GTEST_SKIP() << "f16.json fixture not generated";

    // The shipped cards flow through the scenario's ir_seeker_data_path
    // (the loader parses the key; resolve_ir_seeker_data would throw on
    // a bad path — the loud-failure discipline).
    const auto irst = shipped_irstdata_path();
    const std::string irst_block =
        irst.empty()
            ? ""
            : ",\n              \"ir_seeker_data_path\": \"" + irst + "\"";

    auto scenario = load_scenario_from_string(
        combat_scenario_json(f16, irst_block));
    Simulation sim(std::move(scenario), std::filesystem::path("."));
    sim.initialize();

    EventLog log;
    log.attach(sim.bus());

    const auto shooter_id = sim.aircraft_entities()[0];
    const auto victim_id = sim.aircraft_entities()[1];
    entities::EntityHandle shooter(shooter_id, &sim.world());
    entities::EntityHandle victim(victim_id, &sim.world());

    // The dispenser rides both armed aircraft.
    ASSERT_NE(shooter.get<weapons::CountermeasureComponent>(), nullptr);
    ASSERT_NE(victim.get<weapons::CountermeasureComponent>(), nullptr);

    // The AI fight, end to end: detection -> track -> the BVR rung's
    // own release -> the victim's RWR launch warning -> its defeat
    // module beams -> the intents pass executes the chaff/flare intents
    // through the dispenser -> the AI-launched missile flies with the
    // countermeasure-aware seeker attached. A fixed 120-s window: long
    // enough for the RWR cadence + the fusion rebuild + the deploy pass
    // to react to the AI's launch, short enough to keep the test fast.
    bool victim_deployed = false;
    bool shooter_deployed = false;
    bool ai_launch_seen = false;
    bool ai_missile_seduction_capable = false;
    for (int i = 0; i < static_cast<int>(120.0 / kDt); ++i) {
        sim.tick(kDt);
        for (const auto& l : log.launched) {
            ai_launch_seen = true;
            const entities::EntityHandle m(
                entities::EntityId{l.missile_id}, &sim.world());
            const auto* mc = m.get<weapons::MissileComponent>();
            if (mc != nullptr && mc->decoy_aware_seeker &&
                mc->seeker_source != nullptr) {
                ai_missile_seduction_capable = true;
            }
        }
        for (const auto& d : log.deployed) {
            if (log.launched.empty()) break;   // deploy counts only under fire
            if (d.owner_id == victim_id.value) victim_deployed = true;
            if (d.owner_id == shooter_id.value) shooter_deployed = true;
        }
    }
    ASSERT_TRUE(ai_launch_seen) << "the AI never fired in 300 s";
    if (!ai_missile_seduction_capable || !victim_deployed) {
        const entities::EntityHandle v(victim_id, &sim.world());
        const auto* vdmg = v.get<entities::DamageStateComponent>();
        fprintf(stderr, "[cm-e2e] launches=%zu deployed=%zu "
                "victim_killed=%d live_missiles=%zu\n",
                log.launched.size(), log.deployed.size(),
                vdmg ? int(vdmg->killed) : -1,
                weapons::count_live_missiles(sim.world()));
    }
    EXPECT_TRUE(ai_missile_seduction_capable)
        << "an AI-launched missile flew without the seduction seeker";

    // The victim (under the missile) dispenses. The shooter (no inbound
    // missile at IT — its own shot does not threaten itself) does not —
    // the defeat intents are threat-driven, not lock-driven.
    EXPECT_TRUE(victim_deployed) << "victim never dispensed under fire";
    EXPECT_FALSE(shooter_deployed);
}
