// f4-flight-model/include/f4/flight/stall_state.hpp
//
// Stall state machine for the 6-DOF flight model.
//
// This is the first real consumer of f4-state-machine. It replaces the
// inline stall tracking (a bare `bool stalled` flag) with a 6-state machine
// matching the FreeFalcon baseline (airframe.h StallMode enum, airframe.cpp
// switch(stallMode), eom.cpp body-rate mods, aero.cpp lift mods).
//
// WHY A STATE MACHINE
//   FreeFalcon's stall logic is a switch(stallMode) scattered across four
//   files with duplicated guard logic, no enforcement of valid transitions,
//   and no way to inspect the machine structure without reading all four
//   files. The state-machine formulation makes the transition table a data
//   structure (serializable, inspectable, testable in isolation) and makes
//   every transition observable via the trace.
//
// DESIGN
//   The stall SM is EVENT-DRIVEN (f4-state-machine), but FreeFalcon's logic
//   is POLLING-DRIVEN (conditions checked each frame). The bridge is the
//   StallDetector: each frame, the FlightModel calls detectEvent() which
//   polls the flight state and returns the event (if any) to feed to the SM.
//
//   The SM owns the STATE LIFECYCLE (None -> EnteringDeepStall -> DeepStall
//   -> Spinning/FlatSpin -> Recovering -> None). The Aero layer owns the
//   DETECTION (vcas < stallSpeed || alpha > criticalAOA) and the FORCE
//   MODIFICATION (lift reduction / zeroing). The FlightModel owns the
//   ORCHESTRATION: it calls detectEvent(), feeds the event to the SM, and
//   writes the SM's state back to AeroState for the next frame's force mod.
//
// TRANSITION TABLE (matches Docs/ARCHITECTURE PROPOSAL.md §7.3)
//
//   None --AoAExceed--> EnteringDeepStall
//   EnteringDeepStall --TimerExpired--> DeepStall
//   DeepStall --SpinDetected--> Spinning
//   DeepStall --AsymmetryDetected--> FlatSpin
//   Spinning --RecoveryAttempt--> Recovering
//   FlatSpin --RecoveryAttempt--> Recovering
//   Recovering --Recovered--> None
//   Recovering --AoAExceed--> EnteringDeepStall  (re-stall if AOA climbs again)
//
// The transition guards are simplified versions of FreeFalcon's conditions,
// using only fields F4 currently tracks (alpha, vcas, qbar, stallSpeed).
// FreeFalcon also uses `slice` (sideslip accumulator), `stallMagnitude`,
// `oscillationTimer`, and `loadingFraction` — these are deferred to a future
// fidelity pass. The current implementation captures the STATE STRUCTURE
// and TRANSITION LIFECYCLE; the force-model fidelity can be refined later
// without changing the SM API.

#pragma once

#include "f4/fsm/f4_fsm.hpp"
#include "f4/flight/constants.hpp"

#include <cmath>

namespace f4::flight {

// ---------------------------------------------------------------------------
// Stall states (6 — matches §7.3; Crashing is handled by the ground model,
// not the stall SM).
// ---------------------------------------------------------------------------
enum class StallState {
    None,               // normal flight
    EnteringDeepStall,  // stall detected, transitioning to deep stall
    DeepStall,          // sustained stalled condition
    Spinning,           // yaw spin from asymmetry in deep stall
    FlatSpin,           // terminal flat spin (lift = 0)
    Recovering          // pilot recovering, awaiting clean airflow
};

// ---------------------------------------------------------------------------
// Stall events (6 — matches §7.3).
// ---------------------------------------------------------------------------
enum class StallEvent {
    AoAExceed,           // alpha/speed exceeds stall boundary
    TimerExpired,        // time in EnteringDeepStall elapsed
    SpinDetected,        // negative alpha + asymmetry -> spin
    AsymmetryDetected,   // large asymmetry -> flat spin
    RecoveryAttempt,     // pilot applying recovery controls
    Recovered            // clean airflow restored
};

// ---------------------------------------------------------------------------
// Stall SM configuration (tunable thresholds).
//
// Defaults are derived from FreeFalcon's constants where they map directly;
// others are reasonable starting values for an F-16-class aircraft.
// ---------------------------------------------------------------------------
struct StallConfig {
    /// Time in EnteringDeepStall before transitioning to DeepStall (seconds).
    /// FreeFalcon uses an oscillation-timer-based transition; we simplify to
    /// a fixed dwell time.
    double deepStallDwell_s{2.0};

