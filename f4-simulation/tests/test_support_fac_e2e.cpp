// test_support_fac_e2e.cpp — the Step-15 support/FAC-brain E2E
// (AI_IMPLEMENTATION_PLAN.md §16 Step 15): the specialist support
// compositions, end to end through Simulation::tick over the generated
// F-16 config:
//
//   1. the tanker's racetrack through an AAR cycle: a tanker brain
//      flying a station-hold racetrack services a receiver through the
//      full boom procedure (the test_aar_e2e rig's shape) — the hold
//      and the contact stabilization coexist, and the receiver reaches
//      Done while the tanker is on station
//   2. the support stand-down: the same tanker with a hold-fire bandit
//      in fusion range NEVER picks the fight under the profile (the
//      gate-off twin engages it) — the reference support aircraft run
//      away, they do not fight
//   3. the AWACS: the node flies its racetrack AND the datalink net
//      carries it — the Step-13 GCI picture flows while the node is
//      on station (the node and the brain that flies the station are
//      the same entity)
//   4. the FAC talk-on: the FAC orbits the marked feature, publishes
//      ONE talk-on radio line (the closed vocabulary), and the strike
//      flight's UNMARKED delivery prosecutes the mark — released on
//      the marked feature without the strike's own sensors ever
//      seeing it (the fusion ladder is air-only; the hint did it)
//   5. the gate-off twin: the same world unarmed — no talk-on line,
//      no hint, no release on the target-0 delivery waypoint
//
// Two-ship/duo geometry inline (the Step-14 rig's fragment
// discipline): every raw string closes on its own terms.

#include <gtest/gtest.h>

#include "f4/simulation/combat_transcript.hpp"
#include "f4/simulation/combat_bridge.hpp"
#include "f4/simulation/simulation.hpp"

#include <f4/ai/brain_component.hpp>
#include <f4/ai/modules/fac_talk_on_module.hpp>
#include <f4/ai/modules/refuel_module.hpp>
#include <f4/entities/entity.hpp>
#include <f4/flight/flight_model_component.hpp>
#include <f4/weapons/messages.hpp>

#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <vector>

using namespace f4::simulation;
namespace entities = f4::entities;

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

// One scenario aircraft fragment (spawn-in-air; the per-aircraft route
// and the Step-15 role fields ride `extra`).
std::string aircraft_json(const std::string& f16_path,
                          const std::string& callsign, double x, double y,
                          double z, double heading_rad, double fuel,
                          double vt_fps, const std::string& extra,
                          const std::string& team = "blue") {
    return std::string("{ \"callsign\": \"") + callsign +
           R"(", "aircraft_config_path": ")" + f16_path +
           R"(", "aircraft_name": "F-16C_50", "vis_type_index": 1052,
      "parking_spot": { "x": )" +
           std::to_string(x) + ", \"y\": " + std::to_string(y) +
           ", \"z\": " + std::to_string(z) + R"( },
      "heading_rad": )" +
           std::to_string(heading_rad) +
           R"(, "initial_fuel_lbs": )" +
           std::to_string(fuel) +
           R"(, "initial_vt_fps": )" +
           std::to_string(vt_fps) +
           R"(, "spawn_in_air": true, "team": ")" +
           team + "\"" + extra + " }";
}

std::string route_wp(const std::string& name, double x, double y, double z,
                     double speed_kts, const std::string& extra = "") {
    return std::string("{ \"name\": \"") + name +
           "\", \"position\": { \"x\": " + std::to_string(x) +
           ", \"y\": " + std::to_string(y) + ", \"z\": " +
           std::to_string(z) + " }, \"speed_kts\": " +
           std::to_string(speed_kts) + extra + " }";
}

