// f4-math/quat.hpp
//
// Unit quaternion for representing aircraft attitude.
//
// Convention (matches FreeFalcon's EOM):
//   - Quaternion order: (w, x, y, z) — scalar first.
//   - Hamilton product: standard right-multiply.
//   - Rotation: active rotation, v' = q * v * q^-1 where v is treated as
//     a pure quaternion (0, vx, vy, vz).
//   - Body-to-world: q rotates a body-frame vector into world frame.
//
// FreeFalcon's EOM uses quaternion kinematics:
//   q_dot = 0.5 * q * omega_quat
// where omega_quat = (0, p, q, r) is the angular velocity in body frame.
// This is implemented in the EOM library, not here — this header provides
// only the quaternion algebra.

#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <concepts>
#include <type_traits>

#include "f4/math/vec3.hpp"

namespace f4::math {

template<typename T>
    requires std::is_arithmetic_v<T>
struct Quat {
    T w{};   // scalar part
    T x{};   // i
    T y{};   // j
    T z{};   // k

    // --- Constructors ---
    constexpr Quat() = default;
    constexpr Quat(T w_, T x_, T y_, T z_) noexcept : w(w_), x(x_), y(y_), z(z_) {}

    // Build from scalar + vector parts.
    constexpr Quat(T w_, const Vec3<T>& v) noexcept : w(w_), x(v.x), y(v.y), z(v.z) {}

    // --- Identity ---
    [[nodiscard]] static constexpr Quat identity() noexcept { return {T{1}, T{0}, T{0}, T{0}}; }

    // --- Comparison ---
    constexpr bool operator==(const Quat& o) const noexcept = default;

    // --- Arithmetic ---
    constexpr Quat operator+(const Quat& o) const noexcept {
        return {w + o.w, x + o.x, y + o.y, z + o.z};
    }
    constexpr Quat operator-(const Quat& o) const noexcept {
        return {w - o.w, x - o.x, y - o.y, z - o.z};
    }
    constexpr Quat operator-() const noexcept {
        return {-w, -x, -y, -z};
    }
    constexpr Quat operator*(T s) const noexcept {
        return {w * s, x * s, y * s, z * s};
    }
    constexpr Quat operator/(T s) const noexcept {
        return {w / s, x / s, y / s, z / s};
    }

    // Hamilton product: q1 * q2.
    // Convention: applying q1*q2 to a vector applies q2 first, then q1.
    constexpr Quat operator*(const Quat& o) const noexcept {
        return {
            w * o.w - x * o.x - y * o.y - z * o.z,  // scalar
            w * o.x + x * o.w + y * o.z - z * o.y,  // i
            w * o.y - x * o.z + y * o.w + z * o.x,  // j
            w * o.z + x * o.y - y * o.x + z * o.w   // k
        };
    }
    constexpr Quat& operator*=(const Quat& o) noexcept {
        *this = *this * o;
        return *this;
    }

    // --- Conjugate and inverse ---
    // For a unit quaternion, conjugate == inverse.
    [[nodiscard]] constexpr Quat conjugate() const noexcept {
        return {w, -x, -y, -z};
    }
    [[nodiscard]] constexpr Quat inverse() const noexcept {
        T n2 = norm_squared();
        if (n2 < T{1e-20}) return identity();
        return conjugate() / n2;
    }

    // --- Norm ---
    [[nodiscard]] constexpr T norm_squared() const noexcept {
        return w * w + x * x + y * y + z * z;
    }
    [[nodiscard]] T norm() const noexcept {
        return std::sqrt(norm_squared());
    }
    [[nodiscard]] Quat normalized() const noexcept {
        T n = norm();
        if (n < T{1e-12}) return identity();
        return *this / n;
    }

    // --- Vector rotation ---
    // Rotate v by this quaternion: v' = q * v * q^-1.
    // For unit quaternions, q^-1 = q.conjugate(), which is what we use here
    // (assume unit; if not, normalize first or use rotate_nonunit).
    [[nodiscard]] Vec3<T> rotate(const Vec3<T>& v) const noexcept {
        // Optimized form (avoids building two full quaternions):
        //   t = 2 * cross(xyz_part, v)
        //   v' = v + w * t + cross(xyz_part, t)
        const Vec3<T> qv{x, y, z};
        const Vec3<T> t = qv.cross(v) * T{2};
        return v + t * w + qv.cross(t);
    }

