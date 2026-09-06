// f4-renderer/include/f4/renderer/coord_transform.hpp
//
// Coordinate conversion functions for the F4 rendering pipeline.
// Pure arithmetic — no Raylib types, no GL context — so unit tests
// can verify the math without linking Raylib.
//
// The simulation uses ENU feet (East-North-Up, z-up) for TransformComponent.
// Raylib uses RH Y-up (X right, Y up, Z toward viewer).
// FreeFalcon's BSP model data uses LH Y-up (X right, Y up, Z forward).

#pragma once

#include <cmath>
#include <f4/math/vec3.hpp>

namespace f4::renderer {

/// Float3 is an alias for f4::math::Vec3f. Previously a bare struct
/// {float x,y,z}, now provides length(), normalized(), dot(), cross(),
/// etc. from f4-math. Kept as a short alias because it's used in
/// function-pointer signatures (mesh_builder.hpp).
using Float3 = f4::math::Vec3f;

/// Convert a model-space vertex (LH Y-up, FreeFalcon BSP convention) to
/// RH Y-up coordinates.
///
/// LH Y-up: x=right, y=up, z=forward (into screen)
/// RH Y-up: x=right, y=up, z=toward viewer (out of screen)
///
/// Conversion: (x, y, z) -> (y, -z, -x).
inline f4::math::Vec3f model_vertex_to_raylib(float x, float y, float z) noexcept {
    return f4::math::Vec3f{y, -z, -x};
}

/// glTF model-space vertex → Raylib RH Y-up coordinates.
///
/// The glTF exporter (f4-import/src/gltf_emitter.cpp `to_gltf`) bakes
/// Falcon model space (feet, x=right, y=up, z=forward) into glTF
/// (meters, +Y up): (x, y, z)_falcon → (y, z, −x) × 0.3048.
/// Composing that with model_vertex_to_raylib() gives the runtime-side
/// inverse: (x, y, z)_gltf → (x, −y, z) × (1/0.3048). The handedness
/// flip (det −1) mirrors triangle winding — the draw paths disable
/// backface culling for exactly this reason (FreeFalcon BSP models have
/// inconsistent winding regardless).
inline constexpr float kMetersToFeet = 3.28083989501312f;  // 1/0.3048

inline f4::math::Vec3f gltf_vertex_to_raylib(float x, float y, float z) noexcept {
    return f4::math::Vec3f{x * kMetersToFeet, -y * kMetersToFeet,
                            z * kMetersToFeet};
}

/// glTF normal → Raylib RH Y-up. Same basis change as
/// gltf_vertex_to_raylib WITHOUT the scale (normals are directions; the
/// uniform scale cancels under normalization). The mirror axis makes
/// this the correct normal transform (diag(1,−1,1) is its own
/// inverse-transpose).
inline f4::math::Vec3f gltf_normal_to_raylib(float x, float y, float z) noexcept {
    return f4::math::Vec3f{x, -y, z};
}

/// Convert an ENU position (feet) to RH Y-up coordinates.
///
/// ENU: x=east, y=north, z=up
/// RH Y-up: x=right, y=up, z=toward viewer (-north)
///
/// So a point 100 ft east, 200 ft north, 50 ft up maps to (100, 50, -200).
inline f4::math::Vec3f enu_to_raylib(double east_ft, double north_ft, double up_ft) noexcept {
    return f4::math::Vec3f{
        static_cast<float>(east_ft),
        static_cast<float>(up_ft),
        static_cast<float>(-north_ft)
    };
}

/// Convert an ENU quaternion (Hamilton convention, body-to-world) to
/// Raylib RH Y-up quaternion components.
///
/// The basis change ENU → RH Y-up is (x,y,z)_enu → (x,z,-y)_rh,
/// which gives the Hamilton-form rule:
///   q_rh (Hamilton w,x,y,z) = (qw, qx, qz, -qy)
///
/// Returns {x, y, z, w} in Raylib's Quaternion struct order.
/// The result is normalized to unit length.
struct ENUToRHQuat {
    float x, y, z, w;  // Raylib Quaternion order
};
inline ENUToRHQuat enu_quat_to_raylib(double qw, double qx, double qy, double qz) noexcept {
    ENUToRHQuat q_rh{
        static_cast<float>(qx),    // q.x  (Hamilton i)
        static_cast<float>(qz),    // q.y  (Hamilton k)
        static_cast<float>(-qy),   // q.z  (Hamilton -j)
        static_cast<float>(qw)     // q.w  (Hamilton scalar)
    };
    // Normalize to unit length (guard against drift).
    const float qlen = std::sqrt(q_rh.x*q_rh.x + q_rh.y*q_rh.y +
                                  q_rh.z*q_rh.z + q_rh.w*q_rh.w);
    if (qlen > 0.0001f) {
        q_rh.x /= qlen; q_rh.y /= qlen; q_rh.z /= qlen; q_rh.w /= qlen;
    }
    return q_rh;
}

} // namespace f4::renderer
