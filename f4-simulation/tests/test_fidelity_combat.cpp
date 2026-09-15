// f4-simulation/tests/test_fidelity_combat.cpp
//
// FID-5 — event-driven combat deagg (Docs/FIDELITY_TIERS_PLAN.md
// §4.5–4.6), end to end over a crafted two-side war:
//
//   1. A generated mission (MissionIntent) registers as an AGGREGATE —
//      the spawner defers, no aircraft materialize (the FID-6
//      certificate's "the war's live aircraft are all synthetic" lever).
//   2. The control: deferral OFF spawns straight to Tier-B — the
//      pre-FID-5 shape, byte for byte.
//   3. The synthetic flight's takeoff window ground-spawns it (the TOT
//      anchor + the ops machinery from FID-3/4, now riding intents).
//   4. Two opposing high-altitude aggregates whose predicted tracks
//      converge inside the engagement envelope deaggregate BOTH —
//      the fight runs in-sim, seeded, deterministic (§4.5 trigger B).
//   5. The combat window pins them live, then the standard reagg rules
//      fold them (the transient window; the phase pin).
//   6. A committed Tier-B fighter deaggregates the Tier-A contact it
//      engaged (§4.5 trigger A) — the aggregate feed reached the
//      picture (§4.6) and the radar-backed policy's coarse aggregate
//      rule let the brain commit. The launch veto ate the releases
//      aimed at the phantom in between (deferred_releases).
//   7. combat_deagg=false restores the FID-1..4 shape exactly (no feed,
//      no triggers — the escape hatch the A/B tests ride).
//
// The world: the tier rig's shape, doubled — two squadrons (teams 2/6
// at war), two high-altitude flights on head-on converging routes, and
// a third same-team flight far from the merge (the feed's control).

#include <f4/simulation/campaign_session.hpp>
#include <f4/campaign/campaign.hpp>
#include <f4/flight/flight_model_component.hpp>
#include <f4/entities/entity.hpp>

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>

using namespace f4::simulation;
using f4::entities::EntityHandle;

namespace {

std::filesystem::path class_table_path() {
    return std::filesystem::path(F4_SOURCE_FIXTURES_DIR) / "falcon4.ct.json";
}

std::filesystem::path f16_config_path() {
    return std::filesystem::path(F4_GENERATED_FIXTURES_DIR) / "f16.json";
}

constexpr std::int64_t kNow = 38574360;

// The combat world: teams 2 (ROK) and 6 (DPRK) at war; one airbase;
// two squadrons; flight 5001 (team 2) eastbound at 20,000 ft from
// (400,455); flight 5002 (team 6) westbound at 22,000 ft from
// (424,457) — 24.8 kft apart, head-on, closing ~410 ft/s: the
// predicted miss inside the default 30-kft envelope at the 120-s
// lookahead. Flight 5003 (team 2) cruises far south of the merge —
// same team as 5001, no convergence, never deaggs: the feed's control.
std::string combat_world_json() {
    return R"({
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
    "count": 6,
    "decoded": 6,
    "items": [
      {"type": 200, "id_num": 4281, "unit_class": "squadron",
       "entity_type": 273, "domain": 2,
       "x": 390, "y": 455, "z": 0, "owner": 2,
       "airbase_id": 4101, "class_name": "52 TFS PAK"},
      {"type": 200, "id_num": 4282, "unit_class": "squadron",
       "entity_type": 273, "domain": 2,
       "x": 390, "y": 455, "z": 0, "owner": 6,
       "airbase_id": 4101, "class_name": "105 FES"},
      {"type": 200, "id_num": 5001, "unit_class": "flight",
       "entity_type": 273, "domain": 2,
       "x": 400, "y": 455, "z": 20000, "flight_altitude": 20000,
       "owner": 2, "mission": 7, "squadron_id": 4281,
       "package_id": 7029, "time_on_target": 43739352,
       "waypoints": [
         {"x": 400, "y": 455, "z": 20000, "action": 15},
         {"x": 470, "y": 455, "z": 20000, "action": 17}
       ]},
      {"type": 200, "id_num": 5002, "unit_class": "flight",
       "entity_type": 273, "domain": 2,
       "x": 424, "y": 457, "z": 22000, "flight_altitude": 22000,
       "owner": 6, "mission": 7, "squadron_id": 4282,
       "package_id": 7030, "time_on_target": 43739352,
       "waypoints": [
         {"x": 424, "y": 457, "z": 22000, "action": 15},
         {"x": 354, "y": 457, "z": 22000, "action": 17}
       ]},
      {"type": 200, "id_num": 5003, "unit_class": "flight",
       "entity_type": 273, "domain": 2,
       "x": 300, "y": 400, "z": 18000, "flight_altitude": 18000,
       "owner": 2, "mission": 7, "squadron_id": 4281,
       "package_id": 7031, "time_on_target": 43739352,
       "waypoints": [
         {"x": 300, "y": 400, "z": 18000, "action": 15},
         {"x": 330, "y": 400, "z": 18000, "action": 17}
       ]},
      {"type": 200, "id_num": 6001, "unit_class": "battalion",
       "entity_type": 180, "domain": 3,
       "x": 390, "y": 455, "z": 0, "owner": 6,
       "vehicle_groups": [
         {"group": 0, "vehicle_type": 101, "count": 3, "live_count": 3}
       ]}
    ]
  }
})";
}

