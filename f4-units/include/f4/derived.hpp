#pragma once

#include "quantity.hpp"
#include "units.hpp"

namespace f4 {

// Physical constants for air (ISA standard atmosphere).
namespace detail {
    /// Ratio of specific heats for dry air (dimensionless).
    constexpr double gamma_air = 1.4;

    /// Specific gas constant for dry air: R = 287.058 J/(kg*K)
    constexpr double R_air = 287.058;

    /// Standard gravitational acceleration: g0 = 9.80665 m/s^2 (ISO 80000-3)
    constexpr double g0 = 9.80665;
}

// ============================================================================
// Derived Quantity Functions
// ============================================================================

/// Speed of sound in air: a = sqrt(gamma * R * T)
///
/// @param temperature Absolute temperature in Kelvin.
/// @return Speed of sound in m/s.
///
/// At ISA sea level (288.15 K): a = 340.29 m/s
/// At tropopause (216.65 K): a = 295.07 m/s
///
/// Note: This function assumes dry air (gamma = 1.4) and ideal gas behavior.
/// The full IAtmosphereProvider in f4-flight-model may provide a more
/// accurate model accounting for humidity and non-ideal effects.
inline Quantity<MetersPerSecond> speed_of_sound(Quantity<Kelvin> temperature) {
    double T = temperature.value();
    double a = std::sqrt(detail::gamma_air * detail::R_air * T);
    return Quantity<MetersPerSecond>(a);
}

/// Dynamic pressure: q = 0.5 * rho * V^2
///
/// @param density         Air density in kg/m^3.
/// @param true_airspeed   True airspeed in m/s.
/// @return Dynamic pressure in pascals.
///
/// The dimensional arithmetic is checked at compile time:
///   Density (kg/m^3) * Speed^2 (m^2/s^2) = kg/(m*s^2) = Pa
inline Quantity<Pascals> dynamic_pressure(
    Quantity<KilogramsPerCubicMeter> density,
    Quantity<MetersPerSecond> true_airspeed)
{
    return 0.5 * density * true_airspeed * true_airspeed;
}

/// Mach number: M = V / a
///
/// @param true_airspeed   True airspeed.
/// @param sos              Local speed of sound.
/// @return Mach number (typed as MachUnit phantom).
///
/// This is a pure ratio computation. For a full atmosphere-based
/// conversion (speed -> Mach without supplying sos explicitly),
/// use IAtmosphereProvider::mach_from_tas().
inline Quantity<MachUnit> mach_number(
    Quantity<MetersPerSecond> true_airspeed,
    Quantity<MetersPerSecond> sos)
{
    return Quantity<MachUnit>(true_airspeed.value() / sos.value());
}

/// Wing loading: W / S
///
/// @param weight     Aircraft weight (force, newtons).
/// @param wing_area  Wing reference area (m^2).
/// @return Wing loading in pascals (N/m^2).
///
/// The dimensional arithmetic is checked at compile time:
///   Force (N) / Area (m^2) = Pressure (Pa)
inline auto wing_loading(
    Quantity<Newtons> weight,
    Quantity<SquareMeters> wing_area)
{
    return weight / wing_area;
}

/// Weight from mass: W = m * g
///
/// @param mass   Mass in kg.
/// @return Weight (force) in newtons at standard gravity.
inline Quantity<Newtons> weight_from_mass(Quantity<Kilograms> mass) {
    return mass * Quantity<MetersPerSecondSquared>(detail::g0);
}

// ============================================================================
// ISA Standard Atmosphere Reference Values
// ============================================================================

namespace isa {
    /// Standard sea-level temperature: 15 degC = 288.15 K
    inline constexpr Quantity<Kelvin> sea_level_temp = Quantity<Kelvin>(288.15);

    /// Standard sea-level pressure: 101325 Pa
    inline constexpr Quantity<Pascals> sea_level_pressure = Quantity<Pascals>(101325.0);

    /// Standard sea-level density: 1.225 kg/m^3
    inline constexpr Quantity<KilogramsPerCubicMeter> sea_level_density =
        Quantity<KilogramsPerCubicMeter>(1.225);

    /// Standard sea-level speed of sound: ~340.29 m/s
    inline constexpr Quantity<MetersPerSecond> sea_level_sos =
        Quantity<MetersPerSecond>(340.293988);
}

} // namespace f4
