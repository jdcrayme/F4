// f4-simulation/tests/test_campaign_combat.cpp
//
// C6 — arming the campaign flights (CAMPAIGN_LOOP_PLAN.md §5 C6).
//
// The scenario combat path E2Es through test_combat_integration (the
// M3 chain: detect → track → lock → launch → kill). C6's surface is
// different: the ARM of a campaign-spawned aircraft — the component
// set, the mission-role doctrine, the detection policy, the doctrine
// loadout. These tests pin exactly that:
//
//   1. campaign_combat_role: the 41 mission bytes map to the doctrine
//      (CAP/Sweep/Intercept/Escort fight; everything else defends).
//   2. arm_campaign_combat on a FIGHTER: components attach, the doctrine
//      A/A loadout fills, no archetype, the brain fights, the policy is
//      handed to the caller.
//   3. arm_campaign_combat on a DEFENSIVE role: the disengaged BRAINDAT
//      archetype lands, no A/A fill, no gun, the strike store keeps its
//      bombs, hold_fire stays false (bombs free, A/A stood down).
//   4. Idempotence: a second arm attaches nothing.
//   5. Non-campaign entities (no origin) are rejected harmlessly.
//   6. Simulation::arm_campaign_aircraft on a hand-built late spawn:
//      the entity arms, the policy is owned + installed, the counters
//      book, and ticks run (the radar scans, the brain sees the empty
//      sky, nothing crashes).
//   7. The armed war over the routed kunsan rig: verdicts green,
//      determinism holds (byte-identical ledgers), aircraft armed —
//      the compressed C6 acceptance (the full-horizon acceptance is
//      campaign_qc --war --aa-combat on real data).

#include <gtest/gtest.h>

#include "f4/simulation/campaign_bridge.hpp"
#include "f4/simulation/campaign_origin.hpp"
#include "f4/simulation/campaign_war_harness.hpp"
#include "f4/simulation/combat_bridge.hpp"
#include "f4/simulation/simulation.hpp"
#include "f4/simulation/scenario.hpp"

#include <f4/ai/brain_component.hpp>
#include <f4/ai/modules/navigation_module.hpp>
#include <f4/data/brain_data.hpp>
#include <f4/data/aircraft_config.hpp>
#include <f4/data/config_loader.hpp>
#include <f4/entities/entity.hpp>
#include <f4/entities/types.hpp>
#include <f4/flight/flight_model_component.hpp>
#include <f4/sensors/f4_sensors.hpp>
#include <f4/weapons/f4_weapons.hpp>
#include <f4/weapons/weapon_store.hpp>

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <vector>

using namespace f4::simulation;
using namespace f4::entities;
using f4::ai::BrainComponent;
using f4::flight::FlightModelComponent;
using f4::weapons::WeaponStoreComponent;
using f4::sensors::RadarSimComponent;
using f4::sensors::RwrComponent;
using f4::sensors::SignatureComponent;
using f4::weapons::GunComponent;
using f4::simulation::CampaignOriginComponent;

