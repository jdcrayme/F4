// f4-state-machine/include/f4/fsm/transition.hpp
//
// A single transition in a state machine's transition table:
//   (from_state, event) --[guard?]--> (to_state, action?)
//
// This is the atom from which all state machines in F4 are composed. A
// StateMachine owns a vector of these; process() scans for the first matching
// (from, event) whose guard passes, then fires it.
//
// Template parameters:
//   StateEnum  - the states (an enum class, or any comparable type). Must be
//                EqualityComparable and DefaultConstructible (used for the
//                initial-state entry action's value-initialized event).
//   Event      - the trigger type. Must be EqualityComparable (transitions are
//                matched via t.event == e) and CopyConstructible. May be a
//                plain enum class (no payload) or a std::variant<EventA, ...>
//                of structs (with payload). When using a variant, each
//                alternative must itself be EqualityComparable — for plain
//                structs, add `bool operator==(const T&) const = default;`
//                (C++20). Extract payload in actions via std::get_if.
//
// Action signature: void(const Event&)  -- the action may inspect the event
//   that fired the transition (e.g. read a runway heading payload). This is
//   an upgrade over the architecture proposal's void() action, made so that
//   payload-carrying events (landing clearance with runway, weapon-release
//   with coordinates) don't need side channels.
//
// Guard signature: bool()  -- guards check external/host conditions (is AoA
//   above limit? is gear down?). They capture whatever they need by closure.
//   A guard returning false causes the transition to be skipped (scanning
//   continues to the next matching transition, so fallback transitions work).

#pragma once

#include <functional>

namespace f4::fsm {

template<typename StateEnum, typename Event>
struct Transition {
    StateEnum from{};
    StateEnum to{};
    Event event{};

    /// Optional event matcher. When set, overrides value-based matching
    /// (t.event == e). Use for payload-carrying events (e.g. std::variant)
    /// where matching should be by TYPE, not by payload value:
    ///   .on_if(S::A, S::B, [](const E& e){ return std::holds_alternative<X>(e); })
    /// When null, matching is by value equality (the common enum case).
    std::function<bool(const Event&)> matcher;

    /// Optional guard. Returns false to reject this transition (scan continues).
    /// Captures external/host state by closure.
    std::function<bool()> guard;

    /// Optional action fired AFTER the source state's exit action and BEFORE
    /// the target state's entry action (UML 2 semantics). Receives the event
    /// so payload-carrying events can be inspected.
    std::function<void(const Event&)> action;
};

}  // namespace f4::fsm
