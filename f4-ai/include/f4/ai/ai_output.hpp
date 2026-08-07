// f4-ai/include/f4/ai/ai_output.hpp
//
// AIControlOutput — the per-frame output produced by AI modules.
//
// This is the bridge between the AI brain and the FlightModel. The AI thinks
// in tactical terms (desired pitch/roll/yaw commands, throttle position,
// weapon-release intent); the host converts these into a PilotInput that the
// FlightModel can integrate.
//
// The split between AIControlOutput and PilotInput is intentional:
//   - AIControlOutput is in normalized command space [-1, +1] / [0, 1.5].
//     Modules produce it without knowing the aircraft's control-surface
//     authority limits or dynamic-pressure scaling.
//   - PilotInput (f4/flight/aircraft_state.hpp) is the same normalized space
//     but with the additional guarantee that validate() has been called and
//     every value is in range. The DigitalBrain orchestrator handles the
//     validation step.
//
// Modules may also signal an "override" — when has_override is true, the
// orchestrator ignores all other modules' output and uses this one verbatim.
// This is how collision-avoidance and ground-avoid preempt the active tactic.

#pragma once

#include <cstdint>

namespace f4::ai {

struct AIControlOutput {
    // --- Flight control commands (normalized) ---
    double pitch_cmd{0.0};        // [-1, +1]  -1 = nose down, +1 = nose up
    double roll_cmd{0.0};         // [-1, +1]  -1 = full left, +1 = full right
    double yaw_cmd{0.0};          // [-1, +1]  -1 = full left rudder, +1 = full right
    double throttle_cmd{0.0};     // [0, 1.5]  1.0 = MIL, 1.5 = full afterburner
    double speed_brake_cmd{-1.0}; // [-1, +1] -1 = retracted, +1 = fully extended

    // --- Surface / gear commands ---
    bool   gear_handle_down{false};
    bool   wheel_brakes{false};

    // --- Weapon system intent ---
    bool   trigger_down{false};     // gun trigger held
    bool   weapon_release{false};   // release current weapon (bomb/missile)

    // --- Override flag (collision avoidance, ground avoid) ---
    // When true, the orchestrator ignores all other modules and uses this
    // output verbatim. Set by high-priority defensive modules.
    bool   has_override{false};
};

} // namespace f4::ai