namespace {

// The generated fixtures (f16.json + simdata/braindata.json), same
// resolution pattern as the sibling tests. Empty path = fixture absent;
// the arm tests skip (the role tests run regardless — pure function).
std::string generated_fixture(const char* leaf) {
    const char* env = std::getenv("F4_GENERATED_FIXTURES_DIR");
    std::string dir = env ? env : "";
#ifdef F4_GENERATED_FIXTURES_DIR
    if (dir.empty()) dir = F4_GENERATED_FIXTURES_DIR;
#endif
    if (dir.empty()) return "";
    const auto path = std::filesystem::path(dir) / leaf;
    return std::filesystem::exists(path) ? path.string() : "";
}

bool load_f16(f4::data::AircraftConfig& cfg) {
    const auto path = generated_fixture("f16.json");
    if (path.empty()) return false;
    auto result = f4::data::loadConfig(path);
    if (!result.ok) return false;
    cfg = std::move(result.config);
    return true;
}

bool load_brain_data(f4::data::BrainData& data) {
    const auto path = generated_fixture("simdata/braindata.json");
    if (path.empty()) return false;
    auto result = f4::data::loadBrainData(path);
    if (!result.ok) return false;
    data = std::move(result.data);
    return true;
}

// One campaign-shaped aircraft entity in a bare world: Transform + FM
// (airborne, flying north) + Brain (an enroute mission plan, the shape
// the campaign spawn paths attach) + the strike store + the campaign
// origin stamped with `mission_byte`. The same composition
// spawn_aircraft_for_flight builds, minus the visual component (the
// arm never reads it).
EntityId make_campaign_aircraft(EntityWorld& world,
                                const f4::data::AircraftConfig& cfg,
                                std::uint8_t mission_byte,
                                const char* team,
                                double north_ft,
                                int bomb_stations) {
    auto h = world.create();
    auto& tf = h.add<TransformComponent>();
    tf.position = f4::geo::WorldPosition(0.0, north_ft, 10000.0);
    auto& fm = h.add<FlightModelComponent>();
    fm.init(cfg, /*alt_ft=*/10000.0, /*vt_fps=*/450.0,
            /*heading_rad=*/0.0, /*inAir=*/true,
            /*north_ft=*/north_ft, /*east_ft=*/0.0);
    (void)fm;
    auto& brain = h.add<BrainComponent>();
    // The enroute plan the campaign spawn path attaches: a route north,
    // starting Enroute (spawned airborne). This puts the brain in the
    // phase whose combat block C6 arms.
    f4::ai::MissionPlan plan;
    plan.route.push_back(f4::ai::modules::NavigationModule::Waypoint{
        "FAR_NORTH", f4::geo::WorldPosition(0.0, north_ft + 400000.0,
                                            10000.0),
        450.0});
    plan.start_phase = f4::ai::MissionPlan::StartPhase::Enroute;
    brain.set_mission_plan(std::move(plan));
    h.set_tag(tags::TEAM, TagValue::from(std::string(team)));
    auto& store = h.add<WeaponStoreComponent>();
    const auto table = f4::weapons::WeaponClassTable::with_builtins();
    for (int i = 0; i < bomb_stations; ++i) {
        store.add_station(table.find_by_name("MK-82"), 2,
                          "doctrine bomb");
    }
    auto& origin = h.add<CampaignOriginComponent>();
    origin.flight_vu = 4242u + mission_byte;
    origin.squadron_vu = 777u;
    origin.team_slot = std::string(team) == "red" ? 1 : 0;
    origin.mission_byte = mission_byte;
    return h.id();
}

constexpr double kDt = 1.0 / 60.0;

} // namespace

// ── 1. The doctrine map: all 41 mission bytes ─────────────────────────────

TEST(CampaignCombatRole, MapsMissionBytesToDoctrineRoles) {
    using R = CampaignCombatRole;
    // The fighting vocabulary: CAP family + Patrol, Sweep, Intercept
    // (incl. ALERT), Escort (incl. SEADESCORT).
    const std::uint8_t fighters[] = {
        1, 2, 3, 4, 5, 6, 36,   // BARCAP family + PATROL
        7,                       // SWEEP
        8, 9,                    // ALERT, INTERCEPT
        10, 11,                  // ESCORT, SEADESCORT
    };
    for (const auto b : fighters) {
        EXPECT_EQ(campaign_combat_role(b), R::Fighter)
            << "mission byte " << int(b) << " should fight";
    }
    // Everything else defends: the strike family, CAS, recon, support,
    // untasked, and out-of-range bytes (the corrupt-data rule).
    const std::uint8_t defensive[] = {
        0,                       // AMIS_NONE (untasked)
        12, 13, 14, 15, 16, 24,  // strike family + STRATBOMB
        17,                      // SEAD
        18, 19, 20, 21, 23, 31,  // CAS family + BAI + FAC
        22, 29, 30,              // recon family
        25, 26, 27, 28,          // AWACS/JSTAR/TANKER/ECM
        32, 33, 34, 35,          // SAR/AIRLIFT/ASW/ASHIP
        37, 38,                  // TRAINING/OTHER
        39, 40,                  // TANK/SEARCH
        41, 200,                 // out of range
    };
    for (const auto b : defensive) {
        EXPECT_EQ(campaign_combat_role(b), R::Defensive)
            << "mission byte " << int(b) << " should defend";
    }
}