    /// Alpha below which spin is detected in DeepStall (degrees, negative).
    /// FreeFalcon: alpha < -10 in the DeepStall -> Spinning guard.
    double spinAlphaThreshold_deg{-10.0};

    /// Q-bar above which recovery is possible (lb/ft^2).
    /// FreeFalcon: qbar > 30 in the Recovering -> None guard.
    double recoveryQbar{30.0};

    /// Alpha below which recovery completes (degrees).
    /// FreeFalcon: alpha < 18 in the Recovering -> None guard.
    double recoveryAlphaMax_deg{18.0};

    /// Alpha above which a recovering aircraft re-stalls (degrees).
    /// FreeFalcon: alpha > g_fRecoveryAOA (default ~25) in Recovering.
    double restallAlpha_deg{25.0};
};

// ---------------------------------------------------------------------------
// The stall state machine type.
// ---------------------------------------------------------------------------
using StallSM = fsm::StateMachine<StallState, StallEvent>;

/// Build the stall state machine from the transition table.
/// The SM is built once at init() and reused for the aircraft's lifetime.
/// The optional StallConfig is captured by value into guards/actions.
StallSM makeStallMachine(const StallConfig& cfg = {});

// ---------------------------------------------------------------------------
// StallDetector: polls flight state each frame and returns the event to
// feed to the SM (or nullopt if no event this frame).
//
// This is the bridge between FreeFalcon's polling-based stall logic and
// f4-state-machine's event-driven model. The FlightModel calls detectEvent()
// once per minor frame, after the aero layer has computed `stalled` and
// `stallSpeed`.
// ---------------------------------------------------------------------------
struct StallDetection {
    StallState currentState;    // SM's current state (before this event)
    bool   aeroStalled;         // aero layer's per-frame stall flag
    double alpha_deg;           // current angle of attack
    double vcas_kts;            // calibrated airspeed
    double stallSpeed_kts;      // computed stall speed
    double qbar;                // dynamic pressure
    double timeInState_s;       // time spent in the current state
    double pstick;              // pitch stick input (-1..+1)
};

/// Determine which event (if any) to emit this frame.
/// Returns the event, or a default-constructed StallEvent{} if none.
/// The caller checks the return against the SM's can_fire() to decide
/// whether to call process().
///
/// Logic (mapped from FreeFalcon airframe.cpp switch(stallMode)):
///   None: if aeroStalled -> AoAExceed
///   EnteringDeepStall: if timeInState > dwell -> TimerExpired
///   DeepStall: if alpha < spinThreshold -> SpinDetected
///   Spinning/FlatSpin: if pstick < -0.3 (pushover) -> RecoveryAttempt
///   Recovering: if qbar > recoveryQbar && alpha < recoveryAlpha -> Recovered
///   Recovering: if alpha > restallAlpha -> AoAExceed (re-stall)
StallEvent detectStallEvent(const StallDetection& d, const StallConfig& cfg);

/// Human-readable name for a stall state (for trace/debug output).
inline const char* stallStateName(StallState s) {
    switch (s) {
        case StallState::None:              return "None";
        case StallState::EnteringDeepStall: return "EnteringDeepStall";
        case StallState::DeepStall:         return "DeepStall";
        case StallState::Spinning:          return "Spinning";
        case StallState::FlatSpin:          return "FlatSpin";
        case StallState::Recovering:        return "Recovering";
    }
    return "?";
}

/// Human-readable name for a stall event (for trace/debug output).
inline const char* stallEventName(StallEvent e) {
    switch (e) {
        case StallEvent::AoAExceed:           return "AoAExceed";
        case StallEvent::TimerExpired:        return "TimerExpired";
        case StallEvent::SpinDetected:        return "SpinDetected";
        case StallEvent::AsymmetryDetected:   return "AsymmetryDetected";
        case StallEvent::RecoveryAttempt:     return "RecoveryAttempt";
        case StallEvent::Recovered:           return "Recovered";
    }
    return "?";
}

}  // namespace f4::flight
