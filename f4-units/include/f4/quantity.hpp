#pragma once

#include "unit.hpp"

#include <cmath>
#include <compare>
#include <concepts>
#include <type_traits>

namespace f4 {

// Forward declaration
template<typename UnitT, typename Rep = double>
class Quantity;

// ============================================================================
// Type Traits
// ============================================================================

/// True if T is a specialization of Quantity.
template<typename T>
struct is_quantity : std::false_type {};

template<typename U, typename R>
struct is_quantity<Quantity<U, R>> : std::true_type {};

template<typename T>
inline constexpr bool is_quantity_v = is_quantity<T>::value;

// ============================================================================
// Quantity
// ============================================================================

/// A value paired with a unit, providing compile-time dimensional safety.
///
/// Supports:
///   - Construction from a raw numeric value (explicit)
///   - Conversion between units of the same dimension via to<>()
///   - Same-type arithmetic (+, -, scalar *, scalar /)
///   - Cross-dimension arithmetic (produces correct result dimension)
///   - All comparison operators (same and different units of same dimension)
///   - Integer power (pow<N>) and square root (qsqrt)
///
/// Does NOT support:
///   - Implicit conversion from raw numeric types
///   - Cross-dimension arithmetic for phantom dimensions (CAS, Mach)
///   - Mixing of different dimensions in +, -, or comparisons
template<typename UnitT, typename Rep>
class Quantity {
    static_assert(std::is_arithmetic_v<Rep>,
                  "Quantity representation type must be arithmetic (int, float, double, etc.)");

    Rep value_;

public:
    using unit_type = UnitT;
    using dimension = typename UnitT::dimension;
    using rep       = Rep;

    constexpr explicit Quantity(Rep val) : value_(val) {}

    /// Raw numeric value in the quantity's native unit.
    constexpr Rep value() const { return value_; }

    /// Convert to a different unit of the same dimension.
    ///
    /// For physical dimensions: uses the affine conversion formula
    ///   value_target = (value_src * to_base_src + offset_src - offset_tgt) / to_base_tgt
    ///
    /// For phantom dimensions (CAS, Mach): uses simple proportional scaling.
    ///   Phantom units have no physical SI base; their to_base is just a
    ///   display scaling ratio. Converting 100 CAS-knots to CAS-m/s should
    ///   yield 100 × (0.5144) = 51.44, not 100 / 0.5144 = 194.4.
    template<typename OtherUnit>
    constexpr auto to() const -> Quantity<OtherUnit, Rep> {
        static_assert(same_dimension_v<dimension, typename OtherUnit::dimension>,
                      "Cannot convert between incompatible dimensions");
        if constexpr (dimension::is_phantom) {
            constexpr double ratio = OtherUnit::to_base / UnitT::to_base;
            return Quantity<OtherUnit, Rep>(static_cast<Rep>(value_ * ratio));
        } else {
            constexpr double scale  = UnitT::to_base / OtherUnit::to_base;
            constexpr double offset = (UnitT::offset - OtherUnit::offset) / OtherUnit::to_base;
            return Quantity<OtherUnit, Rep>(static_cast<Rep>(value_ * scale + offset));
        }
    }

    /// Shorthand: extract value in a specific unit.
    /// Equivalent to to<OtherUnit>().value() but saves one conversion step.
    template<typename OtherUnit>
    constexpr Rep in() const {
        return to<OtherUnit>().value();
    }

    /// Convert to SI base units and return the raw value.
    constexpr Rep in_base() const {
        return to<BaseUnit<dimension>>().value();
    }

    // --- Unary operators ---
    constexpr Quantity operator+() const { return *this; }
    constexpr Quantity operator-() const { return Quantity(-value_); }

    // --- Same-type compound assignment ---
    constexpr Quantity& operator+=(Quantity rhs) { value_ += rhs.value_; return *this; }
    constexpr Quantity& operator-=(Quantity rhs) { value_ -= rhs.value_; return *this; }
    constexpr Quantity& operator*=(Rep scalar)    { value_ *= scalar;      return *this; }
    constexpr Quantity& operator/=(Rep scalar)    { value_ /= scalar;      return *this; }

