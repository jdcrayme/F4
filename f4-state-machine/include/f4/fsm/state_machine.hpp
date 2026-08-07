// f4-state-machine/include/f4/fsm/state_machine.hpp
//
// StateMachine: a transition-table-driven finite state machine with a fluent
// builder, per-state entry/exit actions, guard predicates, and a built-in
// transition trace for observability.
//
// DESIGN (see Docs/ARCHITECTURE PROPOSAL.md §7). This implementation extends
// the proposal in three ways, all in service of observability — the property
// the F4Flight digi AI lacked:
//
//   1. Entry/exit actions per state (UML 2 semantics). The 17-state landing
//      SM and the AAR SM need entry actions ("on entering Approach, configure
//      flaps/gear"). Without them, entry side-effects scatter across guards
//      and actions, recreating the switch-statement mess the library replaces.
//
//   2. Actions receive the event: action(const Event&). Payload-carrying
//      events (landing clearance with runway heading, weapon release with
//      coordinates) are inspected at the point of transition, not via globals.
//
//   3. Every transition is recorded in a bounded Trace ring buffer and
//      emitted as parseable text. See trace.hpp for the rationale.
//
// TRANSITION FIRING ORDER (UML 2):
//   1. Scan transitions for first (from==current, event==e) whose guard
//      passes. If none, record a no-op and return current (no state change,
//      no exit/entry actions).
//   2. Call the source state's exit action (if registered), passing e.
//   3. Set current_ = transition.to.
//   4. Call the transition's action (if any), passing e.
//   5. Call the target state's entry action (if registered), passing e.
//   6. Append a TransitionRecord to the trace.
//   7. Invoke the on_transition observer (if any) — the real-time streaming
//      hook for hosts that want to log to file/stdout live.
//
// GUARDS: a guard returning false does NOT fire the transition; scanning
// continues to the next (from, event) match. This makes fallback/default
// transitions possible: list a guarded specific transition first, then an
// unguarded general one.

#pragma once

#include "f4/fsm/trace.hpp"
#include "f4/fsm/transition.hpp"

#include <cstddef>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace f4::fsm {

template<typename StateEnum, typename Event>
class StateMachine {
public:
    using TransitionType = Transition<StateEnum, Event>;
    using ActionType     = std::function<void(const Event&)>;
    using Record         = TransitionRecord<StateEnum, Event>;
    using Observer       = std::function<void(const Record&)>;

    class Builder {
    public:
        /// Set the initial state. Required before build().
        Builder& initial(StateEnum s) { initial_ = s; return *this; }

        /// Register a human-readable name for a state (improves trace output).
        Builder& state(StateEnum s, std::string name) {
            state_names_[s] = std::move(name);
            return *this;
        }

        /// Register a human-readable name for an event value. Works for enum
        /// class events and std::variant events alike (stored as a pair list,
        /// looked up via operator== — no hash required).
        Builder& event_name(Event e, std::string name) {
            event_names_.emplace_back(std::move(e), std::move(name));
            return *this;
        }

        /// Register an action to run whenever state s is ENTERED. The action
        /// receives the triggering event. On reset(), the initial state's
        /// entry action is called with a value-initialized event.
        Builder& on_enter(StateEnum s, ActionType action) {
            entry_actions_[s] = std::move(action);
            return *this;
        }

        /// Register an action to run whenever state s is EXITED.
        Builder& on_exit(StateEnum s, ActionType action) {
            exit_actions_[s] = std::move(action);
            return *this;
        }

        /// Add a transition matched by EVENT VALUE (t.event == e). The common
        /// case for plain enum events. Guards/actions/reason are optional.
        /// Transitions are scanned in insertion order; the first match whose
        /// guard passes wins.
        Builder& on(StateEnum from, StateEnum to, Event event,
                    ActionType action = nullptr,
                    std::function<bool()> guard = nullptr,
                    std::string reason = {})
        {
            TransitionType t;
            t.from   = from;
            t.to     = to;
            t.event  = std::move(event);
            t.guard  = std::move(guard);
            t.action = std::move(action);
            transitions_.push_back(std::move(t));
            reasons_.push_back(std::move(reason));
            return *this;
        }

        /// Add a transition matched by a PREDICATE on the event. Use this for
        /// payload-carrying events (e.g. std::variant) where matching should be
        /// by TYPE, not by payload value:
        ///   .on_if(S::A, S::B,
        ///          [](const Event& e){ return std::holds_alternative<X>(e); })
        /// The predicate receives the event; returning true claims the match
        /// (the guard, if any, is then evaluated). Scanning order and
        /// first-match-wins semantics are identical to on().
        Builder& on_if(StateEnum from, StateEnum to,
                       std::function<bool(const Event&)> matcher,
                       ActionType action = nullptr,
                       std::function<bool()> guard = nullptr,
                       std::string reason = {})
        {
            TransitionType t;
            t.from    = from;
            t.to      = to;
            t.matcher = std::move(matcher);
            t.guard   = std::move(guard);
            t.action  = std::move(action);
            transitions_.push_back(std::move(t));
            reasons_.push_back(std::move(reason));
            return *this;
        }

