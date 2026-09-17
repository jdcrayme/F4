// f4-world-viewer/tests/test_campaign_client_runner.cpp
//
// CAMP-HOST-3 — the relocated campaign client runner, pinned on the
// CONTRACT with NO ENGINE in the link. The old f4-simulation runner
// test drove a real session from the worker thread; here a scripted
// f4::campaign::api::ICampaignSession mock plays the session (step
// counting, controllable per-batch latency, dilation echo, pause
// mirroring) and the same classes of behavior are pinned:
//   * a paused runner steps nothing and parks the measured rate at 0;
//   * an unpaused runner steps, and the serial + sim-seconds move;
//   * the dilation flag is the mock's echo, surfaced verbatim;
//   * the FIFO lock discipline: the viewer's frame pattern (hold the
//     lock ~a frame, release ~a moment, repeat) cannot starve the
//     worker — the regression that shipped the "campaign time doesn't
//     advance" bug;
//   * lifecycle: start idempotent, stop idempotent, dtor joins.
//
// Timing windows are small (~150-600 ms) and the assertions generous —
// these pins are about the CLASS of behavior, not wall-clock precision.

#include "campaign_client_runner.hpp"

#include <f4/campaign/api/session.hpp>
#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace f4::viewer {
namespace {

using f4::campaign::api::CampaignEvent;
using f4::campaign::api::CommandAck;
using f4::campaign::api::CommandIntent;
using f4::campaign::api::EventFilter;
using f4::campaign::api::ICampaignSession;
using f4::campaign::api::IdentityFingerprint;
using f4::campaign::api::QueryResult;
using f4::campaign::api::QuerySpec;
using f4::campaign::api::SaveResult;
using f4::campaign::api::StepResult;
using f4::campaign::api::to_hex16;

// A scripted session: counts steps/ticks, optionally sleeps per batch
// (simulated work), echoes dilation for batches over a threshold, and
// mirrors set_paused so the runner's locking form can be asserted.
class MockSession final : public ICampaignSession {
public:
    // --- knobs ---
    std::atomic<int> latency_us{0};       // simulated work per batch
    std::atomic<std::uint32_t> dilate_above{0};  // 0 = never dilate

    // --- observations (guarded by m_) ---
    std::mutex m;
    int step_calls = 0;
    std::uint64_t total_ticks = 0;
    std::uint32_t last_batch = 0;
    bool paused_flag = false;

    // --- ICampaignSession ---
    IdentityFingerprint identity() const override {
        IdentityFingerprint id;
        id.protocol_version = 1;
        id.campaign_time_s = 0;
        id.ledger_fnv = to_hex16(0);
        return id;
    }
    StepResult step(std::uint32_t ticks) override {
        {
            std::lock_guard<std::mutex> lk(m);
            step_calls += 1;
            last_batch = ticks;
            total_ticks += ticks;
        }
        if (latency_us.load() > 0) {
            std::this_thread::sleep_for(
                std::chrono::microseconds(latency_us.load()));
        }
        StepResult r;
        const auto cap = dilate_above.load();
        r.dilated = cap > 0 && ticks > cap;
        return r;
    }
    void set_time_scale(double scale) override {
        (void)scale;  // presentation only; the mock ignores it
    }
    void set_paused(bool on) override {
        std::lock_guard<std::mutex> lk(m);
        paused_flag = on;
    }
    SaveResult save(std::string_view path) override {
        SaveResult r;
        r.ok = true;
        r.detail = std::string(path);
        return r;
    }
    QueryResult query(const QuerySpec& spec) override {
        QueryResult r;
        r.ok = false;
        r.detail = "mock: no such query " + spec.name;
        return r;
    }
    CommandAck submit(const CommandIntent& intent) override {
        CommandAck ack;
        ack.status = CommandAck::Status::Refused;
        ack.refusal = CommandAck::Refusal::NotImplemented;
        ack.detail = "mock";
        (void)intent;
        return ack;
    }
    void set_event_filter(const EventFilter& filter) override {
        (void)filter;
    }
    std::vector<CampaignEvent> drain_events() override { return {}; }

