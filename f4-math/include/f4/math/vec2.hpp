// f4-math/vec2.hpp
//
// Minimal 2-D vector type. Complements Vec3 for 2D geometry needs
// (ground-layout calculations, map views, taxi-route planning, etc.).
//
// Design mirrors vec3.hpp: parameterized on representation type, small set
// of named operations, zero-length normalization returns zero (total function).

#pragma once

#include <cmath>
#include <concepts>
#include <type_traits>

namespace f4::math {

template<typename T>
    requires std::is_arithmetic_v<T>
struct Vec2 {
    T x{};
    T y{};

    // --- Element access by index (for generic algorithms) ---
    T& operator[](std::size_t i) {
        return (i == 0) ? x : y;
    }
    const T& operator[](std::size_t i) const {
        return (i == 0) ? x : y;
    }

    // --- Arithmetic ---
    constexpr Vec2 operator+(const Vec2& o) const noexcept { return {x + o.x, y + o.y}; }
    constexpr Vec2 operator-(const Vec2& o) const noexcept { return {x - o.x, y - o.y}; }
    constexpr Vec2 operator-() const noexcept { return {-x, -y}; }
    constexpr Vec2& operator+=(const Vec2& o) noexcept { x += o.x; y += o.y; return *this; }
    constexpr Vec2& operator-=(const Vec2& o) noexcept { x -= o.x; y -= o.y; return *this; }

    // Scalar multiply / divide
    constexpr Vec2 operator*(T s) const noexcept { return {x * s, y * s}; }
    constexpr Vec2 operator/(T s) const noexcept { return {x / s, y / s}; }
    constexpr Vec2& operator*=(T s) noexcept { x *= s; y *= s; return *this; }
    constexpr Vec2& operator/=(T s) noexcept { x /= s; y /= s; return *this; }

    // --- Comparison ---
    constexpr bool operator==(const Vec2& o) const noexcept = default;

    // --- Queries ---
    [[nodiscard]] constexpr T dot(const Vec2& o) const noexcept {
        return x * o.x + y * o.y;
    }

    /// 2D cross product (scalar): x1*y2 - y1*x2.
    /// Positive if o is CCW from *this, negative if CW.
    [[nodiscard]] constexpr T cross(const Vec2& o) const noexcept {
        return x * o.y - y * o.x;
    }

    [[nodiscard]] T length_squared() const noexcept {
        return x * x + y * y;
    }

    [[nodiscard]] T length() const noexcept {
        return std::sqrt(length_squared());
    }

    /// Return the unit vector in the direction of *this.
    /// Zero-length vectors return the zero vector (no NaN propagation).
    [[nodiscard]] Vec2 normalized() const noexcept {
        T len = length();
        if (len < T{1e-12}) return *this;
        return *this / len;
    }

    /// Perpendicular (rotate 90° CCW).
    [[nodiscard]] constexpr Vec2 perp_ccw() const noexcept {
        return {-y, x};
    }

    /// Perpendicular (rotate 90° CW).
    [[nodiscard]] constexpr Vec2 perp_cw() const noexcept {
        return {y, -x};
    }

    /// Hadamard (component-wise) product.
    [[nodiscard]] constexpr Vec2 hadamard(const Vec2& o) const noexcept {
        return {x * o.x, y * o.y};
    }

    // --- Static factories ---
    [[nodiscard]] static constexpr Vec2 zero() noexcept { return {T{0}, T{0}}; }
};

// Scalar * Vec2 (commutative convenience)
template<typename T>
    requires std::is_arithmetic_v<T>
constexpr Vec2<T> operator*(T s, const Vec2<T>& v) noexcept {
    return v * s;
}

// ============================================================================
// Free-function aliases
// ============================================================================
template<typename T>
[[nodiscard]] inline T dot(const Vec2<T>& a, const Vec2<T>& b) noexcept {
    return a.dot(b);
}

template<typename T>
[[nodiscard]] inline T cross(const Vec2<T>& a, const Vec2<T>& b) noexcept {
    return a.cross(b);
}

template<typename T>
[[nodiscard]] inline T length(const Vec2<T>& v) noexcept {
    return v.length();
}

template<typename T>
[[nodiscard]] inline Vec2<T> normalize(const Vec2<T>& v) noexcept {
    return v.normalized();
}

// ============================================================================
// Common type aliases
// ============================================================================
using Vec2d = Vec2<double>;
using Vec2f = Vec2<float>;

} // namespace f4::math