// ── 2. The fighter arm ─────────────────────────────────────────────────────

TEST(ArmCampaignCombat, FighterGetsComponentsLoadoutAndPolicy) {
    f4::data::AircraftConfig cfg;
    if (!load_f16(cfg)) GTEST_SKIP() << "f16.json fixture not generated";
    f4::data::BrainData brains;
    if (!load_brain_data(brains)) GTEST_SKIP()
        << "simdata/braindata.json fixture not generated";

    EntityWorld world;
    const auto id = make_campaign_aircraft(
        world, cfg, /*mission_byte=*/9 /* INTERCEPT */, "blue",
        100000.0, /*bomb_stations=*/0);
    auto h = EntityHandle(id, &world);

    const auto table = f4::weapons::WeaponClassTable::with_builtins();
    std::unique_ptr<RadarBackedDetectionPolicy> policy;
    const auto out = arm_campaign_combat(
        h, table, /*seed_base=*/0x46344u, /*arm_index=*/0,
        /*hit_points=*/25.0,
        /*bvr_hold=*/false, /*missiles_hold=*/false, /*guns_hold=*/false,
        &brains, &policy);

    EXPECT_TRUE(out.armed);
    EXPECT_EQ(out.role, CampaignCombatRole::Fighter);
    EXPECT_TRUE(out.components_attached);
    EXPECT_TRUE(out.policy_created);
    EXPECT_EQ(out.aa_stations, 7)
        << "doctrine fill: 4x AMRAAM + 2x AIM-9 + the M61 drum";
    EXPECT_GT(out.gun_rounds, 0) << "the gun drum from the M61A1 station";
    EXPECT_TRUE(out.archetype.empty()) << "fighters fly the default brain";

    // The components the M3 set attaches.
    EXPECT_NE(h.get<RadarSimComponent>(), nullptr);
    EXPECT_NE(h.get<RwrComponent>(), nullptr);
    EXPECT_NE(h.get<SignatureComponent>(), nullptr);
    EXPECT_NE(h.get<DamageStateComponent>(), nullptr);
    EXPECT_NE(h.get<GunComponent>(), nullptr);
    EXPECT_NE(h.get<CampaignIdentityComponent>(), nullptr);
    auto* dmg = h.get<DamageStateComponent>();
    EXPECT_DOUBLE_EQ(dmg->hit_points, 25.0);
    EXPECT_FALSE(dmg->killed);

    // The brain fights: combat on, archetype off, detection policy the
    // caller owns (installed by the Simulation path; here we hold it).
    auto* brain = h.get<BrainComponent>();
    ASSERT_NE(brain, nullptr);
    EXPECT_TRUE(brain->combat_enabled());
    EXPECT_EQ(brain->brain_archetype(), nullptr);
    ASSERT_NE(policy, nullptr);
    EXPECT_EQ(policy->ownship_id(), id.value);

    // The store: no wire A/A existed, the doctrine fill added 6 stations
    // (4 AMRAAM x 2 rounds + 2 AIM-9 x 1 round).
    auto* store = h.get<WeaponStoreComponent>();
    ASSERT_NE(store, nullptr);
    EXPECT_EQ(store->count_for(table.find_by_name("AIM-120C")), 8);
    EXPECT_EQ(store->count_for(table.find_by_name("AIM-9M")), 2);
}

// ── 3. The defensive arm ───────────────────────────────────────────────────