// The shared wrapper: start_enroute, combat on (+ any extra combat
// keys), the Part-III gate.
std::string wrap_scenario(const std::string& aircraft_csv,
                          const std::string& features_csv, bool gate_on,
                          const std::string& combat_extra = "") {
    return R"({
  "name": "support_fac_e2e",
  "theater": "korea",
  "aircraft": [
)" + aircraft_csv + R"(
  ],
  "airfield_features": [
)" + features_csv + R"(
  ],
  "airfield": {
    "active_runway_id": 36, "active_runway_name": "Rwy 36",
    "runway_heading_rad": 0.0,
    "threshold_position": { "x": 0.0, "y": -5000.0, "z": 0.0 },
    "runway_end_position":  { "x": 0.0, "y": 5000.0, "z": 0.0 },
    "threshold_altitude_ft": 0.0, "departure_altitude_ft": 15000.0,
    "taxi_route": [ { "x": 0.0, "y": -5000.0, "z": 0.0 },
                    { "x": 0.0, "y": 0.0, "z": 0.0 } ]
  },
  "start_enroute": true,
  "sim_dt": 0.016666666666666,
  "total_ticks": 40000,
  "record": false,
  "combat": { "enabled": true, "radar_rng_seed": 777,
              "fighter_hit_points": 100)" +
             combat_extra + R"( },
  "ai": { "flight_lead": )" +
           (gate_on ? "true" : "false") + R"( }
})";
}

Simulation make_sim(const std::string& json) {
    auto scenario = load_scenario_from_string(json);
    return Simulation(std::move(scenario), std::filesystem::path("."));
}

entities::EntityId id_of(Simulation& sim, const char* callsign) {
    for (const auto eid : sim.aircraft_entities()) {
        entities::EntityHandle h(eid, &sim.world());
        if (const auto* id = h.get<entities::CampaignIdentityComponent>();
            id != nullptr && id->callsign == callsign) {
            return eid;
        }
    }
    return entities::EntityId{0};
}

f4::ai::BrainComponent* brain_of(Simulation& sim, const char* callsign) {
    return entities::EntityHandle(id_of(sim, callsign), &sim.world())
        .get<f4::ai::BrainComponent>();
}

// ── transcript helpers (the Step-14 rig's shape) ────────────────────

struct Line {
    std::string speaker;
    std::string text;
};

std::vector<Line> snapshot(const CombatTranscript& log) {
    std::vector<Line> out;
    out.reserve(log.size());
    for (std::size_t i = 0; i < log.size(); ++i) {
        const auto* e = log.at(i);
        out.push_back(Line{e->speaker, e->text});
    }
    return out;
}

// The datalink rig's fusion query (the GCI leg's observation).
const f4::ai::TargetInfo* find_target(const f4::ai::SensorFusion& sf,
                                      std::uint64_t id) {
    for (const auto& t : sf.targets()) {
        if (t.entity_id == id) return &t;
    }
    return nullptr;
}

} // namespace

// ============================================================================
// 1. The tanker's racetrack through an AAR cycle.
// ============================================================================

