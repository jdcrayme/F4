// f4-avionics — engine-agnostic avionics logic.
//
// Master include. The avionics layer produces avionics state machines and
// pure view models (page-model structs, cue lists) that any host cockpit —
// raylib, web, whatever — renders and drives; it never draws and never
// reads devices (the charter boundary re-drawn as rendering-only,
// Docs/AVIONICS_PLAN.md §1).
//
// Contents (AVIONICS-1):
//   f4::avionics::InsUnit / InsConfig       — the INS: alignment SM, the
//                                             stored heading/altitude/
//                                             position chain, the seeded
//                                             deterministic drift walk
//   f4::avionics::Steerpoint(Sequence)      — the flight plan
//   f4::avionics::to_steer / current_steer  — BRA solutions to a steerpoint
//   f4::avionics::steer_cue / current_steer_cue — the HSI steering cue
//
// The AI does NOT consume this library (the digi brains keep their
// SensorFusion pipeline — parallel, not shared, same as the reference).
// The PilotInput seam is read-side for now: the unit consumes
// IAircraftState; PilotInput emission arrives with the page models
// (AVIONICS-2+).

#pragma once

#include "f4/avionics/ins.hpp"
#include "f4/avionics/steerpoint.hpp"