TEST(ArmCampaignCombat, DefensiveRoleStandsDownThroughArchetype) {
    f4::data::AircraftConfig cfg;
    if (!load_f16(cfg)) GTEST_SKIP() << "f16.json fixture not generated";
    f4::data::BrainData brains;
    if (!load_brain_data(brains)) GTEST_SKIP()
        << "simdata/braindata.json fixture not generated";

    EntityWorld world;
    // A strike flight carrying doctrine bombs (the A-G slice's shape).
    const auto id = make_campaign_aircraft(
        world, cfg, /*mission_byte=*/14 /* STRIKE */, "red",
        200000.0, /*bomb_stations=*/2);
    auto h = EntityHandle(id, &world);

    const auto table = f4::weapons::WeaponClassTable::with_builtins();
    std::unique_ptr<RadarBackedDetectionPolicy> policy;
    const auto out = arm_campaign_combat(
        h, table, 0x46344u, /*arm_index=*/1, 25.0,
        false, false, true, &brains, &policy);

    EXPECT_TRUE(out.armed);
    EXPECT_EQ(out.role, CampaignCombatRole::Defensive);
    EXPECT_EQ(out.aa_stations, 0) << "defensive roles never release A/A";
    EXPECT_EQ(out.gun_rounds, 0) << "no gun station on a disengaged jet";

    // The doctrine: a disengaged archetype is installed (engagement
    // modes off, missile defense on — the shipped .brn shape).
    auto* brain = h.get<BrainComponent>();
    ASSERT_NE(brain, nullptr);
    ASSERT_NE(brain->brain_archetype(), nullptr);
    const auto* arch = brain->brain_archetype();
    EXPECT_FALSE(arch->name.empty());
    EXPECT_FALSE(arch->mode_enabled(f4::data::BrainModeKey::BVREngage))
        << "the archetype must stand BVR down";
    EXPECT_FALSE(arch->mode_enabled(f4::data::BrainModeKey::WVREngage))
        << "the archetype must stand WVR down";
    EXPECT_TRUE(arch->mode_enabled(f4::data::BrainModeKey::MissileDefeat))
        << "defense stays armed (doctrine, not aggression)";
    // Combat is ON (the defensive rungs run) and hold_fire is OFF —
    // the strike rung keeps dropping its bombs.
    EXPECT_TRUE(brain->combat_enabled());
    EXPECT_FALSE(brain->hold_fire());

    // The damage endpoint exists (defensive jets are killable), the
    // RWR exists (the defeat rung's sensor), no gun component.
    EXPECT_NE(h.get<DamageStateComponent>(), nullptr);
    EXPECT_NE(h.get<RwrComponent>(), nullptr);
    EXPECT_EQ(h.get<GunComponent>(), nullptr);

    // The strike store kept its bombs.
    auto* store = h.get<WeaponStoreComponent>();
    ASSERT_NE(store, nullptr);
    EXPECT_EQ(store->count_for(table.find_by_name("MK-82")), 4);
}

// ── 4. Idempotence ─────────────────────────────────────────────────────────

TEST(ArmCampaignCombat, SecondArmIsANoOp) {
    f4::data::AircraftConfig cfg;
    if (!load_f16(cfg)) GTEST_SKIP() << "f16.json fixture not generated";
    f4::data::BrainData brains;
    if (!load_brain_data(brains)) GTEST_SKIP()
        << "simdata/braindata.json fixture not generated";

    EntityWorld world;
    const auto id = make_campaign_aircraft(
        world, cfg, /*mission_byte=*/1 /* BARCAP */, "blue",
        100000.0, 0);
    auto h = EntityHandle(id, &world);
    const auto table = f4::weapons::WeaponClassTable::with_builtins();

    std::unique_ptr<RadarBackedDetectionPolicy> p0;
    const auto first = arm_campaign_combat(
        h, table, 0x46344u, 0, 25.0, false, false, false, &brains, &p0);
    ASSERT_TRUE(first.armed);

    // Re-arm: armed=false, "<already armed>", and no components move
    // (the component census is stable).
    const auto before = h.world()->size();
    std::unique_ptr<RadarBackedDetectionPolicy> p1;
    const auto second = arm_campaign_combat(
        h, table, 0x46344u, 1, 25.0, false, false, false, &brains, &p1);
    EXPECT_FALSE(second.armed);
    EXPECT_EQ(second.archetype, "<already armed>");
    EXPECT_FALSE(second.policy_created);
    EXPECT_EQ(h.world()->size(), before);
}

