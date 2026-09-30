// f4-campaign/include/f4/campaign/due_queue.hpp
//
// AGG-2a — the deterministic due-queue (Docs/AGGREGATE_CLOCK_PLAN.md
// §4). The plan's named primitive for the aggregate-clock migration:
// work pops strictly by (due_time, priority, insertion_seq) — no RNG,
// no wall clock, no float keys — so two identically-driven sessions
// pop identical work in identical order. The determinism contract is
// satisfied by construction, not by discipline.
//
// Consumers (as-built and planned):
//   * AGG-2b — per-unit detection cadences (each unit's first due time
//     staggered by stagger_phase(VU, interval), then re-armed from its
//     own pop).
//   * AGG-4 — the discrete-event endgame (waypoint arrivals, TOT
//     windows, fuel gates, recovery deadlines) — the queue IS the
//     scheduler there; aggregate propagation between events is zero.
//
// The stagger replaces the reference's rand() jitter (FreeFalcon
// spreads per-unit update times with rand() % interval): hash(VU) %
// interval phases each unit's first due time inside its interval.
// FNV-1a, not std::hash — std::hash is implementation-defined and the
// phase must be pinned across builds and replays.
//
// Header-only, dependency-free (CampaignTime is the campaign's int64
// second). C++20.

#pragma once

#include <f4/campaign/mission_type.hpp>  // CampaignTime

#include <cstddef>
#include <cstdint>
#include <optional>
#include <set>
#include <utility>

namespace f4::campaign {

/// FNV-1a (64-bit) over the VU's four bytes. Not std::hash: the
/// standard leaves it implementation-defined, and the stagger phase
/// must be bit-stable across stdlib builds for the replay axis to
/// hold. Same input, same phase, every build.
[[nodiscard]] inline std::uint64_t vu_hash(std::uint32_t vu) noexcept {
    std::uint64_t h = 14695981039346656037ull;  // FNV-1a offset basis
    for (int shift = 0; shift < 32; shift += 8) {
        h ^= static_cast<std::uint64_t>((vu >> shift) & 0xFFu);
        h *= 1099511628211ull;                  // FNV-1a prime
    }
    return h;
}

/// The deterministic stagger: phase a unit's first due time inside its
/// cadence interval. `stagger_phase(vu, interval) + base` schedules the
/// unit's first tick; the unit re-arms at `now + interval` from its own
/// pop. interval <= 0 degenerates to 0 (always due — the caller's
/// contract decides what that means; the queue itself never guesses).
[[nodiscard]] inline CampaignTime stagger_phase(std::uint32_t vu,
                                                CampaignTime interval) noexcept {
    if (interval <= 0) return 0;
    return static_cast<CampaignTime>(vu_hash(vu) %
                                     static_cast<std::uint64_t>(interval));
}

/// The ordered scheduler. One entry = one unit of due work.
///
/// Ordering: (due_time, priority, insertion_seq) ascending —
///   * due_time first (the clock's order),
///   * priority breaks same-instant ties, LOWER value first (P0 fires
///     before P1 — book-keeping that later work reads must run first),
///   * the insertion sequence breaks same-priority ties FIFO, in
///     schedule() call order.
///
/// pop_due(now) fires every entry with due <= now in exactly that
/// order, handing each payload to the visitor and dropping the entry.
/// Re-scheduling from inside the visitor is safe (the queue's nodes
/// are independent; the visitor's own schedule() cannot invalidate the
/// iteration — pop_due holds its node handle across the call).
template <typename Payload>
class DueQueue {
public:
    /// Schedule `payload` to fire at `due`. Returns the entry's
    /// sequence number (monotonic from 0 over the queue's lifetime —
    /// the FIFO tie-break's ledger, useful for probes).
    std::uint64_t schedule(CampaignTime due, int priority, Payload payload) {
        const std::uint64_t seq = next_seq_++;
        entries_.insert(Entry{due, priority, seq, std::move(payload)});
        return seq;
    }

    /// Fire every entry with due <= now, in key order. The visitor is
    /// `void(Payload&&)`. Work the visitor schedules mid-pass (the
    /// re-arm: a fired unit schedules its next tick) participates
    /// immediately — the cursor re-reads the queue head after every
    /// visit, so a re-scheduled entry whose due <= now fires within
    /// the SAME pass, ordered by the key: anything already in flight
    /// at the same due/priority carries an earlier seq and fires
    /// first; a re-arm with a later due (now + interval, the cadence
    /// shape) waits for its own pop.
    template <typename Fn>
    void pop_due(CampaignTime now, Fn&& fn) {
        for (;;) {
            const auto it = entries_.begin();
            if (it == entries_.end() || it->due > now) break;
            auto node = entries_.extract(it);
            fn(std::move(node.value().payload));
        }
    }

    /// The head's due time (nullopt when empty) — the session's
    /// "how long can this pass sleep" read for the AGG-4 scheduler.
    [[nodiscard]] std::optional<CampaignTime> peek_due() const {
        if (entries_.empty()) return std::nullopt;
        return entries_.begin()->due;
    }

    [[nodiscard]] bool empty() const noexcept { return entries_.empty(); }
    [[nodiscard]] std::size_t size() const noexcept { return entries_.size(); }
    void clear() noexcept { entries_.clear(); }

private:
    struct Entry {
        CampaignTime due;
        int priority;
        std::uint64_t seq;
        Payload payload;

        [[nodiscard]] bool operator<(const Entry& o) const noexcept {
            return std::tie(due, priority, seq) <
                   std::tie(o.due, o.priority, o.seq);
        }
    };

    std::set<Entry> entries_;
    std::uint64_t next_seq_ = 0;
};

} // namespace f4::campaign
