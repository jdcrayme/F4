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
    // explicit: prevents accidental implicit conversion from a 3-double
    // brace initializer (e.g. `{x, y, z}` passed to a function expecting
    // a LatLonAlt). Direct-init `WorldPosition{x, y, z}` still works.
    constexpr explicit WorldPosition(double x_, double y_, double z_) noexcept
        : x(x_), y(y_), z(z_) {}

    auto operator<=>(const WorldPosition&) const = default;

    // Offset arithmetic. The difference of two positions is an offset vector;
    // we keep it as a WorldPosition (treated as a translation) to avoid a
    // proliferating type zoo. For richer vector math (cross products, etc.)
    // convert to f4::math::Vec3d at the call site.
    constexpr WorldPosition operator+(const WorldPosition& o) const noexcept {
        return WorldPosition{x + o.x, y + o.y, z + o.z};
    }
    constexpr WorldPosition operator-(const WorldPosition& o) const noexcept {
        return WorldPosition{x - o.x, y - o.y, z - o.z};
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
    // explicit: the ctor takes (lat_rad, lon_rad, alt_ft) — making it
    // explicit forces callers to either use the typed factories below
    // (from_degrees) or write `LatLonAlt{lat_rad, lon_rad, alt_ft}`
    // explicitly. This closes the most common bug: passing degrees where
    // radians are expected, which compiles silently because both are
    // `double`.
    constexpr explicit LatLonAlt(double lat_rad, double lon_rad, double alt_ft) noexcept
        : lat(lat_rad), lon(lon_rad), alt(alt_ft) {}

    auto operator<=>(const LatLonAlt&) const = default;

    // Altitude in meters, for ECEF interop and DIS serialization.
    [[nodiscard]] constexpr double alt_meters() const noexcept {
        return alt * METERS_PER_FOOT;
    }

    // ---- Typed factories ----
    //
    // Use these when the inputs are in degrees (the common case for
    // theater definitions, DIS input, and human-readable coordinates).
    // The radians ctor above is for code that has already converted
    // (e.g. internal conversions.hpp paths).
    //
    // Example:
    //   LatLonAlt lla = LatLonAlt::from_degrees(38.0, -77.0, 0.0);
    //   // lla.lat == 38 * DEG_TO_RAD, lla.lon == -77 * DEG_TO_RAD
    //
    [[nodiscard]] static constexpr LatLonAlt from_degrees(
        double lat_deg, double lon_deg, double alt_ft) noexcept {
        return LatLonAlt{lat_deg * DEG_TO_RAD, lon_deg * DEG_TO_RAD, alt_ft};
    }
};

// ============================================================================
// NEDPosition — North-East-Down local frame.
//
// Convention: NED (North-East-Down), all in FEET, z-down.
//   x = north (feet)
//   y = east  (feet)
//   z = down  (feet, so altitude = -z)
//
// This is the frame used internally by the flight model (equations of
// motion, aerodynamics, FCS). The EOM integrates position in NED and
// then converts to ENU (WorldPosition) for entity storage via the
// explicit to_enu() / to_ned() crossing functions.
//
// CRITICAL: NEDPosition and WorldPosition are distinct types. You
// cannot accidentally pass a NED position where an ENU position is
// expected. Every crossing is a named call:
//   WorldPosition enu = to_enu(ned);
//   NEDPosition    ned = to_ned(enu);
// ============================================================================
struct NEDPosition {
    double x{};   // north, feet
    double y{};   // east,  feet
    double z{};   // down,  feet (altitude = -z)

    constexpr NEDPosition() = default;
    constexpr explicit NEDPosition(double x_, double y_, double z_) noexcept
        : x(x_), y(y_), z(z_) {}

    auto operator<=>(const NEDPosition&) const = default;

    constexpr NEDPosition operator+(const NEDPosition& o) const noexcept {
        return NEDPosition{x + o.x, y + o.y, z + o.z};
    }
    constexpr NEDPosition operator-(const NEDPosition& o) const noexcept {
        return NEDPosition{x - o.x, y - o.y, z - o.z};
    }
    constexpr NEDPosition& operator+=(const NEDPosition& o) noexcept {
        x += o.x; y += o.y; z += o.z; return *this;
    }

    [[nodiscard]] double distance_to(const NEDPosition& o) const noexcept {
        const double dx = x - o.x, dy = y - o.y, dz = z - o.z;
        return std::sqrt(dx * dx + dy * dy + dz * dz);
    }

    /// Altitude above ground (positive up), derived from the NED z component.
    [[nodiscard]] constexpr double altitude_ft() const noexcept { return -z; }
};

// ============================================================================
// Frame crossing: ENU ↔ NED
//
// These are the ONLY way to convert between WorldPosition (ENU) and
// NEDPosition. The axes are remapped:
//   ENU (x=E, y=N, z=U) ↔ NED (x=N, y=E, z=D)
//   So: ned.x = enu.y,  ned.y = enu.x,  ned.z = -enu.z
//       enu.x = ned.y,  enu.y = ned.x,  enu.z = -ned.z
// ============================================================================

/// Convert ENU (WorldPosition) to NED (NEDPosition).
[[nodiscard]] constexpr NEDPosition to_ned(const WorldPosition& enu) noexcept {
    return NEDPosition{enu.y, enu.x, -enu.z};
}

/// Convert NED (NEDPosition) to ENU (WorldPosition).
[[nodiscard]] constexpr WorldPosition to_enu(const NEDPosition& ned) noexcept {
    return WorldPosition{ned.y, ned.x, -ned.z};
}

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
    // explicit: same reasoning as WorldPosition/LatLonAlt — prevents
    // accidental implicit conversion from a 3-double brace init.
    constexpr explicit ECEFPosition(double x_, double y_, double z_) noexcept
        : x(x_), y(y_), z(z_) {}

    auto operator<=>(const ECEFPosition&) const = default;
};

} // namespace f4::geo
