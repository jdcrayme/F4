// test_datalink_tiers.cpp — the sim-side half of Step 13
// (AI_IMPLEMENTATION_PLAN §15, the DatalinkTier): the gate/host-walk
// tiers the f4-ai net suite (test_datalink_net.cpp — the geometry/gate
// pins over hand-built nodes) named as this file's reason to exist.
// Where that suite pins the NET's semantics, THIS file pins the WALK
// end to end through Simulation::tick — the host building the net from
// live world state and the brain's own fusion consuming it:
//
//   1. the stamp: the scenario's per-aircraft "awacs" flag stamps the
//      AwacsComponent (the scenario-mode twin of the campaign spawn
//      paths' mission_is_datalink stamp)
//   2. the twin: gate on with NO live nodes == gate off — the legacy
//      picture, member-for-member (the golden identity)
//   3. the coverage: a red AWACS lights blue's detected_by_gci on the
//      red brain for a contact own radar cannot reach — the commit
//      rides the net alone
//   4. the isolation: a BLUE node never lights RED's leg (the bitmask
//      is per-team) while the same run lights BLUE's on red contacts
//   5. the node death: the AWACS killed mid-run drops its team's
//      distant GCI contact at the next walk (the GCI-ghost rule)
//   6. the corpse: a splashed contact stops painting through the net
//      the same walk (the mask-fill twin of the policy's corpse
//      early-out)
//   7. the horizon clamp: the node's min_alt_ft gates the leg, live —
//      the walk reads the COMPONENT's geometry, not constants
//   8. the ground-site arm: "gci_ground_sites" collects radar-bearing
//      objectives as nodes; the arm off leaves the same world dark
//
// Geometry (shared): RED1 at the origin, the AWACS 59 NM west, BLUE1
// 151 NM west of RED1 (92 NM west of the AWACS — inside the node's
// 200 NM v1 radius, far beyond any fighter radar card, so the ONLY
// path to the blue contact is the net). Everyone flies north; drift
// over the test windows is single-digit NM against tens-of-NM margins.

#include <gtest/gtest.h>

#include "f4/simulation/combat_bridge.hpp"
#include "f4/simulation/simulation.hpp"

#include <f4/ai/brain_component.hpp>
#include <f4/ai/sensor_fusion.hpp>
#include <f4/entities/entity.hpp>
#include <f4/geo/position.hpp>

#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <vector>

using namespace f4::simulation;
namespace entities = f4::entities;

namespace {

constexpr double kDt = 1.0 / 60.0;

// Locate the generated F-16 aircraft config fixture (the combat tests'
// shape — FlightModelComponent::init requires real aero tables). Empty
// string = not generated; the caller skips.
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

// One scenario aircraft: identity + the shared straight-north cruise +
// the per-aircraft datalink-node flag under test.
struct NodeAircraft {
    const char* callsign;
    const char* team;
    double x;
    double z;
    bool awacs;
};

// The tier rig: three-ship cruise (RED1 + an optional node + BLUE1) over
// the combat block's datalink gates. Identical worlds differ ONLY in
// the flags — the twin tests lean on that.
std::string datalink_json(const std::string& f16_path,
                          const std::vector<NodeAircraft>& aircraft,
                          bool gci_datalink,
                          bool gci_ground_sites) {
    std::string ac;
    for (const auto& a : aircraft) {
        if (!ac.empty()) ac += ",\n    ";
        ac += std::string("{ \"callsign\": \"") + a.callsign +
              R"(", "aircraft_config_path": ")" + f16_path +
              R"(", "aircraft_name": "F-16C_50", "vis_type_index": 1052,
      "parking_spot": { "x": )" +
              std::to_string(a.x) + ", \"y\": 0.0, \"z\": " +
              std::to_string(a.z) + R"( },
      "heading_rad": 0.0, "initial_fuel_lbs": 6500.0,
      "initial_vt_fps": 500.0, "spawn_in_air": true, "team": ")" +
              a.team + "\"";
        if (a.awacs) ac += ", \"awacs\": true";
        ac += " }";
    }

    std::string combat =
        R"(    "combat": { "enabled": true,
              "radar_rng_seed": 777,
              "bvr_hold": true,
              "missiles_hold": true)";
    if (gci_datalink) combat += ",\n              \"gci_datalink\": true";
    if (gci_ground_sites) {
        combat += ",\n              \"gci_ground_sites\": true";
    }
    combat += " }";