        /// Build the state machine. The initial state's entry action is NOT
        /// fired here; call reset() (or rely on the constructor) to fire it.
        StateMachine build() {
            return StateMachine(initial_, std::move(transitions_),
                                std::move(reasons_),
                                std::move(state_names_),
                                std::move(event_names_),
                                std::move(entry_actions_),
                                std::move(exit_actions_));
        }

    private:
        StateEnum initial_{};
        std::vector<TransitionType>     transitions_;
        std::vector<std::string>        reasons_;
        std::unordered_map<StateEnum, std::string>  state_names_;
        // Event names stored as a vector of pairs (linear scan) rather than a
        // map, because Event may be a std::variant (which has no std::hash).
        // Only operator== is required, which the framework already needs for
        // transition matching. Event counts are small (<30), so O(n) lookup
        // is negligible.
        std::vector<std::pair<Event, std::string>>  event_names_;
        std::unordered_map<StateEnum, ActionType>   entry_actions_;
        std::unordered_map<StateEnum, ActionType>   exit_actions_;
    };

    /// Process an event. Returns the state after processing (== current() if
    /// no transition fired). See file header for firing order.
    StateEnum process(const Event& e) {
        ++tick_;
        bool any_match = false;  // any (from, event) pair matched at all?
        for (std::size_t i = 0; i < transitions_.size(); ++i) {
            const auto& t = transitions_[i];
            if (t.from != current_) continue;
            // Match: by predicate (on_if) if present, else by value (on).
            const bool matches = t.matcher ? t.matcher(e) : (t.event == e);
            if (!matches) continue;
            any_match = true;

            const bool has_guard = static_cast<bool>(t.guard);
            const bool guard_ok  = !has_guard || t.guard();

            if (!guard_ok) {
                // Guard rejected — record and continue scanning for a fallback.
                Record r;
                r.tick         = tick_;
                r.from         = current_;
                r.to           = current_;
                r.event        = e;
                r.fired        = false;
                r.had_guard    = has_guard;
                r.guard_passed = false;
                r.reason       = reasons_[i];
                if (trace_) trace_->append(r);
                if (observer_) observer_(r);
                continue;
            }

            // Fire the transition (UML 2 order).
            StateEnum src = current_;
            StateEnum dst = t.to;

            bool had_exit = false;
            auto exit_it = exit_actions_.find(src);
            if (exit_it != exit_actions_.end() && exit_it->second) {
                exit_it->second(e);
                had_exit = true;
            }

            current_ = dst;

            bool had_action = false;
            if (t.action) { t.action(e); had_action = true; }

            bool had_entry = false;
            auto entry_it = entry_actions_.find(dst);
            if (entry_it != entry_actions_.end() && entry_it->second) {
                entry_it->second(e);
                had_entry = true;
            }

            Record r;
            r.tick         = tick_;
            r.from         = src;
            r.to           = dst;
            r.event        = e;
            r.fired        = true;
            r.had_guard    = has_guard;
            r.guard_passed = true;
            r.had_action   = had_action;
            r.had_exit     = had_exit;
            r.had_entry    = had_entry;
            r.reason       = reasons_[i];
            if (trace_) trace_->append(r);
            if (observer_) observer_(r);

            return current_;
        }

        // No matching transition fired. Only record a no-op when NO (from,
        // event) pair was even considered — if a guard rejected, that
        // rejection was already recorded above and a duplicate no-match
        // record would be noise.
        if (!any_match && trace_ && trace_->trace_rejections()) {
            Record r;
            r.tick   = tick_;
            r.from   = current_;
            r.to     = current_;
            r.event  = e;
            r.fired  = false;
            r.reason = "no-matching-transition";
            trace_->append(r);
            if (observer_) observer_(r);
        }
        return current_;
    }

    /// Current state.
    [[nodiscard]] StateEnum current() const noexcept { return current_; }

    /// The initial state set at build time.
    [[nodiscard]] StateEnum initial() const noexcept { return initial_; }

    /// All transitions in insertion order (for inspection/serialization).
    [[nodiscard]] const std::vector<TransitionType>& transitions() const noexcept {
        return transitions_;
    }

    /// Per-transition reason strings (parallel to transitions()).
    [[nodiscard]] const std::vector<std::string>& reasons() const noexcept {
        return reasons_;
    }

