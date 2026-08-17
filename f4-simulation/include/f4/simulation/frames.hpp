// f4-simulation/include/f4/simulation/frames.hpp
//
// Frame conversions between the flight model's NED convention and the
// simulation's ENU convention — for ORIENTATION (quaternions), the
// counterpart of the position mapping enu = (ned_y, ned_x, -ned_z).
//
// CONVENTIONS (the source of a long-standing rendering bug):
//   - The FM/EOM work in NED (x=north, y=east, z=down) with standard aero
//     Euler angles: psi is a COMPASS heading (0=north, + clockwise toward
//     east) — in NED that is a positive right-handed rotation about z=DOWN.
//   - The simulation/renderer work in ENU (x=east, y=north, z=up), where
//     the same compass heading is a NEGATIVE rotation about z=UP.
//
// The NED->ENU basis change is a 180-degree rotation about the NE bisector
// (x,y,z)_ned -> (y,x,-z)_enu, and a 180-degree quaternion conjugation maps
// (w, x, y, z) -> (w, y, x, -z). Composing with the FM's ZYX quaternion
// directly is WRONG (compass yaw mirrors; pitch/roll scramble into the
// "model flying upside down" artifact) — the quaternion must be conjugated.
//
// Pure functions, no dependencies beyond stdlib. C++20.

#pragma once

#include <cmath>

namespace f4::simulation {

/// Hamilton quaternion (w-first).
struct QuatD {
    double w{1.0}, x{0.0}, y{0.0}, z{0.0};
};

/// Rotate a 3-vector by a quaternion (active rotation, body-to-world).
/// v' = v + 2*qv x (qv x v + w*v)
inline void quat_rotate(const QuatD& q, double& vx, double& vy, double& vz) noexcept {
    const double c1x = q.y * vz - q.z * vy;
    const double c1y = q.z * vx - q.x * vz;
    const double c1z = q.x * vy - q.y * vx;
    const double c2x = q.w * vx + c1x;
    const double c2y = q.w * vy + c1y;
    const double c2z = q.w * vz + c1z;
    vx = vx + 2.0 * (q.y * c2z - q.z * c2y);
    vy = vy + 2.0 * (q.z * c2x - q.x * c2z);
    vz = vz + 2.0 * (q.x * c2y - q.y * c2x);
}

/// NED (right-handed, z=down, compass psi positive) orientation
/// quaternion -> ENU (z=up, compass heading negative about +z).
/// 180-deg conjugation about the NE bisector: (w,x,y,z) -> (w,y,x,-z).
inline QuatD ned_quat_to_enu(const QuatD& q) noexcept {
    return QuatD{q.w, q.y, q.x, -q.z};
}

/// Compass heading (radians, 0=north, + clockwise/east) -> ENU quaternion
/// for a level aircraft facing that heading. In ENU this is a NEGATIVE
/// rotation about +z (up).
inline QuatD enu_quat_from_compass(double heading_rad) noexcept {
    const double h = heading_rad * 0.5;
    return QuatD{std::cos(h), 0.0, 0.0, -std::sin(h)};
}

} // namespace f4::simulation
