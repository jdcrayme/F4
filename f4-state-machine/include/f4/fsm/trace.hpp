// f4-state-machine/include/f4/fsm/trace.hpp
//
// Transition trace: a bounded ring-buffer record of every transition a
// StateMachine has fired, emitted as parseable text.
//
// WHY THIS EXISTS
//   The F4Flight digi AI "evolved to a level of complexity where
//   unpredictable behavior was impossible to track down", and text data could
//   not be extracted from scenarios in a way that supported diagnosis. This
//   trace is the direct remedy: every state change is recorded with enough
//   context to diagnose it from text alone — no screenshots, no HTML, no
//   guesswork. A test or a host can dump the last N transitions as plain
//   newline-delimited records and reason over them directly.
//
// FORMAT
//   Each transition is one line:
//     tick=<n> from=<S> to=<S> event=<E> guard=<PASS|FAIL|NONE>
//     action=<RAN|NONE> entry=<RAN|NONE> exit=<RAN|NONE> reason="<text>"
//   "guard=FAIL" lines record a transition that was CONSIDERED but rejected
//   by its guard — these are recorded only when trace_rejections is enabled,
//   because they're the #1 cause of "why didn't it transition?" bugs.
//
// The trace is a ring buffer (bounded capacity, default 1024). When full, the
// oldest record is evicted. This keeps memory bounded for long-running sims
// while always retaining recent history (the part you need when something
// goes wrong). set_capacity(0) = unbounded.

#pragma once

#include <algorithm>
#include <cstddef>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace f4::fsm {

/// One recorded transition (or rejected transition attempt).
template<typename StateEnum, typename Event>
struct TransitionRecord {
    std::size_t tick{0};                 ///< monotonic tick at which process() was called
    StateEnum   from{};                  ///< state before the transition
    StateEnum   to{};                    ///< state after (== from if rejected)
    Event       event{};                 ///< the event submitted
    bool        fired{true};             ///< false => guard rejected, no state change
    bool        had_guard{false};
    bool        guard_passed{false};
    bool        had_action{false};
    bool        had_exit{false};
    bool        had_entry{false};
    std::string reason;                  ///< optional human tag from the guard/caller
};

/// Bounded ring buffer of transition records with text emission.
template<typename StateEnum, typename Event>
class Trace {
public:
    using Record = TransitionRecord<StateEnum, Event>;

    /// Set the max number of records retained. 0 = unbounded. Default 1024.
    void set_capacity(std::size_t n) noexcept {
        capacity_ = n;
        if (n != 0 && records_.size() > n) {
            // Evict oldest until within capacity.
            records_.erase(records_.begin(),
                           records_.begin() + static_cast<std::ptrdiff_t>(records_.size() - n));
        }
    }
    std::size_t capacity() const noexcept { return capacity_; }
    std::size_t size()  const noexcept { return records_.size(); }
    bool        empty() const noexcept { return records_.empty(); }

    /// Enable/disable recording of guard-rejected attempts. Off by default
    /// (fired transitions only) to keep the trace signal-dense. Turn on when
    /// diagnosing "why didn't it transition?".
    void set_trace_rejections(bool on) noexcept { trace_rejections_ = on; }
    bool trace_rejections() const noexcept { return trace_rejections_; }

    /// Append a record. Called by StateMachine::process().
    void append(Record r) {
        if (!r.fired && !trace_rejections_) return;
        if (capacity_ != 0 && records_.size() >= capacity_) {
            records_.erase(records_.begin());
        }
        records_.push_back(std::move(r));
    }

    /// Read-only access to the records.
    const std::vector<Record>& records() const noexcept { return records_; }

    /// Clear the trace (does not reset tick).
    void clear() noexcept { records_.clear(); }

    /// Emit the trace as parseable text, one record per line. Uses the
    /// provided name-of functions to render states/events; if a name function
    /// returns an empty string, the numeric value is used.
    std::string to_text(
        const std::function<std::string_view(StateEnum)>& name_of_state,
        const std::function<std::string_view(Event)>&      name_of_event) const
    {
        std::string out;
        out.reserve(records_.size() * 96);
        for (const auto& r : records_) {
            out += "tick=";
            out += std::to_string(r.tick);
            out += " from=";
            out += render_state(name_of_state, r.from);
            out += " to=";
            out += render_state(name_of_state, r.to);
            out += " event=";
            out += render_event(name_of_event, r.event);
            out += " fired=";
            out += r.fired ? "1" : "0";
            out += " guard=";
            out += !r.had_guard ? "NONE" : (r.guard_passed ? "PASS" : "FAIL");
            out += " action=";
            out += r.had_action ? "RAN" : "NONE";
            out += " exit=";
            out += r.had_exit ? "RAN" : "NONE";
            out += " entry=";
            out += r.had_entry ? "RAN" : "NONE";
            if (!r.reason.empty()) {
                out += " reason=\"";
                out += r.reason;
                out += '\"';
            }
            out += '\n';
        }
        return out;
    }

    /// Summary counts: how many times each (from -> to) pair fired.
    /// One line per distinct pair: "from=A to=B count=<n>".
    std::string summary(
        const std::function<std::string_view(StateEnum)>& name_of_state) const
    {
        // Simple O(n^2) aggregation — traces are bounded and small.
        std::vector<std::pair<std::pair<StateEnum, StateEnum>, std::size_t>> counts;
        for (const auto& r : records_) {
            if (!r.fired) continue;
            std::pair<StateEnum, StateEnum> key{r.from, r.to};
            bool found = false;
            for (auto& c : counts) {
                if (c.first == key) { ++c.second; found = true; break; }
            }
            if (!found) counts.push_back({key, 1});
        }
        std::string out;
        for (const auto& c : counts) {
            out += "from=";
            out += render_state(name_of_state, c.first.first);
            out += " to=";
            out += render_state(name_of_state, c.first.second);
            out += " count=";
            out += std::to_string(c.second);
            out += '\n';
        }
        return out;
    }

private:
    static std::string render_state(
        const std::function<std::string_view(StateEnum)>& name_of_state,
        StateEnum s)
    {
        if (name_of_state) {
            auto sv = name_of_state(s);
            if (!sv.empty()) return std::string(sv);
        }
        return "state(" + std::to_string(static_cast<long long>(s)) + ')';
    }
    static std::string render_event(
        const std::function<std::string_view(Event)>& name_of_event,
        const Event& e)
    {
        if (name_of_event) {
            auto sv = name_of_event(e);
            if (!sv.empty()) return std::string(sv);
        }
        return "event(" + std::to_string(static_cast<long long>(e)) + ')';
    }

    std::vector<Record> records_;
    std::size_t         capacity_{1024};
    bool                trace_rejections_{false};
};

}  // namespace f4::fsm
