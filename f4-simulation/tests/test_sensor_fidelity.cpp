// test_sensor_fidelity.cpp — the passive-sensor fusion + ECM + ir_power
// tranches at the host layer (SENSORS_COUNTERMEASURES_PLAN §8):
//
//   1. RadarBackedDetectionPolicy's passive optical legs: the ownship's
//      IRST/visual contact books answer the `visual` verdict — a fighter
//      with a dead (or absent) radar still SEES what its passive sensors
//      hold. No passive component attached = the pre-fusion verdict
//      (visual false), byte for byte.
//   2. The attach gates: the passive components and the ECM pod only
//      exist when the scenario turned the fidelity on (the golden
//      identity rule — the same discipline the dispenser flies under;
//      the ECM pod additionally needs the per-aircraft "ecm" fit).
//   3. Throttle-driven ir_power: the FM's last-flown throttle selects
//      the target's IR band; gate off = the Afterburner default stands.
//
// Scenario-level tests build from an in-memory JSON (the
// test_combat_integration pattern): two spawn-in-air fighters in a stern
// chase, the f16.json fixture for the FM tables.

#include <gtest/gtest.h>

#include "f4/simulation/simulation.hpp"
#include "f4/simulation/combat_bridge.hpp"

#include <f4/ai/target_info.hpp>
#include <f4/entities/entity.hpp>
#include <f4/flight/flight_model_component.hpp>
#include <f4/messaging/bus.hpp>
#include <f4/sensors/ecm.hpp>
#include <f4/sensors/irst_component.hpp>
#include <f4/sensors/radar_component.hpp>
#include <f4/sensors/rwr.hpp>
#include <f4/sensors/signature.hpp>
#include <f4/sensors/visual_component.hpp>

#include <filesystem>
#include <string>
#include <vector>

namespace entities = f4::entities;
namespace sensors_ns = f4::sensors;
namespace messaging = f4::messaging;
using f4::simulation::RadarBackedDetectionPolicy;
using f4::simulation::load_scenario_from_string;
using f4::simulation::Simulation;

namespace {

constexpr double kDt = 1.0 / 60.0;
constexpr double kFeetPerNm = 6076.11548;

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

/// Two spawn-in-air fighters in a stern chase (the combat_integration
/// geometry): EAGLE1 blue at the origin, BANDIT1 red ~13.2 NM north.
/// `combat_extra` is spliced into the combat block ("" = defaults);
/// `eagle_ecm_fit` adds the per-aircraft "ecm": true fit to EAGLE1.
std::string scenario_json(const std::string& f16_path,
                          const std::string& combat_extra,
                          bool eagle_ecm_fit = false) {
    const std::string fit = eagle_ecm_fit ? R"(, "ecm": true)" : "";
    const std::string combat_seg = combat_extra.empty()
        ? ""
        : ", " + combat_extra;
    return R"({
  "name": "sensor_fidelity",
  "theater": "korea",
  "aircraft": [
    { "callsign": "EAGLE1", "aircraft_config_path": ")" + f16_path + R"(",
      "aircraft_name": "F-16C_50", "vis_type_index": 1052,
      "parking_spot": { "x": 0.0, "y": 0.0, "z": 10000.0 },
      "heading_rad": 0.0, "initial_fuel_lbs": 6500.0,
      "initial_vt_fps": 506.0, "spawn_in_air": true, "team": "blue" )" +
           fit + R"(},
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
  "combat": { "enabled": true )" + combat_seg + R"(}
})";
}

entities::EntityHandle find_by_callsign(f4::simulation::Simulation& sim,
                                        const char* callsign) {
    for (const auto eid : sim.aircraft_entities()) {
        entities::EntityHandle h(eid, &sim.world());
        if (const auto* id = h.get<entities::CampaignIdentityComponent>();
            id != nullptr && id->callsign == callsign) {
            return h;
        }
    }
    return {};
}

} // namespace

// ============================================================================
// 1. The policy's passive optical legs (direct world, no Simulation)
// ============================================================================

namespace {

struct PolicyWorld {
    entities::EntityWorld world;
    messaging::MessageBus bus;
    entities::EntityHandle ownship;
    entities::EntityHandle target;

    /// Ownship at the origin (blue), target 5 NM north flying south —
    /// inside the radar's knee, the IRST's 10 NM card, and the eyeball's
    /// ~10 NM threshold. `with_radar` / `with_passive` compose the
    /// ownship's sensor fit.
    explicit PolicyWorld(bool with_radar, bool with_passive) {
        ownship = world.create();
        ownship.add<entities::TransformComponent>()
               .position = f4::geo::WorldPosition{0.0, 0.0, 20000.0};
        ownship.set_tag(entities::tags::TEAM,
                        entities::TagValue::from(std::string("blue")));
        if (with_radar) {
            auto& r = ownship.add<sensors_ns::RadarSimComponent>();
            r.rng_seed = 0x46344ull;
            r.own_team = "blue";
        }
        if (with_passive) {
            auto& irst = ownship.add<sensors_ns::IrstComponent>();
            irst.rng_seed = 0x49525354ull;
            irst.own_team = "blue";
            ownship.add<sensors_ns::VisualComponent>().own_team = "blue";
        }

        target = world.create();
        auto& tf = target.add<entities::TransformComponent>();
        tf.position =
            f4::geo::WorldPosition{0.0, 5.0 * kFeetPerNm, 20000.0};
        tf.vy = -400.0;
        target.set_tag(entities::tags::TEAM,
                       entities::TagValue::from(std::string("red")));
    }

