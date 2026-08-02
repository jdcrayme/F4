// f4-geo/constants.hpp
//
// Physical constants for geodesy (WGS84 ellipsoid) and unit conversions
// used by the coordinate strong types. Pure compile-time constants, no
// dependencies.

#pragma once

namespace f4::geo {

// ============================================================================
// WGS84 ellipsoid defining parameters
// (NIMA TR8350.2, "Department of Defense World Geodetic System 1984")
// ============================================================================
inline constexpr double WGS84_A  = 6378137.0;                    // semi-major axis (m)
inline constexpr double WGS84_F  = 1.0 / 298.257223563;           // flattening
inline constexpr double WGS84_B  = WGS84_A * (1.0 - WGS84_F);     // semi-minor axis (m)

// First eccentricity squared  e^2 = 2f - f^2
inline constexpr double WGS84_E2  = WGS84_F * (2.0 - WGS84_F);
// Second eccentricity squared e'^2 = e^2 / (1 - e^2)
inline constexpr double WGS84_EP2 = WGS84_E2 / (1.0 - WGS84_E2);

// ============================================================================
// Unit conversions
// ============================================================================
inline constexpr double FEET_PER_METER  = 3.2808398950131;
inline constexpr double METERS_PER_FOOT = 1.0 / FEET_PER_METER;

// 1 nautical mile = 1852 m (exact, international definition).
inline constexpr double METERS_PER_NM   = 1852.0;
inline constexpr double FEET_PER_NM     = METERS_PER_NM * FEET_PER_METER; // 6076.11548...

inline constexpr double PI        = 3.14159265358979323846;
inline constexpr double TWO_PI    = 2.0 * PI;
inline constexpr double DEG_TO_RAD = PI / 180.0;
inline constexpr double RAD_TO_DEG = 180.0 / PI;

} // namespace f4::geo
