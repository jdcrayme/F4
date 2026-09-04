// f4-simulation/tests/test_campaign_session_runner.cpp
//
// V-THREAD + V-3DLIVE — the campaign's own thread and the camera bubble.
//
// CampaignSessionRunner is the worker thread that owns advance() (the
// viewer's old inline per-frame call froze the UI — one advance could
// legally run 240 ticks inside the ImGui frame). These tests pin:
//
//   1. A paused runner advances nothing (the viewer starts paused).
//   2. An unpaused runner advances the session on its own thread —
//      sim time grows WITHOUT the test calling advance() — and read()
//      sees a consistent session concurrently with the worker.
//   3. stop() joins the worker (no leak; idempotent).
//   4. The adaptive tick budget stays in its clamp band while running.
//   5. Session::advance(real, max_steps_override) honors the override
//      (the runner's short-lock-hold mechanism) — deterministic.
//   6. The VIEW BUBBLE: a paused session deaggregates a garrison
//      battalion at the CAMERA position (set_view_bubble) and
//      reaggregates when the bubble moves away (clear_view_bubble
//      returns to the ownship). This is the user-visible "zoom into a
//      ground unit → vehicles appear" behavior.
//
// Fixtures: kunsan_session.world.json (the routed session fixture) for
// the runner, and the crafted garrison world (from the lifetime
// regression — teams at war + airbase + squadron + flight + a
// battalion with vehicle_groups ON the base grid) for the bubble.

#include <f4/simulation/campaign_session.hpp>
#include <f4/simulation/campaign_session_runner.hpp>
#include <f4/simulation/bubble_manager.hpp>
#include <f4/simulation/visual_model_component.hpp>
#include <f4/entities/entity.hpp>

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <thread>

using namespace f4::simulation;
using f4::entities::EntityHandle;
using f4::entities::EntityId;

namespace {

std::filesystem::path kunsan_routed_world() {
    return std::filesystem::path(F4_SIMULATION_TEST_FIXTURES_DIR) /
           "kunsan_session.world.json";
}

std::filesystem::path class_table_path() {
    return std::filesystem::path(F4_SOURCE_FIXTURES_DIR) / "FALCON4.ct";
}

std::filesystem::path f16_config_path() {
    return std::filesystem::path(F4_GENERATED_FIXTURES_DIR) / "f16.json";
}

CampaignSessionOptions make_opts(const std::filesystem::path& world) {
    CampaignSessionOptions o;
    o.world_json = world;
    o.class_table = class_table_path();
    o.aircraft_config = f16_config_path();
    o.mission_profiles = F4_MISSION_PROFILES_JSON;
    o.tasking_cycle_sec = 5;          // short cycle: crosses fast
    o.reinforce_period_sec = 0;       // off: draws only
    o.max_flights = 8;
    o.atm_pipeline = false;           // legacy ladder (fixture-tuned)
    return o;
}

// The crafted garrison world (the lifetime regression's): ROK vs DPRK,
// an AIRBASE objective at grid (390, 455), a squadron + one flight at
// the base, and a DPRK battalion with a 3-vehicle group parked ON the
// base grid — the camera bubble's deagg target.
const char* kGarrisonWorldJson = R"({
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

std::filesystem::path make_temp_dir(const char* tag) {
    static std::atomic<unsigned> counter{0};
    const auto name = std::string(tag) + "_" +
        std::to_string(counter.fetch_add(1)) + "_" +
        std::to_string(static_cast<std::uintptr_t>(
            std::chrono::steady_clock::now().time_since_epoch().count() %
            1000000));
    auto dir = std::filesystem::temp_directory_path() / name;
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    return dir;
}

// A session over the garrison world (paused, as the viewer starts it).
struct GarrisonSession {
    std::unique_ptr<CampaignSession> session;
    std::filesystem::path dir;
};

GarrisonSession make_garrison_session() {
    const auto dir = make_temp_dir("f4_runner");
    const auto world = dir / "garrison.world.json";
    {
        std::ofstream f(world);
        f << kGarrisonWorldJson;
    }

    GarrisonSession out;
    out.dir = dir;
    auto opts = make_opts(world);
    opts.tasking_cycle_sec = 3600;  // quiet: the bubble is the subject
    std::string err;
    out.session = CampaignSession::create(opts, &err);
    return out;
}

} // namespace