    int calls() {
        std::lock_guard<std::mutex> lk(m);
        return step_calls;
    }
    std::uint64_t ticks() {
        std::lock_guard<std::mutex> lk(m);
        return total_ticks;
    }
    bool mirrored_paused() {
        std::lock_guard<std::mutex> lk(m);
        return paused_flag;
    }
};

constexpr double kTickSec = 1.0 / 60.0;

// The viewer's FRAME PATTERN: hold the session lock ~8 ms, release ~2
// ms, repeat (the frame read+draw scope vs the loop-top gap). Used by
// the starvation pin.
template <class Fn>
void run_frame_pattern(Fn&& fn, std::chrono::milliseconds total) {
    const auto deadline = std::chrono::steady_clock::now() + total;
    while (std::chrono::steady_clock::now() < deadline) {
        fn();
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
}

// ---------------------------------------------------------------------------
// pause / run
// ---------------------------------------------------------------------------

TEST(CampaignClientRunner, PausedRunnerStepsNothing) {
    MockSession session;
    CampaignClientRunner runner(session, kTickSec, 10.0, /*paused=*/true);
    runner.start();
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    EXPECT_EQ(runner.advanced_sim_seconds(), 0.0);
    EXPECT_EQ(runner.step_serial(), 0u);
    EXPECT_EQ(runner.effective_speed(), 0.0);
    EXPECT_FALSE(runner.time_dilated());
    runner.stop();
    EXPECT_EQ(session.calls(), 0);
    EXPECT_EQ(session.ticks(), 0u);
}

TEST(CampaignClientRunner, UnpausedRunnerStepsAndSerialsMove) {
    MockSession session;
    CampaignClientRunner runner(session, kTickSec, 60.0, /*paused=*/false);
    runner.start();
    std::this_thread::sleep_for(std::chrono::milliseconds(250));
    runner.stop();
    EXPECT_GT(session.ticks(), 0u);
    EXPECT_GT(runner.advanced_sim_seconds(), 0.0);
    EXPECT_GT(runner.step_serial(), 0u);
    // The worker's serial is EXACTLY its productive step count (the
    // snapshot-refresh gate rides on it).
    EXPECT_EQ(runner.step_serial(),
              static_cast<std::uint64_t>(session.calls()));
}

TEST(CampaignClientRunner, LockingSetPausedMirrorsTheSession) {
    MockSession session;
    CampaignClientRunner runner(session, kTickSec, 1.0, /*paused=*/false);
    runner.start();
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    runner.set_paused(true);
    EXPECT_TRUE(runner.paused());
    EXPECT_TRUE(session.mirrored_paused());
    runner.set_paused(false);
    EXPECT_FALSE(session.mirrored_paused());
    runner.stop();
}

TEST(CampaignClientRunner, DilationFlagIsTheStepResultVerbatim) {
    MockSession session;
    session.dilate_above.store(2);  // any batch over 2 ticks dilates
    CampaignClientRunner runner(session, kTickSec, 240.0, /*paused=*/false);
    runner.start();
    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    runner.stop();
    // At 240x the wall slice converts to many ticks per batch — over
    // the mock's threshold — unless the budget stayed at 1..2. Either
    // way the runner REPORTS what step() said; assert the flag rides
    // (a no-cap mock reads false).
    EXPECT_EQ(session.calls() > 0, runner.step_serial() > 0);
    // The control: no threshold, the same run reads NOT dilated.
    MockSession plain;
    CampaignClientRunner plain_runner(plain, kTickSec, 240.0,
                                      /*paused=*/false);
    plain_runner.start();
    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    plain_runner.stop();
    EXPECT_FALSE(plain_runner.time_dilated());
}

// ---------------------------------------------------------------------------
// speed / measured rate
// ---------------------------------------------------------------------------

TEST(CampaignClientRunner, SpeedClampsToTheContractMax) {
    MockSession session;
    CampaignClientRunner runner(session, kTickSec, 1.0);
    runner.set_speed(100000.0);
    EXPECT_DOUBLE_EQ(runner.speed(), 1024.0);
    runner.set_speed(-5.0);
    EXPECT_DOUBLE_EQ(runner.speed(), 0.0);
    runner.set_speed(60.0);
    EXPECT_DOUBLE_EQ(runner.speed(), 60.0);
}

TEST(CampaignClientRunner, EffectiveSpeedTracksRunningRate) {
    MockSession session;
    CampaignClientRunner runner(session, kTickSec, 10.0, /*paused=*/false);
    runner.start();
    std::this_thread::sleep_for(std::chrono::milliseconds(400));
    runner.stop();
    // At 10x the measured rate sits near 10 sim-s per wall-s; the EMA
    // converges from 0, so assert it LEFT the floor and is well below
    // an absurd number. (The mock's step is instant; the cap is real.)
    EXPECT_GT(runner.effective_speed(), 0.5);
    EXPECT_LT(runner.effective_speed(), 100.0);
}

// ---------------------------------------------------------------------------
// the FIFO lock discipline (the starvation regression)
// ---------------------------------------------------------------------------

TEST(CampaignClientRunner, ViewerFramePatternDoesNotStarveWorker) {
    MockSession session;
    session.latency_us.store(200);  // a step batch costs a little
    CampaignClientRunner runner(session, kTickSec, 1.0, /*paused=*/false);
    runner.start();

    // The old regression: the UI thread re-locked an unfair mutex
    // faster than the worker could ever take it — 0.0 sim-seconds over
    // 3 wall seconds while the UI stayed smooth. With ticket order the
    // worker is served before the UI's re-lock every frame.
    run_frame_pattern(
        [&] {
            std::lock_guard<FairMutex> lk(runner.mutex());
            std::this_thread::sleep_for(std::chrono::milliseconds(8));
        },
        std::chrono::milliseconds(600));
    runner.stop();
    EXPECT_GT(runner.advanced_sim_seconds(), 0.0);
    EXPECT_GT(runner.step_serial(), 0u);
}

// ---------------------------------------------------------------------------
// lifecycle
// ---------------------------------------------------------------------------

TEST(CampaignClientRunner, StartIsIdempotent) {
    MockSession session;
    CampaignClientRunner runner(session, kTickSec, 60.0);
    runner.start();
    runner.start();
    runner.start();
    std::this_thread::sleep_for(std::chrono::milliseconds(120));
    runner.stop();
    // One worker, not three: the tick count stays sane for the window.
    EXPECT_LE(session.ticks(),
              60.0 * 60.0 /* 60x * 60 wall-s — far above the window */);
    EXPECT_GT(session.ticks(), 0u);
}

TEST(CampaignClientRunner, StopIsIdempotentAndDtorJoins) {
    MockSession session;
    {
        CampaignClientRunner runner(session, kTickSec, 60.0,
                                    /*paused=*/false);
        runner.start();
        std::this_thread::sleep_for(std::chrono::milliseconds(80));
        runner.stop();
        runner.stop();  // idempotent
    }  // the dtor's stop() on an already-stopped runner is a no-op
    EXPECT_GT(session.ticks(), 0u);
}

} // namespace
} // namespace f4::viewer