TEST(SupportFacE2E, TankerRacetrackServicesTheAarCycle) {
    const auto f16 = f16_config_path();
    if (f16.empty()) GTEST_SKIP() << "f16.json fixture not generated";

    // TANKER01: a station-hold racetrack (anchor + 3 corners, loop 4)
    // with two 100,000-ft legs — the tanker spawns ON its long
    // southbound closing leg, co-located with the receiver (the
    // tanker_track rig's spawn trick), so the boom procedure runs
    // exactly as it was tuned while the tanker's orbit carries it; the
    // contact stabilization (EMPL-2) holds the leg straight for the
    // boom. RECEIVER: low fuel, refuel waypoint on the route
    // (action 20) — the full boom procedure against an orbiting
    // tanker.
    std::string tanker_route = route_wp("ANCHOR", 0.0, -30000.0, 10000.0,
                                        300.0,
                                        R"(, "station_time_s": 900.0,
                                           "loop_waypoints": 4)") +
                              ",\n        " +
                              route_wp("C1", 15000.0, -30000.0, 10000.0, 300.0) +
                              ",\n        " +
                              route_wp("C2", 15000.0, 70000.0, 10000.0, 300.0) +
                              ",\n        " +
                              route_wp("C3", 0.0, 70000.0, 10000.0, 300.0);
    std::string receiver_route = route_wp("AR_POINT", 0.0, -50000.0, 9500.0,
                                          300.0, R"(, "action": 20)") +
                                 ",\n        " +
                                 route_wp("POST", 0.0, 80000.0, 9000.0, 300.0);

    const std::string json = wrap_scenario(
        "    " +
            aircraft_json(f16, "TANKER01", 0.0, 60000.0, 10000.0,
                          3.14159265358979, 6500.0, 506.3,
                          R"(,
      "tanker": true,
      "route": [
        )" + tanker_route + "]") +
            ",\n    " +
            aircraft_json(f16, "RECEIVER", 140.0, 60000.0, 10000.0,
                          3.14159265358979, 2000.0, 506.3,
                          R"(,
      "route": [
        )" + receiver_route + "]"),
        "", true);

    Simulation sim = make_sim(json);
    sim.initialize();
    ASSERT_EQ(sim.aircraft_entities().size(), 2u);
    ASSERT_TRUE(sim.has_tanker());

    auto* tanker_brain = brain_of(sim, "TANKER01");
    auto* receiver_brain = brain_of(sim, "RECEIVER");
    ASSERT_NE(tanker_brain, nullptr);
    ASSERT_NE(receiver_brain, nullptr);

    // The Part-III arm stamped the support profile on the tanker.
    EXPECT_TRUE(tanker_brain->is_support_profile());
    EXPECT_TRUE(tanker_brain->is_tanker());

    bool saw_hold = false;       // the boom latched
    bool tanker_held_station = false;  // the orbit armed and ran
    bool saw_done = false;
    constexpr int kMaxTicks = 36000;  // 600 s — the orbit costs some time

    for (int i = 0; i < kMaxTicks && !saw_done; ++i) {
        sim.tick(kDt);
        const auto st = receiver_brain->refuel().state();
        if (st == f4::ai::modules::RefuelState::Hold) saw_hold = true;
        if (st == f4::ai::modules::RefuelState::Done) saw_done = true;
        // The racetrack contract arms on the anchor's capture and runs
        // its 900 s — the tanker IS the orbit through the whole window.
        if (tanker_brain->navigation().holding_station()) {
            tanker_held_station = true;
        }
    }

    EXPECT_TRUE(saw_hold)
        << "the receiver never latched the boom off the orbiting tanker";
    EXPECT_TRUE(saw_done)
        << "the receiver never completed the cycle off the orbiting tanker";
    EXPECT_TRUE(tanker_held_station)
        << "the tanker never flew its racetrack station";
    // The station brain never picked a fight (nothing to fight here —
    // the structural pin: no engagement state, ever).
    EXPECT_EQ(tanker_brain->combat_engagement_id(), 0u);
}

// ============================================================================
// 2. The support stand-down: the profiled tanker never picks the fight.
// ============================================================================