    /// Can the event fire ANY transition from the current state (guard
    /// considered)? This is the "is this event meaningful right now" query.
    [[nodiscard]] bool can_fire(const Event& e) const {
        for (const auto& t : transitions_) {
            if (t.from != current_) continue;
            const bool matches = t.matcher ? t.matcher(e) : (t.event == e);
            if (matches && (!t.guard || t.guard()))
                return true;
        }
        return false;
    }

    /// Is there a transition (current -> to) on event e with a passing guard?
    [[nodiscard]] bool can_transition(StateEnum to, const Event& e) const {
        for (const auto& t : transitions_) {
            if (t.from != current_ || t.to != to) continue;
            const bool matches = t.matcher ? t.matcher(e) : (t.event == e);
            if (matches && (!t.guard || t.guard()))
                return true;
        }
        return false;
    }

    /// Reset to the initial state and fire the initial state's entry action
    /// (with a value-initialized event). Clears the tick counter. Does NOT
    /// clear the trace (call trace().clear() if you want that).
    void reset() {
        current_ = initial_;
        tick_ = 0;
        auto it = entry_actions_.find(initial_);
        if (it != entry_actions_.end() && it->second) {
            it->second(Event{});
        }
    }

    /// Force the machine to state `s` WITHOUT firing any entry/exit actions
    /// or recording a transition. This is an administrative reset — use it
    /// to suppress a layer in a LayeredStateMachine (C2 fix) or to restore
    /// a machine to a known state for testing. The tick counter is NOT
    /// cleared (unlike reset()) — the machine's timeline continues.
    ///
    /// IMPORTANT: this bypasses UML 2 transition semantics. Do NOT use
    /// this as a substitute for process() in normal operation. The only
    /// correct use cases are inter-layer inhibition and test fixtures.
    void force_to_state(StateEnum s) noexcept {
        current_ = s;
    }

    /// Monotonic tick counter (increments on every process() call).
    [[nodiscard]] std::size_t tick() const noexcept { return tick_; }

    /// Human-readable name for a state, or empty if unregistered.
    [[nodiscard]] std::string_view name_of(StateEnum s) const {
        auto it = state_names_.find(s);
        return it != state_names_.end() ? std::string_view(it->second)
                                        : std::string_view{};
    }
    /// Human-readable name for an event value, or empty if unregistered.
    /// Linear scan over registered names (Event may be a variant with no hash).
    [[nodiscard]] std::string_view name_of(const Event& e) const {
        for (const auto& [ev, nm] : event_names_) {
            if (ev == e) return nm;
        }
        return {};
    }

    // --- Trace management -------------------------------------------------

    /// Attach a trace. The machine will append a record for every transition
    /// (and every rejected attempt, if trace->trace_rejections() is on).
    void set_trace(Trace<StateEnum, Event>* t) noexcept { trace_ = t; }
    [[nodiscard]] const Trace<StateEnum, Event>* trace() const noexcept { return trace_; }

    /// Attach a real-time observer: called with every TransitionRecord, in
    /// order, as transitions fire. Use to stream a trace to a file/stdout.
    void set_observer(Observer obs) { observer_ = std::move(obs); }

private:
    StateMachine(StateEnum initial,
                 std::vector<TransitionType>     transitions,
                 std::vector<std::string>        reasons,
                 std::unordered_map<StateEnum, std::string>  state_names,
                 std::vector<std::pair<Event, std::string>>  event_names,
                 std::unordered_map<StateEnum, ActionType>   entry_actions,
                 std::unordered_map<StateEnum, ActionType>   exit_actions)
        : current_(initial)
        , initial_(initial)
        , transitions_(std::move(transitions))
        , reasons_(std::move(reasons))
        , state_names_(std::move(state_names))
        , event_names_(std::move(event_names))
        , entry_actions_(std::move(entry_actions))
        , exit_actions_(std::move(exit_actions))
    {
        // Fire the initial state's entry action (UML: entering the initial
        // pseudostate's target). Value-initialized event = "init".
        auto it = entry_actions_.find(initial_);
        if (it != entry_actions_.end() && it->second) {
            it->second(Event{});
        }
    }

    StateEnum       current_{};
    StateEnum       initial_{};
    std::size_t     tick_{0};
    std::vector<TransitionType>     transitions_;
    std::vector<std::string>        reasons_;
    std::unordered_map<StateEnum, std::string>  state_names_;
    std::vector<std::pair<Event, std::string>>  event_names_;
    std::unordered_map<StateEnum, ActionType>   entry_actions_;
    std::unordered_map<StateEnum, ActionType>   exit_actions_;
    Trace<StateEnum, Event>*  trace_{nullptr};
    Observer                  observer_;
};

}  // namespace f4::fsm