    // --- Same-type binary arithmetic ---
    constexpr Quantity operator+(Quantity rhs) const { return Quantity(value_ + rhs.value_); }
    constexpr Quantity operator-(Quantity rhs) const { return Quantity(value_ - rhs.value_); }
    constexpr Quantity operator*(Rep scalar)    const { return Quantity(value_ * scalar); }
    constexpr Quantity operator/(Rep scalar)    const { return Quantity(value_ / scalar); }
};

// ============================================================================
// Free Operators: Scalar interaction
// ============================================================================

/// scalar * Quantity (scalar must not be a Quantity itself).
template<typename U, typename R>
constexpr Quantity<U, R> operator*(R scalar, Quantity<U, R> q)
    requires (!is_quantity_v<R>)
{
    return Quantity<U, R>(scalar * q.value());

}
/// Quantity / scalar.
template<typename U, typename R>
constexpr Quantity<U, R> operator/(Quantity<U, R> q, R scalar)
    requires (!is_quantity_v<R>)
{
    return Quantity<U, R>(q.value() / scalar);
}

/// scalar / Quantity -> inverse dimension (physical only).
/// Example: 1.0 / meters -> inverse meters (1/m).
template<typename U, typename R>
    requires (physical_dimension<typename U::dimension> && !is_quantity_v<R>)
constexpr auto operator/(R scalar, Quantity<U, R> q) {
    using ResultDim  = dim_invert<typename U::dimension>;
    using ResultUnit = BaseUnit<ResultDim>;
    auto base_val = q.template to<BaseUnit<typename U::dimension>>().value();
    return Quantity<ResultUnit, R>(scalar / base_val);
}

// ============================================================================
// Free Operators: Heterogeneous addition/subtraction (same dimension, different unit)
// ============================================================================

/// Result is in the LHS unit.
template<typename U1, typename R1, typename U2, typename R2>
    requires (!std::same_as<Quantity<U1, R1>, Quantity<U2, R2>>)
constexpr auto operator+(Quantity<U1, R1> lhs, Quantity<U2, R2> rhs)
    -> Quantity<U1, std::common_type_t<R1, R2>>
{
    static_assert(same_dimension_v<typename U1::dimension, typename U2::dimension>,
                  "Cannot add quantities of different dimensions");
    using R = std::common_type_t<R1, R2>;
    return Quantity<U1, R>(static_cast<R>(lhs.value()) +
                           static_cast<R>(rhs.template to<U1>().value()));
}

template<typename U1, typename R1, typename U2, typename R2>
    requires (!std::same_as<Quantity<U1, R1>, Quantity<U2, R2>>)
constexpr auto operator-(Quantity<U1, R1> lhs, Quantity<U2, R2> rhs)
    -> Quantity<U1, std::common_type_t<R1, R2>>
{
    static_assert(same_dimension_v<typename U1::dimension, typename U2::dimension>,
                  "Cannot subtract quantities of different dimensions");
    using R = std::common_type_t<R1, R2>;
    return Quantity<U1, R>(static_cast<R>(lhs.value()) -
                           static_cast<R>(rhs.template to<U1>().value()));
}

// ============================================================================
// Free Operators: Comparisons
// ============================================================================

// --- Same type ---

template<typename U, typename R>
constexpr bool operator==(Quantity<U, R> a, Quantity<U, R> b) { return a.value() == b.value(); }

template<typename U, typename R>
constexpr bool operator!=(Quantity<U, R> a, Quantity<U, R> b) { return a.value() != b.value(); }

template<typename U, typename R>
constexpr bool operator<(Quantity<U, R> a, Quantity<U, R> b) { return a.value() < b.value(); }

template<typename U, typename R>
constexpr bool operator<=(Quantity<U, R> a, Quantity<U, R> b) { return a.value() <= b.value(); }

template<typename U, typename R>
constexpr bool operator>(Quantity<U, R> a, Quantity<U, R> b) { return a.value() > b.value(); }

template<typename U, typename R>
constexpr bool operator>=(Quantity<U, R> a, Quantity<U, R> b) { return a.value() >= b.value(); }

// --- Heterogeneous (same dimension, different unit) ---

template<typename U1, typename R1, typename U2, typename R2>
    requires (!std::same_as<Quantity<U1, R1>, Quantity<U2, R2>>)
constexpr bool operator==(Quantity<U1, R1> a, Quantity<U2, R2> b) {
    static_assert(same_dimension_v<typename U1::dimension, typename U2::dimension>,
                  "Cannot compare quantities of different dimensions");
    return a.value() == b.template to<U1>().value();
}

template<typename U1, typename R1, typename U2, typename R2>
    requires (!std::same_as<Quantity<U1, R1>, Quantity<U2, R2>>)
constexpr bool operator!=(Quantity<U1, R1> a, Quantity<U2, R2> b) {
    return !(a == b);
}

template<typename U1, typename R1, typename U2, typename R2>
    requires (!std::same_as<Quantity<U1, R1>, Quantity<U2, R2>>)
constexpr bool operator<(Quantity<U1, R1> a, Quantity<U2, R2> b) {
    static_assert(same_dimension_v<typename U1::dimension, typename U2::dimension>,
                  "Cannot compare quantities of different dimensions");
    return a.value() < b.template to<U1>().value();
}

template<typename U1, typename R1, typename U2, typename R2>
    requires (!std::same_as<Quantity<U1, R1>, Quantity<U2, R2>>)
constexpr bool operator<=(Quantity<U1, R1> a, Quantity<U2, R2> b) {
    return !(b < a);
}

template<typename U1, typename R1, typename U2, typename R2>
    requires (!std::same_as<Quantity<U1, R1>, Quantity<U2, R2>>)
constexpr bool operator>(Quantity<U1, R1> a, Quantity<U2, R2> b) {
    return b < a;
}

template<typename U1, typename R1, typename U2, typename R2>
    requires (!std::same_as<Quantity<U1, R1>, Quantity<U2, R2>>)
constexpr bool operator>=(Quantity<U1, R1> a, Quantity<U2, R2> b) {
    return !(a < b);
}

// ============================================================================
// Free Operators: Cross-dimension multiplication (physical dimensions only)
// ============================================================================

/// Multiplying two physical-dimension quantities produces a result in SI base
/// units with the combined dimension.
///
/// Examples:
///   Speed * Time    = Length   (in meters)
///   Force / Area   = Pressure (in pascals)
///   Length * Length = Area    (in square meters)
template<typename U1, typename R1, typename U2, typename R2>
    requires (physical_dimension<typename U1::dimension> &&
              physical_dimension<typename U2::dimension>)
constexpr auto operator*(Quantity<U1, R1> lhs, Quantity<U2, R2> rhs) {
    using ResultDim  = dim_multiply<typename U1::dimension, typename U2::dimension>;
    using ResultUnit = BaseUnit<ResultDim>;
    using R = std::common_type_t<R1, R2>;
    return Quantity<ResultUnit, R>(
        static_cast<R>(lhs.template to<BaseUnit<typename U1::dimension>>().value()) *
        static_cast<R>(rhs.template to<BaseUnit<typename U2::dimension>>().value()));
}

// ============================================================================
// Free Operators: Cross-dimension division (physical dimensions only)
// ============================================================================

template<typename U1, typename R1, typename U2, typename R2>
    requires (physical_dimension<typename U1::dimension> &&
              physical_dimension<typename U2::dimension>)
constexpr auto operator/(Quantity<U1, R1> lhs, Quantity<U2, R2> rhs) {
    using ResultDim  = dim_divide<typename U1::dimension, typename U2::dimension>;
    using ResultUnit = BaseUnit<ResultDim>;
    using R = std::common_type_t<R1, R2>;
    return Quantity<ResultUnit, R>(
        static_cast<R>(lhs.template to<BaseUnit<typename U1::dimension>>().value()) /
        static_cast<R>(rhs.template to<BaseUnit<typename U2::dimension>>().value()));
}

// ============================================================================
// Power and Square Root
// ============================================================================

/// Raise a physical-dimension quantity to an integer power.
///
/// Example:
///   pow<2>(length) -> Area (in m^2)
///   pow<3>(length) -> Volume (in m^3)
template<int N, typename U, typename R>
    requires (physical_dimension<typename U::dimension>)
constexpr auto qpow(Quantity<U, R> q) {
    using D = typename U::dimension;
    using ResultDim  = Dimension<N * D::length, N * D::mass,
                                  N * D::time, N * D::temperature, N * D::angle>;
    using ResultUnit = BaseUnit<ResultDim>;
    auto base_val = q.template to<BaseUnit<D>>().value();
    if constexpr (N >= 0) {
        R result = static_cast<R>(1);
        for (int i = 0; i < N; ++i) result *= static_cast<R>(base_val);
        return Quantity<ResultUnit, R>(result);
    } else {
        R result = static_cast<R>(1);
        for (int i = 0; i < -N; ++i) result /= static_cast<R>(base_val);
        return Quantity<ResultUnit, R>(result);
    }
}

/// Square root of a physical-dimension quantity.
/// All dimension exponents must be even.
///
/// Example:
///   qsqrt(area) -> Length (in m)
///   qsqrt(pressure / density) -> Speed (in m/s)
template<typename U, typename R>
    requires (physical_dimension<typename U::dimension>)
constexpr auto qsqrt(Quantity<U, R> q)
    -> Quantity<BaseUnit<Dimension<
        U::dimension::length / 2, U::dimension::mass / 2,
        U::dimension::time / 2, U::dimension::temperature / 2,
        U::dimension::angle / 2>>, R>
{
    static_assert(U::dimension::length      % 2 == 0 &&
                  U::dimension::mass        % 2 == 0 &&
                  U::dimension::time        % 2 == 0 &&
                  U::dimension::temperature % 2 == 0 &&
                  U::dimension::angle       % 2 == 0,
                  "qsqrt requires all dimension exponents to be even");
    using ResultDim  = Dimension<U::dimension::length / 2, U::dimension::mass / 2,
                                  U::dimension::time / 2, U::dimension::temperature / 2,
                                  U::dimension::angle / 2>;
    using ResultUnit = BaseUnit<ResultDim>;
    auto base_val = q.template to<BaseUnit<typename U::dimension>>().value();
    return Quantity<ResultUnit, R>(static_cast<R>(std::sqrt(base_val)));
}

/// Absolute value (same unit).
template<typename U, typename R>
constexpr Quantity<U, R> qabs(Quantity<U, R> q) {
    return Quantity<U, R>(std::abs(q.value()));
}

} // namespace f4
