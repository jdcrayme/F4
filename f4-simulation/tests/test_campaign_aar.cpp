// f4-simulation/tests/test_campaign_aar.cpp
//
// EMPL-2 — the CAMPAIGN-path AAR end-to-end test (the scenario-path
// sibling is test_aar_e2e.cpp over tanker_track.json).
//
// The gap EMPL-2 closes: the AAR redesign's receiver arming + tanker
// picture push ran on the SCENARIO path only. The campaign path now:
//   - stamps the tanker role from the mission byte at spawn (the
//     bridge — mission_is_tanker covers the stock war's AMIS_TANK 39
//     and the ATM filings' AMIS_TANKER 27),
//   - stamps receiver eligibility from the flight's own route (the
//     campaign wire's WP_REFUEL = 4 — the byte the real saves carry),
//   - pairs each receiver with the tanker nearest its refuel waypoint
//     (the reference's FindNearestActiveTanker, keyed on the waypoint
//     the planner wrote), sticky while the tanker stays up,
//   - arms the rung inside a 10-NM join ring, and
//   - the RefuelModule flies the join (standoff + braking-curve
//     closure law) into the full USAF protocol.
//
// This test drives that whole chain through the REAL spawn path
// (spawn_mode campaign_flights over a hand-written two-flight world —
// the same shape the stock save's tanker/receiver pairs carry) and
// pins the protocol the SHOWCASE-1 ladder counts: rendezvous →
// pre-contact → contact → hold → disconnect → done, with fuel.

#include <gtest/gtest.h>

#include <f4/simulation/simulation.hpp>
#include <f4/simulation/scenario.hpp>
#include <f4/simulation/campaign_origin.hpp>
#include <f4/ai/brain_component.hpp>
#include <f4/ai/modules/refuel_module.hpp>
#include <f4/ai/atc/messages.hpp>
#include <f4/entities/entity.hpp>
#include <f4/flight/flight_model_component.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#ifdef _WIN32
#include <process.h>
#else
#include <unistd.h>
#endif

using namespace f4::simulation;
using namespace f4::ai;
using namespace f4::ai::modules;
using namespace f4::ai::atc;
namespace entities = f4::entities;

namespace {

// Fixture resolution (the suite-wide convention — see
// test_bvr_intercept_harness.cpp): generic_string() keeps the paths
// forward-slash, because they are embedded in scenario JSON documents
// (a Windows backslash path JSON-escapes: "\f" in f16.json is a form
// feed, "\U" in C:\Users is invalid).
std::string fixture_dir(const char* macro_dir) {
    const char* env = std::getenv(macro_dir);
    std::string dir = env ? env : "";
    return dir;
}

#ifdef F4_GENERATED_FIXTURES_DIR
const std::string f16_config_path =
    (std::filesystem::path(fixture_dir("F4_GENERATED_FIXTURES_DIR").empty()
         ? F4_GENERATED_FIXTURES_DIR
         : fixture_dir("F4_GENERATED_FIXTURES_DIR")) / "f16.json")
        .generic_string();
const std::string class_table_path =
    (std::filesystem::path(fixture_dir("F4_SOURCE_FIXTURES_DIR").empty()
         ? F4_SOURCE_FIXTURES_DIR
         : fixture_dir("F4_SOURCE_FIXTURES_DIR")) / "falcon4.ct.json")
        .generic_string();
#else
const std::string f16_config_path = "generated_fixtures/f16.json";
const std::string class_table_path = "falcon4.ct.json";
#endif

// A two-flight world: one tanker (mission 39 — the stock war's tanker
// byte, TestCamp's own shape) and one receiver (a strike flight whose
// route carries the campaign wire's WP_REFUEL waypoint at the tanker's
// station — the reference's tanker-covered-package shape). Both at the
// same airbase; the station ~13 NM out at 20,000 ft.
std::string aar_world_json() {
    return R"JSON({
  "theater": "aar-e2e",
  "version": 71,
  "campaign": {
    "current_time": 38574360,
    "te_team": 2,
    "teams": [
      { "slot": 2, "flags": 0, "colour": 2, "name": "ROK", "motto": "" },
      { "slot": 6, "flags": 0, "colour": 6, "name": "DPRK", "motto": "" }
    ]
  },
  "objectives": {
    "count": 1,
    "items": [
      { "type": 100, "id_num": 4101, "id_creator": 0, "objective_type": 1,
        "entity_type": 100, "x": 390, "y": 455, "z": 0, "owner": 2,
        "nameid": 0, "priority": 20, "camp_id": 50 }
    ]
  },
  "units": {
    "count": 3,
    "items": [
      { "unit_class": "squadron", "domain": 2, "id_num": 4281,
        "x": 390, "y": 455, "z": 0, "owner": 2, "camp_id": 51,
        "airbase_id": 4101, "name_id": 72 },
      { "unit_class": "package", "domain": 2, "id_num": 7029,
        "x": 392, "y": 451, "z": 0, "owner": 2 },
      { "unit_class": "flight", "domain": 2, "id_num": 5001,
        "x": 392, "y": 451, "z": 0, "owner": 2,
        "mission": 39,
        "package_id": 7029, "squadron_id": 4281,
        "callsign_id": 125, "callsign_num": 1,
        "wp_count": 4,
        "waypoints": [
          { "x": 392, "y": 451, "z": 0,     "action": 1, "flags": 16512 },
          { "x": 402, "y": 458, "z": 20000, "action": 0, "flags": 16384 },
          { "x": 412, "y": 464, "z": 20000, "action": 0, "flags": 16384 },
          { "x": 392, "y": 451, "z": 0,     "action": 7, "flags": 2304 }
        ] },
      { "unit_class": "flight", "domain": 2, "id_num": 5002,
        "x": 392, "y": 451, "z": 0, "owner": 2,
        "mission": 13,
        "package_id": 7029, "squadron_id": 4281,
        "callsign_id": 125, "callsign_num": 2,
        "wp_count": 3,
        "waypoints": [
          { "x": 392, "y": 451, "z": 0,     "action": 1, "flags": 16512 },
          { "x": 412, "y": 464, "z": 20000, "action": 4, "flags": 16384 },
          { "x": 392, "y": 451, "z": 0,     "action": 7, "flags": 2304 }
        ] }
    ]
  }
}
)JSON";
}

