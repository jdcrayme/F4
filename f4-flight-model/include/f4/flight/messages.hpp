// f4-flight-model/include/f4/flight/messages.hpp
//
// Flight-model message types published on the sim MessageBus.
//
// This is the first real consumer of f4-messaging. The pattern is:
//   - The FlightModel owns an OPTIONAL MessageBus* (nullptr by default,
//     set via set_message_bus()).
//   - When the bus is attached, the FlightModel publishes plain-data
//     messages on interesting events (state-machine transitions, etc.).
//   - When no bus is attached, the FlightModel behaves exactly as before
//     (zero overhead — a single null-pointer check per event).
//
// Why state-change messages (not per-frame telemetry)?
//   The bus is for CROSS-SUBSYSTEM notification, not high-frequency
//   telemetry. A StallStateChangeMsg fires ~10 times per sortie; a
//   per-frame "alpha=12.3" message would flood the bus at 360 Hz. Per-frame
//   state stays in AircraftState (read directly by the sim loop); the bus
//   carries only the small set of events that other subsystems (UI audio
//   cues, AI threat assessment, debrief logging) actually need to react to.
//
// Naming convention (per §9.2 examples — DamageMessage, MissileFireMessage,
// WingmanCommandMessage):
//   - Past-tense or noun-phrase names describing WHAT happened.
//   - Plain struct, public fields, no inheritance, no virtuals.
//   - One struct per logical event; do not bundle unrelated events into
//     a single "FlightModelEvent" + enum — that defeats the type-indexed
//     dispatch that makes MessageBus type-safe.
//
// All units match the rest of the flight model: Imperial (feet, ft/s,
// degrees, lb/ft^2). This is intentional — the bus is internal to the
// sim, not a wire format; converting units at the bus boundary would
// add cost without benefit.

#pragma once

#include "f4/flight/stall_state.hpp"

#include <cstdint>

namespace f4::flight {

// ============================================================================
// StallStateChangeMessage
//
// Published when the stall state machine transitions to a new state.
// Fires AT MOST once per minor frame (and usually far less — a typical
// sortie sees 0-5 of these). Consumers:
//   - UI: trigger the StallWarning audio/visual cue when entering any
//     stall state, clear it when returning to None.
//   - AI: re-evaluate the aircraft's tactical posture (a stalled wingman
//     cannot hold formation; a stalled target is an easy kill).
//   - Debrief log: record the stall entry/recovery times for after-action
//     review.
//
// Fields:
//   aircraft_id : host-supplied entity ID (0 if unassigned — the host
//                 sets this via set_aircraft_id() on the FlightModel).
//   from_state  : the previous stall state.
//   to_state    : the new stall state.
//   sim_time_s  : host-supplied sim time at the transition (0 if unassigned).
//   alpha_deg   : angle of attack at the transition (context for "why").
//   vcas_kts    : calibrated airspeed at the transition (context).
//   qbar_psf    : dynamic pressure at the transition (context).
// ============================================================================
struct StallStateChangeMessage {
    std::uint64_t aircraft_id{0};
    StallState    from_state{StallState::None};
    StallState    to_state{StallState::None};
    double        sim_time_s{0.0};
    double        alpha_deg{0.0};
    double        vcas_kts{0.0};
    double        qbar_psf{0.0};
};

// ============================================================================
// StallWarningMessage
//
// Published when the aero layer FIRST detects a stall condition in a
// previously-unstalled aircraft (the rising edge of `aero.stalled`).
// This is the "STALL" caution light / aural "stall, stall" cue trigger.
//
// Distinct from StallStateChangeMessage because the aero detection fires
// BEFORE the SM processes the event — the warning is the "I just noticed
// we're stalling" signal, the state change is the "the SM has now decided
// what to do about it" signal. Some consumers (aural cue) want the
// earliest possible notification; others (formation AI) want the SM's
// authoritative state.
//
// In a typical stall entry, both fire in the same minor frame: warning
// first, then state change.
// ============================================================================
struct StallWarningMessage {
    std::uint64_t aircraft_id{0};
    double        sim_time_s{0.0};
    double        alpha_deg{0.0};
    double        vcas_kts{0.0};
    double        stall_speed_kts{0.0};  // computed stall speed for context
};

}  // namespace f4::flight