TEST(SupportFacE2E, SupportProfileStandsTheEngagementDown) {
    const auto f16 = f16_config_path();
    if (f16.empty()) GTEST_SKIP() << "f16.json fixture not generated";

    // One tanker on a hold, one hold-fire bandit cruising 8 NM ahead —
    // inside radar range, never shooting. The tanker is the world's
    // only attacker. The bandit cruises a small racetrack of its own
    // (a Ground-phase parked airframe is not a fightable contact —
    // the combat classes it below the fighter floor).
    auto make = [&](bool gate_on) {
        std::string tanker_route =
            route_wp("ANCHOR", 0.0, 0.0, 12000.0, 300.0,
                     R"(, "station_time_s": 900.0, "loop_waypoints": 4)") +
            ",\n        " +
            route_wp("C1", 4000.0, 0.0, 12000.0, 300.0) + ",\n        " +
            route_wp("C2", 4000.0, 8000.0, 12000.0, 300.0) + ",\n        " +
            route_wp("C3", 0.0, 8000.0, 12000.0, 300.0);
        std::string bandit_route =
            route_wp("B_ANCHOR", 0.0, 46000.0, 12000.0, 420.0,
                     R"(, "station_time_s": 900.0, "loop_waypoints": 4)") +
            ",\n        " +
            route_wp("B_C1", 3000.0, 46000.0, 12000.0, 420.0) + ",\n        " +
            route_wp("B_C2", 3000.0, 50000.0, 12000.0, 420.0) + ",\n        " +
            route_wp("B_C3", 0.0, 50000.0, 12000.0, 420.0);
        return wrap_scenario(
            "    " +
                aircraft_json(f16, "TANKER01", 0.0, -2000.0, 12000.0, 0.0,
                              356000.0, 506.3,
                              R"(,
      "tanker": true,
      "route": [
        )" + tanker_route + "]") +
                ",\n    " +
                aircraft_json(f16, "BANDIT1", 0.0, 48000.0, 12000.0,
                              3.14159265358979, 6500.0, 506.3,
                              R"(,
      "hold_fire": true,
      "route": [
        )" + bandit_route + "]",
                "red"),
            "", gate_on);
    };

    // Gate OFF: the pre-Step-15 world — the tanker (a fighter airframe
    // with a fighter loadout) detects the hostile and engages it.
    {
        Simulation sim = make_sim(make(false));
        sim.initialize();
        auto* tanker = brain_of(sim, "TANKER01");
        ASSERT_NE(tanker, nullptr);
        EXPECT_FALSE(tanker->is_support_profile());
        bool engaged = false;
        for (int i = 0; i < 7200 && !engaged; ++i) {  // 120 s
            sim.tick(kDt);
            if (tanker->combat_engagement_id() != 0) engaged = true;
        }
        EXPECT_TRUE(engaged)
            << "the gate-off tanker never engaged the bandit in range "
               "(the delta this tier pins is gone)";
    }

    // Gate OFF: the pre-Step-15 world — the tanker (a fighter airframe
    // with a fighter loadout) detects the hostile and engages it.
    {
        Simulation sim = make_sim(make(false));
        sim.initialize();
        auto* tanker = brain_of(sim, "TANKER01");
        ASSERT_NE(tanker, nullptr);
        EXPECT_FALSE(tanker->is_support_profile());
        bool engaged = false;
        for (int i = 0; i < 7200 && !engaged; ++i) {  // 120 s
            sim.tick(kDt);
            if (tanker->combat_engagement_id() != 0) engaged = true;
        }
        EXPECT_TRUE(engaged)
            << "the gate-off tanker never engaged the bandit in range "
               "(the delta this tier pins is gone)";
    }

    // Gate ON: the support profile stands the engagement rungs down —
    // the tanker flies its station through the same threat.
    {
        Simulation sim = make_sim(make(true));
        sim.initialize();
        auto* tanker = brain_of(sim, "TANKER01");
        ASSERT_NE(tanker, nullptr);
        EXPECT_TRUE(tanker->is_support_profile());
        bool ever_engaged = false;
        bool on_station = false;
        for (int i = 0; i < 7200; ++i) {  // 120 s
            sim.tick(kDt);
            if (tanker->combat_engagement_id() != 0) ever_engaged = true;
            if (tanker->navigation().holding_station()) on_station = true;
        }
        EXPECT_FALSE(ever_engaged)
            << "the support-profiled tanker picked the fight";
        EXPECT_TRUE(on_station) << "the tanker never held its racetrack";
    }
}

// ============================================================================
// 3. The AWACS: the node flies the racetrack AND the net carries it.
// ============================================================================

