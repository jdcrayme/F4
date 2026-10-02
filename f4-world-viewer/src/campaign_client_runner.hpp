// f4-world-viewer/src/campaign_client_runner.hpp
//
// CampaignClientRunner — the campaign's own THREAD, on the contract.
//
// CAMP-HOST-3 provenance: this is f4-simulation's CampaignSessionRunner
// relocated into the host that used it, with the ONE change that matters
// — it drives f4::campaign::api::ICampaignSession (the four-surface
// contract), not the engine's CampaignSession. Pacing is a host concern
// (Docs/CAMP_HOST_PLAN.md §2.2); the engine no longer ships a thread
// wrapper. The threading architecture is the V-THREAD design, verbatim:
//
//   worker thread ──► { lock; step(tick batch); unlock; sleep(1ms); } ──► repeat
//   UI thread     ──► { lock; read/draw; unlock; } once per frame
//
// ONE mutex guards the whole session (the contract's step/query/submit
// are not individually thread-safe — the adapter wraps the engine's
// session, whose advance() mutates everything a query reads). The lock
// is a FairMutex (FIFO ticket order — see fair_mutex.hpp for the
// starvation regression it exists to prevent): the viewer's frame scope
// holds it for ~a whole frame and releases for only ~tens of
// microseconds; ticket order guarantees the worker is served before the
// UI's re-lock, every frame. The worker keeps each hold SHORT via an
// adaptive tick budget: it measures how long each step() batch took and
// scales the per-call tick cap to target a ~6-12 ms hold, so the UI
// thread's frame lock waits at most one batch.
//
// Pacing on the contract (the one semantic port): the engine's advance()
// fed WALL seconds to an accumulator that drained whole ticks; the
// contract's step(ticks) is tick-granular. The worker therefore converts
// its wall×speed slice to a tick count itself, carrying the sub-tick
// fraction batch to batch. Dilation is now a GOVERNOR, not a silent
// drop (the HandleCampaignThread lesson — freefalcon-central
// campaign.cpp:2680 "Slow things down"): when a batch hits the cap, the
// runner halves its DELIVERY SCALE (an AIMD multiplier under the preset)
// so the next batches feed what the CPU can actually drain — the war's
// clock slows and every fed tick lands. When delivery runs clean again,
// the scale doubles back toward the preset (the reference's "Back to
// full speed"). The preset stays the ceiling REQUEST; time_dilated()
// now means even the fully de-rated feed outran the CPU (drops resumed).
//
// Pause semantics: the runner's paused flag is the UI's clock switch —
// the worker stops stepping but KEEPS waking (cheap), and the session's
// own pause flag is kept in sync (a paused step() no-ops anyway; belt
// and braces for any host that reads the time query's paused).
//
// Threading/ownership:
//   - The session is LENT (must outlive the runner — the runner's dtor
//     stops + joins the worker before the host may destroy it).
//   - The worker touches ONLY the contract (no GL, no raylib, no ImGui).
//   - Single lock, two lock sites (worker step, host read), no nesting:
//     deadlock-free by construction.
//   - set_paused/set_speed use an atomic + the session lock only to
//     mirror the flag; they never block for long (FIFO order means at
//     most one worker batch — ~6-12 ms — ahead of them).
//
// Dependencies: f4-campaign-api (the contract), std::thread/mutex.
// C++20.
#pragma once

#include <f4/campaign/api/session.hpp>

#include "fair_mutex.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <thread>

namespace f4::viewer {

class CampaignClientRunner {
public:
    /// Construct over a session (lent — must outlive the runner; the
    /// runner's dtor joins the worker before the host frees it).
    /// Does NOT start the thread — call start().
    /// \param session  the live session to drive (the contract iface).
    /// \param tick_sec the session's fixed dt (the `time` query's
    ///                 tick_sec — the engine's tuned 1/60 s; the worker
    ///                 converts wall×speed to ticks with it).
    /// \param speed    initial wall-clock multiplier (1.0 = real time).
    /// \param paused   initial clock state (the viewer starts paused).
    explicit CampaignClientRunner(f4::campaign::api::ICampaignSession& session,
                                  double tick_sec, double speed = 1.0,
                                  bool paused = false);

