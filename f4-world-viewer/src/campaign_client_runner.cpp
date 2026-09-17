// f4-world-viewer/src/campaign_client_runner.cpp
//
// CampaignClientRunner — implementation. See the header for the
// architecture: one worker thread stepping the session (through the
// f4-campaign-api contract) in short mutex-guarded batches, one adaptive
// tick budget keeping each lock hold ~6-12 ms so the UI thread's frame
// lock always lands promptly.

#include "campaign_client_runner.hpp"

#include <algorithm>
#include <cmath>

namespace f4::viewer {

namespace {

// The lock-hold target band (milliseconds). Below LO -> double the
// budget (fewer, bigger batches); above HI -> halve it (more, smaller
// batches). The floor is 1 tick — a batch is never empty, the worker
// always makes progress while unpaused.
constexpr double kHoldTargetLoMs = 6.0;
constexpr double kHoldTargetHiMs = 12.0;

// Never feed more than this much wall-clock time in one pacing slice,
// even after a stall (debugger, window drag, OS sleep). The dilation
// rule drops debt the budget can't pay; this clamp just keeps the
// conversion from absurd spikes in the first place.
constexpr double kMaxWallSliceSec = 0.25;

// The adaptive budget's CEILING. Below the hold target the budget
// doubles — an INSTANT session (a mock in tests, a trivial world)
// would otherwise double forever and overflow the int (a latent bug
// the engine-side runner carried; a real war's advance cost always
// pushed the hold back above the band). 4096 ticks ≈ 68 sim-s per
// batch — the engine's own max-steps-per-advance league.
constexpr int kMaxTickBudget = 4096;

} // namespace

CampaignClientRunner::CampaignClientRunner(
    f4::campaign::api::ICampaignSession& session, double tick_sec,
    double speed, bool paused)
    : session_(session), paused_(paused), tick_sec_(tick_sec) {
    set_speed(speed);
}

CampaignClientRunner::~CampaignClientRunner() {
    stop();
}

void CampaignClientRunner::start() {
    if (started_) return;
    started_ = true;
    stop_.store(false);
    worker_ = std::thread([this] { worker_loop_(); });
}

void CampaignClientRunner::stop() {
    stop_.store(true);
    if (worker_.joinable()) {
        worker_.join();
    }
}

void CampaignClientRunner::set_paused(bool p) {
    paused_.store(p);
    // Mirror onto the session itself under the lock — a paused step()
    // no-ops anyway; this keeps the session's pause flag honest for
    // hosts that read the time query (the viewer's canvas layer does).
    // Blocking here is bounded: FIFO order puts at most one worker
    // batch (~6-12 ms) ahead.
    std::lock_guard<FairMutex> lock(session_mutex_);
    session_.set_paused(p);
}

void CampaignClientRunner::set_speed(double s) noexcept {
    constexpr double kMaxSpeed = 1024.0;
    speed_.store(std::clamp(s, 0.0, kMaxSpeed));
}

void CampaignClientRunner::worker_loop_() {
    using clock = std::chrono::steady_clock;
    auto last = clock::now();

    while (!stop_.load(std::memory_order_relaxed)) {
        if (paused_.load(std::memory_order_relaxed)) {
            // Parked: don't accrue debt while the clock is off; reset
            // the pacing origin so unpause starts fresh.
            last = clock::now();
            effective_speed_.store(0.0);
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
            continue;
        }

        const auto now = clock::now();
        double wall_sec =
            std::chrono::duration<double>(now - last).count();
        last = now;
        if (wall_sec < 0.0) wall_sec = 0.0;                 // clock weirdness
        if (wall_sec > kMaxWallSliceSec) wall_sec = kMaxWallSliceSec;
        const double sim_seconds = wall_sec * speed_.load();

        {
            std::lock_guard<FairMutex> lock(session_mutex_);
            // NEVER step while a stop was requested — the joining
            // thread (stop()) must not wait on a fresh batch.
            if (stop_.load(std::memory_order_relaxed)) break;

            // Wall×speed → ticks (the pacing accumulator this runner
            // now owns — the engine's advance() used to carry it).
            const double desired =
                tick_carry_ + (tick_sec_ > 0.0
                                   ? sim_seconds / tick_sec_
                                   : 0.0);
            double ticks = std::floor(desired);
            tick_carry_ = desired - ticks;

            const auto t0 = clock::now();
            bool capped = false;
            if (ticks >= 1.0) {
                const auto budget =
                    static_cast<std::uint32_t>(tick_budget_.load());
                if (ticks > static_cast<double>(budget)) {
                    // The adaptive budget caps the batch: the excess is
                    // DROPPED, never queued (the dilation discipline —
                    // the UI surfaces it, the clock does not catch up).
                    ticks = static_cast<double>(budget);
                    capped = true;
                    tick_carry_ = 0.0;
                }
                const auto res = session_.step(
                    static_cast<std::uint32_t>(ticks));
                capped = capped || res.dilated;
                step_serial_.fetch_add(1, std::memory_order_relaxed);
            }
            const double hold_ms =
                std::chrono::duration<double, std::milli>(
                    clock::now() - t0).count();

            const double sim_advanced = ticks * tick_sec_;
            advanced_sim_s_.store(advanced_sim_s_.load() + sim_advanced);
            time_dilated_.store(capped);

            // Measured delivery rate — an EMA over the batches (see
            // effective_speed()). wall_sec is this iteration's wall
            // budget, so rate is the honest sim-seconds-per-wall-second
            // actually delivered, cap and CPU included.
            if (wall_sec > 1e-4) {
                const double rate = sim_advanced / wall_sec;
                double cur = effective_speed_.load();
                cur += 0.25 * (rate - cur);
                effective_speed_.store(cur);
            }

            // Adaptive budget (the ~6-12 ms lock-hold target): below
            // the band, fewer bigger batches; above it, more smaller
            // ones. Floor 1 — progress even on a loaded box; ceiling
            // kMaxTickBudget — the doubling stops somewhere sane.
            if (hold_ms < kHoldTargetLoMs) {
                tick_budget_.store(std::min(
                    kMaxTickBudget, tick_budget_.load() * 2));
            } else if (hold_ms > kHoldTargetHiMs &&
                       tick_budget_.load() > 1) {
                tick_budget_.store(std::max(1, tick_budget_.load() / 2));
            }
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}

} // namespace f4::viewer
