// f4-flight-model/src/stall_state.cpp
//
// Stall state machine factory and event detector.
//
// See stall_state.hpp for the full design documentation.

#include "f4/flight/stall_state.hpp"

#include <optional>

namespace f4::flight {

// ---------------------------------------------------------------------------
// makeStallMachine: build the transition table.
//
// The entry action on EnteringDeepStall resets the stall timer (via a
// callback the FlightModel provides). Guards check the StallConfig thresholds.
// ---------------------------------------------------------------------------
StallSM makeStallMachine(const StallConfig& cfg) {
    return StallSM::Builder()
        .initial(StallState::None)
        .state(StallState::None,              "None")
        .state(StallState::EnteringDeepStall, "EnteringDeepStall")
        .state(StallState::DeepStall,         "DeepStall")
        .state(StallState::Spinning,          "Spinning")
        .state(StallState::FlatSpin,          "FlatSpin")
        .state(StallState::Recovering,        "Recovering")
        .event_name(StallEvent::AoAExceed,           "AoAExceed")
        .event_name(StallEvent::TimerExpired,        "TimerExpired")
        .event_name(StallEvent::SpinDetected,        "SpinDetected")
        .event_name(StallEvent::AsymmetryDetected,   "AsymmetryDetected")
        .event_name(StallEvent::RecoveryAttempt,     "RecoveryAttempt")
        .event_name(StallEvent::Recovered,           "Recovered")
        // None -> EnteringDeepStall: stall detected by aero layer
        .on(StallState::None, StallState::EnteringDeepStall, StallEvent::AoAExceed,
            nullptr, nullptr, "stall-detected")
        // EnteringDeepStall -> DeepStall: dwell timer expired
        .on(StallState::EnteringDeepStall, StallState::DeepStall, StallEvent::TimerExpired,
            nullptr, nullptr, "deep-stall-dwell-elapsed")
        // DeepStall -> Spinning: negative alpha (nose-down spin entry)
        .on(StallState::DeepStall, StallState::Spinning, StallEvent::SpinDetected,
            nullptr, nullptr, "spin-alpha-threshold")
        // DeepStall -> FlatSpin: large asymmetry (stub — F4 doesn't track
        // asymmetry yet; the guard in detectStallEvent never emits this
        // event currently. The transition exists for future fidelity.)
        .on(StallState::DeepStall, StallState::FlatSpin, StallEvent::AsymmetryDetected,
            nullptr, nullptr, "asymmetry-detected")
        // Spinning -> Recovering: pilot recovery attempt
        .on(StallState::Spinning, StallState::Recovering, StallEvent::RecoveryAttempt,
            nullptr, nullptr, "recovery-controls-applied")
        // FlatSpin -> Recovering: pilot recovery (harder — near-terminal)
        .on(StallState::FlatSpin, StallState::Recovering, StallEvent::RecoveryAttempt,
            nullptr, nullptr, "flat-spin-recovery-attempt")
        // Recovering -> None: clean airflow restored
        .on(StallState::Recovering, StallState::None, StallEvent::Recovered,
            nullptr, nullptr, "clean-airflow-restored")
        // Recovering -> EnteringDeepStall: re-stall if AOA climbs again
        .on(StallState::Recovering, StallState::EnteringDeepStall, StallEvent::AoAExceed,
            nullptr, nullptr, "re-stall-during-recovery")
        .build();
}

// ---------------------------------------------------------------------------
// detectStallEvent: poll flight state and return the event to emit (if any).
//
// This function bridges FreeFalcon's polling-based stall logic (checked every
// frame in the switch(stallMode) block) to f4-state-machine's event-driven
// model. The FlightModel calls this once per minor frame after the aero layer
// has computed `stalled` and `stallSpeed`.
//
// At most ONE event is returned per frame. If multiple conditions are true,
// the one most relevant to the current state is chosen (matching FreeFalcon's
// switch-case priority).
// ---------------------------------------------------------------------------
StallEvent detectStallEvent(const StallDetection& d, const StallConfig& cfg) {
    switch (d.currentState) {
        case StallState::None:
            // FreeFalcon: stall entry when vcas < stallSpeed || alpha > criticalAOA
            // The aero layer computes this as the `stalled` boolean.
            if (d.aeroStalled) return StallEvent::AoAExceed;
            break;

        case StallState::EnteringDeepStall:
            // FreeFalcon: complex condition (sinthe, cosphi, oscillation timer).
            // Simplified: dwell timer expired.
            if (d.timeInState_s >= cfg.deepStallDwell_s)
                return StallEvent::TimerExpired;
            // Also check: if stall condition cleared, go to Recovering
            if (!d.aeroStalled) return StallEvent::RecoveryAttempt;
            break;

        case StallState::DeepStall:
            // FreeFalcon: |slice| > 0.3 AND alpha < -10 -> Spinning
            // F4 doesn't track slice; use alpha < -10 as the spin trigger.
            if (d.alpha_deg < cfg.spinAlphaThreshold_deg)
                return StallEvent::SpinDetected;
            // Also check: if stall condition cleared (speed recovered)
            if (!d.aeroStalled && d.qbar > cfg.recoveryQbar)
                return StallEvent::RecoveryAttempt;
            break;

        case StallState::Spinning:
            // FreeFalcon: |slice| < 0.25 -> back to DeepStall (not Recovering).
            // §7.3: RecoveryAttempt -> Recovering. We emit RecoveryAttempt
            // when the pilot pushes the stick forward (pstick < -0.3).
            if (d.pstick < -0.3 && d.qbar > cfg.recoveryQbar * 0.5)
                return StallEvent::RecoveryAttempt;
            break;

        case StallState::FlatSpin:
            // FreeFalcon: terminal. §7.3: RecoveryAttempt possible.
            // Very hard to recover — require strong pushover + good qbar.
            if (d.pstick < -0.5 && d.qbar > cfg.recoveryQbar)
                return StallEvent::RecoveryAttempt;
            break;

        case StallState::Recovering:
            // FreeFalcon: qbar > 30 && alpha < 18 -> None (recovered)
            if (d.qbar > cfg.recoveryQbar && d.alpha_deg < cfg.recoveryAlphaMax_deg)
                return StallEvent::Recovered;
            // FreeFalcon: alpha > recoveryAOA -> re-enter stall
            if (d.alpha_deg > cfg.restallAlpha_deg)
                return StallEvent::AoAExceed;
            break;
    }
    // No event this frame. Return a default; the caller should check
    // sm.can_fire() before calling process() to avoid a no-op.
    return StallEvent::AoAExceed;  // sentinel — caller must check can_fire()
}

}  // namespace f4::flight
