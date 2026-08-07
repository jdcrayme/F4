// f4-math/vec3.hpp
//
// Minimal 3-D vector type with the arithmetic the flight model needs.
//
// FreeFalcon uses a mix of `Tpoint` (3 floats), `Tvector` (3 doubles), and
// raw `float[3]` throughout the codebase, with operations inlined by hand
// at every call site. This header replaces that zoo with one typed Vec3
// (parameterized on the representation type) and a small set of named
// operations. The intent is that future f4-flight-model code uses
// `Vec3<double>` for world-frame positions/velocities and `Vec3<float>` is
// available if memory layout matters (e.g. for entity storage).
//
// Not a full linear-algebra library — no general matrix, no projection.
// Just enough to express EOM, body↔world transforms, and dot/cross products.
// Anything more belongs in a separate f4-linalg library if it ever becomes
// necessary.

#pragma once

#include <cmath>
#include <concepts>
#include <type_traits>

namespace f4::math {

template<typename T>
    requires std::is_arithmetic_v<T>
struct Vec3 {
    T x{};
    T y{};
    T z{};

    // --- Element access by index (for generic algorithms) ---
    T& operator[](std::size_t i) {
        switch (i) {
            case 0: return x;
            case 1: return y;
            default: return z;
        }
    }
    const T& operator[](std::size_t i) const {
        switch (i) {
            case 0: return x;
            case 1: return y;
            default: return z;
        }
    }

    // --- Arithmetic ---
    constexpr Vec3 operator+(const Vec3& o) const noexcept { return {x + o.x, y + o.y, z + o.z}; }
    constexpr Vec3 operator-(const Vec3& o) const noexcept { return {x - o.x, y - o.y, z - o.z}; }
    constexpr Vec3 operator-() const noexcept { return {-x, -y, -z}; }
    constexpr Vec3& operator+=(const Vec3& o) noexcept { x += o.x; y += o.y; z += o.z; return *this; }
    constexpr Vec3& operator-=(const Vec3& o) noexcept { x -= o.x; y -= o.y; z -= o.z; return *this; }

    // Scalar multiply / divide
    constexpr Vec3 operator*(T s) const noexcept { return {x * s, y * s, z * s}; }
    constexpr Vec3 operator/(T s) const noexcept { return {x / s, y / s, z / s}; }
    constexpr Vec3& operator*=(T s) noexcept { x *= s; y *= s; z *= s; return *this; }
    constexpr Vec3& operator/=(T s) noexcept { x /= s; y /= s; z /= s; return *this; }

    // --- Comparison ---
    constexpr bool operator==(const Vec3& o) const noexcept = default;

    // --- Queries ---
    [[nodiscard]] constexpr T dot(const Vec3& o) const noexcept {
        return x * o.x + y * o.y + z * o.z;
    }

    [[nodiscard]] constexpr Vec3 cross(const Vec3& o) const noexcept {
        return {
            y * o.z - z * o.y,
            z * o.x - x * o.z,
            x * o.y - y * o.x
        };
    }

    [[nodiscard]] T length_squared() const noexcept {
        return x * x + y * y + z * z;
    }

    [[nodiscard]] T length() const noexcept {
        return std::sqrt(length_squared());
    }

    /// Return the unit vector in the direction of *this.
    ///
    /// Zero-length vectors return the zero vector (no NaN propagation, no
    /// crash). This matches the test `NormalizeZeroVectorReturnsZero` and
    /// the principle that math primitives should be total functions —
    /// crashing on degenerate inputs in debug builds hides bugs in calling
    /// code rather than surfacing them cleanly.
    ///
    /// The previous implementation used `assert(len >= T{1e-12})` here,
    /// which aborted the process in debug builds and caused MSVC's test
    /// runner to mark all subsequent tests in the binary as NotExecuted.
    /// The assert contradicted the test spec; the test is the spec.
    [[nodiscard]] Vec3 normalized() const noexcept {
        T len = length();
        if (len < T{1e-12}) return *this;  // zero or degenerate -> zero
        return *this / len;
    }

    // Component-wise multiply (Hadamard). Useful for scaling vectors by
    // per-axis factors (e.g. moment-of-inertia tensor application).
    [[nodiscard]] constexpr Vec3 hadamard(const Vec3& o) const noexcept {
        return {x * o.x, y * o.y, z * o.z};
    }
};

// Scalar * Vec3 (commutative convenience)
template<typename T>
    requires std::is_arithmetic_v<T>
constexpr Vec3<T> operator*(T s, const Vec3<T>& v) noexcept {
    return v * s;
}

// ============================================================================
// Free-function aliases for the common arithmetic.
// ============================================================================
template<typename T>
[[nodiscard]] inline T dot(const Vec3<T>& a, const Vec3<T>& b) noexcept {
    return a.dot(b);
}

template<typename T>
[[nodiscard]] inline Vec3<T> cross(const Vec3<T>& a, const Vec3<T>& b) noexcept {
    return a.cross(b);
}

template<typename T>
[[nodiscard]] inline T length(const Vec3<T>& v) noexcept {
    return v.length();
}

template<typename T>
[[nodiscard]] inline Vec3<T> normalize(const Vec3<T>& v) noexcept {
    return v.normalized();
}

// ============================================================================
// Common type aliases
// ============================================================================
using Vec3d = Vec3<double>;
using Vec3f = Vec3<float>;

} // namespace f4::math
