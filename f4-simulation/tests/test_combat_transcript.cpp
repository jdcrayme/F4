// test_combat_transcript.cpp — CombatTranscript unit + E2E tests.
//
// The transcript is the M4 observability piece: it formats bus combat
// transitions as brevity radio calls. Tested here:
//   1. missile_brevity_word: guidance kind -> FOX 1/2/3 (pure function)
//   2. callsign map + "#<hex>" fallback
//   3. ring buffer: clear(), set_capacity() keeps the newest entries,
//      at() bounds
//   4. E2E: the AiVersusAi fight narrated — every link of the chain
//      (contact -> spike -> FOX 3 -> launch warning -> impact -> splash)
//      appears, timestamps non-decreasing, EAGLE1 is the speaker of the
//      launch, BANDIT1 the speaker of the defense calls.

#include <gtest/gtest.h>

#include "f4/simulation/combat_transcript.hpp"
#include "f4/simulation/simulation.hpp"

#include <f4/entities/entity.hpp>
#include <f4/weapons/weapon_types.hpp>

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <string>

using namespace f4::simulation;
namespace weapons = f4::weapons;

namespace {

// Same locator as test_combat_integration.cpp.
std::string f16_config_path() {
    const char* env = std::getenv("F4_GENERATED_FIXTURES_DIR");
    std::string dir = env ? env : "";
#ifdef F4_GENERATED_FIXTURES_DIR
    if (dir.empty()) dir = F4_GENERATED_FIXTURES_DIR;
#endif
    if (dir.empty()) return "";
    const auto path = std::filesystem::path(dir) / "f16.json";
    return std::filesystem::exists(path) ? path.string() : "";
}

constexpr double kDt = 1.0 / 60.0;

std::string combat_scenario_json(const std::string& f16_path) {
    return R"({
  "name": "combat_transcript_e2e",
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
  "combat": { "enabled": true, "radar_rng_seed": 777 }
})";
}

} // namespace

// ============================================================================
// 1. Pure function: guidance kind -> brevity word.
// ============================================================================
TEST(CombatTranscript, BrevityWordByGuidanceKind) {
    using GK = weapons::GuidanceKind;
    EXPECT_EQ(missile_brevity_word(static_cast<int>(GK::ActiveRadar)), "FOX 3");
    EXPECT_EQ(missile_brevity_word(static_cast<int>(GK::SemiActiveRadar)), "FOX 1");
    EXPECT_EQ(missile_brevity_word(static_cast<int>(GK::Ir)), "FOX 2");
    EXPECT_EQ(missile_brevity_word(static_cast<int>(GK::None)), "missile");
}

// ============================================================================
// 2. Ring buffer semantics (no sim needed — but attach requires one for the
//    callsign map, so use the E2E scenario).
// ============================================================================
TEST(CombatTranscript, RingBufferAndCallsigns) {
    const auto f16 = f16_config_path();
    if (f16.empty()) GTEST_SKIP() << "f16.json fixture not generated";

    auto scenario = load_scenario_from_string(combat_scenario_json(f16));
    Simulation sim(std::move(scenario), std::filesystem::path("."));
    sim.initialize();

    CombatTranscript log;
    log.set_capacity(8);
    EXPECT_EQ(log.capacity(), 8u);
    log.attach(sim);

    // Callsign map: index-aligned with the scenario.
    const auto shooter = sim.aircraft_entities()[0].value;
    const auto bandit = sim.aircraft_entities()[1].value;
    EXPECT_EQ(log.callsign_of(shooter), "EAGLE1");
    EXPECT_EQ(log.callsign_of(bandit), "BANDIT1");
    // Unknown id -> "#<hex>" fallback (never empty, never throws).
    EXPECT_EQ(log.callsign_of(9999), "#270f");

    // Empty ring: at() out of bounds is nullptr.
    EXPECT_EQ(log.size(), 0u);
    EXPECT_EQ(log.at(0), nullptr);

    // Run the fight to the kill so every transition has landed (acquire,
    // spike, FOX 3, launch warning, impact, splash). Bus events are
    // TRANSITIONS only: re-ticking without new state changes adds nothing.
    const auto bandit_handle =
        f4::entities::EntityHandle(sim.aircraft_entities()[1], &sim.world());
    for (int i = 0; i < 150 * 60; ++i) {
        sim.tick(kDt);
        const auto* dmg = bandit_handle.get<f4::entities::DamageStateComponent>();
        if (dmg && dmg->killed) break;
    }
    EXPECT_GT(log.size(), 2u);
    EXPECT_LE(log.size(), 8u);
    for (std::size_t i = 1; i < log.size(); ++i) {
        ASSERT_NE(log.at(i - 1), nullptr);
        ASSERT_NE(log.at(i), nullptr);
        EXPECT_GE(log.at(i)->time_s, log.at(i - 1)->time_s)
            << "entries not in chronological order";
    }

    // set_capacity shrink keeps the NEWEST entries; clear() empties.
    ASSERT_GT(log.size(), 2u);
    const auto newest_text = log.at(log.size() - 1)->text;
    log.set_capacity(2);
    EXPECT_EQ(log.size(), 2u);
    ASSERT_NE(log.at(1), nullptr);
    EXPECT_EQ(log.at(1)->text, newest_text);

    log.clear();
    EXPECT_EQ(log.size(), 0u);
    EXPECT_EQ(log.at(0), nullptr);
}

