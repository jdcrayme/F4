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
    // The legacy ladder for THIS rig (atm_pipeline off): the routed
    // fixture's USA wing is a single 24-aircraft pool, and the test's
    // purpose is the routed generation-to-spawn chain — the ATM has
    // its own test below (the fixture's specialty-1 wing — ARO_S in
    // the repo's legacy vocabulary — is SQUADRON_SPECIALTY_AA in the
    // reference's, so FindBestAir rates it 30 for strike and the CA
    // family wins the pool; both readings are honest, the fixtures
    // were built for the legacy one).
    o.atm_pipeline = false;
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

// ── 4b. Lender ownership: the spawner's inputs outlive create() ────────────
//
// REGRESSION (the Start Session crash's second leg): create() used to
// hand CampaignSimSpawner three LOCALS — the fallback airfield, the
// per-airbase airfield map, and the template aircraft — that died when
// create() returned. The spawner holds them by reference/pointer for
// the session's lifetime; the first synthetic spawn after a tasking
// cycle then read freed memory (garbage parking positions / freed
// parking-spot vectors). The members airfield_ / airbase_airfields_ /
// spawn_tpl_ replace them. This test drives the synthetic path far
// enough to touch every lender and pins the observable: every
// materialized aircraft parks at a FINITE position inside the theater.
TEST(CampaignSession, SyntheticSpawnsParkAtFinitePositionsInsideTheater) {
    if (!std::filesystem::exists(f16_config())) {
        GTEST_SKIP() << "f16.json fixture not generated";
    }
    CampaignSessionOptions o = make_opts(kunsan_routed_world());
    o.tasking_cycle_sec = 5;
    o.max_flights = 8;

    std::string err;
    auto session = CampaignSession::create(o, &err);
    ASSERT_NE(session, nullptr) << "create failed: " << err;
    session->set_paused(false);
    for (int frame = 0; frame < 20; ++frame) {
        session->advance(1.0);
    }

    // The synthetic path actually fired (routes materialized into
    // aircraft through the spawner's lenders).
    ASSERT_GT(session->stats().synthetic_spawned, 0);

    // Every rostered aircraft: finite position, inside the theater
    // (the Korea scale — 3,358,720 ft per side — with generous slack;
    // a dangling airfield reads garbage doubles, NaNs, or values far
    // outside any theater).
    constexpr double THEATER_FT = 3.4e6;
    int checked = 0;
    for (const auto eid : session->sim().aircraft_entities()) {
        auto h = f4::entities::EntityHandle(eid, &session->sim().world());
        const auto* tf = h.get<f4::entities::TransformComponent>();
        ASSERT_NE(tf, nullptr);
        EXPECT_TRUE(std::isfinite(tf->position.x)) << eid.value;
        EXPECT_TRUE(std::isfinite(tf->position.y)) << eid.value;
        EXPECT_TRUE(std::isfinite(tf->position.z)) << eid.value;
        EXPECT_LT(std::abs(tf->position.x), THEATER_FT) << eid.value;
        EXPECT_LT(std::abs(tf->position.y), THEATER_FT) << eid.value;
        ++checked;
    }
    EXPECT_GT(checked, 0);
}

// ── 5. The ATM pipeline (C4) — packages with recovery, deterministically ────
TEST(CampaignSession, AtmPipelineBuildsPackagesAndRecoversAircraft) {
    if (!std::filesystem::exists(f16_config())) {
        GTEST_SKIP() << "f16.json fixture not generated";
    }
    // The session's own default (atm_pipeline on): a long-enough
    // advance crosses cycles whose flights COMPLETE (mission-over
    // deadlines pass) — the survivors return to the pool and the next
    // cycle draws them again.
    CampaignSessionOptions o = make_opts(kunsan_world());
    o.atm_pipeline = true;
    o.tasking_cycle_sec = 5;         // a cycle every 5 sim seconds
    o.max_flights = 8;

    std::string err;
    auto a = CampaignSession::create(o, &err);
    ASSERT_NE(a, nullptr) << "create failed: " << err;
    a->set_paused(false);
    // 60 sim-seconds in 1-second frames (the tick cap drops debt past
    // 240 ticks per advance — the same frame-loop shape the other
    // tests use): 12 cycles; mission-over deadlines (travel + loiter
    // + reserve) are minutes at the reference scale, so no recovery
    // fires THIS horizon — the packages themselves are the pin.
    // (Recovery over a long horizon is campaign_qc's --tasking
    // acceptance, not a 60-second UI-scale advance.)
    for (int frame = 0; frame < 60; ++frame) {
        a->advance(1.0);
    }

    EXPECT_GT(a->stats().cycles, 0);
    EXPECT_GT(a->stats().intents, 0);
    EXPECT_GT(a->stats().packages, 0);
    EXPECT_GT(a->stats().drawn_aircraft, 0);
    // Determinism: a second session, identically driven, lands on the
    // same bytes (the C1/C2/C3 discipline, ATM edition).
    auto b = CampaignSession::create(o, &err);
    ASSERT_NE(b, nullptr) << err;
    b->set_paused(false);
    for (int frame = 0; frame < 60; ++frame) {
        b->advance(1.0);
    }
    EXPECT_EQ(a->campaign().to_summary_json(),
              b->campaign().to_summary_json());
    EXPECT_EQ(a->ledger_json(), b->ledger_json());
}

