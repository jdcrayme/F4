// f4-math/scalar.hpp
//
// Small scalar utilities used throughout the flight model and FCS:
//   - limit        : clamp to [lo, hi] or [-mag, +mag]
//   - deadBand     : zero inside a band, pass through outside
//   - wrapPi       : wrap angle (radians) to (-pi, +pi]
//   - wrap2Pi      : wrap angle (radians) to [0, 2*pi)
//   - lerp         : linear interpolation
//   - rescale      : affine map from [in_min, in_max] to [out_min, out_max]
//   - sign         : signum (-1, 0, +1)
//   - squared      : x*x (avoids redundant multiplication)
//
// Direct ports of the scalar helpers in FreeFalcon's simlib/math.cpp
// (SIMLIB_MATH_CLASS::Limit, DeadBand, Resolve, Resolve0) and the
// RESCALE macro used by lookuptable.cpp. Behaviour matches the original
// at the level that callers depend on; the implementations here are
// templated and constexpr so they can be used with any numeric type
// (including f4-units quantities, when the host library chooses to mix them).

#pragma once

#include <f4/math/constants.hpp>

#include <cmath>
#include <concepts>
#include <type_traits>

namespace f4::math {

// ============================================================================
// Numeric concept: anything that supports the arithmetic we need.
// Includes double, float, int, plus any user-defined Quantity<> that
// supports scalar multiply / comparison (f4-units satisfies this).
// ============================================================================
template<typename T>
concept Numeric = std::is_arithmetic_v<T>;

// ============================================================================
// limit — clamp x to [lo, hi]. Direct port of SIMLIB_MATH_CLASS::Limit.
// ============================================================================
template<Numeric T>
constexpr T limit(T x, T lo, T hi) noexcept {
    return (x < lo) ? lo : (x > hi) ? hi : x;
}

// Symmetric overload: clamp x to [-mag, +mag].
template<Numeric T>
constexpr T limit(T x, T mag) noexcept {
    return limit(x, static_cast<T>(-mag), mag);
}

// Alias for readability at call sites that prefer the standard-library spelling.
template<Numeric T>
constexpr T clamp(T x, T lo, T hi) noexcept {
    return limit(x, lo, hi);
}

// ============================================================================
// deadBand — zero inside [-band, +band], pass-through outside.
//
// Port of SIMLIB_MATH_CLASS::DeadBand. Note the original returns the raw
// input value outside the band (NOT input - band), matching FF behaviour.
// If you want the "subtract band" variant, compose deadBand(x, b) + sign(x)*b
// at the call site.
// ============================================================================
template<Numeric T>
constexpr T deadBand(T x, T band) noexcept {
    return (x > band) ? x : (x < -band) ? x : T{0};
}

// ============================================================================
// wrapPi — wrap angle (radians) to (-pi, +pi].
//
// Port of SIMLIB_MATH_CLASS::Resolve(2*pi). The original FF code uses
// `input - 2*pi * floor((input + pi) / (2*pi))`; we use the equivalent
// fmod formulation that is branch-free on most modern CPUs.
//
// Boundary: +pi maps to +pi (not -pi). This matters for callers that
// compare wrapped angles for equality.
// ============================================================================
inline double wrapPi(double x) noexcept {
    double y = std::fmod(x + PI, TWO_PI);
    if (y < 0.0) y += TWO_PI;
    y -= PI;
    // Map -pi boundary to +pi so result is in (-pi, +pi].
    if (y <= -PI + 1e-15) y = PI;
    return y;
}

inline float wrapPi(float x) noexcept {
    constexpr float TWO_PI_f = static_cast<float>(TWO_PI);
    constexpr float PI_f     = static_cast<float>(PI);
    float y = std::fmod(x + PI_f, TWO_PI_f);
    if (y < 0.0f) y += TWO_PI_f;
    y -= PI_f;
    if (y <= -PI_f + 1e-7f) y = PI_f;
    return y;
}

// ============================================================================
// wrap2Pi — wrap angle (radians) to [0, 2*pi).
// ============================================================================
inline double wrap2Pi(double x) noexcept {
    double y = std::fmod(x, TWO_PI);
    if (y < 0.0) y += TWO_PI;
    return y;
}

inline float wrap2Pi(float x) noexcept {
    constexpr float TWO_PI_f = static_cast<float>(TWO_PI);
    float y = std::fmod(x, TWO_PI_f);
    if (y < 0.0f) y += TWO_PI_f;
    return y;
}

// ============================================================================
// lerp — linear interpolation: a + t*(b - a).
// Not clamped; pass t = limit(t, 0.0, 1.0) at the call site if needed.
// ============================================================================
template<Numeric T, Numeric U>
constexpr auto lerp(T a, T b, U t) noexcept {
    return a + static_cast<U>(b - a) * t;
}

// ============================================================================
// rescale — affine map from [in_min, in_max] to [out_min, out_max].
//
// Direct port of the RESCALE macro in falclib/lookuptable.cpp. Division by
// zero is the caller's responsibility: if in_min == in_max the result is
// undefined (NaN); this matches FF behaviour. Use table.hpp if you need
// boundary-safe interpolation.
// ============================================================================
template<Numeric T>
constexpr T rescale(T in, T in_min, T in_max, T out_min, T out_max) noexcept {
    return (in - in_min) * (out_max - out_min) / (in_max - in_min) + out_min;
}

// ============================================================================
// sign — signum: -1 if x<0, 0 if x==0, +1 if x>0.
// ============================================================================
template<Numeric T>
constexpr T sign(T x) noexcept {
    return (x > T{0}) ? T{1} : (x < T{0}) ? T{-1} : T{0};
}

// ============================================================================
// squared — x*x. Tiny helper, but it makes intent explicit at call sites
// like `const auto mach_sq = squared(mach);` and avoids the common typo
// `mach * mach` being silently miscompiled to `mach * mach2` in long
// expressions.
// ============================================================================
template<Numeric T>
constexpr T squared(T x) noexcept { return x * x; }

} // namespace f4::math