// ============================================================================
// 3. E2E: the fight narrated, link by link.
// ============================================================================
TEST(CombatTranscript, NarratesTheAiVersusAiFight) {
    const auto f16 = f16_config_path();
    if (f16.empty()) GTEST_SKIP() << "f16.json fixture not generated";

    auto scenario = load_scenario_from_string(combat_scenario_json(f16));
    Simulation sim(std::move(scenario), std::filesystem::path("."));
    sim.initialize();

    CombatTranscript log;
    log.attach(sim);

    const auto bandit = sim.aircraft_entities()[1];
    bool killed = false;
    const int max_ticks = static_cast<int>(150.0 / kDt);
    for (int i = 0; i < max_ticks && !killed; ++i) {
        sim.tick(kDt);
        auto h = f4::entities::EntityHandle(bandit, &sim.world());
        if (const auto* dmg = h.get<f4::entities::DamageStateComponent>()) {
            killed = dmg->killed;
        }
    }
    ASSERT_TRUE(killed) << "the fight never resolved (upstream failure — "
                          "see test_combat_integration E2E)";

    // Collect the narration as "SPEAKER|TEXT" strings for substring checks.
    std::vector<std::string> lines;
    for (std::size_t i = 0; i < log.size(); ++i) {
        const auto* e = log.at(i);
        ASSERT_NE(e, nullptr);
        lines.push_back(e->speaker + "|" + e->text);
    }
    ASSERT_FALSE(lines.empty());

    auto any_line_has = [&](const std::string& needle) {
        return std::any_of(lines.begin(), lines.end(),
            [&](const std::string& l) { return l.find(needle) != std::string::npos; });
    };

    // The OODA loop narrated end-to-end. NOTE: only EAGLE1's radar acquires
    // — the stern-chase geometry puts EAGLE1 BEHIND BANDIT1's north-facing
    // scan bar, so the red side never forms a radar picture (it defends on
    // RWR alone — the M2 policy design).
    EXPECT_TRUE(any_line_has("EAGLE1|Radar contact, BANDIT1."))
        << "no acquisition call";
    EXPECT_TRUE(any_line_has("BANDIT1|Spike from EAGLE1"))
        << "no RWR spike call when EAGLE1 locked";
    EXPECT_TRUE(any_line_has("EAGLE1|FOX 3, AIM-120C away on BANDIT1."))
        << "no AMRAAM launch call";
    EXPECT_TRUE(any_line_has("BANDIT1|Missile launch"))
        << "the victim never called the launch warning";
    EXPECT_TRUE(any_line_has("C2|Splash BANDIT1! Shot down by EAGLE1."))
        << "no splash call";
    EXPECT_TRUE(any_line_has("impact on BANDIT1")) << "no impact call";

    // The splash call carries Kill severity (so does the terminal damage
    // line — both are the kill moment; assert the splash one specifically).
    bool splash_is_kill = false;
    for (std::size_t i = 0; i < log.size(); ++i) {
        const auto* e = log.at(i);
        ASSERT_NE(e, nullptr);
        if (e->text.find("Splash BANDIT1") != std::string::npos) {
            EXPECT_EQ(e->speaker, "C2");
            if (e->severity == CombatTranscript::Severity::Kill) {
                splash_is_kill = true;
            }
        }
    }
    EXPECT_TRUE(splash_is_kill) << "the splash call is not Kill-severity";

    // Chronological order across the whole fight.
    for (std::size_t i = 1; i < log.size(); ++i) {
        EXPECT_GE(log.at(i)->time_s, log.at(i - 1)->time_s);
    }
}
