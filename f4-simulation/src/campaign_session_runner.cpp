// f4-simulation/src/campaign_session_runner.cpp
//
// CampaignSessionRunner — implementation. See the header for the
// architecture: one worker thread advancing the session in short
// mutex-guarded batches, one adaptive tick budget keeping each lock
// hold ~6-12 ms so the UI thread's frame lock always lands promptly.

#include <f4/simulation/campaign_session_runner.hpp>

#include <algorithm>

namespace f4::simulation {

namespace {

// The lock-hold target band (milliseconds). Below LO -> double the
// budget (fewer, bigger batches); above HI -> halve it (more, smaller
// batches). The floor is 1 tick — a batch is never empty, the worker
// always makes progress while unpaused.
constexpr double kHoldTargetLoMs = 6.0;
constexpr double kHoldTargetHiMs = 12.0;

// Never feed more than this much wall-clock time in one accumulator
// slice, even after a stall (debugger, window drag, OS sleep). The
// session's own cap drops debt it can't pay; this clamp just keeps the
// accumulator from absurd spikes in the first place.
constexpr double kMaxWallSliceSec = 0.25;

} // namespace

CampaignSessionRunner::CampaignSessionRunner(CampaignSession& session,
                                             double speed, bool paused)
    : session_(session), paused_(paused) {
    set_speed(speed);
}

CampaignSessionRunner::~CampaignSessionRunner() {
    stop();
}

void CampaignSessionRunner::start() {
    if (started_) return;
    started_ = true;
    stop_.store(false);
    worker_ = std::thread([this] { worker_loop_(); });
}

void CampaignSessionRunner::stop() {
    stop_.store(true);
    if (worker_.joinable()) {
        worker_.join();
    }
}

void CampaignSessionRunner::set_paused(bool p) {
    paused_.store(p);
    // Mirror onto the session itself under the lock — advance() no-ops
    // while paused anyway; this keeps session.paused() honest for hosts
    // that read it (the viewer's canvas layer does). Blocking here is
    // bounded: FIFO order puts at most one worker batch (~6-12 ms) ahead.
    std::lock_guard<FairMutex> lock(session_mutex_);
    session_.set_paused(p);
}

void CampaignSessionRunner::set_speed(double s) noexcept {
    constexpr double kMaxSpeed = 1024.0;
    speed_.store(std::clamp(s, 0.0, kMaxSpeed));
}

void CampaignSessionRunner::worker_loop_() {
    using clock = std::chrono::steady_clock;
    auto last = clock::now();

    while (!stop_.load(std::memory_order_relaxed)) {
        if (paused_.load(std::memory_order_relaxed)) {
            // Parked: don't accrue debt while the clock is off; reset
            // the pacing origin so unpause starts fresh.
            last = clock::now();
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
            continue;
        }

        const auto now = clock::now();
        double wall_sec =
            std::chrono::duration<double>(now - last).count();
        last = now;
        if (wall_sec < 0.0) wall_sec = 0.0;                 // clock weirdness
        if (wall_sec > kMaxWallSliceSec) wall_sec = kMaxWallSliceSec;
        const double real_seconds = wall_sec * speed_.load();

        {
            std::lock_guard<FairMutex> lock(session_mutex_);
            // NEVER call advance() while a stop was requested — the
            // joining thread (stop()) must not wait on a fresh batch.
            if (stop_.load(std::memory_order_relaxed)) break;

            const double sim_before = session_.sim().sim_time_s();
            const auto t0 = clock::now();
            const bool capped =
                session_.advance(real_seconds, tick_budget_.load());
            const double hold_ms =
                std::chrono::duration<double, std::milli>(
                    clock::now() - t0).count();

            advanced_sim_s_.store(
                advanced_sim_s_.load() +
                (session_.sim().sim_time_s() - sim_before));
            time_dilated_.store(capped);

            // Adaptive budget: keep the next lock hold in the target
            // band. Bounded [1, session cap] — advance() clamps to the
            // session's own option anyway, so a big budget on a tiny
            // world is still just "whatever the accumulator owes".
            int budget = tick_budget_.load();
            if (hold_ms < kHoldTargetLoMs) {
                budget = budget * 2 + 1;      // grow aggressively
            } else if (hold_ms > kHoldTargetHiMs) {
                budget = budget / 2;          // shrink conservatively
            }
            budget = std::clamp(budget, 1, 1 << 16);
            tick_budget_.store(budget);
        }

        // Yield the session to the UI thread between batches — the
        // frame lock lands here, not mid-tick.
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}

} // namespace f4::simulation
