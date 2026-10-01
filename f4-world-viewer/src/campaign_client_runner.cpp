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

// The delivery GOVERNOR (the HandleCampaignThread lesson: the reference
// halves gameCompressionRatio when the campaign falls behind and
// restores it on catch-up — the clock slows, the CPU never pegs). The
// worker feeds preset × delivery_scale_; a capped batch halves the
// scale (multiplicative decrease — one cap halves, it does not
// inch down), a clean streak doubles it back. The floor keeps a Debug
// build's residual drops bounded: below kMinDeliveryScale the feed is
// already tiny, and time_dilated_ honestly reports whatever remains.
constexpr double kMinDeliveryScale = 1.0 / 64.0;

// Clean batches required before the scale doubles back (AIMD's additive
// patience: ~0.1-0.2 s of fully-delivered feed per doubling at the
// worker's ~7-13 ms iteration period — quick to recover, slow to
// oscillate at the capacity edge).
constexpr int kCleanBatchesPerRecover = 16;

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
    // A new preset is TRIED at full feed (the reference's
    // SetTemporaryCompression on a ratio change): the governor then
    // finds what the CPU sustains — the preset is the request, not a
    // promise.
    delivery_scale_.store(1.0);
    clean_batches_.store(0);
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
        // The GOVERNORED feed: preset × delivery_scale — the scale is
        // 1.0 until the CPU says otherwise, then AIMD keeps the fed
        // ticks just inside what a batch can fully drain (no dropped
        // time in steady state; the war slows, it does not lose
        // seconds).
        //
        // AGG-3 — the DoCompressionLoop clamp under the preset: while
        // action is live in the observer bubble, the feed holds at 1×
        // (the reference's campaign.cpp:2394-2520 rule — the FM the
        // player is watching never meets compression). The preset is
        // untouched (the radio keeps the user's request), the
        // delivery governor's scale carries over (the CPU's answer
        // stays the CPU's answer), and the clamp lifts the moment the
        // bubble clears — no governor reset, no fake full-feed spike.
        const double preset =
            bubble_action_.load(std::memory_order_relaxed)
                ? std::min(speed_.load(), 1.0)
                : speed_.load();
        const double sim_seconds =
            wall_sec * preset * delivery_scale_.load();
        int clean_batches = clean_batches_.load(std::memory_order_relaxed);

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
                    // The governor below reacts: the NEXT batches feed
                    // inside this batch's demonstrated capacity, so
                    // steady-state runs stop dropping entirely.
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

            // The governor's AIMD update. A capped batch halves the
            // feed (the reference's "Slow things down"); a clean streak
            // doubles it back toward the preset ("Back to full speed").
            // The drops this batch already made are the signal — the
            // NEXT batches feed what the CPU just demonstrated it can
            // drain.
            if (capped) {
                const double cur = delivery_scale_.load();
                delivery_scale_.store(
                    std::max(kMinDeliveryScale, cur * 0.5));
                clean_batches = 0;
            } else if (ticks >= 1.0) {
                ++clean_batches;
                if (clean_batches >= kCleanBatchesPerRecover) {
                    delivery_scale_.store(
                        std::min(1.0, delivery_scale_.load() * 2.0));
                    clean_batches = 0;
                }
            }
            clean_batches_.store(clean_batches, std::memory_order_relaxed);

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
