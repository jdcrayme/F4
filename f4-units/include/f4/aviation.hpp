#pragma once

#include "dimensions.hpp"
#include "unit.hpp"

namespace f4 {

// ============================================================================
// Calibrated Airspeed (CAS)
// ============================================================================
//
// CAS is a phantom dimension. It cannot be added to Speed, compared to
// Pressure, or used in dimensional arithmetic. The type system enforces
// this at compile time.
//
// Display-unit conversion (knots <-> m/s) IS a simple constant ratio,
// because both are just different scalings of the same CAS value.
//
// Conversion between CAS and TAS/Mach requires atmospheric state and
// is provided by IAtmosphereProvider in f4-flight-model.

/// CAS reference display unit: value represents CAS in knots.
using CASKnots = Unit<CASDim, 1.0>;

/// CAS in meters-per-second equivalent display unit.
/// 1 CAS-knot = 1852/3600 CAS-m/s (same ratio as speed knots <-> m/s).
using CASMetersPerSecond = Unit<CASDim, 1852.0 / 3600.0>;

// ============================================================================
// Mach Number
// ============================================================================
//
// Mach is a phantom dimension representing the dimensionless ratio
// V/a (true airspeed / local speed of sound). Despite being physically
// dimensionless, it is typed to prevent misuse as a plain ratio.
//
// Conversion to/from speed requires atmospheric state.

/// Mach number (single unit, no meaningful display-unit alternatives).
using MachUnit = Unit<MachDim, 1.0>;

// ============================================================================
// Compile-time safety assertions
// ============================================================================

// CAS must be isolated from all physical dimensions with similar exponents.
static_assert(!same_dimension_v<CASDim, SpeedDim>,
    "CAS must not be the same dimension as Speed");
static_assert(!same_dimension_v<CASDim, PressureDim>,
    "CAS must not be the same dimension as Pressure");
static_assert(!same_dimension_v<CASDim, FrequencyDim>,
    "CAS must not be the same dimension as Frequency");
static_assert(!same_dimension_v<CASDim, Dimensionless>,
    "CAS must not be the same dimension as Dimensionless");

// Mach must be isolated from Dimensionless.
static_assert(!same_dimension_v<MachDim, Dimensionless>,
    "Mach must not be the same dimension as Dimensionless");

// CAS and Mach must be distinct from each other.
static_assert(!same_dimension_v<CASDim, MachDim>,
    "CAS and Mach must be distinct dimensions");

// Both must be phantom (non-physical).
static_assert(CASDim::is_phantom,  "CAS must be a phantom dimension");
static_assert(MachDim::is_phantom, "Mach must be a phantom dimension");

} // namespace f4