    return R"({
  "name": "datalink_tiers",
  "theater": "korea",
  "aircraft": [
    )" + ac + R"(
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
    { "name": "NORTH", "position": { "x": 0.0, "y": 1500000.0, "z": 20000.0 },
      "speed_kts": 450.0 }
  ],
  "start_enroute": true,
  "sim_dt": 0.016666666666666,
  "total_ticks": 30000,
)" + combat + R"(
})";
}

// The shared tier geometry: RED1 (red, the brain under test), the node
// (red or blue, flagged per test), BLUE1 (blue, the distant contact).
// x in feet; the node sits between them (59 NM west of RED1), BLUE1
// 151 NM west of RED1 — 92 NM west of the node, inside the 200 NM v1
// radius, outside every fighter radar card.
std::vector<NodeAircraft> tier_ship(const char* node_callsign,
                                    const char* node_team, bool node_flag) {
    return {{"RED1", "red", 0.0, 20000.0, false},
            {node_callsign, node_team, -360000.0, 33000.0, node_flag},
            {"BLUE1", "blue", -920000.0, 20000.0, false}};
}

Simulation make_sim(const std::string& f16_path,
                    const std::vector<NodeAircraft>& aircraft,
                    bool gci_datalink, bool gci_ground_sites = false) {
    auto scenario = load_scenario_from_string(
        datalink_json(f16_path, aircraft, gci_datalink, gci_ground_sites));
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

const f4::ai::TargetInfo* find_target(const f4::ai::SensorFusion& sf,
                                      std::uint64_t id) {
    for (const auto& t : sf.targets()) {
        if (t.entity_id == id) return &t;
    }
    return nullptr;
}

// Poll until the brain's fusion holds the contact through the GCI leg.
const f4::ai::TargetInfo* wait_for_gci(Simulation& sim,
                                       f4::ai::BrainComponent* brain,
                                       std::uint64_t id, double seconds) {
    const int ticks = static_cast<int>(seconds / kDt);
    for (int i = 0; i < ticks; ++i) {
        sim.tick(kDt);
        if (const auto* t = find_target(brain->sensors(), id);
            t != nullptr && t->detected_by_gci) {
            return t;
        }
    }
    return nullptr;
}

// Poll until the leg goes dark (the row drops, or the flag clears).
bool wait_for_gci_drop(Simulation& sim, f4::ai::BrainComponent* brain,
                       std::uint64_t id, double seconds) {
    const int ticks = static_cast<int>(seconds / kDt);
    for (int i = 0; i < ticks; ++i) {
        sim.tick(kDt);
        if (const auto* t = find_target(brain->sensors(), id);
            t == nullptr || !t->detected_by_gci) {
            return true;
        }
    }
    return false;
}

// Poll asserting the leg NEVER lights (the isolation windows).
bool gci_stays_dark(Simulation& sim, f4::ai::BrainComponent* brain,
                    std::uint64_t id, double seconds) {
    const int ticks = static_cast<int>(seconds / kDt);
    for (int i = 0; i < ticks; ++i) {
        sim.tick(kDt);
        if (const auto* t = find_target(brain->sensors(), id);
            t != nullptr && t->detected_by_gci) {
            return false;
        }
    }
    return true;
}

// The golden-identity comparator: two deterministic runs of the same
// world must agree member-for-member on the TargetInfo.
void expect_same_target(const f4::ai::TargetInfo& a,
                        const f4::ai::TargetInfo& b) {
    EXPECT_EQ(a.entity_id, b.entity_id);
    EXPECT_EQ(a.is_hostile, b.is_hostile);
    EXPECT_EQ(a.is_missile, b.is_missile);
    EXPECT_EQ(a.combat_class, b.combat_class);
    EXPECT_DOUBLE_EQ(a.threat_score, b.threat_score);
    EXPECT_DOUBLE_EQ(a.range_ft, b.range_ft);
    EXPECT_DOUBLE_EQ(a.range_nm, b.range_nm);
    EXPECT_DOUBLE_EQ(a.azimuth_rad, b.azimuth_rad);
    EXPECT_DOUBLE_EQ(a.elevation_rad, b.elevation_rad);
    EXPECT_DOUBLE_EQ(a.ata_rad, b.ata_rad);
    EXPECT_DOUBLE_EQ(a.ata_from_rad, b.ata_from_rad);
    EXPECT_DOUBLE_EQ(a.atadot, b.atadot);
    EXPECT_DOUBLE_EQ(a.rangedot, b.rangedot);
    EXPECT_EQ(a.detected_by_radar, b.detected_by_radar);
    EXPECT_EQ(a.detected_by_rwr, b.detected_by_rwr);
    EXPECT_EQ(a.detected_by_visual, b.detected_by_visual);
    EXPECT_EQ(a.detected_by_gci, b.detected_by_gci);
    EXPECT_DOUBLE_EQ(a.position.x, b.position.x);
    EXPECT_DOUBLE_EQ(a.position.y, b.position.y);
    EXPECT_DOUBLE_EQ(a.position.z, b.position.z);
    EXPECT_DOUBLE_EQ(a.velocity.x, b.velocity.x);
    EXPECT_DOUBLE_EQ(a.velocity.y, b.velocity.y);
    EXPECT_DOUBLE_EQ(a.velocity.z, b.velocity.z);
    EXPECT_DOUBLE_EQ(a.age_s, b.age_s);
}

} // namespace