// The tier rig's single-flight world, reused for the synthetic-intent
// tests (a friendly base to spawn from; the flight gives the engine its
// baseline aggregate).
std::string synthetic_world_json() {
    return R"({
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
    "count": 3,
    "decoded": 3,
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
       ]},
      {"type": 200, "id_num": 6001, "unit_class": "battalion",
       "entity_type": 180, "domain": 3,
       "x": 390, "y": 455, "z": 0, "owner": 6,
       "vehicle_groups": [
         {"group": 0, "vehicle_type": 101, "count": 3, "live_count": 3}
       ]}
    ]
  }
})";
}

std::filesystem::path make_temp_dir(const char* tag) {
    static std::atomic<unsigned> counter{0};
    const auto dir = std::filesystem::temp_directory_path() /
                     (std::string(tag) + "_" +
                      std::to_string(counter.fetch_add(1)) + "_" +
                      std::to_string(
                          std::chrono::steady_clock::now()
                              .time_since_epoch()
                              .count()));
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    return dir;
}

struct CombatRig {
    std::filesystem::path dir;
    std::filesystem::path world;
    std::unique_ptr<CampaignSession> session;

    static CombatRig make(CampaignSessionOptions opts,
                          const std::string& world_json,
                          const char* tag = "f4_fid5") {
        CombatRig rig;
        rig.dir = make_temp_dir(tag);
        rig.world = rig.dir / "combat.world.json";
        {
            std::ofstream f(rig.world);
            f << world_json;
        }
        opts.world_json = rig.world;
        opts.class_table = class_table_path();
        opts.aircraft_config = f16_config_path();
        opts.mission_profiles = F4_MISSION_PROFILES_JSON;
        // No ladder-generated intents in the window the tests advance:
        // the assertions count only what each test publishes itself.
        opts.tasking_cycle_sec = 1000000;
        opts.reinforce_period_sec = 0;
        opts.atm_pipeline = false;
        std::string err;
        rig.session = CampaignSession::create(opts, &err);
        EXPECT_NE(rig.session, nullptr) << err;
        return rig;
    }

    // One synthetic generated mission on the session's bus (the same
    // publish the ladder's tasking cycle makes). TOT is RELATIVE to the
    // ladder's clock (the intent's own contract).
    static f4::campaign::MissionIntent make_intent(
        std::uint32_t flight_id, std::uint8_t team,
        std::int64_t tot_relative) {
        f4::campaign::MissionIntent in;
        in.synthetic = true;
        in.flight_id = flight_id;
        in.package_id = flight_id;
        in.squadron_id = 4281;
        in.team = team;
        in.mission_byte = 13;
        in.mission_name = "Sweep";
        in.aircraft_count = 2;
        in.issued_time = 0;
        in.time_on_target = tot_relative;
        f4::campaign::RouteWaypoint wp;
        wp.x = 390; wp.y = 455; wp.altitude_ft = 0; wp.action = 1;
        in.route.push_back(wp);
        wp.x = 430; wp.y = 470; wp.altitude_ft = 2500; wp.action = 15;
        in.route.push_back(wp);
        wp.x = 390; wp.y = 455; wp.altitude_ft = 0; wp.action = 7;
        in.route.push_back(wp);
        return in;
    }

