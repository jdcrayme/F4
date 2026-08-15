// f4-math/mat3.hpp
//
// 3×3 matrix type with arithmetic operations, axis rotation factories,
// and an affine transform (rotation + translation) compose/apply API.
//
// Row-major: m[row][col]. Matches the existing f4-models::Mat3x3 layout
// (float m[3][3]) so the binary parsers' sizeof is unchanged.
//
// Promoted from f4-models/src/poly_parser.hpp and
// f4-models/src/geometry_extractor.cpp to make 3×3 matrix algebra
// available to any module without depending on f4-models.

#pragma once

#include <cmath>
#include <concepts>
#include <type_traits>

#include "f4/math/vec3.hpp"

namespace f4::math {

// ============================================================================
// Mat3 — 3×3 row-major matrix
// ============================================================================
template<typename T>
    requires std::is_arithmetic_v<T>
struct Mat3 {
    // Row-major: m[row][col]
    T m[3][3] = {{1,0,0},{0,1,0},{0,0,1}};

    // --- Static factories ---

    [[nodiscard]] static constexpr Mat3 identity() noexcept {
        return {{{1,0,0},{0,1,0},{0,0,1}}};
    }

    [[nodiscard]] static constexpr Mat3 zero() noexcept {
        return {{{0,0,0},{0,0,0},{0,0,0}}};
    }

    /// Rotation about X axis (radians).
    [[nodiscard]] static Mat3 rotation_x(T angle) noexcept {
        const T c = std::cos(angle), s = std::sin(angle);
        return {{{1,0,0},{0,c,-s},{0,s,c}}};
    }

    /// Rotation about Y axis (radians).
    [[nodiscard]] static Mat3 rotation_y(T angle) noexcept {
        const T c = std::cos(angle), s = std::sin(angle);
        return {{{c,0,s},{0,1,0},{-s,0,c}}};
    }

    /// Rotation about Z axis (radians).
    [[nodiscard]] static Mat3 rotation_z(T angle) noexcept {
        const T c = std::cos(angle), s = std::sin(angle);
        return {{{c,-s,0},{s,c,0},{0,0,1}}};
    }

    /// Uniform scale matrix.
    [[nodiscard]] static constexpr Mat3 scale(T s) noexcept {
        return {{{s,0,0},{0,s,0},{0,0,s}}};
    }

    // --- Matrix arithmetic ---

    /// Matrix multiply.
    [[nodiscard]] constexpr Mat3 operator*(const Mat3& o) const noexcept {
        Mat3 result = zero();
        for (int i = 0; i < 3; ++i)
            for (int j = 0; j < 3; ++j)
                for (int k = 0; k < 3; ++k)
                    result.m[i][j] += m[i][k] * o.m[k][j];
        return result;
    }

    /// Matrix-vector multiply (transforms a column vector).
    [[nodiscard]] constexpr Vec3<T> operator*(const Vec3<T>& v) const noexcept {
        return {
            m[0][0]*v.x + m[0][1]*v.y + m[0][2]*v.z,
            m[1][0]*v.x + m[1][1]*v.y + m[1][2]*v.z,
            m[2][0]*v.x + m[2][1]*v.y + m[2][2]*v.z
        };
    }

    /// Transpose.
    [[nodiscard]] constexpr Mat3 transposed() const noexcept {
        Mat3 result;
        for (int i = 0; i < 3; ++i)
            for (int j = 0; j < 3; ++j)
                result.m[i][j] = m[j][i];
        return result;
    }

    /// Determinant.
    [[nodiscard]] constexpr T determinant() const noexcept {
        return m[0][0] * (m[1][1]*m[2][2] - m[1][2]*m[2][1])
             - m[0][1] * (m[1][0]*m[2][2] - m[1][2]*m[2][0])
             + m[0][2] * (m[1][0]*m[2][1] - m[1][1]*m[2][0]);
    }

    /// Check if approximately identity.
    [[nodiscard]] bool is_identity(T epsilon = T{1e-6}) const noexcept {
        for (int i = 0; i < 3; ++i)
            for (int j = 0; j < 3; ++j) {
                T expected = (i == j) ? T{1} : T{0};
                if (std::abs(m[i][j] - expected) > epsilon) return false;
            }
        return true;
    }
};

// Common aliases
using Mat3f = Mat3<float>;
using Mat3d = Mat3<double>;

// ============================================================================
// AffineTransform — rotation + translation: v' = rotation * v + translation
// ============================================================================
template<typename T>
    requires std::is_arithmetic_v<T>
struct AffineTransform {
    Mat3<T> rotation    = Mat3<T>::identity();
    Vec3<T> translation = Vec3<T>::zero();

    // --- Static factories ---

    [[nodiscard]] static constexpr AffineTransform identity() noexcept {
        return {};
    }

    /// Rotation-only transform about X axis.
    [[nodiscard]] static AffineTransform rotation_x(T angle) noexcept {
        AffineTransform t;
        t.rotation = Mat3<T>::rotation_x(angle);
        return t;
    }

    /// Rotation-only transform about Y axis.
    [[nodiscard]] static AffineTransform rotation_y(T angle) noexcept {
        AffineTransform t;
        t.rotation = Mat3<T>::rotation_y(angle);
        return t;
    }

    /// Rotation-only transform about Z axis.
    [[nodiscard]] static AffineTransform rotation_z(T angle) noexcept {
        AffineTransform t;
        t.rotation = Mat3<T>::rotation_z(angle);
        return t;
    }

    /// Pure translation.
    [[nodiscard]] static constexpr AffineTransform translate(const Vec3<T>& offset) noexcept {
        AffineTransform t;
        t.translation = offset;
        return t;
    }

    /// Uniform scale.
    [[nodiscard]] static constexpr AffineTransform uniform_scale(T s) noexcept {
        AffineTransform t;
        t.rotation = Mat3<T>::scale(s);
        return t;
    }

    // --- Apply ---

    /// Apply to a point: rotation * p + translation
    [[nodiscard]] constexpr Vec3<T> apply_point(const Vec3<T>& p) const noexcept {
        return rotation * p + translation;
    }

    /// Apply rotation only to a direction (normal): rotation * n
    [[nodiscard]] constexpr Vec3<T> apply_direction(const Vec3<T>& n) const noexcept {
        return rotation * n;
    }

    // --- Compose ---

    /// Compose two transforms: result = a ∘ b  (apply b first, then a)
    /// result.rotation    = a.rotation * b.rotation
    /// result.translation = a.rotation * b.translation + a.translation
    [[nodiscard]] static constexpr AffineTransform compose(const AffineTransform& a,
                                                           const AffineTransform& b) noexcept {
        AffineTransform result;
        result.rotation = a.rotation * b.rotation;
        result.translation = a.apply_direction(b.translation) + a.translation;
        return result;
    }

    // --- Queries ---

    /// Check if approximately identity.
    [[nodiscard]] bool is_identity(T epsilon = T{1e-6}) const noexcept {
        return rotation.is_identity(epsilon) &&
               translation.length_squared() < epsilon * epsilon;
    }
};

// Common aliases
using AffineTransformf = AffineTransform<float>;
using AffineTransformd = AffineTransform<double>;

} // namespace f4::math