    // Inverse rotation: rotate by q^-1.
    [[nodiscard]] Vec3<T> rotate_inverse(const Vec3<T>& v) const noexcept {
        return conjugate().rotate(v);
    }

    // --- Build from axis-angle (axis assumed normalized) ---
    [[nodiscard]] static Quat from_axis_angle(const Vec3<T>& axis, T angle_rad) noexcept {
        T half = angle_rad * T{0.5};
        T s = std::sin(half);
        return {std::cos(half), axis.x * s, axis.y * s, axis.z * s};
    }

    // --- Build from Euler angles (body 3-2-1 / yaw-pitch-roll) ---
    // This is the standard aerospace convention: first yaw about z, then
    // pitch about y, then roll about x, applied in that order to a body
    // frame. The resulting quaternion rotates body-frame vectors into
    // the world frame.
    [[nodiscard]] static Quat from_euler_zyx(T yaw, T pitch, T roll) noexcept {
        T cy = std::cos(yaw   * T{0.5}), sy = std::sin(yaw   * T{0.5});
        T cp = std::cos(pitch * T{0.5}), sp = std::sin(pitch * T{0.5});
        T cr = std::cos(roll  * T{0.5}), sr = std::sin(roll  * T{0.5});
        return {
            cr * cp * cy + sr * sp * sy,
            sr * cp * cy - cr * sp * sy,
            cr * sp * cy + sr * cp * sy,
            cr * cp * sy - sr * sp * cy
        };
    }

    // --- Extract Euler angles (body 3-2-1) ---
    // Inverse of from_euler_zyx. Returns {yaw, pitch, roll}.
    [[nodiscard]] std::array<T, 3> to_euler_zyx() const noexcept {
        // Standard extraction; handles gimbal-lock edge case approximately.
        T pitch = std::asin(std::clamp(T{2} * (w * y - z * x), T{-1}, T{1}));
        T yaw   = std::atan2(T{2} * (w * z + x * y), T{1} - T{2} * (y * y + z * z));
        T roll  = std::atan2(T{2} * (w * x + y * z), T{1} - T{2} * (x * x + y * y));
        return {yaw, pitch, roll};
    }

    // --- Spherical linear interpolation (slerp) ---
    // Returns the quaternion interpolated from *this (t=0) to other (t=1).
    // Handles the negative-double-cover by choosing the shorter path.
    [[nodiscard]] Quat slerp(const Quat& other, T t) const noexcept {
        T cos_theta = w * other.w + x * other.x + y * other.y + z * other.z;
        Quat b = other;
        if (cos_theta < T{0}) {
            b = -b;
            cos_theta = -cos_theta;
        }
        // If very close, use linear interpolation to avoid divide-by-zero.
        if (cos_theta > T{1} - T{1e-6}) {
            return Quat{
                w + (b.w - w) * t,
                x + (b.x - x) * t,
                y + (b.y - y) * t,
                z + (b.z - z) * t
            }.normalized();
        }
        T theta = std::acos(cos_theta);
        T sin_theta = std::sin(theta);
        T a = std::sin((T{1} - t) * theta) / sin_theta;
        T b_coef = std::sin(t * theta) / sin_theta;
        return {
            a * w + b_coef * b.w,
            a * x + b_coef * b.x,
            a * y + b_coef * b.y,
            a * z + b_coef * b.z
        };
    }

    // --- Vector part accessor ---
    [[nodiscard]] constexpr Vec3<T> vec() const noexcept { return {x, y, z}; }
};

// Scalar * Quat
template<typename T>
    requires std::is_arithmetic_v<T>
constexpr Quat<T> operator*(T s, const Quat<T>& q) noexcept {
    return q * s;
}

// Common aliases
using Quatd = Quat<double>;
using Quatf = Quat<float>;

} // namespace f4::math