// ============================================================================
// 1. The stamp: the scenario's "awacs" flag arms the node component.
// ============================================================================

TEST(DatalinkTiers, ScenarioAwacsFlagStampsTheNodeComponent) {
    const auto f16 = f16_config_path();
    if (f16.empty()) GTEST_SKIP() << "f16.json fixture not generated";

    auto sim = make_sim(f16, tier_ship("AWACS1", "red", true), true);
    sim.initialize();

    // The flagged aircraft carries the component with the documented v1
    // geometry defaults; the unflagged pair carry nothing.
    entities::EntityHandle awacs(id_of(sim, "AWACS1"), &sim.world());
    const auto* node = awacs.get<AwacsComponent>();
    ASSERT_NE(node, nullptr) << "\"awacs\": true did not stamp the node";
    EXPECT_DOUBLE_EQ(node->range_nm, 200.0);
    EXPECT_DOUBLE_EQ(node->min_alt_ft, 0.0);

    for (const char* cs : {"RED1", "BLUE1"}) {
        entities::EntityHandle h(id_of(sim, cs), &sim.world());
        EXPECT_EQ(h.get<AwacsComponent>(), nullptr)
            << cs << " grew a datalink node without the flag";
    }
}

// ============================================================================
// 2. The twin: gate on with NO live nodes == gate off.
// ============================================================================