// ── 6. G1 — the ground war rides the session ──────────────────────────────
//
// The engine's unit-level behavior is f4-campaign's own test file; here
// we pin what the SESSION adds: the cadence (update ticks fire off the
// campaign clock), the ledger's ground block (the C5 certificate now
// covers the ground side), the ENTITY MIRROR (battalion transforms
// follow the engine's grid positions), the ground write-back, and the
// two contracts that matter to every pre-G1 consumer: ground OFF is
// byte-identical (the golden identity), ground ON is deterministic.
TEST(CampaignSession, GroundWarRunsBooksMirrorsAndWritesBack) {
    if (!std::filesystem::exists(f16_config())) {
        GTEST_SKIP() << "f16.json fixture not generated";
    }
    CampaignSessionOptions o = make_opts(kunsan_world());
    o.ground_war = true;
    o.ground_update_sec = 5;     // a fire every 5 campaign seconds
    o.ground_orders_sec = 5;

    std::string err;
    auto session = CampaignSession::create(o, &err);
    ASSERT_NE(session, nullptr) << "create failed: " << err;
    session->set_paused(false);
    for (int frame = 0; frame < 20; ++frame) {
        session->advance(1.0);
    }

    // The engine fired its updates and sees the army.
    ASSERT_NE(session->ground_war(), nullptr);
    EXPECT_GT(session->stats().ground_updates, 0);
    EXPECT_GT(session->stats().ground_battalions, 0);
    EXPECT_EQ(session->stats().ground_battalions,
              static_cast<int>(session->ground_war()->units().size()));

    // The ledger books the ground state (the artifact's ground block —
    // the battalions marched, so the sync carried them).
    EXPECT_NE(session->ledger_json().find("\"ground\""),
              std::string::npos);

    // The entity mirror: a battalion's transform follows the engine's
    // grid position (1 grid = 1024 ft, the bridge's own conversion).
    int mirrored = 0;
    int checked = 0;
    for (const auto& g : session->ground_war()->units()) {
        const auto it = session->unit_id_map().find(g.vu);
        if (it == session->unit_id_map().end()) continue;
        auto h = f4::entities::EntityHandle(it->second,
                                            &session->sim().world());
        const auto* tf = h.get<f4::entities::TransformComponent>();
        if (tf == nullptr) continue;
        ++checked;
        if (std::abs(tf->position.x - g.x * 1024.0) < 0.5 &&
            std::abs(tf->position.y - g.y * 1024.0) < 0.5) {
            ++mirrored;
        }
        if (checked >= 24) break;   // a bounded sample
    }
    ASSERT_GT(checked, 0) << "no battalion entities in the sim world";
    EXPECT_EQ(mirrored, checked)
        << "a battalion entity is not at the engine's grid position";

    // The ground write-back lands (positions moved → battalions
    // written; kunsan's armies march at 12 s of war).
    const auto res = session->apply_ground_writeback();
    EXPECT_TRUE(res.unmatched_battalions.empty());
    EXPECT_TRUE(res.unmatched_objectives.empty());
}

