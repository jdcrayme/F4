// f4-simulation/include/f4/simulation/fair_mutex.hpp
//
// FairMutex — a first-in-first-out (ticket-order) mutex.
//
// WHY IT EXISTS (the "campaign time doesn't advance" regression):
// the world viewer's frame scope holds the campaign session lock for
// essentially the WHOLE frame — the input pass, the canvas + ImGui draw,
// and raylib's EndDrawing() 60 FPS pace wait — then releases it for only
// tens of microseconds (process_session_stop + the loop-top checks)
// before re-locking. A plain std::mutex is NOT fair under that duty
// cycle: the UI thread's uncontended fast-path re-lock beats the woken
// waiter essentially every time (measured on a 2-core box: the
// CampaignSessionRunner worker advanced 0.0 sim-seconds over 3 wall
// seconds — a COMPLETE starvation, campaign time frozen while the UI
// stayed perfectly smooth).
//
// Ticket-order service fixes the class of bug, not the instance:
// whoever queued first is served first, and every queued waiter is
// served before any newcomer. With two users (the runner's worker and
// the viewer's frame) they strictly alternate — the worker is
// guaranteed at least one advance batch per frame, so at 1x the
// campaign clock tracks wall-clock by construction, and no host-side
// lock pattern, however lopsided, can freeze it again.
//
// Interface-compatible with std::mutex (lock / unlock / try_lock) so
// std::lock_guard / std::unique_lock work unchanged. Waiters BLOCK on a
// condition variable — no spinning, safe on single/dual-core machines
// (a spin lock would burn the very CPU the worker needs).
//
// Cost: two users means at most two waiters; unlock() notifies both and
// each re-checks its ticket — microseconds, at frame/batch rates.
//
// Dependencies: f4-simulation (header-only), C++20.
#pragma once

#include <condition_variable>
#include <cstdint>
#include <mutex>

namespace f4::simulation {

class FairMutex {
public:
    FairMutex() = default;
    ~FairMutex() = default;
    FairMutex(const FairMutex&) = delete;
    FairMutex& operator=(const FairMutex&) = delete;

    /// Blocks until every waiter that queued earlier has released.
    /// Queue position is fixed at call time — a newcomer arriving while
    /// we wait can never jump ahead.
    void lock() {
        std::unique_lock<std::mutex> guard(state_mu_);
        const std::uint64_t ticket = next_ticket_++;
        while (ticket != served_) {
            cv_.wait(guard);  // spurious wakes re-checked by the loop
        }
    }

    /// True when the mutex is free AND nobody is queued (a newcomer
    /// would be served immediately); false when ownership or the queue
    /// is ahead. Never jumps the queue.
    bool try_lock() {
        std::lock_guard<std::mutex> guard(state_mu_);
        if (next_ticket_ == served_) {  // unlocked, no waiters
            ++next_ticket_;
            return true;
        }
        return false;
    }

    /// Serves the next ticket in line. Wakes all waiters; only the one
    /// holding the served ticket proceeds, the rest re-sleep.
    void unlock() {
        {
            std::lock_guard<std::mutex> guard(state_mu_);
            ++served_;
        }
        cv_.notify_all();
    }

private:
    std::mutex state_mu_;                 // guards the two counters
    std::condition_variable cv_;
    std::uint64_t next_ticket_ = 0;       // last ticket handed out
    std::uint64_t served_ = 0;            // ticket currently holding
};

} // namespace f4::simulation
