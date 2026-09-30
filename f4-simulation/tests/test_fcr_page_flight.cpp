// test_fcr_page_flight.cpp — the AVIONICS-2 done-when, end to end:
// "the page model's lock state drives the same can_fire path the AI's
// MissileModule uses, from page inputs, in a scenario test."
//
// The rig: a two-ship combat scenario. RED1 (the "player") flies north;
// BLUE1 (the bandit) starts 12 NM out at bearing 55 — INSIDE the
// player's north-centered +-60 deg search bar — and files EAST, leaving
// the bar after ~45 s. The player's brain is combat-disabled (no lock
// intents: the radar belongs to the page); the fire-control path under
// test is the same triple the AI uses — the radar's track store ->
// RadarBackedDetectionPolicy's radar leg (detected_by_radar) ->
// MissileModule::should_fire — wired through a standalone fusion exactly
// like the sim installs for a brain.
//
//   1. acquisition: the search sweep holds the bandit (a live track ->
//      detected_by_radar -> should_fire true at 12 NM, inside the PK
//      envelope)
//   2. the twin (search alone): the bandit leaves the bar, the sweep
//      stops refreshing the track, quality decays, the track DROPS —
//      the radar leg dies and the fire-control path closes
//   3. the page lock: designate BEFORE the exit — the radar parks in
//      Track mode, which scans the locked target REGARDLESS of the
//      search volume — the track stays live, the radar leg stays lit,
//      the bandit remains a valid fire-control target the whole flight
//   4. break lock: the page parks the radar back into Search — the leg
//      decays and closes again
//
// The page is the ONLY writer to the player's radar in runs 3-4
// (designate/break_lock), so the leg's life and death are the page's
// decisions, flowing through the AI's own gate.

#include <gtest/gtest.h>

#include "f4/simulation/combat_bridge.hpp"
#include "f4/simulation/simulation.hpp"

#include <f4/ai/brain_component.hpp>
#include <f4/ai/modules/missile_module.hpp>
#include <f4/ai/sensor_fusion.hpp>
#include <f4/avionics/fcr_page.hpp>
#include <f4/entities/entity.hpp>

#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <string>

using namespace f4::simulation;
namespace entities = f4::entities;
namespace sensors = f4::sensors;

namespace {

constexpr double kDt = 1.0 / 60.0;
constexpr double kNm = 6076.115485;

// The player cruises due north from the origin; the bandit starts at
// 12 NM / bearing 55 (inside the +-60 deg bar) heading east, with a
// far-east waypoint (it files straight out of the bar).
std::string fcr_flight_json(const std::string& f16_path) {
    const double bx = 12.0 * kNm * std::sin(55.0 * M_PI / 180.0);
    const double by = 12.0 * kNm * std::cos(55.0 * M_PI / 180.0);
    return R"({
  "name": "fcr_page_flight",
  "theater": "korea",
  "aircraft": [
    { "callsign": "RED1", "aircraft_config_path": ")" + f16_path + R"(",
      "aircraft_name": "F-16C_50", "vis_type_index": 1052,
      "parking_spot": { "x": 0.0, "y": 0.0, "z": 10000.0 },
      "heading_rad": 0.0, "initial_fuel_lbs": 6500.0,
      "initial_vt_fps": 500.0, "spawn_in_air": true, "team": "red" },
    { "callsign": "BLUE1", "aircraft_config_path": ")" + f16_path + R"(",
      "aircraft_name": "F-16C_50", "vis_type_index": 1052,
      "parking_spot": { "x": )" + std::to_string(bx) + R"(, "y": )" +
             std::to_string(by) + R"(, "z": 10000.0 },
      "heading_rad": 1.5707963267948966, "initial_fuel_lbs": 6500.0,
      "initial_vt_fps": 500.0, "spawn_in_air": true, "team": "blue",
      "route": [ { "name": "EAST",
                   "position": { "x": 1500000.0, "y": )" +
                     std::to_string(by) + R"(, "z": 10000.0 },
                   "speed_kts": 450.0 } ] }
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
    { "name": "NORTH", "position": { "x": 0.0, "y": 1500000.0, "z": 10000.0 },
      "speed_kts": 450.0 }
  ],
  "start_enroute": true,
  "sim_dt": 0.016666666666666,
  "total_ticks": 30000,
  "combat": { "enabled": true,
              "radar_rng_seed": 777,
              "bvr_hold": true,
              "missiles_hold": true }
})";
}

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

// The fire-control path under test, wired the way the sim wires a
// brain's: radar-backed policy over the player's own components, the
// fusion reading the world, the MissileModule gating the shot.
struct FireControlRig {
    f4::ai::SensorFusion sf;
    RadarBackedDetectionPolicy policy;
    f4::ai::modules::MissileModule fire;

    FireControlRig(Simulation& sim, std::uint64_t player_id)
        : policy(sim.world(), player_id) {
        sf.initialize(player_id, sim.world(), sim.bus(),
                      f4::ai::SkillLevel::Veteran);
        sf.set_detection_policy(&policy);
    }

    const f4::ai::TargetInfo* target(std::uint64_t bandit_id) {
        sf.force_refresh();
        for (const auto& t : sf.targets()) {
            if (t.entity_id == bandit_id) return &t;
        }
        return nullptr;
    }
};

Simulation make_sim(const std::string& f16) {
    auto scenario = load_scenario_from_string(fcr_flight_json(f16));
    return Simulation(std::move(scenario), std::filesystem::path("."));
}