    void run(double seconds, double tick = 0.2) {
        for (double i = 0; i < seconds; i += tick) {
            sensors_ns::RadarSimComponent::set_sim_time(
                sensors_ns::RadarSimComponent::sim_time() + tick);
            sensors_ns::IrstComponent::set_sim_time(
                sensors_ns::IrstComponent::sim_time() + tick);
            sensors_ns::VisualComponent::set_sim_time(
                sensors_ns::VisualComponent::sim_time() + tick);
            world.update_all(tick, bus);
        }
    }

    f4::ai::TargetInfo bandit_info() const {
        f4::ai::TargetInfo t;
        t.entity_id = target.id().value;
        t.range_nm = 5.0;
        return t;
    }
};

} // namespace

TEST(PassiveFusionLegs, DeadRadarStillSeesThroughThePassiveBooks) {
    PolicyWorld s{/*with_radar=*/false, /*with_passive=*/true};
    s.run(2.5);  // both passive scans fire at t=1.0 and t=2.0

    // The contacts exist (the scans saw the bandit); the policy answers
    // visual from them, radar stays false — no radar is mounted.
    const auto* irst =
        s.ownship.get<sensors_ns::IrstComponent>()->find(
            s.target.id().value);
    ASSERT_NE(irst, nullptr) << "IRST scan never saw the bandit";

    RadarBackedDetectionPolicy policy(s.world, s.ownship.id().value);
    policy.prepare_batch();
    const auto v = policy.classify(s.bandit_info());
    EXPECT_FALSE(v.radar);
    EXPECT_TRUE(v.visual)
        << "a fighter with a dead radar still sees its IRST contacts";
    EXPECT_FALSE(v.gci);
}

TEST(PassiveFusionLegs, RadarOnlyPolicyKeepsTheVisualVerdictFalse) {
    PolicyWorld s{/*with_radar=*/true, /*with_passive=*/false};
    s.run(2.5);

    RadarBackedDetectionPolicy policy(s.world, s.ownship.id().value);
    policy.prepare_batch();
    const auto v = policy.classify(s.bandit_info());
    EXPECT_TRUE(v.radar) << "the radar tracked the bandit";
    EXPECT_FALSE(v.visual)
        << "no passive component attached: the pre-fusion verdict stands";
    EXPECT_FALSE(v.gci);
}

TEST(PassiveFusionLegs, PassiveContactsDoNotFabricateARadarTrack) {
    // The passive legs widen WHAT the brain sees, not what its radar
    // holds: the IRST book cannot answer the radar verdict (a radar
    // missile still needs the radar's own track).
    PolicyWorld s{/*with_radar=*/true, /*with_passive=*/true};
    // Zero ticks: no scan of any kind has run — every verdict false.
    RadarBackedDetectionPolicy policy(s.world, s.ownship.id().value);
    policy.prepare_batch();
    const auto v = policy.classify(s.bandit_info());
    EXPECT_FALSE(v.radar);
    EXPECT_FALSE(v.visual);
}

// ============================================================================
// 2. The attach gates (scenario level)
// ============================================================================

TEST(SensorAttachGates, PassiveSensorsAttachOnlyWhenGated) {
    const auto f16 = f16_config_path();
    if (f16.empty()) GTEST_SKIP() << "f16.json fixture not generated";

    {
        auto scenario = load_scenario_from_string(
            scenario_json(f16, "\"passive_sensors\": true"));
        f4::simulation::Simulation sim(std::move(scenario),
                                       std::filesystem::path("."));
        sim.initialize();
        for (const auto eid : sim.aircraft_entities()) {
            entities::EntityHandle h(eid, &sim.world());
            EXPECT_NE(h.get<sensors_ns::IrstComponent>(), nullptr);
            EXPECT_NE(h.get<sensors_ns::VisualComponent>(), nullptr);
        }
    }
    {
        // Gate off (the default): nothing attaches — the pre-fusion
        // component set, byte for byte.
        auto scenario =
            load_scenario_from_string(scenario_json(f16, ""));
        f4::simulation::Simulation sim(std::move(scenario),
                                       std::filesystem::path("."));
        sim.initialize();
        for (const auto eid : sim.aircraft_entities()) {
            entities::EntityHandle h(eid, &sim.world());
            EXPECT_EQ(h.get<sensors_ns::IrstComponent>(), nullptr);
            EXPECT_EQ(h.get<sensors_ns::VisualComponent>(), nullptr);
            EXPECT_EQ(h.get<sensors_ns::EcmComponent>(), nullptr);
        }
    }
}