TEST(DatalinkTiers, GateOnWithNoNodesIsTheLegacyPicture) {
    const auto f16 = f16_config_path();
    if (f16.empty()) GTEST_SKIP() << "f16.json fixture not generated";

    // Identical three-ships, NO node flag anywhere. Run A arms the
    // datalink gate; run B leaves it unset. With no live node the walk
    // unwires the net (the plan's "gate on with no live nodes = the
    // legacy leg" rule) — the two runs must be member-for-member
    // identical, and the distant hostile must stay unseen in both.
    // Sequential runs (one live Simulation at a time), the snapshot
    // carried between them.
    f4::ai::TargetInfo snapshot{};
    bool had_snapshot = false;

    {
        auto on = make_sim(f16, tier_ship("MID1", "red", false), true);
        on.initialize();
        const auto blue_on = id_of(on, "BLUE1").value;
        auto* brain_on = brain_of(on, "RED1");
        ASSERT_NE(brain_on, nullptr);
        for (int i = 0; i < 600; ++i) on.tick(kDt);   // 10 s: rebuilds
        const auto* t_on = find_target(brain_on->sensors(), blue_on);
        ASSERT_NE(t_on, nullptr) << "the picture lost the distant hostile";
        EXPECT_FALSE(t_on->detected_by_gci)
            << "the gate lit GCI with no node in the world";
        EXPECT_FALSE(f4::ai::SensorFusion::can_see(*t_on));
        snapshot = *t_on;
        had_snapshot = true;
    }
    {
        auto off = make_sim(f16, tier_ship("MID1", "red", false), false);
        off.initialize();
        const auto blue_off = id_of(off, "BLUE1").value;
        auto* brain_off = brain_of(off, "RED1");
        ASSERT_NE(brain_off, nullptr);
        for (int i = 0; i < 600; ++i) off.tick(kDt);
        const auto* t_off = find_target(brain_off->sensors(), blue_off);
        ASSERT_NE(t_off, nullptr) << "the picture lost the distant hostile";
        ASSERT_TRUE(had_snapshot);
        EXPECT_FALSE(t_off->detected_by_gci);
        EXPECT_FALSE(f4::ai::SensorFusion::can_see(*t_off));
        expect_same_target(snapshot, *t_off);
    }
}

// ============================================================================
// 3. The coverage: the node lights the leg own radar cannot reach.
// ============================================================================

TEST(DatalinkTiers, AwacsNodeLightsTheGciLegBeyondOwnRadar) {
    const auto f16 = f16_config_path();
    if (f16.empty()) GTEST_SKIP() << "f16.json fixture not generated";

    auto sim = make_sim(f16, tier_ship("AWACS1", "red", true), true);
    sim.initialize();

    const auto blue_id = id_of(sim, "BLUE1");
    auto* brain = brain_of(sim, "RED1");
    ASSERT_NE(brain, nullptr);

    // BLUE1 is 151 NM from RED1 — no fighter radar card reaches; the
    // ONLY path is red's node (92 NM from the contact, inside its
    // 200 NM v1 radius).
    const auto* seen = wait_for_gci(sim, brain, blue_id.value, 30.0);
    ASSERT_NE(seen, nullptr)
        << "the red brain never saw the blue contact through its own "
           "team's net in 30 s";
    EXPECT_TRUE(seen->detected_by_gci);
    EXPECT_FALSE(seen->detected_by_radar)
        << "own radar reached 151 NM — the geometry stopped testing the "
           "net";
    EXPECT_FALSE(seen->detected_by_rwr);
    EXPECT_FALSE(seen->detected_by_visual);
    EXPECT_TRUE(seen->is_hostile);
    EXPECT_TRUE(f4::ai::SensorFusion::can_see(*seen));

    // The commit: the brain's threat ladder takes the net-only contact
    // (the Step-13 E2E's "CAP commits onto the strike package" rung).
    const auto* threat = brain->sensors().threat_target();
    ASSERT_NE(threat, nullptr)
        << "a GCI-only hostile never became the threat target";
    EXPECT_EQ(threat->entity_id, blue_id.value);
}

// ============================================================================
// 4. The isolation: the other team's node never lights your leg.
// ============================================================================

TEST(DatalinkTiers, TheOtherTeamsNodeNeverLightsYourLeg) {
    const auto f16 = f16_config_path();
    if (f16.empty()) GTEST_SKIP() << "f16.json fixture not generated";

    // The node is BLUE's: red must stay blind to the distant blue
    // contact while — the same run — blue's brains see RED1 through
    // their own net (59 NM from the blue node).
    auto sim = make_sim(f16, tier_ship("AWACS1", "blue", true), true);
    sim.initialize();

    const auto blue_id = id_of(sim, "BLUE1").value;
    const auto red_id = id_of(sim, "RED1").value;
    auto* red_brain = brain_of(sim, "RED1");
    auto* blue_brain = brain_of(sim, "AWACS1");
    ASSERT_NE(red_brain, nullptr);
    ASSERT_NE(blue_brain, nullptr);

    ASSERT_TRUE(gci_stays_dark(sim, red_brain, blue_id, 15.0))
        << "blue's node lit red's GCI leg — the bitmask leaked across "
           "teams";
    const auto* blue_seen =
        wait_for_gci(sim, blue_brain, red_id, 15.0);
    ASSERT_NE(blue_seen, nullptr)
        << "the blue net never carried red1 to a blue brain in the same "
           "run";
    EXPECT_TRUE(blue_seen->detected_by_gci);
    EXPECT_TRUE(blue_seen->is_hostile);
}

