// f4-ai/include/f4/ai/atc/atc_interface.hpp
//
// IAirTrafficControl — the ATC interface the Simulation holds.
//
// Both implementations (StubATC — grant everything; TowerATC — sequence and
// defer) satisfy this surface. wire_atc() picks one from the scenario
// config; the AI modules only ever see MessageBus traffic, so the swap is
// invisible to them (the stub's original design contract, kept).
//
// Dependencies: f4-ai/atc/airfield_config.hpp. C++20.

#pragma once

#include "f4/ai/atc/airfield_config.hpp"

namespace f4::ai::atc {

class IAirTrafficControl {
public:
    virtual ~IAirTrafficControl() = default;

    // Advance wall-clock-driven ATC state (occupancy timers). The stub is
    // purely reactive and does nothing; the tower ages its runway claims
    // and fires the occupancy timeout when an occupant stops reporting.
    virtual void tick(double dt) = 0;

    virtual void set_airfield(const AirfieldConfig& config) = 0;

    // Per-airbase registry (campaign path): requests carrying airbase_id
    // are answered from THAT airfield; unknown/zero ids fall back to the
    // default set_airfield() config.
    virtual void set_airbase_airfield(std::uint64_t airbase_id,
                                      const AirfieldConfig& config) = 0;

    virtual void set_tanker(const TankerConfig& config) = 0;
};

} // namespace f4::ai::atc
