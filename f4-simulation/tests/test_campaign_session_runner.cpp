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
#include <f4/simulation/fair_mutex.hpp>
#include <f4/entities/entity.hpp>

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <thread>
#include <vector>

using namespace f4::simulation;
using f4::entities::EntityHandle;
using f4::entities::EntityId;

namespace {

std::filesystem::path kunsan_routed_world() {
    return std::filesystem::path(F4_SIMULATION_TEST_FIXTURES_DIR) /
           "kunsan_session.world.json";
}

std::filesystem::path class_table_path() {
    return std::filesystem::path(F4_SOURCE_FIXTURES_DIR) / "falcon4.ct.json";
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

// ── 6. The viewer's REAL frame pattern must not starve the worker ─────────
//
// REGRESSION (the "campaign time doesn't advance" report): run()'s frame
// scope holds the runner's mutex for the WHOLE frame — the draw AND
// EndDrawing's 60 FPS pace wait live inside the lock — then releases for
// only ~tens of microseconds (process_session_stop + the loop-top checks)
// before re-locking. A plain std::mutex under that pattern can starve the
// worker thread nearly completely: campaign time crawls to a stop while
// the UI stays smooth. This test reproduces the exact lock duty cycle and
// pins a floor on how much sim time the worker must still advance.
TEST(CampaignSessionRunner, ViewerFramePatternDoesNotStarveWorker) {
    if (!std::filesystem::exists(f16_config_path())) {
        GTEST_SKIP() << "f16.json fixture not generated";
    }
    auto s = make_garrison_session();
    ASSERT_NE(s.session, nullptr);
    s.session->set_paused(false);

    CampaignSessionRunner runner(*s.session, 1.0, /*paused=*/false);
    runner.start();

    // The "UI thread": 60 FPS frames, each holding the session lock for
    // the full 16.67 ms frame (draw + the vsync wait inside the lock),
    // then a ~50 us unlocked gap — the OLD run() duty cycle (the fix
    // ALSO moved EndDrawing out of the scope; this pattern stays the
    // worst case on purpose — fairness must hold even without that).
    constexpr int kFrames = 180;  // ~3 s of wall time
    for (int i = 0; i < kFrames; ++i) {
        {
            std::lock_guard frame(runner.mutex());
            std::this_thread::sleep_for(std::chrono::milliseconds(16));
        }  // lock released — the worker's only window
        // run()'s unlocked gap: resize/key checks + adopt + stop
        // processing — tens of microseconds, no sleeping.
        volatile int spin = 0;
        for (int k = 0; k < 500; ++k) spin += k;
        (void)spin;
    }

    // 3 s at 1x owes ~3 s of sim time. A healthy worker keeps pace;
    // even a heavily starved one must make REAL progress — the bug
    // measured ~0. Floor at a conservative 0.25x of wall time.
    const double advanced = runner.advanced_sim_seconds();
    EXPECT_GT(advanced, 0.75)
        << "worker starved under the viewer frame pattern: only "
        << advanced << " sim-s advanced in ~3 wall-s";
    runner.stop();
    std::filesystem::remove_all(s.dir);
}

// ── 7. FairMutex itself: FIFO service, no queue jumping, mutual exclusion ─
//
// Determinism note: a FairMutex ticket is assigned at the TOP of lock(),
// BEFORE the waiter parks — so "main holds the lock + the waiter has
// slept long enough to park" makes the waiter's ticket strictly earlier
// than anything launched afterwards. That makes the service order
// (waiter before newcomer) provable, not probabilistic.
TEST(CampaignSessionRunner, FairMutexServesFifoAndTryLockNeverJumps) {
    FairMutex m;
    std::vector<int> order;

    // Phase 1: queued waiter is served BEFORE a later newcomer, and
    // try_lock never succeeds while held/queued.
    m.lock();  // main holds ticket 0
    std::thread waiter([&] {
        m.lock();               // ticket 1 — parks while main holds
        order.push_back(1);
        m.unlock();
    });
    // Long enough that the waiter has taken ticket 1 and parked (it
    // cannot acquire — main holds; parking is the only path).
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    std::thread newcomer([&] {
        m.lock();               // ticket 2 — behind the waiter, strictly
        order.push_back(2);
        m.unlock();
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    // Held (and queued waiters ahead) — try_lock must refuse.
    EXPECT_FALSE(m.try_lock());

    m.unlock();                 // hands off: waiter (ticket 1) first,
    waiter.join();              // newcomer (ticket 2) second — FIFO
    newcomer.join();

    ASSERT_EQ(order.size(), 2u);
    EXPECT_EQ(order[0], 1);     // the queued waiter was NOT jumped
    EXPECT_EQ(order[1], 2);

    // Phase 2: free lock — try_lock succeeds, balanced by unlock.
    EXPECT_TRUE(m.try_lock());
    m.unlock();

    // Phase 3: mutual exclusion hammer — 3 threads x 4000 critical
    // sections over a plain counter; a missed exclusion loses counts.
    std::atomic<bool> go{false};
    std::atomic<int> shared{0};
    std::vector<std::thread> hammers;
    for (int t = 0; t < 3; ++t) {
        hammers.emplace_back([&] {
            while (!go.load()) std::this_thread::yield();
            for (int i = 0; i < 4000; ++i) {
                std::lock_guard lock(m);
                const int v = shared.load();
                std::this_thread::yield();  // widen the race window
                shared.store(v + 1);
            }
        });
    }
    go.store(true);
    for (auto& h : hammers) h.join();
    EXPECT_EQ(shared.load(), 12000);
}

// ── 8. effective_speed(): the measured-rate EMA the viewer reads ──────────
// Fresh = 0; parked batches store 0; running batches converge toward the
// sim-seconds-per-wall-second actually delivered (request here is 10x —
// the EMA must sit well under the request's neighborhood, well above
// zero, on any box that advances at all).
TEST(CampaignSessionRunner, EffectiveSpeedZeroWhenFreshAndWhenParked) {
    if (!std::filesystem::exists(f16_config_path())) {
        GTEST_SKIP() << "f16.json fixture not generated";
    }
    auto s = make_garrison_session();
    ASSERT_NE(s.session, nullptr);

    CampaignSessionRunner runner(*s.session, 10.0, /*paused=*/true);
    EXPECT_EQ(runner.effective_speed(), 0.0);  // fresh, never started
    runner.start();
    std::this_thread::sleep_for(std::chrono::milliseconds(250));
    EXPECT_EQ(runner.effective_speed(), 0.0);  // parked batches store 0
    runner.stop();
}

TEST(CampaignSessionRunner, EffectiveSpeedTracksRunningRate) {
    if (!std::filesystem::exists(f16_config_path())) {
        GTEST_SKIP() << "f16.json fixture not generated";
    }
    auto s = make_garrison_session();
    ASSERT_NE(s.session, nullptr);

    CampaignSessionRunner runner(*s.session, 10.0, /*paused=*/false);
    runner.start();
    std::this_thread::sleep_for(std::chrono::milliseconds(1200));
    const double eff = runner.effective_speed();

    // α = 0.25 per batch converges within a handful of the ~ms-spaced
    // batches, so 1.2 s of wall is far past settle time. Bounds are
    // deliberately wide: any box that advances at all clears the floor
    // (0.2x), and the requested 10x caps the honest ceiling (the worker
    // never delivers more than the request; 15x slack covers EMA lag).
    EXPECT_GT(eff, 0.2) << "measured rate never rose above zero";
    EXPECT_LT(eff, 15.0) << "measured rate exceeded the requested preset";

    // Pause → the parked branch stores 0 within one 5 ms loop pass.
    runner.set_paused(true);
    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    EXPECT_EQ(runner.effective_speed(), 0.0);
    runner.stop();
}