TEST(SupportFacE2E, AwacsStationsWhileTheNetCarriesIt) {
    const auto f16 = f16_config_path();
    if (f16.empty()) GTEST_SKIP() << "f16.json fixture not generated";

    // AWACS1 on a racetrack; a hostile ~111 NM north; the blue
    // receiver holding a racetrack 111 NM south of the hostile. The
    // GCI tier feeds the receiver the contact the node sees (the net's
    // word, whatever the receiver's own radar answers — the flag reads
    // the net when one is wired). Everyone cruises a route: a
    // Ground-phase brain is not a picture participant.
    std::string awacs_route =
        route_wp("ANCHOR", 100000.0, 690000.0, 25000.0, 300.0,
                 R"(, "station_time_s": 900.0, "loop_waypoints": 4)") +
        ",\n        " +
        route_wp("C1", 110000.0, 690000.0, 25000.0, 300.0) + ",\n        " +
        route_wp("C2", 110000.0, 700000.0, 25000.0, 300.0) + ",\n        " +
        route_wp("C3", 100000.0, 700000.0, 25000.0, 300.0);
    std::string blue_route =
        route_wp("B_ANCHOR", 0.0, 56000.0, 15000.0, 350.0,
                 R"(, "station_time_s": 900.0, "loop_waypoints": 4)") +
        ",\n        " +
        route_wp("B_C1", 4000.0, 56000.0, 15000.0, 350.0) + ",\n        " +
        route_wp("B_C2", 4000.0, 64000.0, 15000.0, 350.0) + ",\n        " +
        route_wp("B_C3", 0.0, 64000.0, 15000.0, 350.0);
    std::string red_route =
        route_wp("R_ANCHOR", 0.0, 726000.0, 15000.0, 350.0,
                 R"(, "station_time_s": 900.0, "loop_waypoints": 4)") +
        ",\n        " +
        route_wp("R_C1", 4000.0, 726000.0, 15000.0, 350.0) + ",\n        " +
        route_wp("R_C2", 4000.0, 734000.0, 15000.0, 350.0) + ",\n        " +
        route_wp("R_C3", 0.0, 734000.0, 15000.0, 350.0);

    const std::string json = wrap_scenario(
        "    " +
            aircraft_json(f16, "AWACS1", 100000.0, 685000.0, 25000.0, 0.0,
                          6500.0, 506.3,
                          R"(,
      "awacs": true,
      "route": [
        )" + awacs_route + "]") +
            ",\n    " +
            aircraft_json(f16, "BLUE1", 0.0, 60000.0, 15000.0, 0.0, 6500.0,
                          506.3,
                          R"(,
      "route": [
        )" + blue_route + "]") +
            ",\n    " +
            aircraft_json(f16, "RED1", 0.0, 730000.0, 15000.0,
                          3.14159265358979, 6500.0, 506.3,
                          R"(,
      "hold_fire": true,
      "route": [
        )" + red_route + "]",
                "red"),
        "", true,
        R"(,
              "gci_datalink": true)");

    Simulation sim = make_sim(json);
    sim.initialize();

    auto* awacs = brain_of(sim, "AWACS1");
    ASSERT_NE(awacs, nullptr);
    EXPECT_TRUE(awacs->is_support_profile());
    entities::EntityHandle awacs_h(id_of(sim, "AWACS1"), &sim.world());
    ASSERT_NE(awacs_h.get<AwacsComponent>(), nullptr);

    const std::uint64_t red1 = id_of(sim, "RED1").value;
    auto* blue = brain_of(sim, "BLUE1");
    ASSERT_NE(blue, nullptr);

    bool on_station = false;
    bool gci_contact = false;
        for (int i = 0; i < 7200 && !gci_contact; ++i) {  // 120 s
        sim.tick(kDt);
        if (awacs->navigation().holding_station()) on_station = true;
        const auto* t = find_target(blue->sensors(), red1);
        if (t != nullptr && t->detected_by_gci) gci_contact = true;
    }
    EXPECT_TRUE(on_station)
        << "the node never flew its racetrack station";
    EXPECT_TRUE(gci_contact)
        << "the net never carried the node's contact while the node "
           "flew the station";
}

// ============================================================================
// 4. The FAC talk-on drives the strike's unmarked delivery.
// ============================================================================

