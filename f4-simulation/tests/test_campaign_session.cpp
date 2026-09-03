// f4-world-viewer/tests/test_campaign_session.cpp
//
// V-CAMP — the live campaign session over the kunsan fixture.
//
// CampaignSession is the campaign_qc wiring repackaged for an
// interactive host (see src/campaign_session.hpp). These tests pin the
// headless contract the viewer's render loop depends on:
//
//   1. create() builds the whole graph (sim + ladder + ledger + route
//      builder + spawner + sink) over a world with teams at war.
//   2. advance() runs THE WHOLE LOOP in one world: the ladder ticks
//      with the sim clock, generates missions with routes, the spawner
//      materializes them INTO the sim's world, register_aircraft adds
//      them to the roster (the one-world closure), and the ledger
//      books the draws (C2's one pool).
//   3. Two sessions advanced identically produce byte-identical
//      artifacts (the determinism contract every other harness pins).
//   4. pause stops the drain; a fresh session changes nothing (the C1
//      golden identity, session edition).
//
// The kunsan fixture has NO Flight units and no airbase-class
// objective — which is exactly the degraded path the session exists
// to tolerate (the QC hard-fails both): the scenario falls back to
// the template aircraft, the spawner's parking falls back to the
// route's takeoff waypoint, and the loop still runs. The route-builder
// and spawner behavior over this same fixture is pinned by
// f4-campaign's test_route_builder / f4-simulation's spawner tests;
// here we assert only what the SESSION adds on top.

#include <f4/simulation/campaign_session.hpp>
#include <f4/flight/flight_model_component.hpp>
#include <f4/entities/entity.hpp>

#include <gtest/gtest.h>

#include <cmath>
#include <filesystem>
#include <string>

using namespace f4::simulation;

namespace {

std::filesystem::path kunsan_world() {
    return std::filesystem::path(F4_SIMULATION_TEST_FIXTURES_DIR) /
           "kunsan_campaign.world.json";
}

// The ROUTED-session fixture: kunsan + the one patch f4-campaign's
// RoutePlannerArmsIntentsWithTargetsAndRoutes test applies in-memory
// (the USA squadron → ARO_S strike, airbase 2659). The RAW kunsan
// squadrons are counter-air at airbase 0 — fine for create/pause/
// determinism, but delivery routes (and therefore generation-to-
// spawn) need the endpoints. The patch is the campaign test's own,
// committed as a fixture instead of in-test mutation (the session
// builds the whole graph in create() — no mutation window exists).
std::filesystem::path kunsan_routed_world() {
    return std::filesystem::path(F4_SIMULATION_TEST_FIXTURES_DIR) /
           "kunsan_session.world.json";
}

std::filesystem::path class_table() {
    return std::filesystem::path(F4_SOURCE_FIXTURES_DIR) / "FALCON4.ct";
}

std::filesystem::path f16_config() {
    return std::filesystem::path(F4_GENERATED_FIXTURES_DIR) / "f16.json";
}

// A fast, deterministic rig: the campaign cycle shortened to 5 s so a
// few advance() calls cross it (the real cadence — 1800 s — is the
// viewer default; the C2 campaign tests use the same shortening).
CampaignSessionOptions make_opts(const std::filesystem::path& world) {
    CampaignSessionOptions o;
    o.world_json = world;
    o.class_table = class_table();
    o.aircraft_config = f16_config();
    o.mission_profiles = F4_MISSION_PROFILES_JSON;
    o.tasking_cycle_sec = 5;
    o.reinforce_period_sec = 0;   // off: draws only, no deliveries
    o.max_flights = 8;
    // STRICT role gate: the routed fixture's USA wing IS an ARO_S
    // squadron (the patch), so the exact match exists — and the 24-
    // aircraft pool cannot survive the fallback's counter-air family
    // (bytes 1-11 draw ~2-4 ships each) BEFORE the delivery family
    // (byte 12+) is reached. The fallback is for all-counter-air
    // saves like TestCamp, where the pools are deep enough to feed
    // both families; over this single-wing fixture it starves the
    // routed family this test exists to exercise.
    o.role_fallback = false;
    return o;
}

} // namespace

// ── 1. The graph builds over a flight-less world ───────────────────────────
TEST(CampaignSession, CreatesOverKunsanAndSeesTheWar) {
    if (!std::filesystem::exists(f16_config())) {
        GTEST_SKIP() << "f16.json fixture not generated";
    }
    std::string err;
    auto session = CampaignSession::create(make_opts(kunsan_world()), &err);
    ASSERT_NE(session, nullptr) << "create failed: " << err;

    // The sim is alive with the template aircraft (the flight-less
    // fallback: no saved flights, so the roster is the anchor).
    EXPECT_EQ(session->sim().aircraft_entities().size(), 1u);

    // The war the corrected RelType decode sees: USA(1), ROK(2), DPRK(6)
    // — the threat map is built from the FIRST belligerent's view.
    const auto war = session->campaign().belligerent_teams();
    ASSERT_EQ(war.size(), std::size_t{3});
    EXPECT_EQ(session->threat_viewer_team(), 1u);

    // One clock, anchored on the save's epoch: nothing has moved yet.
    EXPECT_EQ(session->campaign_time(),
              session->world_state().campaign.current_time);
}

