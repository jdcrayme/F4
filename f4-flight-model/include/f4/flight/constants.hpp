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
//
// The angle constants (PI, HALF_PI, TWO_PI, DTR, RTD) are re-exported from
// f4-math/include/f4/math/constants.hpp so the project has a single source
// of truth for pi. New code should include <f4/math/constants.hpp> directly.

#pragma once

#include <f4/math/constants.hpp>

namespace f4::flight {

// ---------------------------------------------------------------------------
// Angle constants — re-exported from f4-math for backward compatibility.
// New call sites should include <f4/math/constants.hpp> directly.
// ---------------------------------------------------------------------------
using f4::math::PI;
using f4::math::HALF_PI;
using f4::math::TWO_PI;
using f4::math::DTR;
using f4::math::RTD;

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

// ---------------------------------------------------------------------------
// Epsilon / floor values
//
// Small-number guards used throughout the physics code to prevent
// division-by-zero, NaN, and degenerate quaternion normalization.
// ---------------------------------------------------------------------------
constexpr double QSOM_FLOOR     = 1e-6;   // near-zero normalized dynamic pressure
constexpr double MIN_VT         = 1e-3;   // near-zero true airspeed (ft/s)
constexpr double QUAT_SMALL     = 1e-10;  // small-angle threshold for quaternion normalization
constexpr double MASS_FLOOR     = 1e-6;   // near-zero mass (slugs) — guards div-by-zero

// ---------------------------------------------------------------------------
// Ground effect
//
// Within 0.2*span of the ground, lift is multiplied by 1.13 (13% increase
// from ground cushion). Between 0.2 and 1.0 span, the factor fades linearly
// back to 1.0.
// ---------------------------------------------------------------------------
constexpr double GROUND_EFFECT_CL_MULT = 1.13;  // 13% lift increase near ground

// ---------------------------------------------------------------------------
// Gear friction coefficients
//
// Rolling / braking friction for different surface types. The carrier deck
// value is very high (effectively infinite) to model the arresting cable.
// ---------------------------------------------------------------------------
constexpr double MU_PAVED   = 0.04;   // rolling friction on paved surface
constexpr double MU_GRASS   = 0.5;    // rolling friction on grass
constexpr double MU_BRAKING = 0.7;    // braking friction
constexpr double MU_CARRIER = 20.0;   // carrier deck friction (very high)

// ---------------------------------------------------------------------------
// EOM / steering limits
//
// Ground-steering rate limits and body-rate clamps used by the equations of
// motion. The body-rate clamps prevent quaternion tumbling during transients
// (e.g. stall, ground contact).
// ---------------------------------------------------------------------------
constexpr double TAXI_STEER_RATE    = 30.0;  // deg/s max taxi steering rate
constexpr double STEER_RATE_LOW_VT  = 50.0;  // ft/s boundary for steer rate fade
constexpr double STEER_RATE_HIGH_VT = 150.0; // ft/s boundary for steer rate fade
constexpr double MIN_HEIGHT_MARGIN  = 5.0;   // ft margin above gear min height
constexpr double MAX_BODY_RATE_P    = 4.5;   // rad/s body roll rate clamp
constexpr double MAX_BODY_RATE_Q    = 3.0;   // rad/s body pitch rate clamp
constexpr double MAX_BODY_RATE_R    = 4.0;   // rad/s body yaw rate clamp

// ---------------------------------------------------------------------------
// Engine model constants
//
// These are the hard-coded physics parameters from FreeFalcon's engine.cpp.
// They are not aircraft-specific (those come from AuxAero/EngineTable);
// they define the shape of the RPM/FTIT/fuel-flow schedules.
// ---------------------------------------------------------------------------

// Spool rate schedule
constexpr double SPOOL_ALT_DIV       = 25000.0;  // ft   altitude normalization divisor
constexpr double SPOOL_MACH_DIV      = 2.0;       //      Mach divisor for spool rate
constexpr double SPOOL_RATE_FLOOR    = 0.1;       // 1/s  minimum spool rate

// RPM thresholds and schedule
constexpr double RPM_LIGHTUP_THRESH  = 0.68;  // below this RPM, engine enters lightup zone
constexpr double RPM_IDLE            = 0.7;   // normalized idle RPM (0-1 scale)
constexpr double RPM_MIL_RANGE       = 0.3;   // RPM range from idle (0.7) to MIL (1.0)
constexpr double RPM_AB_GAIN         = 0.06;  // AB RPM increment: 1.0 + 0.06*(throttle-1.0)
constexpr double RPM_AB_LIGHTUP      = 0.95;  // RPM threshold for AB lightup flag

// Fuel flow
constexpr double AI_FUEL_FLOW_FACTOR = 0.75;  // AI simplified fuel-flow scaling
constexpr double FUEL_FLOW_TAU       = 0.1;   // s   fuel-flow smoothing lag time constant
constexpr double SEC_PER_HOUR        = 3600.0; // s/hr  for lb/hr -> lb/s conversion

// FTIT (turbine inlet temperature, normalized 0-10 scale)
constexpr double FTIT_IDLE_TEMP      = 5.1;   // FTIT at idle RPM (0.7)
constexpr double FTIT_MIL_LOW_RPM    = 0.9;   // RPM boundary: low-MIL -> high-MIL FTIT band
constexpr double FTIT_MIL_LOW_RANGE  = 0.2;   // RPM range from idle (0.7) to 0.9
constexpr double FTIT_MIL_LOW_GAIN   = 1.0;   // FTIT gain in low-MIL band
constexpr double FTIT_MIL_HIGH_RANGE = 0.1;   // RPM range from 0.9 to 1.0
constexpr double FTIT_MIL_HIGH_GAIN  = 1.5;   // FTIT gain in high-MIL band
constexpr double FTIT_AB_RPM_RANGE   = 0.03;  // RPM range per unit throttle above MIL in AB
constexpr double FTIT_AB_GAIN        = 0.1;   // FTIT gain in AB band
constexpr double FTIT_MAX            = 10.0;  // FTIT normalized clamp maximum
constexpr double FTIT_TAU            = 0.7;   // s   FTIT lag time constant

// Nozzle / vectored thrust
constexpr double NOZZLE_POS_THRESH   = 1e-6;  // threshold: nozzle position > 0 means vectored

// Lift-off detection
constexpr double LIFTOFF_LIFT_MARGIN = 1.05;  // lift must exceed weight by 5% for lift-off
constexpr double LIFTOFF_ZDOT_THRESH = 0.5;   // ft/s  zdot threshold for lift-off detection

}  // namespace f4::flight