    void publish(const f4::campaign::MissionIntent& in) {
        session->sim().bus().publish(in);
    }
};

CampaignSessionOptions combat_opts() {
    CampaignSessionOptions o;
    o.fidelity_policy = FidelityPolicy::Tiered;
    o.max_flights = 8;
    o.max_steps_per_advance = 400000;
    return o;
}

std::uint32_t synthetic_vu(std::uint32_t flight_id) {
    return 0x53590000u | (flight_id & 0xFFFFu);
}

} // namespace

// ── 1. A generated mission registers as an AGGREGATE ───────────────────────

TEST(SyntheticAggregates, IntentRegistersAsAggregateNotTierB) {
    if (!std::filesystem::exists(f16_config_path())) {
        GTEST_SKIP() << "f16.json fixture not generated";
    }
    auto opts = combat_opts();
    auto rig = CombatRig::make(opts, synthetic_world_json(), "f4_fid5_syn");
    ASSERT_NE(rig.session, nullptr);
    ASSERT_TRUE(rig.session->tiered());
    EXPECT_EQ(rig.session->stats().agg_flights, 1);

    // The TOT sits beyond every window: the flight registers and holds.
    const auto intent = CombatRig::make_intent(7, 6, 3600);
    rig.publish(intent);

    // The session heard it FIRST: the aggregate engine grew, the
    // spawner deferred (no Tier-B aircraft materialized).
    EXPECT_EQ(rig.session->stats().synthetic_aggregates, 1);
    EXPECT_EQ(rig.session->stats().agg_flights, 2);
    EXPECT_EQ(rig.session->stats().live_aircraft, 0);
    EXPECT_EQ(rig.session->spawner_stats().synthetic_deferred, 1);
    EXPECT_EQ(rig.session->spawner_stats().synthetic_spawned, 0);

    // The synthetic flight is a first-class tier citizen (the flights
    // table sees it; the canvas draws it) under its reserved-namespace
    // vu, parked at its route's takeoff waypoint.
    const auto tiers = rig.session->flight_tiers();
    ASSERT_EQ(tiers.size(), 2u);
    EXPECT_EQ(tiers[1].vu, synthetic_vu(7));
    EXPECT_EQ(tiers[1].team, 6);
    EXPECT_EQ(tiers[1].aircraft_count, 2);
    EXPECT_FALSE(tiers[1].live);
    EXPECT_DOUBLE_EQ(tiers[1].x_grid, 390.0);

    // Holding: no aircraft, no movement in the first window.
    rig.session->advance(60.0);
    EXPECT_EQ(rig.session->stats().live_aircraft, 0);
}

// ── 2. The control: deferral OFF is the pre-FID-5 shape ────────────────────

TEST(SyntheticAggregates, DeferralOffSpawnsStraightToTierB) {
    if (!std::filesystem::exists(f16_config_path())) {
        GTEST_SKIP() << "f16.json fixture not generated";
    }
    auto opts = combat_opts();
    opts.synthetic_as_aggregates = false;
    auto rig = CombatRig::make(opts, synthetic_world_json(), "f4_fid5_syn2");
    ASSERT_NE(rig.session, nullptr);

    const auto intent = CombatRig::make_intent(7, 6, 3600);
    rig.publish(intent);

    // The spawner materialized the intent's aircraft directly; the
    // aggregate engine never grew. The adopt cadence puts it on the
    // SIM's roster at the next campaign second (the one-world closure).
    rig.session->advance(2.0);
    EXPECT_EQ(rig.session->stats().synthetic_aggregates, 0);
    EXPECT_EQ(rig.session->stats().agg_flights, 1);
    EXPECT_EQ(rig.session->spawner_stats().synthetic_spawned, 1);
    EXPECT_EQ(rig.session->spawner_stats().synthetic_deferred, 0);
    EXPECT_EQ(rig.session->stats().live_aircraft, 1);
}