namespace {

// The FAC + strike world: the FAC orbits (60000, 14000..20000) with
// the mark on the airfield feature at (40000, 44000) — the ground
// position of the strike's unmarked DELIVERY ANCHOR (action 17, no
// target — the planner knew the area, the FAC names the point). The
// circuit's far legs sit outside the Mk-82's ~18.5k ft ballistic
// envelope so the only release windows are the aligned ones (the
// ingress leg and the closing leg), and the talk-on fills the aim
// while the strike is still on ingress — the release happens in the
// mark's bore, the same employment geometry the route-aimed
// deliveries fly.
std::string fac_strike_json(const std::string& f16, bool gate_on) {
    std::string fac_route =
        route_wp("ANCHOR", 60000.0, 14000.0, 12000.0, 350.0,
                 R"(, "station_time_s": 900.0, "loop_waypoints": 4)") +
        ",\n        " +
        route_wp("C1", 64000.0, 14000.0, 12000.0, 350.0) + ",\n        " +
        route_wp("C2", 64000.0, 20000.0, 12000.0, 350.0) + ",\n        " +
        route_wp("C3", 60000.0, 20000.0, 12000.0, 350.0);
    std::string strike_route =
        route_wp("INGRESS", 40000.0, 14000.0, 12000.0, 400.0) +
        ",\n        " +
        route_wp("DELIVER", 40000.0, 44000.0, 12000.0, 400.0,
                 R"(, "action": 17, "station_time_s": 600.0,
                    "loop_waypoints": 4)") +
        ",\n        " +
        route_wp("C1", 40000.0, 66000.0, 12000.0, 400.0) + ",\n        " +
        route_wp("C2", 62000.0, 66000.0, 12000.0, 400.0) + ",\n        " +
        route_wp("C3", 62000.0, 44000.0, 12000.0, 400.0);
    return wrap_scenario(
        "    " +
            aircraft_json(f16, "PEEWEE", 60000.0, 8000.0, 12000.0, 0.0,
                          6500.0, 506.3,
                          R"(,
      "fac": true, "mark_feature": 0,
      "route": [
        )" + fac_route + "]") +
            ",\n    " +
            aircraft_json(f16, "STRIKE1", 40000.0, 0.0, 12000.0, 0.0,
                          6500.0, 506.3,
                          R"(,
      "route": [
        )" + strike_route + "]"),
        "    { \"name\": \"MARK\", \"vis_type_index\": 1052,"
        " \"position\": { \"x\": 40000.0, \"y\": 44000.0, \"z\": 0.0 },"
        " \"heading_rad\": 0.0 }",
        gate_on);
}

struct BombObserver {
    std::vector<f4::weapons::BombReleasedMessage> releases;
    std::vector<f4::weapons::BombImpactMessage> impacts;
};

BombObserver observe(Simulation& sim) {
    BombObserver o;
    sim.bus().subscribe<f4::weapons::BombReleasedMessage>(
        [&o](const f4::weapons::BombReleasedMessage& m) {
            o.releases.push_back(m);
        });
    sim.bus().subscribe<f4::weapons::BombImpactMessage>(
        [&o](const f4::weapons::BombImpactMessage& m) {
            o.impacts.push_back(m);
        });
    return o;
}

void arm_bombs(Simulation& sim, const char* callsign) {
    entities::EntityHandle h(id_of(sim, callsign), &sim.world());
    auto* store = h.get<f4::weapons::WeaponStoreComponent>();
    ASSERT_NE(store, nullptr);
    store->add_station(
        sim.weapon_table().find_by_name("MK-82"), 8, "mk82");
}

} // namespace

