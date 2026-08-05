// f4-flight-model/constants.hpp
//
// Physical constants for the flight model.
//
// All quantities are in Imperial units (feet, slugs, Rankine, ft/s) to match
// the original Falcon 4 aerodynamic coefficient tables exactly. Converting
// to SI at this layer would require re-tuning every coefficient; instead we
// keep Imperial internally and convert at the host boundary.
//
// Ported from F4Flight's core/constants.h, which itself preserves the
// original Falcon 4 values. The gravity constant (32.177) is intentionally
// the legacy value, NOT the standard 32.17405 — the coefficient tables were
// tuned for 32.177 and changing it would alter the flight feel.

#pragma once

namespace f4::flight {

// ---------------------------------------------------------------------------
// Angle conversions
// ---------------------------------------------------------------------------
constexpr double RTD = 57.2957795130823208767;   // radians -> degrees
constexpr double DTR = 0.017453292519943295769;  // degrees -> radians

// ---------------------------------------------------------------------------
// Circle constants
// ---------------------------------------------------------------------------
constexpr double PI      = 3.14159265358979323846;
constexpr double HALF_PI = 1.57079632679489661923;
constexpr double TWO_PI  = 6.28318530717958647692;

// ---------------------------------------------------------------------------
// Gravitational acceleration
//
// NOTE: 32.177 ft/s^2 is the legacy Falcon 4 value, slightly off from the
// standard 32.17405 ft/s^2. We preserve it so the coefficient tables
// produce the same flight feel as the original.
// ---------------------------------------------------------------------------
constexpr double GRAVITY = 32.177;  // ft/s^2

// ---------------------------------------------------------------------------
// Sea-level standard atmosphere (Imperial)
//
// These are the 1962 US Standard Atmosphere values in Imperial units.
// They define the reference state (rho0, P0, a0, T0) at sea level.
// ---------------------------------------------------------------------------
constexpr double RHOASL = 0.0023769;  // slugs/ft^3  air density at sea level
constexpr double PASL   = 2116.22;    // lb/ft^2     pressure at sea level
constexpr double AASL   = 1116.44;    // ft/s        speed of sound at sea level
constexpr double AASLK  = 661.48;     // knots       speed of sound at sea level
constexpr double TASL   = 518.7;      // deg R       temperature at sea level

// ---------------------------------------------------------------------------
// Unit conversions (Imperial <-> nautical / SI)
// ---------------------------------------------------------------------------
constexpr double FTPSEC_TO_KNOTS = 0.592474;    // ft/s  -> knots
constexpr double KNOTS_TO_FTPSEC = 1.687836;    // knots -> ft/s
constexpr double FT_TO_METERS    = 0.3048;      // ft    -> m
constexpr double METERS_TO_FT    = 3.28084;     // m     -> ft
constexpr double NM_TO_FT        = 6076.11549;  // nm    -> ft
constexpr double LBS_TO_KG       = 0.453592;    // lb    -> kg
constexpr double SLUGS_TO_KG     = 14.5939;     // slugs -> kg

// ---------------------------------------------------------------------------
// Atmosphere layer breakpoints (1962/1976 US Standard Atmosphere)
//
// The atmosphere model has 3 layers:
//   Troposphere:       0 to 36089 ft   (temperature decreases with altitude)
//   Lower stratosphere: 36089 to 65617 ft (isothermal, constant temperature)
//   Upper stratosphere: 65617+ ft       (temperature increases with altitude)
// ---------------------------------------------------------------------------
constexpr double TROPO_ALT_FT  = 36089.0;  // ft  tropopause
constexpr double TROPO_ALT2_FT = 65617.0;  // ft  second breakpoint

// ---------------------------------------------------------------------------
// Atmosphere lapse/gradient constants
//
// Derived from the formulas in FreeFalcon's atmos.cpp. Exposed so tests
// can validate them independently.
// ---------------------------------------------------------------------------
constexpr double TROPO_LAPSE     = 6.875e-6;   // 1/ft   temperature lapse rate
constexpr double TROPO_RHO_EXP   = 4.255876;   //        rho exponent (gamma)
constexpr double STRATO_TTHETA   = 0.751865;   //        T/T0 in lower stratosphere
constexpr double STRATO_RHO_BASE = 0.297076;   //        rho ratio base at tropopause
constexpr double STRATO_RHO_K    = 4.806e-5;   // 1/ft   rho exponential coefficient

// ---------------------------------------------------------------------------
// Gear actuation rate
//
// Full gear travel (up <-> down) takes 3 seconds. This matches the
// visual actuation time in FreeFalcon.
// ---------------------------------------------------------------------------
constexpr double GEAR_RATE = 1.0 / 3.0;  // 1/s  (full travel in 3 s)

// ---------------------------------------------------------------------------
// Flap actuation rates
//
// TEF (trailing-edge flaps) take 3 seconds for full travel.
// LEF (leading-edge flaps) take 1.5 seconds — they deploy faster because
// they're primarily a high-AOA lift enhancer, not a landing device.
// ---------------------------------------------------------------------------
constexpr double TEF_RATE = 1.0 / 3.0;   // 1/s  (full travel in 3 s)
constexpr double LEF_RATE = 1.0 / 1.5;   // 1/s  (full travel in 1.5 s)

// ---------------------------------------------------------------------------
// Stall speed coefficient
//
// The stall speed formula is: V_stall = K_STALL * sqrt((W/S) / |CL|)
// where K_STALL = 17.16 knots * sqrt(ft^2/lb). This constant comes from
// the unit conversion in the original FreeFalcon aero.cpp:
//   17.16 = sqrt(2 * g / rho_0) * (1 / KNOTS_TO_FTPSEC)
// where g = 32.177 ft/s^2, rho_0 = 0.0023769 slugs/ft^3.
// Previously this was a bare magic number in aerodynamics.cpp.
// ---------------------------------------------------------------------------
constexpr double K_STALL = 17.16;  // knots * sqrt(ft^2/lb)

}  // namespace f4::flight