// ── 3. The synthetic takeoff window ground-spawns ──────────────────────────

TEST(SyntheticAggregates, TakeoffWindowGroundSpawns) {
    if (!std::filesystem::exists(f16_config_path())) {
        GTEST_SKIP() << "f16.json fixture not generated";
    }
    auto opts = combat_opts();
    opts.ops_window_sec = 600;
    auto rig = CombatRig::make(opts, synthetic_world_json(), "f4_fid5_syn3");
    ASSERT_NE(rig.session, nullptr);

    // TOT 300 s out: the takeoff gate clamps to the next second — the
    // takeoff window (and the TOT window behind it) fires immediately,
    // and a flight that has not departed GROUND-spawns (the ATC path).
    const auto intent = CombatRig::make_intent(7, 6, 300);
    rig.publish(intent);
    EXPECT_EQ(rig.session->stats().live_aircraft, 0);   // publish ≠ deagg

    rig.session->advance(2.0);
    EXPECT_EQ(rig.session->stats().tier_deaggs, 1);
    EXPECT_EQ(rig.session->stats().live_aircraft, 1);
    EXPECT_EQ(rig.session->stats().agg_live, 1);

    // The spawned aircraft is ON THE GROUND at its base (inAir false —
    // an air spawn at deck altitude is never the takeoff answer).
    ASSERT_EQ(rig.session->sim().aircraft_entities().size(), 1u);
    EntityHandle h(rig.session->sim().aircraft_entities().front(),
                   &rig.session->sim().world());
    auto* fm = h.get<f4::flight::FlightModelComponent>();
    ASSERT_NE(fm, nullptr);
    EXPECT_FALSE(fm->model().state().gear.inAir);
}

// ── 4. Convergence: the §4.5 trigger B ──────────────────────────────────────

TEST(CombatDeagg, ConvergingAggregatesDeaggregateBoth) {
    if (!std::filesystem::exists(f16_config_path())) {
        GTEST_SKIP() << "f16.json fixture not generated";
    }
    auto opts = combat_opts();
    opts.combat_window_sec = 120;
    auto rig = CombatRig::make(opts, combat_world_json(), "f4_fid5_conv");
    ASSERT_NE(rig.session, nullptr);
    EXPECT_EQ(rig.session->stats().agg_flights, 3);

    // Two sim seconds: the per-second block crosses its whole-second
    // gate (60 ticks land exactly ON the 1.0 floating-point boundary —
    // the FID-3 tests' own lesson), the tier pass runs, then the combat
    // pass — the closing head-on pair's predicted tracks land inside
    // the 30-kft envelope at the 120-s lookahead; BOTH deaggregate (the
    // same-team third flight stays aggregate).
    rig.session->advance(2.0);
    const auto& st = rig.session->stats();
    EXPECT_EQ(st.tier_deaggs, 2);
    EXPECT_EQ(st.combat_deaggs, 2);
    EXPECT_EQ(st.agg_live, 2);
    EXPECT_EQ(st.live_aircraft, 2);
    // The feed dropped the deaggregated pair; the far same-team flight
    // is still a published contact (the picture's aggregate form).
    EXPECT_EQ(st.agg_contacts, 1);

    // Both spawned AIRBORNE at their aggregates' poses (20k/22k ft —
    // an air spawn; the flights had departed long before).
    for (const auto eid : rig.session->sim().aircraft_entities()) {
        EntityHandle h(eid, &rig.session->sim().world());
        auto* fm = h.get<f4::flight::FlightModelComponent>();
        ASSERT_NE(fm, nullptr);
        EXPECT_TRUE(fm->model().state().gear.inAir);
    }
}

// ── 5. The combat window pins, then the standard rules fold ─────────────────

