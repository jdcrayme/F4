// f4-flight-api/pilot_input.hpp
//
// PilotInput — per-frame control input from pilot or AI to the flight model.
//
// All control inputs are normalized:
//   pstick, rstick, ypedal: [-1, +1]
//   throttle: [0, 1.5] where 1.0 = MIL power, 1.5 = full afterburner
//   speedBrake: [-1, +1] where -1 = retracted, +1 = fully extended
//   gearHandle: [-1, +1] where -1 = up, +1 = down
//   tefCmd, lefCmd: [0, 1] where 0 = retracted, 1 = fully extended
//
// Phase 2+: Moved from f4-flight-model/aircraft_state.hpp to this
// lightweight API library so that f4-ai and other consumers can depend
// on the control contract without pulling in the full flight model
// (and its transitive deps: f4-data, f4-math, f4-state-machine, etc.).
//
// C++20.

#pragma once

namespace f4::flight {

// ---------------------------------------------------------------------------
// Pilot / AI input
// ---------------------------------------------------------------------------
struct PilotInput {
    double pstick{0.0};      // pitch stick: -1 (nose down) .. +1 (nose up)
    double rstick{0.0};      // roll stick:  -1 (full left) .. +1 (full right)
    double ypedal{0.0};      // rudder pedal: -1 (full left) .. +1 (full right)
    double throttle{0.0};    // 0..1.5 (1.0 = MIL, 1.5 = full AB)
    double speedBrake{-1.0}; // -1 (retract) .. +1 (extend); default retracted
    double gearHandle{1.0};  // -1 (up) .. +1 (down); default down
    double hookHandle{0.0};  // -1 (up) .. +1 (down)

    double tefCmd{0.0};      // trailing-edge flap command, 0..1
    double lefCmd{0.0};      // leading-edge flap command, 0..1

    // EXPERIMENT S: Roll-limit commands from the steering layer.
    // When maxRollDeg < 80.0, the FCS clamps the bank to ±maxRollDeg and
    // applies a roll-rate taper as the bank approaches the limit (Falcon's
    // maxRollDelta mechanism). maxRollDeltaDeg sets the taper window width.
    // Negative values (the default) mean "no limit set" — the FCS uses its
    // internal defaults (80° bank, 5° taper window).
    double maxRollDeg{-1.0};       // bank limit (deg); <0 = no override
    double maxRollDeltaDeg{-1.0};  // roll-rate taper window (deg); <0 = no override

    bool wheelBrakes{false};
    bool parkingBrake{false};
    bool noseSteerOn{true};
    bool refueling{false};

    /// Validate and clamp all inputs to their documented ranges.
    ///
    /// This is the boundary between external input (AI, scripting, network)
    /// and the flight model — the most hardened interface in the system.
    /// Call this before passing PilotInput to FlightModel::update().
    /// Out-of-range inputs are clamped; in debug builds, out-of-range
    /// values also trigger an assertion failure.
    void validate() noexcept;
};

} // namespace f4::flight
