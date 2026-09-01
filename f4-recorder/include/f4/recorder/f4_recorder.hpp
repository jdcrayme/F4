// f4-recorder/include/f4/recorder/f4_recorder.hpp
//
// Umbrella header for f4-recorder — flight recording and path analysis.
//
// Include this to get the full public API. The recorder captures simulation
// state, exports it as JSON for replay/debugging, and computes path geometry
// for validation.
//
// Components:
//   f4::recorder::FlightSnapshot    — per-tick state snapshot (aircraft/missile)
//   f4::recorder::CombatEvent       — discrete combat transition (M4)
//   f4::recorder::FlightRecorder    — accumulator + JSON export
//   f4::recorder::PathSegment       — intended path geometry
//   f4::recorder::FlightPath        — intended + actual path for one aircraft
//   f4::recorder::MultiAircraftScenario — multi-aircraft recording
//
// Dependencies: f4-geo, f4-json. C++20.

#pragma once

#include <f4/recorder/snapshot.hpp>
#include <f4/recorder/flight_recorder.hpp>
#include <f4/recorder/path_geometry.hpp>