// ── 2. advance() runs the whole loop in ONE world ──────────────────────────
TEST(CampaignSession, AdvanceTicksCampaignGeneratesAndMaterializes) {
    if (!std::filesystem::exists(f16_config())) {
        GTEST_SKIP() << "f16.json fixture not generated";
    }
    std::string err;
    auto session = CampaignSession::create(make_opts(kunsan_routed_world()),
                                           &err);
    ASSERT_NE(session, nullptr) << "create failed: " << err;

    const auto t0 = session->stats().sim_time_s;
    // 12 wall seconds in 60-tick frames (the render loop's shape).
    for (int frame = 0; frame < 20; ++frame) {
        session->advance(1.0 / 60.0 * 60);   // 1 sim second per frame
    }
    // 20 frames x 60 ticks x 1/60 s: 1199.99... whole ticks — one
    // frame's accumulator lands a hair under 1.0000 s, so the total is
    // 19.983 s (the fixed-point accumulator's honest arithmetic, not a
    // bug: 60 binary 1/60s sum to 0.99999...). One clock, ~20 s.
    EXPECT_NEAR(session->stats().sim_time_s, t0 + 20.0, 0.05);
    EXPECT_GE(session->campaign().clock(), 19);
    EXPECT_LE(session->campaign().clock(), 20);

    // The cycle (5 s) fired ~4 times and generated missions.
    EXPECT_GE(session->campaign().cycles_fired(), 3);
    EXPECT_FALSE(session->intents().empty());

    // Generation → spawn → REGISTRATION: the spawner materialized
    // synthetic aircraft into the SIM's world and the roster grew.
    EXPECT_GT(session->stats().synthetic_spawned, 0);
    EXPECT_GT(session->stats().live_aircraft, 1);
    EXPECT_EQ(session->stats().live_aircraft,
              static_cast<int>(session->sim().aircraft_entities().size()));

    // Every generated mission with a route has a LIVE aircraft whose
    // transform follows its FM (the one-world closure the QC lacks).
    const auto& roster = session->sim().aircraft_entities();
    int synced = 0;
    for (const auto eid : roster) {
        auto h = f4::entities::EntityHandle(eid, &session->sim().world());
        auto* fm = h.get<f4::flight::FlightModelComponent>();
        auto* tf = h.get<f4::entities::TransformComponent>();
        if (fm == nullptr || tf == nullptr) continue;
        // ENU: tf.y = north (kin.x), tf.x = east (kin.y).
        if (std::abs(tf->position.y - fm->state().kin.x) < 1.0 &&
            std::abs(tf->position.x - fm->state().kin.y) < 1.0) {
            ++synced;
        }
    }
    EXPECT_GE(synced, session->stats().synthetic_spawned)
        << "a spawned aircraft is not transform-synced (roster gap)";

    // The draws booked the ONE pool (C2): availability the next cycle
    // reads already reflects what this run drew.
    EXPECT_GT(session->stats().drawn_aircraft, 0);
    EXPECT_EQ(session->ledger().mission_draw_aircraft(),
              session->stats().drawn_aircraft);
}

// ── 3. Determinism: identical advances → identical artifacts ───────────────
TEST(CampaignSession, IdenticalRunsAreByteIdentical) {
    if (!std::filesystem::exists(f16_config())) {
        GTEST_SKIP() << "f16.json fixture not generated";
    }
    std::string ea, eb;
    const auto opts = make_opts(kunsan_routed_world());
    auto a = CampaignSession::create(opts, &ea);
    auto b = CampaignSession::create(opts, &eb);
    ASSERT_NE(a, nullptr) << ea;
    ASSERT_NE(b, nullptr) << eb;

    for (int frame = 0; frame < 6; ++frame) {
        a->advance(1.0);
        b->advance(1.0);
    }
    EXPECT_EQ(a->campaign().to_summary_json(),
              b->campaign().to_summary_json());
    EXPECT_EQ(a->ledger_json(), b->ledger_json());
}

// ── 4. Pause + the fresh-session identity ──────────────────────────────────
TEST(CampaignSession, PauseStopsTheDrainAndFreshChangesNothing) {
    if (!std::filesystem::exists(f16_config())) {
        GTEST_SKIP() << "f16.json fixture not generated";
    }
    std::string err;
    auto session = CampaignSession::create(make_opts(kunsan_world()), &err);
    ASSERT_NE(session, nullptr) << "create failed: " << err;

    session->advance(1.0);
    const auto t = session->stats().sim_time_s;
    session->set_paused(true);
    session->advance(10.0);   // no-op while paused
    EXPECT_EQ(session->stats().sim_time_s, t);
    EXPECT_TRUE(session->paused());
    session->set_paused(false);

    // The zero-event ledger is the identity element: a session that
    // never crosses a tasking cycle books nothing (C1's golden
    // identity, session edition — cycles cost draws, 1 s costs none).
    auto fresh = CampaignSession::create(make_opts(kunsan_world()), &err);
    ASSERT_NE(fresh, nullptr) << err;
    fresh->advance(1.0);
    EXPECT_TRUE(fresh->ledger().empty());
}