TEST(SensorAttachGates, EcmPodNeedsTheGateAndTheAircraftFit) {
    const auto f16 = f16_config_path();
    if (f16.empty()) GTEST_SKIP() << "f16.json fixture not generated";

    // The gate alone fits nobody: the pod needs the per-aircraft "ecm"
    // field, which this scenario carries on no aircraft.
    {
        auto scenario = load_scenario_from_string(
            scenario_json(f16, "\"ecm\": true"));
        f4::simulation::Simulation sim(std::move(scenario),
                                       std::filesystem::path("."));
        sim.initialize();
        for (const auto eid : sim.aircraft_entities()) {
            entities::EntityHandle h(eid, &sim.world());
            EXPECT_EQ(h.get<sensors_ns::EcmComponent>(), nullptr);
        }
    }
    {
        // Gate off + the fit declared: still nobody — both must agree.
        auto scenario = load_scenario_from_string(
            scenario_json(f16, "", /*eagle_ecm_fit=*/true));
        f4::simulation::Simulation sim(std::move(scenario),
                                       std::filesystem::path("."));
        sim.initialize();
        for (const auto eid : sim.aircraft_entities()) {
            entities::EntityHandle h(eid, &sim.world());
            EXPECT_EQ(h.get<sensors_ns::EcmComponent>(), nullptr);
        }
    }
    {
        // Gate on + the fit: EAGLE1 carries the pod (own_team blue),
        // BANDIT1 does not.
        auto scenario = load_scenario_from_string(
            scenario_json(f16, "\"ecm\": true", /*eagle_ecm_fit=*/true));
        f4::simulation::Simulation sim(std::move(scenario),
                                       std::filesystem::path("."));
        sim.initialize();
        const auto eagle = find_by_callsign(sim, "EAGLE1");
        const auto bandit = find_by_callsign(sim, "BANDIT1");
        ASSERT_TRUE(eagle.valid());
        ASSERT_TRUE(bandit.valid());
        const auto* pod = eagle.get<sensors_ns::EcmComponent>();
        ASSERT_NE(pod, nullptr);
        EXPECT_EQ(pod->own_team, "blue");
        EXPECT_EQ(bandit.get<sensors_ns::EcmComponent>(), nullptr);
    }
}

// ============================================================================
// 3. Throttle-driven ir_power (scenario level)
// ============================================================================

TEST(ThrottleIrPower, GateOffKeepsTheAfterburnerDefault) {
    const auto f16 = f16_config_path();
    if (f16.empty()) GTEST_SKIP() << "f16.json fixture not generated";

    auto scenario = load_scenario_from_string(scenario_json(f16, ""));
    f4::simulation::Simulation sim(std::move(scenario),
                                   std::filesystem::path("."));
    sim.initialize();
    for (int i = 0; i < 120; ++i) sim.tick(kDt);
    for (const auto eid : sim.aircraft_entities()) {
        entities::EntityHandle h(eid, &sim.world());
        const auto* sig = h.get<sensors_ns::SignatureComponent>();
        ASSERT_NE(sig, nullptr);
        EXPECT_EQ(sig->ir_power, sensors_ns::IrPowerMode::Afterburner)
            << "gate off: the authored default band stands";
    }
}

TEST(ThrottleIrPower, GateOnStampsTheBandTheFlewLastTick) {
    const auto f16 = f16_config_path();
    if (f16.empty()) GTEST_SKIP() << "f16.json fixture not generated";

    auto scenario = load_scenario_from_string(
        scenario_json(f16, "\"throttle_ir_power\": true"));
    f4::simulation::Simulation sim(std::move(scenario),
                                   std::filesystem::path("."));
    sim.initialize();
    for (int i = 0; i < 120; ++i) sim.tick(kDt);

    for (const auto eid : sim.aircraft_entities()) {
        entities::EntityHandle h(eid, &sim.world());
        const auto* sig = h.get<sensors_ns::SignatureComponent>();
        const auto* fm = h.get<f4::flight::FlightModelComponent>();
        ASSERT_NE(sig, nullptr);
        ASSERT_NE(fm, nullptr);
        const double throttle = fm->last_consumed_input().throttle;
        const auto expected = throttle >= 1.05
            ? sensors_ns::IrPowerMode::Max
            : throttle >= 0.6 ? sensors_ns::IrPowerMode::Afterburner
                              : sensors_ns::IrPowerMode::Baseline;
        EXPECT_EQ(sig->ir_power, expected)
            << "throttle " << throttle
            << " must select its band through the documented mapping";
    }
}
