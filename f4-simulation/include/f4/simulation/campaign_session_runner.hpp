// f4-simulation/include/f4/simulation/campaign_session_runner.hpp
//
// CampaignSessionRunner — the campaign's own THREAD.
//
// V-THREAD: the world viewer used to drive CampaignSession::advance()
// inline from its render loop — one frame's advance could legally run
// 240 ticks (the spiral-of-death cap) over hundreds of aircraft, which
// is seconds of work INSIDE the ImGui frame: the window stopped
// responding while the campaign ran (the user's report: "when running
// the session the UI becomes unresponsive").
//
// This class owns the loop instead:
//
//   worker thread ──► { lock; advance(wall_delta * speed, budget); unlock; sleep(1ms); } ──► repeat
//   UI thread     ──► { lock; read/draw; unlock; } once per frame
//
// ONE mutex guards the whole CampaignSession (the sim's EntityWorld,
// the ladder, the ledger — everything advance() mutates and everything
// the UI reads). The lock is a FairMutex (FIFO ticket order): the
// viewer's frame scope holds it for ~a whole frame and releases for
// only ~tens of microseconds — under a plain std::mutex that starved
// the worker COMPLETELY (0 sim-seconds advanced over 3 wall-seconds,
// the "campaign time doesn't advance" regression); ticket order
// guarantees the worker is served before the UI's re-lock, every
// frame. The worker keeps each hold SHORT via an adaptive tick
// budget: it measures how long each advance() batch took and scales
// the per-call tick cap (1..session cap) to target a ~6-12 ms hold,
// so the UI thread's frame lock waits at most one batch.
//
// Pause semantics: the runner's paused flag is the UI's clock switch —
// the worker stops advancing but KEEPS waking (cheap), and the session
// object's own paused_ flag is kept in sync (advance() would no-op
// anyway; belt and braces for any host that reads session.paused()).
//
// Threading/ownership:
//   - The session is LENT (must outlive the runner — the runner's dtor
//     stops + joins the worker before the host may destroy it).
//   - The worker touches ONLY the session (no GL, no raylib, no ImGui
//     — campaign_session.cpp stays headless).
//   - Single lock, two lock sites (worker advance, host read), no
//     nesting: deadlock-free by construction.
//   - set_paused/set_speed use an atomic + the session lock only to
//     mirror the flag; they never block for long (FIFO order means at
//     most one worker batch — ~6-12 ms — ahead of them).
//
// The QC and every unit test still drive advance() directly on their
// own thread — the runner is a host-side composition (the interactive
// world viewer's), exactly like the old per-frame call, relocated.
//
// Dependencies: f4-simulation (CampaignSession), std::thread/mutex.
// C++20.
#pragma once

#include <f4/simulation/campaign_session.hpp>
#include <f4/simulation/fair_mutex.hpp>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <thread>

namespace f4::simulation {

class CampaignSessionRunner {
public:
    /// Construct over a session (lent — must outlive the runner; the
    /// runner's dtor joins the worker before the host frees it).
    /// Does NOT start the thread — call start().
    /// \param session  the live session to drive.
    /// \param speed    initial wall-clock multiplier (1.0 = real time).
    /// \param paused   initial clock state (the viewer starts paused).
    explicit CampaignSessionRunner(CampaignSession& session,
                                   double speed = 1.0, bool paused = false);

    /// Stops + joins the worker (idempotent). MUST run before the
    /// session dies — hence the runner is destroyed first (declare it
    /// AFTER the session unique_ptr, so reverse-order destruction does
    /// the right thing for free).
    ~CampaignSessionRunner();

    CampaignSessionRunner(const CampaignSessionRunner&) = delete;
    CampaignSessionRunner& operator=(const CampaignSessionRunner&) = delete;
    CampaignSessionRunner(CampaignSessionRunner&&) = delete;
    CampaignSessionRunner& operator=(CampaignSessionRunner&&) = delete;

    /// Spawn the worker thread. Idempotent (a second call is a no-op).
    void start();

    /// Signal the worker to stop and JOIN it. Safe from the UI thread.
    /// MUST NOT be called while holding mutex() (the worker needs it to
    /// finish its current batch — stop() would self-deadlock). Idempotent.
    void stop();

    // --- Thread-safe controls (UI thread) ---------------------------------

    /// The clock switch — LOCKING form: for callers NOT holding mutex()
    /// (library hosts, tests). Sets the runner's atomic AND mirrors the
    /// session's own paused_ flag under the session lock (bounded wait:
    /// the worker's holds are ~6-12 ms by design).
    void set_paused(bool p);

    /// The clock switch — ATOMIC-ONLY form: for callers ALREADY holding
    /// mutex() (the viewer's frame read scope — the Campaign window's
    /// Play/Pause button, the Space shortcut). set_paused() there would
    /// re-lock the mutex we already hold = self-deadlock. The caller
    /// mirrors the session's own flag itself (it holds the lock, so a
    /// direct session_.set_paused is consistent — the worker can't be
    /// mid-advance).
    void set_paused_flag(bool p) noexcept { paused_.store(p); }

    [[nodiscard]] bool paused() const noexcept { return paused_.load(); }

    /// The wall-clock multiplier (speed presets: 1x/10x/60x/240x).
    /// Clamped to [0.0, 1024.0]. The tick dt itself NEVER scales (the
    /// FM's tuned 1/60 s discretization) — only how much sim time the
    /// worker feeds per wall second.
    void set_speed(double s) noexcept;
    [[nodiscard]] double speed() const noexcept { return speed_.load(); }

    /// True when the last advance hit the tick cap — the UI surfaces
    /// "time dilated" (the debt is dropped, the preset outran the CPU).
    [[nodiscard]] bool time_dilated() const noexcept {
        return time_dilated_.load();
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
    /// read+draw scope; the worker takes it per advance batch. One
    /// lock, no nesting — and whatever the host's duty cycle, the
    /// worker's queued lock is always served before the host's re-lock.
    [[nodiscard]] FairMutex& mutex() noexcept { return session_mutex_; }

    /// Run `fn(session)` under the session lock — the fine-grained read
    /// path for hosts that prefer scoped lambdas over a frame-wide
    /// lock. Returns fn's result.
    template <class F>
    auto read(F&& fn) -> decltype(fn(std::declval<CampaignSession&>())) {
        std::lock_guard<FairMutex> lock(session_mutex_);
        return fn(session_);
    }

    /// Sim time advanced by the worker so far (seconds) — diagnostics.
    [[nodiscard]] double advanced_sim_seconds() const noexcept {
        return advanced_sim_s_.load();
    }

    /// The worker's current tick budget (ticks per advance call) —
    /// diagnostics/tests. Starts small and adapts to the measured
    /// per-batch cost (target lock hold: ~6-12 ms).
    [[nodiscard]] int tick_budget() const noexcept {
        return tick_budget_.load();
    }

private:
    void worker_loop_();

    CampaignSession& session_;
    FairMutex session_mutex_;   // FIFO — see fair_mutex.hpp

    std::atomic<bool> stop_{false};
    std::atomic<bool> paused_;
    std::atomic<double> speed_{1.0};
    std::atomic<bool> time_dilated_{false};
    std::atomic<double> advanced_sim_s_{0.0};
    std::atomic<double> effective_speed_{0.0};
    std::atomic<int> tick_budget_{4};   // adaptive: 1..session cap

    std::thread worker_;
    bool started_{false};
};

} // namespace f4::simulation