// ============================================================================
// 5. The node death: a killed AWACS stops broadcasting.
// ============================================================================

TEST(DatalinkTiers, NodeDeathDropsTheTeamsGciLeg) {
    const auto f16 = f16_config_path();
    if (f16.empty()) GTEST_SKIP() << "f16.json fixture not generated";

    auto sim = make_sim(f16, tier_ship("AWACS1", "red", true), true);
    sim.initialize();

    const auto awacs_id = id_of(sim, "AWACS1");
    const auto blue_id = id_of(sim, "BLUE1").value;
    auto* brain = brain_of(sim, "RED1");
    ASSERT_NE(brain, nullptr);

    const auto* seen = wait_for_gci(sim, brain, blue_id, 30.0);
    ASSERT_NE(seen, nullptr) << "precondition: the net lit the leg";

    // Kill the node mid-run. Its transform freezes (the M2 corpse
    // shape) but the NEXT walk must not re-collect it — the dead
    // radar stops broadcasting the same walk, and with no red node
    // left the net unwires (the legacy-leg rule) and the distant
    // contact drops off the fusion.
    entities::EntityHandle awacs(awacs_id, &sim.world());
    auto* dmg = awacs.get<entities::DamageStateComponent>();
    ASSERT_NE(dmg, nullptr);
    dmg->killed = true;

    EXPECT_TRUE(wait_for_gci_drop(sim, brain, blue_id, 30.0))
        << "the GCI-ghost broadcast through a dead AWACS";
    const auto* after = find_target(brain->sensors(), blue_id);
    if (after != nullptr) {
        EXPECT_FALSE(f4::ai::SensorFusion::can_see(*after))
            << "the distant contact stayed visible with every red node "
               "dead";
    }
}

// ============================================================================
// 6. The corpse: a splashed contact stops painting through the net.
// ============================================================================

TEST(DatalinkTiers, CorpsesStopPaintingThroughTheNet) {
    const auto f16 = f16_config_path();
    if (f16.empty()) GTEST_SKIP() << "f16.json fixture not generated";

    auto sim = make_sim(f16, tier_ship("AWACS1", "red", true), true);
    sim.initialize();

    const auto blue_id = id_of(sim, "BLUE1");
    auto* brain = brain_of(sim, "RED1");
    ASSERT_NE(brain, nullptr);

    const auto* seen = wait_for_gci(sim, brain, blue_id.value, 30.0);
    ASSERT_NE(seen, nullptr) << "precondition: the net lit the leg";

    // Kill the CONTACT (not the node): the AWACS keeps broadcasting,
    // the corpse's frozen transform stays inside the node's geometry —
    // the mask must still go dark, the same all-false the policy's
    // corpse early-out gives the sensor legs.
    entities::EntityHandle blue(blue_id, &sim.world());
    auto* dmg = blue.get<entities::DamageStateComponent>();
    ASSERT_NE(dmg, nullptr);
    dmg->killed = true;

    EXPECT_TRUE(wait_for_gci_drop(sim, brain, blue_id.value, 30.0))
        << "a splashed contact kept painting through the net";
    const auto* after = find_target(brain->sensors(), blue_id.value);
    if (after != nullptr) {
        EXPECT_FALSE(f4::ai::SensorFusion::can_see(*after))
            << "the corpse stayed visible through the net";
    }
}

// ============================================================================
// 7. The horizon clamp: the node's own geometry gates the leg, live.
// ============================================================================

