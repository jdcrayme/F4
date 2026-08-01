#pragma once

#include <type_traits>

namespace f4 {

// ============================================================================
// Physical Dimension
// ============================================================================

/// Physical dimension represented as exponents of 5 base dimensions.
/// L = Length, M = Mass, T = Time, Th = Temperature, A = Angle.
///
/// Angle is treated as a separate base dimension (following Boost.Units)
/// to prevent accidental mixing with dimensionless ratios like Mach number.
template<int L, int M, int T, int Th, int A>
struct Dimension {
    static constexpr int length      = L;
    static constexpr int mass        = M;
    static constexpr int time        = T;
    static constexpr int temperature = Th;
    static constexpr int angle       = A;
    static constexpr bool is_phantom = false;
};

// ============================================================================
// Phantom Dimension
// ============================================================================

/// Phantom dimension for quantities that do not participate in dimensional
/// arithmetic (multiplication, division, sqrt, etc.).
///
/// Use this for aviation-specific quantities that share dimensional exponents
/// with physical quantities but must never be mixed with them:
///   - Calibrated Airspeed (CAS): has the exponents of 1/T, but is
///     semantically a pressure-equivalent displayed in speed units.
///   - Mach number: is truly dimensionless, but must not be used as a
///     plain ratio or mixed with other dimensionless quantities.
///
/// Phantom quantities support:
///   - Construction from a raw value
///   - Value extraction
///   - Conversion between display units of the same phantom type
///   - Scalar multiplication and division
///
/// Phantom quantities do NOT support:
///   - Cross-dimension multiplication or division
///   - Mixing with any physical-dimension quantity
template<typename Tag>
struct PhantomDimension {
    static constexpr int length      = 0;
    static constexpr int mass        = 0;
    static constexpr int time        = 0;
    static constexpr int temperature = 0;
    static constexpr int angle       = 0;
    static constexpr bool is_phantom = true;
    using tag = Tag;
};

// ============================================================================
// Dimension Comparison
// ============================================================================

// Helper: only check tag identity when BOTH dimensions are phantom.
// Physical dimensions don't have a ::tag member, so accessing it would fail.
template<bool BothPhantom, typename D1, typename D2>
struct tags_match : std::true_type {};

template<typename D1, typename D2>
struct tags_match<true, D1, D2>
    : std::is_same<typename D1::tag, typename D2::tag> {};

/// Type trait: true if two dimensions represent the same physical quantity.
///
/// Physical dimensions match when all exponents are identical.
/// Phantom dimensions additionally require tag identity.
/// A phantom dimension never matches a physical dimension, even if
/// all exponents happen to be zero (e.g., Mach vs. Dimensionless).
template<typename D1, typename D2>
struct same_dimension_impl : std::bool_constant<
    (D1::length      == D2::length) &&
    (D1::mass        == D2::mass) &&
    (D1::time        == D2::time) &&
    (D1::temperature == D2::temperature) &&
    (D1::angle       == D2::angle) &&
    (D1::is_phantom  == D2::is_phantom) &&
    tags_match<D1::is_phantom && D2::is_phantom, D1, D2>::value
> {};

template<typename D1, typename D2>
inline constexpr bool same_dimension_v = same_dimension_impl<D1, D2>::value;

// ============================================================================
// Dimension Arithmetic
// ============================================================================

/// Dimension multiplication: result exponents are the sum of operand exponents.
template<typename D1, typename D2>
using dim_multiply = Dimension<
    D1::length      + D2::length,
    D1::mass        + D2::mass,
    D1::time        + D2::time,
    D1::temperature + D2::temperature,
    D1::angle       + D2::angle
>;

/// Dimension division: result exponents are the difference of operand exponents.
template<typename D1, typename D2>
using dim_divide = Dimension<
    D1::length      - D2::length,
    D1::mass        - D2::mass,
    D1::time        - D2::time,
    D1::temperature - D2::temperature,
    D1::angle       - D2::angle
>;

/// Dimension inversion (reciprocal): all exponents negated.
template<typename D>
using dim_invert = Dimension<
    -D::length, -D::mass, -D::time, -D::temperature, -D::angle
>;

// ============================================================================
// Concepts
// ============================================================================

/// Concept: a dimension that participates in cross-dimensional arithmetic.
/// Phantom dimensions (CAS, Mach) do not satisfy this concept.
template<typename D>
concept physical_dimension = !D::is_phantom;

} // namespace f4
