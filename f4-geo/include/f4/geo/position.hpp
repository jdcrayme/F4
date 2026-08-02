// f4-geo/position.hpp
//
// Strong-typed absolute position representations. Three distinct types model
// the same physical point in different frames:
//
//   WorldPosition  — simulation-local ENU frame, FEET, z-up.
//                    The SOURCE OF TRUTH stored in the entity TransformComponent.
//   LatLonAlt      — geodetic on the WGS84 ellipsoid (radians + feet).
//   ECEFPosition   — Earth-Centered Earth-Fixed cartesian, METERS (WGS84).
//
// Design rule: these are distinct types with NO implicit conversion. Every
// crossing is a named, explicit call (see conversions.hpp). A WorldPosition
// is never accidentally passed where a LatLonAlt is expected, and a LatLonAlt
// is never silently treated as ECEF. This is the f4-units philosophy
// (phantom dimensions for Mach/CAS) extended to coordinate frames.
//
// Relative/reference-based representations (BRA, BullseyeOffset) live in
// relative.hpp — they are derived views, not storage types, and are not part
// of the free conversion lattice because they carry a reference point.

#pragma once

#include <cmath>
#include <compare>

#include "constants.hpp"

namespace f4::geo {

// ============================================================================
// WorldPosition — the simulation's native local frame.
//
// Convention: theater-local ENU (East-North-Up), all in FEET, z-up.
//   x = east   (feet)
//   y = north  (feet)
//   z = up     (feet)
//
// This is what the flight model, physics, and spatial index operate on at
// high frequency. Earth-frame conversions happen only at I/O and reporting
// boundaries, never on the hot path.
// ============================================================================
struct WorldPosition {
    double x{};   // east,  feet
    double y{};   // north, feet
    double z{};   // up,    feet

    constexpr WorldPosition() = default;
    constexpr WorldPosition(double x_, double y_, double z_) noexcept
        : x(x_), y(y_), z(z_) {}

    auto operator<=>(const WorldPosition&) const = default;

    // Offset arithmetic. The difference of two positions is an offset vector;
    // we keep it as a WorldPosition (treated as a translation) to avoid a
    // proliferating type zoo. For richer vector math (cross products, etc.)
    // convert to f4::math::Vec3d at the call site.
    constexpr WorldPosition operator+(const WorldPosition& o) const noexcept {
        return {x + o.x, y + o.y, z + o.z};
    }
    constexpr WorldPosition operator-(const WorldPosition& o) const noexcept {
        return {x - o.x, y - o.y, z - o.z};
    }
    constexpr WorldPosition& operator+=(const WorldPosition& o) noexcept {
        x += o.x; y += o.y; z += o.z; return *this;
    }

    [[nodiscard]] double distance_to(const WorldPosition& o) const noexcept {
        const double dx = x - o.x, dy = y - o.y, dz = z - o.z;
        return std::sqrt(dx * dx + dy * dy + dz * dz);
    }
    [[nodiscard]] double distance_horiz_to(const WorldPosition& o) const noexcept {
        const double dx = x - o.x, dy = y - o.y;
        return std::sqrt(dx * dx + dy * dy);
    }
};

// ============================================================================
// LatLonAlt — geodetic position on the WGS84 ellipsoid.
//   lat = latitude  (radians, +north, [-pi/2, +pi/2])
//   lon = longitude (radians, +east,  [-pi, +pi])
//   alt = altitude above the ellipsoid (FEET — matches sim convention)
// ============================================================================
struct LatLonAlt {
    double lat{};
    double lon{};
    double alt{};

    constexpr LatLonAlt() = default;
    constexpr LatLonAlt(double lat_, double lon_, double alt_) noexcept
        : lat(lat_), lon(lon_), alt(alt_) {}

    auto operator<=>(const LatLonAlt&) const = default;

    // Altitude in meters, for ECEF interop and DIS serialization.
    [[nodiscard]] constexpr double alt_meters() const noexcept {
        return alt * METERS_PER_FOOT;
    }
};

// ============================================================================
// ECEFPosition — Earth-Centered Earth-Fixed cartesian (WGS84).
//   x, y, z in METERS.
//
// This is the wire format for DIS and most interoperability standards.
// Conversion to/from LatLonAlt is exact (WGS84). Conversion to/from
// WorldPosition composes through LatLonAlt and requires a TheaterDatum.
// ============================================================================
struct ECEFPosition {
    double x{};
    double y{};
    double z{};

    constexpr ECEFPosition() = default;
    constexpr ECEFPosition(double x_, double y_, double z_) noexcept
        : x(x_), y(y_), z(z_) {}

    auto operator<=>(const ECEFPosition&) const = default;
};

} // namespace f4::geo
