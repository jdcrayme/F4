// f4-simulation/include/f4/simulation/f4_simulation.hpp
//
// Umbrella header for the f4-simulation library.
//
// f4-simulation is the orchestration layer proposed in
// Docs/ARCHITECTURE PROPOSAL.md §13. It owns the EntityWorld + MessageBus +
// ModelDatabase + AircraftConfig registry and runs the tick loop. NO
// rendering — that's the executable's job (f4-scenario-player).
//
// This library is where new components that depend on BOTH f4-entities and
// f4-models live (e.g. VisualModelComponent). It's also where the Scenario
// JSON loader and the Simulation class live.
//
// Dependencies: f4-entities, f4-messaging, f4-flight-model, f4-flight-api,
// f4-ai, f4-data, f4-geo, f4-math, f4-units, f4-state-machine, f4-models,
// f4-recorder, f4-json, f4-io. C++20.

#pragma once

#include "f4/simulation/visual_model_component.hpp"
#include "f4/simulation/scenario.hpp"
#include "f4/simulation/simulation.hpp"