Scenario make_aar_scenario(const std::filesystem::path& dir,
                           const std::filesystem::path& world_path) {
    const auto scenario_file = dir / "aar_campaign.scenario.json";
    {
        std::ofstream out(scenario_file);
        out << "{\n";
        out << "  \"name\": \"campaign_aar_e2e\",\n";
        out << "  \"spawn_mode\": \"campaign_flights\",\n";
        // generic_string: the paths are embedded in a scenario JSON
        // document — Windows backslashes would JSON-escape ("\U" in
        // C:\Users is invalid; the suite-wide convention).
        out << "  \"world_json_path\": \"" << world_path.generic_string()
            << "\",\n";
        out << "  \"class_table_path\": \""
            << class_table_path << "\",\n";
        out << "  \"aircraft\": [{\n";
        out << "    \"callsign\": \"AAR\",\n";
        out << "    \"aircraft_config_path\": \""
            << f16_config_path << "\",\n";
        out << "    \"aircraft_name\": \"F-16C_50\",\n";
        out << "    \"vis_type_index\": 1052,\n";
        out << "    \"parking_spot\": {\"x\": 0.0, \"y\": 0.0, \"z\": 0.0},\n";
        out << "    \"heading_rad\": 0.0\n";
        out << "  }],\n";
        out << "  \"sim_dt\": " << (1.0 / 60.0) << ",\n";
        out << "  \"total_ticks\": 90000,\n";
        out << "  \"record\": false\n";
        out << "}\n";
    }
    return load_scenario(scenario_file);
}

} // namespace

TEST(CampaignAarE2E, SavedTankerAndReceiverFlyTheFullProcedure) {
    if (!std::filesystem::exists(f16_config_path)) {
        GTEST_SKIP() << "f16.json fixture not generated";
    }
    if (!std::filesystem::exists(class_table_path)) {
        GTEST_SKIP() << "falcon4.ct.json fixture not found";
    }

    // Unique temp root (the suite's own convention — no shared /tmp
    // collisions between parallel ctest workers).
    const auto dir = std::filesystem::temp_directory_path() /
                     ("f4_campaign_aar_e2e_" +
#ifdef _WIN32
                      std::to_string(static_cast<long>(_getpid())));
#else
                      std::to_string(static_cast<long>(getpid())));
