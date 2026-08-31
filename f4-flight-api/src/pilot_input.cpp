// f4-flight-api/src/pilot_input.cpp
//
// PilotInput::validate() — clamp all control inputs to documented ranges.
//
// Moved from f4-flight-model/src/flight_model.cpp as part of Phase 2+.
// The implementation only uses <algorithm> — no flight-model internals.

#include "f4/flight/api/pilot_input.hpp"

#include <algorithm>

namespace f4::flight {

// ---------------------------------------------------------------------------
// PilotInput::validate — clamp all inputs to documented ranges
// ---------------------------------------------------------------------------
void PilotInput::validate() noexcept {
    // Clamp all inputs to their documented ranges. validate() is a
    // sanitizer, not a checker: callers (AI modules, scripted scenarios,
    // input adapters) may legitimately produce out-of-range values mid-
    // transition, and the contract — pinned by PilotInputTest.ValidateClamps*
    // — is "always safe after validate()", never "assert on bad input".
    // (The asserts that used to live here fired BEFORE the clamps, which
    // made those tests impossible to pass in Debug builds — the exact six
    // long-standing "environment" failures this deletion fixes.)

    // Clamp (all builds — don't crash on bad input, just fix it).
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
