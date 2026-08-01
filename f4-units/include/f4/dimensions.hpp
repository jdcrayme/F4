#pragma once

#include "dimension.hpp"

namespace f4 {

// ============================================================================
// Base Dimensions
// ============================================================================

using Dimensionless  = Dimension<0, 0, 0, 0, 0>;
using LengthDim      = Dimension<1, 0, 0, 0, 0>;
using MassDim        = Dimension<0, 1, 0, 0, 0>;
using TimeDim        = Dimension<0, 0, 1, 0, 0>;
using TemperatureDim = Dimension<0, 0, 0, 1, 0>;
using AngleDim       = Dimension<0, 0, 0, 0, 1>;

// ============================================================================
// Derived Physical Dimensions
// ============================================================================

using SpeedDim          = Dimension<1, 0,-1, 0, 0>;
using AccelerationDim   = Dimension<1, 0,-2, 0, 0>;
using ForceDim          = Dimension<1, 1,-2, 0, 0>;
using PressureDim       = Dimension<-1, 1,-2, 0, 0>;
using AreaDim           = Dimension<2, 0, 0, 0, 0>;
using VolumeDim         = Dimension<3, 0, 0, 0, 0>;
using DensityDim        = Dimension<-3, 1, 0, 0, 0>;
using EnergyDim         = Dimension<2, 1,-2, 0, 0>;
using PowerDim          = Dimension<2, 1,-3, 0, 0>;
using FrequencyDim      = Dimension<0, 0,-1, 0, 0>;
using MassFlowRateDim   = Dimension<0, 1,-1, 0, 0>;
using TorqueDim         = Dimension<2, 1,-2, 0, 0>;  // Same dimensions as Energy

// ============================================================================
// Phantom Dimensions (do not participate in dimensional arithmetic)
// ============================================================================

/// Calibrated Airspeed: a pressure-equivalent quantity displayed in speed
/// units (knots, m/s). Conversion to/from true speed or Mach requires
/// atmospheric state (provided by an IAtmosphereProvider).///
/// CAS is NOT a Speed. The type system enforces this separation.
/// Display-unit conversion (knots <-> m/s) IS a simple constant ratio.
struct CASTag {};
using CASDim = PhantomDimension<CASTag>;

/// Mach number: the ratio of true airspeed to local speed of sound.
/// Truly dimensionless in physics, but given its own phantom type to
/// prevent misuse as a plain ratio.
///
/// Conversion to/from speed requires atmospheric state.
struct MachTag {};
using MachDim = PhantomDimension<MachTag>;

} // namespace f4
