// f4-math/constants.hpp
//
// Circle and angle-conversion constants. Single source of truth for pi,
// two-pi, half-pi, and degrees<->radians across the project.
//
// Historical note: before this header existed, the project had FOUR
// independent copies of these constants:
//
//   f4-geo/include/f4/geo/constants.hpp         — PI, TWO_PI, DEG_TO_RAD, RAD_TO_DEG
//   f4-flight-model/include/f4/flight/constants.hpp — PI, HALF_PI, TWO_PI, DTR, RTD
//   f4-math/include/f4/math/scalar.hpp          — local TWO_PI / PI inside wrapPi/wrap2Pi
//   f4-convert/src/dat_parser.cpp               — local kDTR
//
// All four defined the same numerical values (to within ULP) but under
// different names (DTR vs DEG_TO_RAD vs kDTR) and different storage
// qualifiers (constexpr vs inline constexpr). This header consolidates
// them under one name set so cross-module code stops referring to the
// same constant by three different names.
//
// Naming: the project's dominant convention is ALL_CAPS for mathematical
// constants (matches f4-geo and f4-flight). We expose BOTH `DTR` and
// `DEG_TO_RAD` (and `RTD`/`RAD_TO_DEG`) as aliases so existing call sites
// keep compiling; new code should prefer the long form for clarity.
//
// This file is header-only and has no includes. It sits in f4-math (the
// leaf dependency of every consumer) so any module can pull it in without
// creating a new dependency edge.

#pragma once

namespace f4::math {

// ============================================================================
// Circle constants — full precision (18-19 sig digits, fits in IEEE 754 double)
// ============================================================================
inline constexpr double PI      = 3.14159265358979323846;
inline constexpr double HALF_PI = 1.57079632679489661923;
inline constexpr double TWO_PI  = 6.28318530717958647692;

// ============================================================================
// Angle conversions
// ============================================================================
inline constexpr double DEG_TO_RAD = PI / 180.0;
inline constexpr double RAD_TO_DEG = 180.0 / PI;

// Short aliases retained for backward compatibility with existing call sites.
// Prefer the long names in new code.
inline constexpr double DTR = DEG_TO_RAD;  // Degrees To Radians
inline constexpr double RTD = RAD_TO_DEG;  // Radians To Degrees

} // namespace f4::math