// ── 5. Non-campaign entities ───────────────────────────────────────────────

TEST(ArmCampaignCombat, RejectsEntitiesWithoutCampaignOrigin) {
    f4::data::AircraftConfig cfg;
    if (!load_f16(cfg)) GTEST_SKIP() << "f16.json fixture not generated";
    f4::data::BrainData brains;
    (void)brains;

    EntityWorld world;
    // A brain + store but NO origin: the scenario-list shape. The arm
    // is a campaign-only API — it must refuse.
    auto h = world.create();
    (void)h.add<BrainComponent>();
    (void)h.add<WeaponStoreComponent>();
    EntityHandle hh(h.id(), &world);

    const auto table = f4::weapons::WeaponClassTable::with_builtins();
    const auto out = arm_campaign_combat(
        hh, table, 0x46344u, 0, 25.0, false, false, false,
        nullptr, nullptr);
    EXPECT_FALSE(out.armed);
    EXPECT_FALSE(out.components_attached);
}

// ── 6. The Simulation surface: arm a late spawn, tick it ───────────────────

TEST(SimulationArmCampaign, ArmsLateSpawnAndBooksTheCounters) {
    f4::data::AircraftConfig cfg;
    if (!load_f16(cfg)) GTEST_SKIP() << "f16.json fixture not generated";

    // The anchor scenario: one airborne scenario-list aircraft (combat
    // on so the tick's combat sweeps run; campaign_armed on so the
    // brain-data eager load runs — the scenario-list spawn arms the
    // anchor through the M3 path, NOT the counters).
    const auto f16 = generated_fixture("f16.json");
    const std::string json = R"({
  "name": "campaign_arm",
  "theater": "korea",
  "combat": { "enabled": true, "campaign_armed": true },
  "aircraft": [
    { "callsign": "ANCHOR", "aircraft_config_path": ")" + f16 + R"(",
      "aircraft_name": "F-16C_50", "vis_type_index": 1052,
      "parking_spot": { "x": 0.0, "y": 0.0, "z": 10000.0 },
      "heading_rad": 0.0, "initial_vt_fps": 500.0,
      "spawn_in_air": true, "team": "blue" }
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
  "sim_dt": 0.016666666666666,
  "total_ticks": 60000,
  "record": false
})";
    auto scenario = load_scenario_from_string(json);
    Simulation sim(std::move(scenario), std::filesystem::path("."));
    sim.initialize();

    // The anchor armed through the SCENARIO path: counters still zero
    // (campaign arming is the campaign-spawn surface only).
    EXPECT_EQ(sim.campaign_armed_aircraft(), 0);

    // The late-comer: the campaign shape (origin + brain + store + FM),
    // the composition the spawner materializes.
    const auto id = make_campaign_aircraft(
        sim.world(), cfg, /*mission_byte=*/1 /* BARCAP */, "red",
        600000.0 /* ~100 NM north — beyond the anchor's radar */, 0);
    EXPECT_TRUE(sim.register_aircraft(id));

    // The C6 surface under test: register then ARM (the session's
    // adopt cadence pairing).
    EXPECT_TRUE(sim.arm_campaign_aircraft(id));
    EXPECT_EQ(sim.campaign_armed_aircraft(), 1);
    EXPECT_EQ(sim.campaign_armed_fighters(), 1);
    EXPECT_EQ(sim.campaign_armed_defensive(), 0);

    // Idempotence through the sim method too.
    EXPECT_FALSE(sim.arm_campaign_aircraft(id));
    EXPECT_EQ(sim.campaign_armed_aircraft(), 1);

    // The entity's shape: combat components + fighting brain.
    {
        auto h = EntityHandle(id, &sim.world());
        EXPECT_NE(h.get<RadarSimComponent>(), nullptr);
        EXPECT_NE(h.get<RwrComponent>(), nullptr);
        auto* brain = h.get<BrainComponent>();
        ASSERT_NE(brain, nullptr);
        EXPECT_TRUE(brain->combat_enabled());
    }

    // 5 seconds of sim: the radar scans (the combat clocks run), the
    // brain sees an empty sky (the anchor is ~100 NM out, beyond the
    // 40 NM detection knee), nothing crashes, the roster identity holds.
    for (int i = 0; i < 5 * 60; ++i) sim.tick(kDt);
    EXPECT_EQ(static_cast<int>(sim.aircraft_entities().size()), 2);
    EXPECT_EQ(sim.campaign_armed_aircraft(), 1);

    // A defensive late-comer arms too (the counters split by role).
    const auto id2 = make_campaign_aircraft(
        sim.world(), cfg, /*mission_byte=*/33 /* AIRLIFT */, "red",
        700000.0, 0);
    EXPECT_TRUE(sim.register_aircraft(id2));
    EXPECT_TRUE(sim.arm_campaign_aircraft(id2));
    EXPECT_EQ(sim.campaign_armed_aircraft(), 2);
    EXPECT_EQ(sim.campaign_armed_defensive(), 1);
    for (int i = 0; i < 60; ++i) sim.tick(kDt);
}