    /// Stops + joins the worker (idempotent). MUST run before the
    /// session dies — hence the runner is destroyed first (declare it
    /// AFTER the session unique_ptr, so reverse-order destruction does
    /// the right thing for free).
    ~CampaignClientRunner();

    CampaignClientRunner(const CampaignClientRunner&) = delete;
    CampaignClientRunner& operator=(const CampaignClientRunner&) = delete;
    CampaignClientRunner(CampaignClientRunner&&) = delete;
    CampaignClientRunner& operator=(CampaignClientRunner&&) = delete;

    /// Spawn the worker thread. Idempotent (a second call is a no-op).
    void start();

    /// Signal the worker to stop and JOIN it. Safe from the UI thread.
    /// MUST NOT be called while holding mutex() (the worker needs it to
    /// finish its current batch — stop() would self-deadlock). Idempotent.
    void stop();

    // --- Thread-safe controls (UI thread) ---------------------------------

    /// The clock switch — LOCKING form: for callers NOT holding mutex()
    /// (library hosts, tests). Sets the runner's atomic AND mirrors the
    /// session's own pause under the session lock (bounded wait: the
    /// worker's holds are ~6-12 ms by design).
    void set_paused(bool p);

    /// The clock switch — ATOMIC-ONLY form: for callers ALREADY holding
    /// mutex() (the viewer's frame read scope — the Campaign window's
    /// Play/Pause button, the Space shortcut). set_paused() there would
    /// re-lock the mutex we already hold = self-deadlock. The caller
    /// mirrors the session's own flag itself (it holds the lock, so a
    /// direct session_->set_paused is consistent — the worker can't be
    /// mid-step).
    void set_paused_flag(bool p) noexcept { paused_.store(p); }

    [[nodiscard]] bool paused() const noexcept { return paused_.load(); }

    /// The wall-clock multiplier (speed presets: 1x/10x/60x/240x).
    /// Clamped to [0.0, 1024.0]. The tick dt itself NEVER scales (the
    /// FM's tuned 1/60 s discretization) — only how much sim time the
    /// worker feeds per wall second. A new preset resets the delivery
    /// governor to full feed (the reference's SetTimeCompression — a
    /// new ratio is TRIED, then the CPU says what it sustains).
    void set_speed(double s) noexcept;
    [[nodiscard]] double speed() const noexcept { return speed_.load(); }

    /// The governor's current delivery scale in (0, 1] — the fraction
    /// of the preset the worker is FEEDING while dilated (AIMD: halved
    /// per capped batch, doubled back per clean streak, clamped to the
    /// preset). 1.0 = the full preset is being fed. Diagnostics: the
    /// UI can show "240x (delivering 6.2x)" — the preset is the
    /// request, this is the governor's answer.
    [[nodiscard]] double delivery_scale() const noexcept {
        return delivery_scale_.load();
    }

    /// True when a step batch hit the cap EVEN AFTER the governor
    /// de-rated the feed — the UI surfaces "time dilated" (only the
    /// residual case drops ticks now; steady-state overload slows the
    /// clock instead, like the reference's compression halving).
    [[nodiscard]] bool time_dilated() const noexcept {
        return time_dilated_.load();
    }

    /// AGG-3 — the DoCompressionLoop state (the authenticity rule):
    /// TRUE while any deaggregated aircraft is live in the observer
    /// bubble. While set, the worker's feed holds at 1× — full-fidelity
    /// aircraft the player is watching never fast-forward (the
    /// reference's freefalcon-central campaign.cpp:2394-2520 rule).
    /// The preset radio keeps the user's request; this is the clamp
    /// under it, and it lifts the moment the bubble clears.
    ///
    /// PV-2b: the WORKER maintains this flag now — each step batch's
    /// result carries the engine's live count (StepResult::bubble_live),
    /// so the clamp engages per batch at the reference's own
    /// compression-loop cadence, independent of the UI's snapshot
    /// refresh throttle. set_bubble_action() remains for hosts that
    /// drive the contract without stepping through this runner.
    void set_bubble_action(bool a) noexcept {
        bubble_action_.store(a, std::memory_order_relaxed);
    }
    [[nodiscard]] bool bubble_action() const noexcept {
        return bubble_action_.load(std::memory_order_relaxed);
    }