TEST(CombatDeagg, CombatWindowPinsThenFolds) {
    if (!std::filesystem::exists(f16_config_path())) {
        GTEST_SKIP() << "f16.json fixture not generated";
    }
    auto opts = combat_opts();
    opts.combat_window_sec = 120;
    opts.deagg_cooldown_sec = 30.0;
    auto rig = CombatRig::make(opts, combat_world_json(), "f4_fid5_pin");
    ASSERT_NE(rig.session, nullptr);

    rig.session->advance(2.0);
    ASSERT_EQ(rig.session->stats().agg_live, 2);

    // Inside the window the pin holds: 60 more seconds and the fight
    // is still live (the phase pin — no bubble hysteresis applies).
    rig.session->advance(60.0);
    EXPECT_EQ(rig.session->stats().agg_live, 2);
    EXPECT_EQ(rig.session->stats().tier_reaggs, 0);

    // Past the window (120) + cooldown (30): headless, unobserved —
    // the standard reagg rules fold both back into aggregates.
    rig.session->advance(120.0);
    EXPECT_EQ(rig.session->stats().agg_live, 0);
    EXPECT_GE(rig.session->stats().tier_reaggs, 2);
}

// ── 6. Commit: the Tier-B fighter deaggregates the contact it fought ────────

TEST(CombatDeagg, CommittedFighterDeaggregatesItsContact) {
    if (!std::filesystem::exists(f16_config_path())) {
        GTEST_SKIP() << "f16.json fixture not generated";
    }
    auto opts = combat_opts();
    opts.aa_combat = true;          // the armed war: the full ladder
    opts.combat_window_sec = 600;
    auto rig = CombatRig::make(opts, combat_world_json(), "f4_fid5_commit");
    ASSERT_NE(rig.session, nullptr);

    // Force the observer: flight 5001 (team 2) deaggregated and armed —
    // its radar-backed policy now carries the aggregate set, and the
    // feed publishes 5002 (22 kft, closing, hostile) into the picture.
    rig.session->force_deaggregate_flight(5001);
    ASSERT_EQ(rig.session->stats().agg_live, 1);

    // Forty sim seconds: the fusion rebuilds on its skill cadence, the
    // coarse aggregate rule paints 5002 radar-visible (well inside the
    // reference card), the BVR rung commits — and the next combat pass
    // deaggregates the flight the brain engaged (§4.5 trigger A).
    rig.session->advance(40.0);
    const auto& st = rig.session->stats();
    EXPECT_GE(st.combat_deaggs, 1) << "the committed brain never pulled "
                                      "its contact out of the aggregate";
    EXPECT_GE(st.tier_deaggs, 2);
    EXPECT_EQ(st.agg_live, 2);
    EXPECT_EQ(st.live_aircraft, 2);

    // The commit window's own load: releases aimed at the phantom id
    // were vetoed, never flown (an aggregate resolves to no entity —
    // a missile at it would be a ghost).
    EXPECT_GT(rig.session->stats().deferred_releases, 0)
        << "the fighter released against the aggregate id — the veto "
           "did not stand";
}

// ── 7. The escape hatch: combat_deagg=false is FID-1..4 exactly ─────────────

TEST(CombatDeagg, DisarmedSessionKeepsTheFid14Shape) {
    if (!std::filesystem::exists(f16_config_path())) {
        GTEST_SKIP() << "f16.json fixture not generated";
    }
    auto opts = combat_opts();
    opts.combat_deagg = false;
    auto rig = CombatRig::make(opts, combat_world_json(), "f4_fid5_off");
    ASSERT_NE(rig.session, nullptr);

    // The same converging pair, the same hour: no combat triggers, no
    // feed, no deaggs — the pre-FID-5 tiered session's silence.
    rig.session->advance(60.0);
    const auto& st = rig.session->stats();
    EXPECT_EQ(st.tier_deaggs, 0);
    EXPECT_EQ(st.combat_deaggs, 0);
    EXPECT_EQ(st.agg_live, 0);
    EXPECT_EQ(st.live_aircraft, 0);
    EXPECT_EQ(st.agg_contacts, 0);
}