TEST(DatalinkTiers, NodeHorizonClampGatesTheLeg) {
    const auto f16 = f16_config_path();
    if (f16.empty()) GTEST_SKIP() << "f16.json fixture not generated";

    auto sim = make_sim(f16, tier_ship("AWACS1", "red", true), true);
    sim.initialize();

    const auto awacs_id = id_of(sim, "AWACS1");
    const auto blue_id = id_of(sim, "BLUE1").value;
    auto* brain = brain_of(sim, "RED1");
    ASSERT_NE(brain, nullptr);

    const auto* seen = wait_for_gci(sim, brain, blue_id, 30.0);
    ASSERT_NE(seen, nullptr) << "precondition: the net lit the leg";

    // Raise the node's horizon clamp above the contact's cruise
    // altitude: the same component the walk reads at spawn time is
    // re-read every walk — the leg drops, then re-lights when the
    // clamp returns to the deck.
    entities::EntityHandle awacs(awacs_id, &sim.world());
    auto* node = awacs.get<AwacsComponent>();
    ASSERT_NE(node, nullptr);
    node->min_alt_ft = 30000.0;   // BLUE1 cruises at 20,000 ft

    EXPECT_TRUE(wait_for_gci_drop(sim, brain, blue_id, 30.0))
        << "the leg stayed lit above the node's horizon clamp";

    node->min_alt_ft = 0.0;
    const auto* relit = wait_for_gci(sim, brain, blue_id, 30.0);
    ASSERT_NE(relit, nullptr)
        << "the leg never re-lit after the clamp returned to the deck";
}

// ============================================================================
// 8. The ground-site arm: radar objectives join the net when armed.
// ============================================================================

TEST(DatalinkTiers, GroundSiteArmCollectsRadarObjectives) {
    const auto f16 = f16_config_path();
    if (f16.empty()) GTEST_SKIP() << "f16.json fixture not generated";

    // The white-box stand-in for a radar-bearing objective: a bare
    // world entity (stationary at deck level — clutter to the CONTACT
    // rule) with the objective radar component. The walk's arm reads
    // exactly this component when "gci_ground_sites" is set; the range
    // conversion is the arm's own (km -> feet -> NM through the net's
    // constant: 300 km = 161.8 NM, covering BLUE1 at 143.2 NM from the
    // site).
    const auto plant_site = [](Simulation& sim) {
        entities::EntityHandle site = sim.world().create();
        auto& tf = site.add<entities::TransformComponent>();
        tf.position = f4::geo::WorldPosition{-50000.0, 0.0, 0.0};
        site.set_tag(entities::tags::TEAM,
                     entities::TagValue::from(std::string("red")));
        auto& rad = site.add<entities::RadarComponent>();
        rad.range_km = 300.0f;
    };

    // Run A — the arm ON: the site joins the net, the red brain sees
    // the distant blue contact through it.
    {
        auto sim = make_sim(f16, tier_ship("MID1", "red", false), true,
                            true);
        sim.initialize();
        plant_site(sim);

        auto* brain = brain_of(sim, "RED1");
        ASSERT_NE(brain, nullptr);
        const auto blue_id = id_of(sim, "BLUE1").value;
        const auto* seen = wait_for_gci(sim, brain, blue_id, 30.0);
        ASSERT_NE(seen, nullptr)
            << "the armed ground site never joined the net";
        EXPECT_TRUE(seen->detected_by_gci);
        EXPECT_FALSE(seen->detected_by_radar)
            << "own radar reached 151 NM — the geometry stopped testing "
               "the site";
    }

    // Run B — the arm OFF, the same world: the radar-bearing entity is
    // just clutter; the leg stays dark.
    {
        auto sim = make_sim(f16, tier_ship("MID1", "red", false), true,
                            false);
        sim.initialize();
        plant_site(sim);

        auto* brain = brain_of(sim, "RED1");
        ASSERT_NE(brain, nullptr);
        const auto blue_id = id_of(sim, "BLUE1").value;
        EXPECT_TRUE(gci_stays_dark(sim, brain, blue_id, 15.0))
            << "the ground site joined the net with the arm off";
        const auto* after = find_target(brain->sensors(), blue_id);
        if (after != nullptr) {
            EXPECT_FALSE(f4::ai::SensorFusion::can_see(*after));
        }
    }
}
