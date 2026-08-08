// f4-flight-api — Flight model API interfaces.
//
// Master include. This lightweight library defines the contract between
// the AI brain (f4-ai) and the flight model (f4-flight-model) without
// requiring either to depend on the other's implementation.
//
// Contents:
//   f4::flight::PilotInput       — per-frame control input (moved from aircraft_state.hpp)
//   f4::flight::IAircraftState   — read-only aircraft state for AI modules
//   f4::flight::IPilotInputSink  — write interface for delivering control input
//
// This library has NO dependencies beyond the C++ standard library.
// It is intentionally minimal so that f4-ai, f4-recorder, and future
// consumers can depend on the flight model's contract without pulling
// in f4-data, f4-math, f4-state-machine, etc.

#pragma once

#include "f4/flight/api/pilot_input.hpp"
#include "f4/flight/api/i_aircraft_state.hpp"
#include "f4/flight/api/i_pilot_input_sink.hpp"