// ── 1. A paused runner advances nothing ────────────────────────────────────
TEST(CampaignSessionRunner, PausedRunnerAdvancesNothing) {
    if (!std::filesystem::exists(f16_config_path())) {
        GTEST_SKIP() << "f16.json fixture not generated";
    }
    auto s = make_garrison_session();
    ASSERT_NE(s.session, nullptr);

    CampaignSessionRunner runner(*s.session, 10.0, /*paused=*/true);
    runner.start();
    std::this_thread::sleep_for(std::chrono::milliseconds(250));
    EXPECT_EQ(runner.advanced_sim_seconds(), 0.0);
    EXPECT_TRUE(runner.paused());
    runner.stop();
    runner.stop();  // idempotent
    std::filesystem::remove_all(s.dir);
}

// ── 2. The worker advances the session on its own thread ─────────────────
TEST(CampaignSessionRunner, WorkerAdvancesSessionAndReadsStayConsistent) {
    if (!std::filesystem::exists(f16_config_path())) {
        GTEST_SKIP() << "f16.json fixture not generated";
    }
    std::string err;
    auto session = CampaignSession::create(
        make_opts(kunsan_routed_world()), &err);
    ASSERT_NE(session, nullptr) << "create failed: " << err;
    session->set_paused(false);

    CampaignSessionRunner runner(*session, 10.0, /*paused=*/false);
    runner.start();

    // Let the worker run for a generous slice (slow containers), while
    // a concurrent read() — the fine-grained host path — repeatedly
    // snapshots the stats (the same lock discipline the viewer's frame
    // scope uses, just scoped tighter).
    int reads = 0;
    double last_sim_time = -1.0;
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(600);
    while (std::chrono::steady_clock::now() < deadline) {
        const double t = runner.read([](CampaignSession& cs) {
            return cs.sim().sim_time_s();
        });
        EXPECT_GE(t, last_sim_time);  // monotonic under the lock
        last_sim_time = t;
        ++reads;
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }

    // The worker must have advanced SOMETHING (a 10x worker over 600 ms
    // of wall time owes 6 s of sim; even a starved batch of one tick
    // is 1/60 s — and the kunsan rig runs far faster than that).
    EXPECT_GT(runner.advanced_sim_seconds(), 0.0);
    EXPECT_GT(last_sim_time, 0.0);
    EXPECT_GT(reads, 10);  // the read path never starved either

    // The budget adapted within its clamp band.
    EXPECT_GE(runner.tick_budget(), 1);
    EXPECT_LE(runner.tick_budget(), 1 << 16);

    runner.stop();
}

// ── 3. advance() honors the max_steps override (deterministic) ───────────
TEST(CampaignSessionRunner, AdvanceHonorsMaxStepsOverride) {
    if (!std::filesystem::exists(f16_config_path())) {
        GTEST_SKIP() << "f16.json fixture not generated";
    }
    std::string err;
    auto session = CampaignSession::create(
        make_opts(kunsan_routed_world()), &err);
    ASSERT_NE(session, nullptr) << "create failed: " << err;
    session->set_paused(false);

    // 60 sim-seconds of debt, but only 3 ticks allowed this call:
    // capped (debt dropped), exactly 3 × sim_dt of sim time.
    const bool capped = session->advance(60.0, /*max_steps=*/3);
    EXPECT_TRUE(capped);
    EXPECT_NEAR(session->sim().sim_time_s(), 3.0 / 60.0, 1e-9);

    // The override never RAISES the session cap: a huge override is
    // clamped to max_steps_per_advance (240 default) — 60 s of debt
    // at 1/60 s/tick = 3600 ticks > 240 → still capped.
    const bool capped2 = session->advance(60.0, /*max_steps=*/100000);
    EXPECT_TRUE(capped2);
    EXPECT_NEAR(session->sim().sim_time_s(), 243.0 / 60.0, 1e-9);
}

