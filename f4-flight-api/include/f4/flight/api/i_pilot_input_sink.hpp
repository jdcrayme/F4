// f4-flight-api/i_pilot_input_sink.hpp
//
// IPilotInputSink — write interface for delivering pilot/AI control
// input to a flight model.
//
// Phase 2 (H1): BrainComponent writes to this interface instead of
// directly accessing FlightModelComponent::pending_input(). This:
//   1. Makes the write contract explicit (AI produces PilotInput,
//      flight model consumes it)
//   2. Allows alternative sinks (e.g. a recording sink for replay,
//      a network sink for remote control) without changing the AI
//   3. Removes the AI's need to know about FlightModelComponent
//      internals (the pending_input_ slot, its lifecycle of
//      write-then-consume-then-clear)
//
// Phase 2+: Moved to f4-flight-api so f4-ai can depend on this
// lightweight API instead of the full f4-flight-model library.
//
// C++20.

#pragma once

#include "f4/flight/api/pilot_input.hpp"

namespace f4::flight {

// ============================================================================
// IPilotInputSink
// ============================================================================
class IPilotInputSink {
public:
    virtual ~IPilotInputSink() = default;

    /// Write the given PilotInput to the pending-input slot.
    /// The flight model will consume this input on its next update()
    /// call, then clear the slot back to idle controls.
    ///
    /// Thread safety: must only be called from the sim thread
    /// (the same thread that calls FlightModel::update()).
    virtual void set_pending_input(const PilotInput& input) = 0;
};

} // namespace f4::flight
