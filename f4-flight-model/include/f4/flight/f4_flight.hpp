// f4-flight-model — 6-DOF flight dynamics model.
//
// Master include. Pulls in every f4-flight-model header.
//
// Dependencies:
//   - f4-data   (for AircraftConfig)
//   - f4-math   (for Table2D, Vec3, Quat, filters)
//
// All quantities are in Imperial units (ft, slugs, lb, ft/s) to match the
// original Falcon 4 coefficient tables.

#pragma once

#include "f4/flight/constants.hpp"
#include "f4/flight/aircraft_state.hpp"
#include "f4/flight/atmosphere.hpp"
#include "f4/flight/aerodynamics.hpp"
#include "f4/flight/fcs.hpp"
#include "f4/flight/engine.hpp"
#include "f4/flight/eom.hpp"
#include "f4/flight/gear.hpp"
#include "f4/flight/flight_model.hpp"
#include "f4/flight/i_aircraft_state.hpp"
#include "f4/flight/i_pilot_input_sink.hpp"
#include "f4/flight/flight_model_component.hpp"