// ── 4. The VIEW BUBBLE: paused session + camera position → vehicles ──────
TEST(CampaignSessionRunner, ViewBubbleDeaggregatesWhilePaused) {
    if (!std::filesystem::exists(f16_config_path())) {
        GTEST_SKIP() << "f16.json fixture not generated";
    }
    auto s = make_garrison_session();
    ASSERT_NE(s.session, nullptr);
    auto& session = *s.session;
    session.set_paused(true);  // the viewer's start state

    const auto* bm = session.sim().bubble_manager();
    ASSERT_NE(bm, nullptr);

    // Nothing deaggregated yet (the ownship is parked AT the base too —
    // but the paused session never ticked, so no bubble update ran).
    // The camera arrives on the base grid (390, 455 → ENU feet):
    EXPECT_EQ(bm->vehicle_entities().size(), std::size_t{0});

    session.set_view_bubble(
        2048.0, f4::geo::WorldPosition(390.0 * 1024.0, 455.0 * 1024.0, 0.0));

    // The garrison battalion's 3 vehicles spawned — WHILE PAUSED (the
    // "zoom into a ground unit" contract; no tick ran: the session's
    // sim clock is untouched).
    EXPECT_EQ(session.sim().sim_time_s(), 0.0);
    EXPECT_EQ(bm->vehicle_entities().size(), std::size_t{3});
    EXPECT_EQ(bm->deaggregated_unit_count(), std::size_t{1});

    // Every vehicle carries its vis type (V-3DLIVE: the session's db
    // is empty — the identity lives in the class table, not a record).
    for (const auto vid : bm->vehicle_entities()) {
        EntityHandle h(vid,
            const_cast<f4::entities::EntityWorld*>(
                &session.sim().world()));
        const auto* vis = h.get<VisualModelComponent>();
        ASSERT_NE(vis, nullptr);
        EXPECT_EQ(vis->vis_type, 225);  // vehicle_type 101 → vis 225
        EXPECT_EQ(vis->model_record, nullptr);  // empty session db
    }

    // The camera moves to the far corner: units outside the bubble
    // reaggregate (applied immediately — still no tick).
    session.set_view_bubble(
        2048.0, f4::geo::WorldPosition(3.0e6, 3.0e6, 0.0));
    EXPECT_EQ(bm->vehicle_entities().size(), std::size_t{0});

    // Clearing returns to the ownship bubble (parked ON the base → the
    // garrison deaggregates again — the FreeFalcon default behavior).
    session.clear_view_bubble();
    EXPECT_EQ(bm->vehicle_entities().size(), std::size_t{3});
    EXPECT_EQ(session.sim().sim_time_s(), 0.0);  // never ticked

    std::filesystem::remove_all(s.dir);
}

// ── 5. The runner survives the worker dying mid-run (dtor join) ──────────
TEST(CampaignSessionRunner, DestructorJoinsWithoutExplicitStop) {
    if (!std::filesystem::exists(f16_config_path())) {
        GTEST_SKIP() << "f16.json fixture not generated";
    }
    auto s = make_garrison_session();
    ASSERT_NE(s.session, nullptr);
    s.session->set_paused(false);

    {
        CampaignSessionRunner runner(*s.session, 240.0, /*paused=*/false);
        runner.start();
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        // No stop(): the dtor must join (a joinable std::thread dtor
        // would terminate() — the runner contract).
    }
    // The session outlived the runner and is still usable.
    EXPECT_GT(s.session->sim().sim_time_s(), 0.0);
    std::filesystem::remove_all(s.dir);
}