// ── 7. The armed war over the routed kunsan rig ────────────────────────────

TEST(ArmedWar, RunsGreenArmsAircraftAndStaysDeterministic) {
    const auto f16 = generated_fixture("f16.json");
    if (f16.empty()) GTEST_SKIP() << "f16.json fixture not generated";

    WarHarnessOptions o;
    o.session.world_json =
        std::filesystem::path(F4_SIMULATION_TEST_FIXTURES_DIR) /
        "kunsan_session.world.json";
    o.session.class_table =
        std::filesystem::path(F4_SOURCE_FIXTURES_DIR) / "FALCON4.ct";
    o.session.aircraft_config = f16;
    o.session.mission_profiles = F4_MISSION_PROFILES_JSON;
    o.session.tasking_cycle_sec = 5;
    o.session.reinforce_period_sec = 0;
    o.session.atm_pipeline = false;
    o.session.max_flights = 8;
    o.session.aa_combat = true;  // C6: the armed war
    o.horizon_sec = 40;
    o.sample_sec = 10.0;
    o.runs = 2;
    o.wreck_hold_sec = 60.0;

    std::string err;
    auto harness = CampaignWarHarness::create(o, &err);
    ASSERT_NE(harness, nullptr) << err;
    harness->execute();
    const auto& r = harness->report();

    EXPECT_FALSE(r.aborted) << r.abort_reason;
    // The C5 gates still hold on the ARMED war.
    EXPECT_TRUE(r.verdict.deterministic) << "armed war must be deterministic";
    EXPECT_TRUE(r.verdict.ledger_consistent) << r.verdict.ledger_drift;
    EXPECT_TRUE(r.verdict.entities_bounded) << r.verdict.entity_leak;
    EXPECT_TRUE(r.verdict.war_alive) << r.verdict.war_stall;
    // The armed war's provenance + the C6 counters.
    EXPECT_TRUE(r.aa_combat);
    EXPECT_GT(r.armed_aircraft, 0) << "the rig's synthetic flights "
                                      "materialized but none armed";
    // Determinism certificate shape (run 0 + run 1 both present).
    EXPECT_EQ(r.verdict.ledger_md5_run0.size(), 32u);
    EXPECT_EQ(r.verdict.ledger_md5_run1.size(), 32u);
    EXPECT_EQ(r.verdict.ledger_md5_run0, r.verdict.ledger_md5_run1);
}
