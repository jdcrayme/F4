// f4-flight-model/angle.hpp
//
// Strongly-typed angle and angular-rate wrappers built on f4-units.
//
// PROBLEM
//   AircraftState historically stored angles as raw `double`s with the unit
//   convention documented only in comments (alpha/beta in degrees, euler
//   angles in radians, alpha_dot in deg/s). This was the single largest
//   correctness hazard in the flight model: passing a degree value where
//   radians were expected (or vice versa) compiled silently, because both
//   are `double`. The F-16 aero tables are physically indexed in degrees
//   (so converting the table data would be wrong), which made the rad/deg
//   mixing inside the model inevitable as long as the values were untyped.
//
// SOLUTION
//   `Angle` is an alias for `f4::Quantity<f4::Radians>` — the SI base unit
//   for plane angle. Construction from raw values is explicit (no implicit
//   `double -> Angle`), so callers must pick a side: `Angle::from_degrees(d)`
//   or `Angle::from_radians(r)`. Extraction is similarly explicit:
//   `a.to_degrees()` / `a.to_radians()` return `double`.
//
//   The canonical storage is radians (the SI base), so all internal math
//   (sin/cos/integration) reads `a.to_radians()` — a no-op conversion
//   (Quantity's `to<>` short-circuits when the target unit is already
//   canonical). The F-16 aero tables continue to take degrees via
//   `a.to_degrees()` at the lookup call site; this is the SINGLE place
//   where the degree convention survives, and it's now explicit.
//
//   `AngularRate` is `Quantity<RadiansPerSecond>` — derived from
//   Angle / Seconds via the standard f4-units dimensional arithmetic.
//
// INFRASTRUCTURE
//   f4-units already provides:
//     - Quantity<U,R> (see f4/units/quantity.hpp)
//     - Radians, Degrees (see f4/units/units.hpp)
//     - dimension arithmetic (AngleDim + TimeDim^-1 etc.)
//     - User-defined literals: 5.0_rad, 30.0_deg
//   This header just provides flight-model-local conveniences on top:
//     - The `Angle` / `AngularRate` aliases
//     - `from_degrees()` / `from_radians()` factories (named ctors read better
//       than Quantity<Radians>(...) at call sites)
//     - `to_degrees()` / `to_radians()` short names
//     - A `zero()` factory for default construction
//
// INTEROPERABILITY
//   f4-units already supports:
//     - a + b, a - b  (same dimension)
//     - a * scalar, a / scalar
//     - a == b, a < b, etc.
//     - 5.0_deg + 30.0_deg -> Angle
//   So `Angle` works as a drop-in replacement for raw `double` in arithmetic
//   that doesn't cross dimension boundaries. Cross-dimension arithmetic
//   (e.g. Angle * angular rate) still works via the standard f4-units
//   machinery — see test_derived.cpp for examples.

#pragma once

#include <f4/quantity.hpp>
#include <f4/units.hpp>
#include <f4/dimensions.hpp>

namespace f4::flight {

// ============================================================================
// Angle — strong type for plane angle (radians canonical storage).
//
// Use this everywhere the legacy code used raw `double` for alpha, beta,
// sigma, gmma, mu, psi, theta, phi. The dimension is AngleDim (one of the
// five base dimensions in f4-units), so the type system prevents
// accidental mixing with dimensionless ratios like Mach or with time.
// ============================================================================
using Angle = Quantity<Radians>;

// ============================================================================
// AngularRate — strong type for time-derivative of angle (rad/s canonical).
//
// Use for alpha_dot, beta_dot, body rates (p, q, r) when they cross
// subsystem boundaries as state. The kinematic body rates p/q/r in
// KinematicState stay as raw `double` (rad/s) because they are integrated
// into the quaternion by the EOM and never compared with degrees elsewhere;
// see the per-field notes in aircraft_state.hpp.
// ============================================================================
using RadiansPerSecond = Unit<Dimension<0, 0, -1, 0, 1>, 1.0, 0.0>;  // rad / s
using AngularRate = Quantity<RadiansPerSecond>;

// ============================================================================
// Factories — named constructors. Prefer these over Quantity<Radians>(x)
// because the name documents intent (degrees vs radians) at the call site.
// ============================================================================
inline constexpr Angle angle_from_degrees(double deg) noexcept {
    return Quantity<Degrees>(deg).to<Radians>();
}
inline constexpr Angle angle_from_radians(double rad) noexcept {
    return Angle(rad);
}
inline constexpr AngularRate angular_rate_from_degrees_per_second(double deg_per_s) noexcept {
    // deg/s -> rad/s via the standard Quantity conversion.
    // We construct deg and divide by seconds, which the f4-units machinery
    // reduces to the correct rad/s base value.
    return (Quantity<Degrees>(deg_per_s) / Quantity<Seconds>(1.0))
        .to<RadiansPerSecond>();
}
inline constexpr AngularRate angular_rate_from_radians_per_second(double rad_per_s) noexcept {
    return AngularRate(rad_per_s);
}

// ============================================================================
// Convenience accessors — short names for the two units we actually use.
// ============================================================================
inline constexpr double to_degrees(Angle a) noexcept { return a.to<Degrees>().value(); }
inline constexpr double to_radians(Angle a) noexcept { return a.to<Radians>().value(); }
inline constexpr double to_rad_per_s(AngularRate r) noexcept {
    return r.to<RadiansPerSecond>().value();
}
inline constexpr double to_deg_per_s(AngularRate r) noexcept {
    // rad/s -> deg/s: 1 rad = 180/pi deg.
    // Use the f4-units conversion by dividing rad by 1 second.
    return (r * Quantity<Seconds>(1.0)).to<Degrees>().value();
}

// ============================================================================
// Zero literals — useful for default struct initialization.
// ============================================================================
inline constexpr Angle zero_angle() noexcept { return Angle(0.0); }
inline constexpr AngularRate zero_angular_rate() noexcept { return AngularRate(0.0); }

}  // namespace f4::flight