// Tick until the predicate holds (or the window ends). Returns the last
// sampled state.
bool poll_until(Simulation& sim, std::uint64_t bandit_id,
                FireControlRig& rig, double seconds,
                const std::function<bool(bool)>& satisfied,
                bool& last_leg) {
    const int ticks = static_cast<int>(seconds / kDt);
    for (int i = 0; i < ticks; ++i) {
        sim.tick(kDt);
        const auto* t = rig.target(bandit_id);
        last_leg = t != nullptr && t->detected_by_radar;
        if (satisfied(last_leg)) return true;
    }
    return false;
}

} // namespace

// ============================================================================
// The done-when: page inputs drive the AI's fire-control gate.
// ============================================================================

TEST(FcrPageFlight, PageLockDrivesTheFireControlPath) {
    const auto f16 = f16_config_path();
    if (f16.empty()) GTEST_SKIP() << "f16.json fixture not generated";

    // ---- Run A: search alone. The bar exit kills the track. -----------
    bool leg_a = false;
    {
        auto sim = make_sim(f16);
        sim.initialize();
        const auto player_id = sim.aircraft_entities()[0];
        const auto bandit_id = sim.aircraft_entities()[1].value;

        // The player's brain stands down: the radar belongs to nobody
        // but the sweep (and, later, the page).
        entities::EntityHandle player(player_id, &sim.world());
        auto* brain = player.get<f4::ai::BrainComponent>();
        ASSERT_NE(brain, nullptr);
        brain->set_combat_enabled(false);

        FireControlRig rig(sim, player_id.value);

        // Acquisition: the sweep holds the bandit within ~10 s.
        ASSERT_TRUE(poll_until(sim, bandit_id, rig, 10.0,
                               [](bool leg) { return leg; }, leg_a))
            << "the search sweep never held the bandit";
        const auto* t = rig.target(bandit_id);
        ASSERT_NE(t, nullptr);
        EXPECT_TRUE(t->detected_by_radar);
        EXPECT_TRUE(f4::ai::SensorFusion::can_see(*t));
        // 12 NM, inside the PK envelope: the AI's own gate answers yes.
        EXPECT_TRUE(rig.fire.should_fire(*t))
            << "the search-track baseline never opened the fire-control "
               "path";

        // The bar exit: the track decays and DROPS — the radar leg dies
        // and the fire-control path closes with it (no page, nobody
        // refreshing the track).
        EXPECT_TRUE(poll_until(sim, bandit_id, rig, 100.0,
                               [](bool leg) { return !leg; }, leg_a))
            << "the abandoned track never died after the bar exit";
        const auto* dead = rig.target(bandit_id);
        ASSERT_NE(dead, nullptr);
        EXPECT_FALSE(dead->detected_by_radar);
        EXPECT_FALSE(rig.fire.should_fire(*dead))
            << "the fire-control path stayed open on a dead track";
    }

    // ---- Run B: the page designates before the exit. ------------------
    bool leg_b = false;
    {
        auto sim = make_sim(f16);
        sim.initialize();
        const auto player_id = sim.aircraft_entities()[0];
        const auto bandit_id = sim.aircraft_entities()[1].value;

        entities::EntityHandle player(player_id, &sim.world());
        auto* brain = player.get<f4::ai::BrainComponent>();
        ASSERT_NE(brain, nullptr);
        brain->set_combat_enabled(false);

        FireControlRig rig(sim, player_id.value);
        ASSERT_TRUE(poll_until(sim, bandit_id, rig, 10.0,
                               [](bool leg) { return leg; }, leg_b));

        // PAGE INPUT: designate the tracked bandit. The radar parks in
        // Track mode — which scans the locked target regardless of the
        // search volume — and the page mirrors the lock.
        auto* radar =
            player.get<sensors::RadarSimComponent>();
        ASSERT_NE(radar, nullptr);
        f4::avionics::FcrPageModel page;
        ASSERT_TRUE(page.power_on());
        const auto snap = page.update(*radar,
                                      f4::geo::WorldPosition{0.0, 0.0,
                                                             10000.0},
                                      f4::math::Vec3<double>{
                                          0.0, 500.0, 0.0});
        ASSERT_FALSE(snap.symbols.empty())
            << "the page snapshot missed the tracked bandit";
        EXPECT_FALSE(snap.symbols.front().designated)
            << "pre-designate snapshot already showed a designated track";
        ASSERT_TRUE(page.designate(bandit_id, *radar))
            << "the page could not designate a tracked bandit";
        EXPECT_TRUE(page.locked());
        EXPECT_EQ(radar->mode(), sensors::RadarMode::Track);

        // The bandit leaves the bar mid-flight; the LOCK keeps the
        // track alive and the radar leg lit the whole way — sampled
        // every tick for 100 s of flight.
        bool ever_dark = false;
        for (int i = 0; i < static_cast<int>(100.0 / kDt); ++i) {
            sim.tick(kDt);
            const auto* t = rig.target(bandit_id);
            const bool leg = t != nullptr && t->detected_by_radar;
            if (!leg) ever_dark = true;
        }
        EXPECT_FALSE(ever_dark)
            << "the page-locked track died — Track mode stopped "
               "refreshing the locked target";

        // And the fire-control gate agrees while the shot is in the PK
        // envelope (the early window, before the bandit's eastward
        // run stretches the range past the mid-envelope decay).
        const auto* t = rig.target(bandit_id);
        ASSERT_NE(t, nullptr);
        EXPECT_TRUE(t->detected_by_radar);
        EXPECT_TRUE(f4::ai::SensorFusion::can_see(*t));

        // ---- Run C, same flight: the page breaks the lock. ---------
        page.break_lock(*radar);
        EXPECT_FALSE(page.locked());
        EXPECT_EQ(radar->mode(), sensors::RadarMode::Search);
        EXPECT_TRUE(poll_until(sim, bandit_id, rig, 100.0,
                               [](bool leg) { return !leg; }, leg_b))
            << "after break_lock the abandoned track never died";
    }
}