TEST(SupportFacE2E, FacTalkOnDrivesTheUnmarkedDelivery) {
    const auto f16 = f16_config_path();
    if (f16.empty()) GTEST_SKIP() << "f16.json fixture not generated";

    Simulation sim = make_sim(fac_strike_json(f16, true));
    sim.initialize();
    BombObserver obs = observe(sim);
    arm_bombs(sim, "STRIKE1");

    // The FAC's mark: the one airfield feature entity — resolved from
    // the FAC brain's armed mark (arm_support_brains set it from the
    // scenario's mark_feature index).
    entities::EntityId feature{0};
    {
        auto* fac = brain_of(sim, "PEEWEE");
        ASSERT_NE(fac, nullptr);
        feature = entities::EntityId{fac->fac_talk_on().mark_id()};
    }
    ASSERT_NE(feature.value, 0u) << "the FAC armed without a mark";

    CombatTranscript log;
    log.attach(sim);

    bool talk_on = false;
    bool released = false;
    bool impacted = false;
    double miss_ft = -1.0;
    for (int i = 0; i < 30000 && !impacted; ++i) {  // 500 s
        sim.tick(kDt);
        if (!talk_on) {
            auto* fac = brain_of(sim, "PEEWEE");
            talk_on = fac != nullptr && fac->fac_talk_on().published();
        }
        if (!released && !obs.releases.empty()) released = true;
        if (!obs.impacts.empty()) {
            impacted = true;
            miss_ft = obs.impacts.front().miss_distance_ft;
        }
    }

    EXPECT_TRUE(talk_on) << "the FAC never went on station / never talked";
    EXPECT_TRUE(released) << "the strike never released on the mark";

    // The release is the MARK: the bomb's target is the feature entity,
    // the shooter is the strike.
    ASSERT_FALSE(obs.releases.empty());
    bool on_mark = false;
    for (const auto& r : obs.releases) {
        if (r.target_id == feature.value &&
            r.shooter_id == id_of(sim, "STRIKE1").value) {
            on_mark = true;
        }
    }
    EXPECT_TRUE(on_mark)
        << "no bomb was released against the marked feature";

    // The talk-on reached the strike brain as the hint.
    auto* strike = brain_of(sim, "STRIKE1");
    ASSERT_NE(strike, nullptr);
    EXPECT_EQ(strike->talk_on_target_id(), feature.value);

    // The radio line: ONE talk-on (the closed vocabulary, one-shot),
    // rendered in the host.
    const auto lines = snapshot(log);
    std::size_t talk_lines = 0;
    std::string talk_text;
    for (const auto& l : lines) {
        if (l.text.find("talk-on bearing") != std::string::npos) {
            ++talk_lines;
            talk_text = l.text;
        }
    }
    EXPECT_EQ(talk_lines, 1u)
        << "v1 marks ONE target — exactly one talk-on line";
    EXPECT_NE(talk_text.find("STRIKE1"), std::string::npos)
        << "the talk-on names the assigned flight: " << talk_text;
    EXPECT_NE(talk_text.find("bearing 000"), std::string::npos)
        << "the mark is due north of the strike's orbit: " << talk_text;
    EXPECT_NE(talk_text.find("ground assets"), std::string::npos)
        << "the closed description row renders: " << talk_text;

    // The impact: on the marked feature (the ballistic solution the
    // brain resolved from the hint).
    EXPECT_TRUE(impacted) << "no bomb impact was ever recorded";
    if (impacted) {
        EXPECT_EQ(obs.impacts.front().target_id, feature.value);
        // The release geometry IS the aim resolution's job; the miss
        // stays inside the Mk-82's lethal radius (the EMPL discipline
        // the route-aimed deliveries already fly).
        EXPECT_LT(miss_ft, 300.0)
            << "the talk-on-driven stick missed the mark: " << miss_ft;
    }
}

// ============================================================================
// 5. The gate-off twin: no FAC, no hint, no release.
// ============================================================================

TEST(SupportFacE2E, GateOffTwinNeverDelivers) {
    const auto f16 = f16_config_path();
    if (f16.empty()) GTEST_SKIP() << "f16.json fixture not generated";

    Simulation sim = make_sim(fac_strike_json(f16, false));
    sim.initialize();
    BombObserver obs = observe(sim);
    arm_bombs(sim, "STRIKE1");

    CombatTranscript log;
    log.attach(sim);

    auto* fac = brain_of(sim, "PEEWEE");
    ASSERT_NE(fac, nullptr);
    EXPECT_FALSE(fac->is_fac()) << "the FAC armed without the gate";
    EXPECT_FALSE(fac->is_support_profile());

    for (int i = 0; i < 30000; ++i) {  // the full 500 s
        sim.tick(kDt);
    }

    EXPECT_FALSE(fac->fac_talk_on().published());
    auto* strike = brain_of(sim, "STRIKE1");
    ASSERT_NE(strike, nullptr);
    EXPECT_EQ(strike->talk_on_target_id(), 0u)
        << "the strike grew a hint without the pipe";

    // No talk-on line anywhere in the radio log.
    const auto lines = snapshot(log);
    for (const auto& l : lines) {
        EXPECT_EQ(l.text.find("talk-on"), std::string::npos)
            << "an unarmed world rendered a talk-on: " << l.text;
    }

    // No release: the unmarked delivery waypoint alone arms nothing.
    EXPECT_TRUE(obs.releases.empty())
        << "the target-0 delivery released without the hint";
    EXPECT_TRUE(obs.impacts.empty());

    // The strike flew its route and SAT in the delivery circuit (the
    // anchor's station hold is route mechanics, not gate-gated) — the
    // delivery posture was there, the AIM never was.
    EXPECT_TRUE(strike->navigation().holding_station())
        << "the strike never reached its delivery orbit";
}