    /// Rolling MEASURED rate — sim seconds advanced per wall second
    /// (EMA over the worker's batches; 0 while paused). The UI compares
    /// this with speed(): a preset the CPU can't sustain delivers less
    /// than requested, and without this readout every unsustainable
    /// preset moves the clock at the same (identical) rate.
    [[nodiscard]] double effective_speed() const noexcept {
        return effective_speed_.load();
    }

    /// The session lock — FAIR (FIFO ticket order; see fair_mutex.hpp
    /// for the starvation regression it exists to prevent). The host
    /// takes it (std::unique_lock / std::lock_guard) for its frame
    /// read+draw scope; the worker takes it per step batch. One lock,
    /// no nesting — and whatever the host's duty cycle, the worker's
    /// queued lock is always served before the host's re-lock.
    [[nodiscard]] FairMutex& mutex() noexcept { return session_mutex_; }

    /// Run `fn(session)` under the session lock — the fine-grained read
    /// path for hosts that prefer scoped lambdas over a frame-wide
    /// lock. Returns fn's result.
    template <class F>
    auto read(F&& fn)
        -> decltype(fn(std::declval<f4::campaign::api::ICampaignSession&>())) {
        std::lock_guard<FairMutex> lock(session_mutex_);
        return fn(session_);
    }

    /// Sim time advanced by the worker so far (seconds) — diagnostics.
    [[nodiscard]] double advanced_sim_seconds() const noexcept {
        return advanced_sim_s_.load();
    }

    /// The worker's current tick budget (ticks per step call) —
    /// diagnostics/tests. Starts small and adapts to the measured
    /// per-batch cost (target lock hold: ~6-12 ms).
    [[nodiscard]] int tick_budget() const noexcept {
        return tick_budget_.load();
    }

    // --- PV-1: the worker's batch composition (F4_FRAME_PROF readout) ---

    /// The LAST step batch's lock-hold time (ms) — the duty-cycle
    /// numerator (what the sim got) against the frame period (what the
    /// UI took).
    [[nodiscard]] double last_hold_ms() const noexcept {
        return last_hold_ms_.load(std::memory_order_relaxed);
    }
    /// EMA of the batch hold (ms) — steadier than the last sample for
    /// the duty-cycle readout.
    [[nodiscard]] double hold_ema_ms() const noexcept {
        return hold_ema_ms_.load(std::memory_order_relaxed);
    }
    /// The LAST batch's tick count (0 = the batch fed nothing — a
    /// paused or fully de-rated iteration).
    [[nodiscard]] int last_batch_ticks() const noexcept {
        return last_batch_ticks_.load(std::memory_order_relaxed);
    }

    /// Counts the worker's productive step() calls (ticks > 0). The UI
    /// gates its per-frame query refresh on this — "the numbers refresh
    /// once per advance, never per draw".
    [[nodiscard]] std::uint64_t step_serial() const noexcept {
        return step_serial_.load();
    }

private:
    void worker_loop_();

    f4::campaign::api::ICampaignSession& session_;
    FairMutex session_mutex_;   // FIFO — see fair_mutex.hpp

    std::atomic<bool> stop_{false};
    std::atomic<bool> paused_;
    std::atomic<bool> bubble_action_{false};   // AGG-3: the 1× hold
    std::atomic<double> speed_{1.0};
    std::atomic<double> delivery_scale_{1.0};   // AIMD governor, (0, 1]
    std::atomic<int> clean_batches_{0};         // governor's recover streak
    std::atomic<bool> time_dilated_{false};
    std::atomic<double> advanced_sim_s_{0.0};
    std::atomic<double> effective_speed_{0.0};
    std::atomic<int> tick_budget_{4};   // adaptive: 1..batch cap
    std::atomic<std::uint64_t> step_serial_{0};

    // PV-1: the worker's batch composition, mirrored for the frame
    // profiler (relaxed — diagnostics only, no ordering deps).
    std::atomic<double> last_hold_ms_{0.0};
    std::atomic<double> hold_ema_ms_{0.0};
    std::atomic<int> last_batch_ticks_{0};

    /// The sub-tick fraction carried batch to batch (the pacing
    /// accumulator the engine's advance() used to own).
    double tick_carry_ = 0.0;

    double tick_sec_;
    std::thread worker_;
    bool started_{false};
};

} // namespace f4::viewer
