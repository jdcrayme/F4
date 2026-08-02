// f4-state-machine/include/f4/fsm/serialize.hpp
//
// Serialization of a StateMachine's transition table and a Trace to plain,
// parseable text. No JSON library dependency (f4-state-machine is zero-dep).
//
// TABLE FORMAT (to_text(sm)):
//   Each transition is one line:
//     transition from=<S> to=<S> event=<E> guard=<yes|no> action=<yes|no> reason="<text>"
//   Preceded by a header line:
//     statemachine initial=<S> states=<n> transitions=<n>
//   This is the contract surface for "what CAN this machine do?" — load it,
//   inspect it, diff it against another build, without running the machine.
//
// TRACE FORMAT (see trace.hpp): one TransitionRecord per line.
//
// Both formats are line-oriented and grep-friendly. The intent (learned from
// the F4Flight viz dead-end) is that a human OR a script can extract
// conclusions from the text directly — no screenshot parsing required.

#pragma once

#include "f4/fsm/state_machine.hpp"
#include "f4/fsm/trace.hpp"

#include <string>

namespace f4::fsm {

/// Render a state machine's transition table as parseable text.
template<typename StateEnum, typename Event>
std::string to_text(const StateMachine<StateEnum, Event>& sm) {
    std::string out;
    const auto& ts = sm.transitions();
    out.reserve(64 + ts.size() * 80);

    out += "statemachine initial=";
    out += render_state_name(sm, sm.initial());
    out += " transitions=";
    out += std::to_string(ts.size());
    out += '\n';

    for (std::size_t i = 0; i < ts.size(); ++i) {
        const auto& t = ts[i];
        out += "transition from=";
        out += render_state_name(sm, t.from);
        out += " to=";
        out += render_state_name(sm, t.to);
        out += " event=";
        out += render_event_name(sm, t.event);
        out += " guard=";
        out += t.guard ? "yes" : "no";
        out += " action=";
        out += t.action ? "yes" : "no";
        const auto& reason = sm.reasons()[i];
        if (!reason.empty()) {
            out += " reason=\"";
            out += reason;
            out += '\"';
        }
        out += '\n';
    }
    return out;
}

/// Render a trace as parseable text. Convenience wrapper around Trace::to_text
/// that supplies the state machine's name registries.
template<typename StateEnum, typename Event>
std::string to_text(const StateMachine<StateEnum, Event>& sm,
                    const Trace<StateEnum, Event>& trace)
{
    return trace.to_text(
        [&sm](StateEnum s){ return sm.name_of(s); },
        [&sm](const Event& e){ return sm.name_of(e); });
}

/// Render a trace summary (per (from,to) transition counts).
template<typename StateEnum, typename Event>
std::string summary_text(const StateMachine<StateEnum, Event>& sm,
                         const Trace<StateEnum, Event>& trace)
{
    return trace.summary([&sm](StateEnum s){ return sm.name_of(s); });
}

// --- internal render helpers ---------------------------------------------

template<typename StateEnum, typename Event>
std::string render_state_name(const StateMachine<StateEnum, Event>& sm,
                              StateEnum s)
{
    auto sv = sm.name_of(s);
    if (!sv.empty()) return std::string(sv);
    return "state(" + std::to_string(static_cast<long long>(s)) + ')';
}

template<typename StateEnum, typename Event>
std::string render_event_name(const StateMachine<StateEnum, Event>& sm,
                              const Event& e)
{
    auto sv = sm.name_of(e);
    if (!sv.empty()) return std::string(sv);
    return "event(" + std::to_string(static_cast<long long>(e)) + ')';
}

}  // namespace f4::fsm
