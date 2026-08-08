// f4-flight-api/src/pilot_input.cpp
//
// PilotInput::validate() — clamp all control inputs to documented ranges.
//
// Moved from f4-flight-model/src/flight_model.cpp as part of Phase 2+.
// The implementation only uses <algorithm> and <cassert> — no flight-model
// internals.

#include "f4/flight/api/pilot_input.hpp"

#include <algorithm>
#include <cassert>

namespace f4::flight {

// ---------------------------------------------------------------------------
// PilotInput::validate — clamp all inputs to documented ranges
// ---------------------------------------------------------------------------
void PilotInput::validate() noexcept {
    // In debug, assert before clamping so the caller knows they sent bad data.
    assert(pstick    >= -1.0 && pstick    <= 1.0 && "pstick out of range [-1, +1]");
    assert(rstick    >= -1.0 && rstick    <= 1.0 && "rstick out of range [-1, +1]");
    assert(ypedal    >= -1.0 && ypedal    <= 1.0 && "ypedal out of range [-1, +1]");
    assert(throttle  >=  0.0 && throttle  <= 1.5 && "throttle out of range [0, 1.5]");
    assert(speedBrake >= -1.0 && speedBrake <= 1.0 && "speedBrake out of range [-1, +1]");
    assert(gearHandle >= -1.0 && gearHandle <= 1.0 && "gearHandle out of range [-1, +1]");
    assert(hookHandle >= -1.0 && hookHandle <= 1.0 && "hookHandle out of range [-1, +1]");
    assert(tefCmd    >=  0.0 && tefCmd    <= 1.0 && "tefCmd out of range [0, 1]");
    assert(lefCmd    >=  0.0 && lefCmd    <= 1.0 && "lefCmd out of range [0, 1]");

    // Clamp (release builds — don't crash on bad input, just fix it).
    pstick    = std::clamp(pstick,    -1.0, 1.0);
    rstick    = std::clamp(rstick,    -1.0, 1.0);
    ypedal    = std::clamp(ypedal,    -1.0, 1.0);
    throttle  = std::clamp(throttle,   0.0, 1.5);
    speedBrake = std::clamp(speedBrake, -1.0, 1.0);
    gearHandle = std::clamp(gearHandle, -1.0, 1.0);
    hookHandle = std::clamp(hookHandle, -1.0, 1.0);
    tefCmd    = std::clamp(tefCmd,     0.0, 1.0);
    lefCmd    = std::clamp(lefCmd,     0.0, 1.0);
}

} // namespace f4::flight