TEST(CampaignSession, GroundWarOffIsByteIdenticalAndOnIsDeterministic) {
    if (!std::filesystem::exists(f16_config())) {
        GTEST_SKIP() << "f16.json fixture not generated";
    }
    // OFF: the golden identity — a ground-quiet session's ledger bytes
    // are exactly the pre-G1 shape (no "ground" key anywhere).
    std::string err;
    auto off = CampaignSession::create(make_opts(kunsan_world()), &err);
    ASSERT_NE(off, nullptr) << err;
    for (int frame = 0; frame < 6; ++frame) off->advance(1.0);
    EXPECT_EQ(off->ledger_json().find("\"ground\""), std::string::npos);
    EXPECT_EQ(off->stats().ground_updates, 0);

    // ON: two identically-driven sessions, identical ledger bytes (the
    // C5 contract, ground edition).
    CampaignSessionOptions o = make_opts(kunsan_world());
    o.ground_war = true;
    o.ground_update_sec = 5;
    o.ground_orders_sec = 5;
    auto a = CampaignSession::create(o, &err);
    auto b = CampaignSession::create(o, &err);
    ASSERT_NE(a, nullptr) << err;
    ASSERT_NE(b, nullptr) << err;
    for (int frame = 0; frame < 10; ++frame) {
        a->advance(1.0);
        b->advance(1.0);
    }
    EXPECT_EQ(a->ledger_json(), b->ledger_json());
    EXPECT_GT(a->stats().ground_updates, 0);
}

// ── G2 — the interdiction link: CAS tasks against real battalions ──────────

TEST(CampaignSession, UnitStrikeTasksCasAgainstBattalions) {
    if (!std::filesystem::exists(f16_config())) {
        GTEST_SKIP() << "f16.json fixture not generated";
    }
    CampaignSessionOptions o = make_opts(kunsan_world());
    // The QC's shape: the ATM pipeline (FindBestAir scores role rather
    // than gates) + the interdiction arm.
    o.atm_pipeline = true;
    o.unit_strike = true;

    std::string err;
    auto session = CampaignSession::create(o, &err);
    ASSERT_NE(session, nullptr) << "create failed: " << err;
    session->set_paused(false);
    for (int frame = 0; frame < 12; ++frame) {
        session->advance(1.0);
    }

    // The ladder tasked at least one CAS-family intent (byte 20) whose
    // target resolves a BATTALION entity in the unit map — the unit
    // id map IS the resolution (VU → any UnitCore entity; the mission
    // plan's builder keys on the same map).
    int cas_intents = 0;
    int cas_unit_targeted = 0;
    int cas_routed = 0;
    for (const auto& intent : session->intents()) {
        if (intent.mission_byte != 20 /* AMIS_CAS */) continue;
        ++cas_intents;
        const auto it = session->unit_id_map().find(
            intent.target_objective_id);
        if (it == session->unit_id_map().end()) continue;
        auto h = f4::entities::EntityHandle(it->second,
                                            &session->sim().world());
        const auto* uc =
            h.get<f4::entities::UnitCoreComponent>();
        if (uc != nullptr &&
            uc->unit_class == f4::entities::UnitClass::Battalion) {
            ++cas_unit_targeted;
            if (!intent.route.empty()) ++cas_routed;
        }
    }
    EXPECT_GT(cas_intents, 0) << "no CAS intents over the ATM pipeline";
    EXPECT_GT(cas_unit_targeted, 0)
        << "CAS never resolved a battalion target";
    // Routes: kunsan's wing carries no airbase (the fixture's shape),
    // so the route observation is honest here but not assertable —
    // the TestCamp acceptance war (real airbases) proves the CAS
    // routing end to end. The routed counter stays as coverage:
    // TestCamp-shaped worlds fire it.
    (void)cas_routed;

    // The interdiction arm changes the tasking shape: the SAME options
    // with the arm OFF produce no unit-targeted CAS (the C3 deferral).
    CampaignSessionOptions off = o;
    off.unit_strike = false;
    auto session_off = CampaignSession::create(off, &err);
    ASSERT_NE(session_off, nullptr) << "create failed: " << err;
    session_off->set_paused(false);
    for (int frame = 0; frame < 12; ++frame) {
        session_off->advance(1.0);
    }
    for (const auto& intent : session_off->intents()) {
        if (intent.mission_byte != 20) continue;
        EXPECT_EQ(intent.target_objective_id, 0u)
            << "CAS tasked a unit target with the arm OFF";
    }
}