#endif
    std::filesystem::create_directories(dir);
    const auto world_file = dir / "aar_campaign.world.json";
    {
        std::ofstream out(world_file);
        out << aar_world_json();
    }

    auto scenario = make_aar_scenario(dir, world_file);
    Simulation sim(std::move(scenario), dir);
    sim.initialize();

    // Both flights spawned through the bulk campaign path, each with a
    // plan built from its saved waypoints.
    const auto& roster = sim.aircraft_entities();
    ASSERT_EQ(roster.size(), 2u);

    // The mission byte → role wiring: the AMIS_TANK flight IS the
    // tanker; the WP_REFUEL-carrying flight IS a receiver — the
    // EMPL-2 spawn-time stamps.
    entities::EntityHandle t1(roster[0], &sim.world());
    entities::EntityHandle t2(roster[1], &sim.world());
    auto* b1 = t1.get<BrainComponent>();
    auto* b2 = t2.get<BrainComponent>();
    ASSERT_NE(b1, nullptr);
    ASSERT_NE(b2, nullptr);
    // Spawn order follows the unit order: 5001 (tanker) then 5002.
    const auto* o1 = t1.get<CampaignOriginComponent>();
    const auto* o2 = t2.get<CampaignOriginComponent>();
    ASSERT_NE(o1, nullptr);
    ASSERT_NE(o2, nullptr);
    EXPECT_EQ(o1->mission_byte, 39);
    EXPECT_EQ(o2->mission_byte, 13);
    entities::EntityHandle tanker_h = (o1->mission_byte == 39) ? t1 : t2;
    entities::EntityHandle receiver_h = (o1->mission_byte == 39) ? t2 : t1;
    auto* tanker_brain = tanker_h.get<BrainComponent>();
    auto* receiver_brain = receiver_h.get<BrainComponent>();
    ASSERT_NE(tanker_brain, nullptr);
    ASSERT_NE(receiver_brain, nullptr);
    EXPECT_TRUE(tanker_brain->is_tanker())
        << "the AMIS_TANK(39) flight did not spawn into the tanker role";
    EXPECT_FALSE(receiver_brain->is_tanker());
    EXPECT_TRUE(receiver_brain->refuel_eligible())
        << "the WP_REFUEL-carrying flight did not spawn as a refuel receiver";
    EXPECT_FALSE(tanker_brain->refuel_eligible())
        << "the tanker's own refuel-marked leg must not arm the tanker";
    EXPECT_FALSE(receiver_brain->refuel_armed())
        << "no receiver arms before a valid tanker picture exists";

    // The SHOWCASE-1 protocol counters — the same subscriptions the QC
    // arms run.
    int made = 0, complete = 0;
    sim.bus().subscribe<ContactMade>([&made](const ContactMade&) { ++made; });
    sim.bus().subscribe<RefuelComplete>(
        [&complete](const RefuelComplete&) { ++complete; });

    bool saw_precontact = false, saw_cleared = false, saw_hold = false;
    bool saw_departing = false, saw_done = false;

    constexpr double kDt = 1.0 / 60.0;
    constexpr int kMaxTicks = 108000;   // 30 min — takeoff, transit, join
    // The join funnel trace (F4_AAR_DEBUG=1): both aircraft's state
    // every 60 s — the acceptance run's evidence.
    const bool aar_debug = std::getenv("F4_AAR_DEBUG") != nullptr;
    for (int i = 0; i < kMaxTicks; ++i) {
        sim.tick(kDt);
        if (aar_debug && i % 600 == 0) {
            const auto* rtf = receiver_h.get<entities::TransformComponent>();
            const auto* ttf = tanker_h.get<entities::TransformComponent>();
            const auto* rfm = receiver_h.get<f4::flight::FlightModelComponent>();
            if (rtf != nullptr && ttf != nullptr && rfm != nullptr) {
                const double dist = std::hypot(rtf->position.x - ttf->position.x,
                                               rtf->position.y - ttf->position.y);
                std::printf("[diag] t=%6.1fs phase=%s refuel_leg=%d "
                            "armed=%d rstate=%s dist=%.0fft ralt=%.0f talt=%.0f "
                            "rpos=(%.0f,%.0f) rhdg=%.2f\n",
                            i * kDt, receiver_brain->phase_name(),
                            receiver_brain->at_refuel_waypoint() ? 1 : 0,
                            receiver_brain->refuel_armed() ? 1 : 0,
                            receiver_brain->refuel().state_name().c_str(),
                            dist, rtf->position.z, ttf->position.z,
                            rtf->position.x, rtf->position.y,
                            rfm->state().kin.psi.value());
                std::fflush(stdout);
            }
        }
        switch (receiver_brain->refuel().state()) {
            case RefuelState::PreContact:      saw_precontact = true; break;
            case RefuelState::ClearedContact:  saw_cleared = true; break;
            case RefuelState::Hold:            saw_hold = true; break;
            case RefuelState::Departing:       saw_departing = true; break;
            case RefuelState::Done:
                saw_done = true;
                break;
            default: break;
        }
        if (saw_done) break;
    }

    sim.write_recording();

    EXPECT_GE(made, 1) << "the boom never latched (zero ContactMade)";
    EXPECT_TRUE(saw_precontact) << "receiver never reached PreContact";
    EXPECT_TRUE(saw_cleared) << "receiver never reached ClearedContact";
    EXPECT_TRUE(saw_hold) << "receiver never reached Hold (boom latched)";
    EXPECT_TRUE(saw_departing) << "receiver never reached Departing";
    EXPECT_TRUE(saw_done) << "receiver never reached Done";
    if (saw_done) {
        EXPECT_GT(receiver_brain->refuel().fuel_received_lbs(), 0.0)
            << "reached Done without receiving fuel";
    }
    EXPECT_GE(complete, 1) << "RefuelComplete never fired";

    std::filesystem::remove_all(dir);
}
