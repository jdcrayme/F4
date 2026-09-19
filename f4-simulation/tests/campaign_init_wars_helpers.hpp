// campaign_init_wars_helpers.hpp — shared helpers for the CAMP-INIT-1 war
// gates + their Tier 3.1 fast-accel counterparts.
//
// Extracted from test_campaign_init_wars.cpp so the fast-tier variants
// (test_campaign_init_wars_fast.cpp) can reuse the exact same setup +
// pins — only WarHarnessOptions::speed differs between the nightly
// (1x, the real 24-hour harness) and the daily (~3 s at 60x) tiers.

#pragma once

#include <f4/simulation/campaign_war_harness.hpp>

#include <gtest/gtest.h>

#include <cctype>
#include <filesystem>
#include <string>

namespace f4_test {

inline std::filesystem::path generated_world(const char* name) {
    return std::filesystem::path(F4_CAMPINIT_FIXTURES_DIR) /
           (std::string("campinit_") + name + ".world.json");
}
inline std::filesystem::path class_table() {
    return std::filesystem::path(F4_SOURCE_FIXTURES_DIR) / "falcon4.ct.json";
}
inline std::filesystem::path f16_config() {
    return std::filesystem::path(F4_GENERATED_FIXTURES_DIR) / "f16.json";
}

inline bool fixtures_ready() {
    return std::filesystem::exists(f16_config()) &&
           std::filesystem::exists(generated_world("small"));
}

// The generated-war rig: the REAL tasking pipeline (the generated teams
// carry the stock profile and their ATM airbase rows), the campaign's
// own default tasking cadence, ground war armed (the packs carry
// battalions), tiered fidelity for the long horizons.
//
// `speed` defaults to 1.0 (the real-fidelity nightly variant). Pass 60.0
// for the Tier 3.1 fast tier — the harness's own acceptance preset
// (FIDELITY_TIERS: 58.1x sustained vs 25.3x full-fidelity baseline).
inline f4::simulation::WarHarnessOptions make_opts(const char* world,
                                                    std::int64_t horizon_sec,
                                                    double sample_sec,
                                                    double speed = 1.0) {
    f4::simulation::WarHarnessOptions o;
    o.session.world_json = generated_world(world);
    o.session.class_table = class_table();
    o.session.aircraft_config = f16_config();
    o.session.mission_profiles = F4_MISSION_PROFILES_JSON;
    o.session.tasking_cycle_sec = 1800;
    o.session.atm_pipeline = true;
    o.session.ground_war = true;
    o.session.fidelity_policy = f4::simulation::FidelityPolicy::Tiered;
    o.session.max_flights = 24;
    o.horizon_sec = horizon_sec;
    o.sample_sec = sample_sec;
    o.runs = 2;
    o.speed = speed;  // Tier 3.1: the accel knob (1.0 = nightly, 60.0 = daily)
    return o;
}

inline bool is_hex_32(const std::string& s) {
    if (s.size() != 32) return false;
    for (const char c : s) {
        if (!std::isdigit(static_cast<unsigned char>(c)) &&
            (c < 'a' || c > 'f')) {
            return false;
        }
    }
    return true;
}

inline void expect_green(const f4::simulation::WarReport& r) {
    const f4::simulation::WarVerdict& v = r.verdict;
    EXPECT_TRUE(v.drew_aircraft) << "a generated war must draw aircraft";
    EXPECT_TRUE(v.routes_built);
    EXPECT_TRUE(v.materialized);
    EXPECT_TRUE(v.packages_built);
    EXPECT_TRUE(v.deterministic);
    EXPECT_TRUE(v.ledger_consistent) << v.ledger_drift;
    EXPECT_TRUE(v.entities_bounded) << v.entity_leak;
    EXPECT_TRUE(v.war_alive) << v.war_stall;
}

} // namespace f4_test
