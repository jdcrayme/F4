#pragma once

#include "dimensions.hpp"
#include "unit.hpp"

namespace f4 {

// ============================================================================
// Length
// Conversion factors to SI base unit: meters
// ============================================================================

using Millimeters   = Unit<LengthDim, 0.001>;
using Centimeters   = Unit<LengthDim, 0.01>;
using Meters        = Unit<LengthDim, 1.0>;
using Kilometers    = Unit<LengthDim, 1000.0>;
using Inches        = Unit<LengthDim, 0.0254>;
using Feet          = Unit<LengthDim, 0.3048>;
using Miles         = Unit<LengthDim, 1609.344>;
using NauticalMiles = Unit<LengthDim, 1852.0>;

// ============================================================================
// Mass
// Conversion factors to SI base unit: kilograms
// ============================================================================

using Grams    = Unit<MassDim, 0.001>;
using Kilograms = Unit<MassDim, 1.0>;
using Ounces   = Unit<MassDim, 0.028349523125>;
using Pounds   = Unit<MassDim, 0.45359237>;
using Slugs    = Unit<MassDim, 14.593903>;

// ============================================================================
// Time
// Conversion factors to SI base unit: seconds
// ============================================================================

using Milliseconds = Unit<TimeDim, 0.001>;
using Seconds      = Unit<TimeDim, 1.0>;
using Minutes      = Unit<TimeDim, 60.0>;
using Hours        = Unit<TimeDim, 3600.0>;

// ============================================================================
// Temperature (affine conversions)
// Formula: value_in_base (Kelvin) = value * to_base + offset
// ============================================================================

/// Kelvin: SI base unit. K = 1.0*K + 0
using Kelvin    = Unit<TemperatureDim, 1.0, 0.0>;

/// Celsius: K = 1.0*C + 273.15
using Celsius   = Unit<TemperatureDim, 1.0, 273.15>;

/// Rankine: K = (5/9)*R + 0
using Rankine   = Unit<TemperatureDim, 5.0 / 9.0, 0.0>;

/// Fahrenheit: K = (5/9)*F + 459.67*(5/9)
/// 0 degF = 255.372222... K, 32 degF = 273.15 K
using Fahrenheit = Unit<TemperatureDim, 5.0 / 9.0, 459.67 * 5.0 / 9.0>;

// ============================================================================
// Angle
// Conversion factors to SI base unit: radians
// ============================================================================

using Radians    = Unit<AngleDim, 1.0>;
using Degrees    = Unit<AngleDim, 3.14159265358979323846 / 180.0>;
using ArcMinutes = Unit<AngleDim, 3.14159265358979323846 / 10800.0>;
using ArcSeconds = Unit<AngleDim, 3.14159265358979323846 / 648000.0>;
using Mils       = Unit<AngleDim, 2.0 * 3.14159265358979323846 / 6400.0>;  // NATO mils: full circle = 6400 mils

// ============================================================================
// Speed
// Conversion factors to SI base unit: meters per second
// ============================================================================

using MetersPerSecond   = Unit<SpeedDim, 1.0>;
using FeetPerSecond     = Unit<SpeedDim, 0.3048>;
using KilometersPerHour = Unit<SpeedDim, 1.0 / 3.6>;
using Knots             = Unit<SpeedDim, 1852.0 / 3600.0>;
using MilesPerHour      = Unit<SpeedDim, 1609.344 / 3600.0>;

// ============================================================================
// Acceleration
// Conversion factors to SI base unit: m/s^2
// ============================================================================

using MetersPerSecondSquared = Unit<AccelerationDim, 1.0>;
using FeetPerSecondSquared   = Unit<AccelerationDim, 0.3048>;

/// Standard gravity: exactly 9.80665 m/s^2 (ISO 80000-3)
using Gs = Unit<AccelerationDim, 9.80665>;

// ============================================================================
// Force
// Conversion factors to SI base unit: newtons
// ============================================================================

using Newtons     = Unit<ForceDim, 1.0>;
using KiloNewtons = Unit<ForceDim, 1000.0>;

/// Exact: 1 lbf = 4.4482216152605 N (NIST)
using PoundForce   = Unit<ForceDim, 4.4482216152605>;

/// Standard gravity * 1 kg (not a true force unit in strict SI, but widely used)
using KilogramForce = Unit<ForceDim, 9.80665>;

// ============================================================================
// Pressure
// Conversion factors to SI base unit: pascals (N/m^2)
// ============================================================================

using Pascals     = Unit<PressureDim, 1.0>;
using Hectopascals = Unit<PressureDim, 100.0>;       // 1 hPa = 100 Pa
using Millibars   = Unit<PressureDim, 100.0>;       // 1 mbar = 1 hPa (exact)
using Kilopascals = Unit<PressureDim, 1000.0>;

/// Exact: 1 psi = 6894.757293168361 Pa (NIST)
using PSI = Unit<PressureDim, 6894.757293168361>;

/// 1 psf = 47.88025898033584 Pa
using PSF = Unit<PressureDim, 47.88025898033584>;

/// Standard atmosphere: exactly 101325 Pa
using Atmospheres = Unit<PressureDim, 101325.0>;

/// Inches of mercury at 0 degC: 3386.389 Pa
using InHg = Unit<PressureDim, 3386.389>;

/// Millimeters of mercury (torr): 133.3223684211 Pa
using MillimetersOfMercury = Unit<PressureDim, 133.3223684211>;

// ============================================================================
// Area
// Conversion factors to SI base unit: square meters
// ============================================================================

using SquareMeters        = Unit<AreaDim, 1.0>;
using SquareFeet          = Unit<AreaDim, 0.09290304>;
using SquareKilometers    = Unit<AreaDim, 1'000'000.0>;
using SquareNauticalMiles = Unit<AreaDim, 3'429'904.0>;  // 1852^2
using SquareMiles         = Unit<AreaDim, 2'589'988.110336>;  // 1609.344^2

// ============================================================================
// Volume
// Conversion factors to SI base unit: cubic meters
// ============================================================================

using CubicMeters     = Unit<VolumeDim, 1.0>;
using CubicFeet       = Unit<VolumeDim, 0.028316846592>;
using Liters          = Unit<VolumeDim, 0.001>;
using USGallons       = Unit<VolumeDim, 0.003785411784>;
using ImperialGallons = Unit<VolumeDim, 0.00454609>;

// ============================================================================
// Density
// Conversion factors to SI base unit: kg/m^3
// ============================================================================

using KilogramsPerCubicMeter = Unit<DensityDim, 1.0>;

/// 1 slug/ft^3 = 515.3788184 kg/m^3
using SlugsPerCubicFoot = Unit<DensityDim, 515.3788184>;

// ============================================================================
// Energy
// Conversion factors to SI base unit: joules
// ============================================================================

using Joules    = Unit<EnergyDim, 1.0>;
using KiloJoules = Unit<EnergyDim, 1000.0>;
using BTU       = Unit<EnergyDim, 1055.06>;         // ISO 18431-1
using FootPounds = Unit<EnergyDim, 1.3558179483>;

// ============================================================================
// Power
// Conversion factors to SI base unit: watts
// ============================================================================

using Watts               = Unit<PowerDim, 1.0>;
using KiloWatts           = Unit<PowerDim, 1000.0>;
using Horsepower          = Unit<PowerDim, 745.69987158227022>;  // mechanical (imperial)
using PoundFeetPerSecond  = Unit<PowerDim, 1.3558179483>;

// ============================================================================
// Frequency
// Conversion factors to SI base unit: hertz (1/s)
// ============================================================================

using Hertz    = Unit<FrequencyDim, 1.0>;
using KiloHertz = Unit<FrequencyDim, 1000.0>;
using MegaHertz = Unit<FrequencyDim, 1'000'000.0>;

// ============================================================================
// Mass Flow Rate
// Conversion factors to SI base unit: kg/s
// ============================================================================

using KilogramsPerSecond = Unit<MassFlowRateDim, 1.0>;
using KilogramsPerHour   = Unit<MassFlowRateDim, 1.0 / 3600.0>;

/// 1 lb/hr = 0.45359237 kg / 3600 s = 0.00012599788056 kg/s
using PoundsPerHour = Unit<MassFlowRateDim, 0.00012599788055556>;

// ============================================================================
// Torque (same dimensions as energy, but semantically distinct)
// ============================================================================

using NewtonMeters = Unit<TorqueDim, 1.0>;
using PoundFeet    = Unit<TorqueDim, 1.3558179483>;
using InchPounds  = Unit<TorqueDim, 0.1129848290276167>;

} // namespace f4
