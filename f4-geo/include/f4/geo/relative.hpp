// f4-geo/relative.hpp
//
// Relative / reference-based position representations: BRA and BullseyeOffset.
//
// These are NOT part of the free conversion lattice in conversions.hpp.
// They carry an implicit reference point (ownship for BRA, the theater
// bullseye for BullseyeOffset) and therefore cannot be converted back to an
// absolute WorldPosition without that reference. The inverse constructors
// (from_bullseye, bra_target) take the reference explicitly, making the
// dependency visible in the type system.
//
// They are derived views — computed from absolute positions on demand for
// human-facing reporting (radio calls, brevity), never stored as the source
// of truth.

#pragma once

#include <cmath>
#include <compare>

#include "constants.hpp"
#include "position.hpp"

namespace f4::geo {

// ============================================================================
// BRA — Bearing, Range, Altitude.
//
// Bearing is the true bearing (clockwise from north) from `from` to `target`.
// Range is the slant range. Altitude is the target's MSL altitude (so the
// recipient also knows the target's absolute altitude, not just the relative
// vertical offset — matching how BRA is called on the radio).
// ============================================================================
struct BRA {
    double bearing_rad{};   // [0, 2*pi), 0 = north, clockwise positive
    double range_ft{};      // slant range
    double altitude_ft{};   // target altitude MSL

    auto operator<=>(const BRA&) const = default;

    [[nodiscard]] constexpr double bearing_deg() const noexcept { return bearing_rad * RAD_TO_DEG; }
    [[nodiscard]] constexpr double range_nm()    const noexcept { return range_ft / FEET_PER_NM; }
};

[[nodiscard]] inline BRA to_bra(const WorldPosition& from,
                                const WorldPosition& target) noexcept {
    const double dx = target.x - from.x;   // east
    const double dy = target.y - from.y;   // north
    const double dz = target.z - from.z;   // up
    double brg = std::atan2(dx, dy);       // atan2(east, north) -> bearing CW from north
    if (brg < 0.0) brg += TWO_PI;
    return BRA{brg, std::sqrt(dx * dx + dy * dy + dz * dz), target.z};
}

// ============================================================================
// BullseyeOffset — bearing & range from the theater bullseye.
//
// Planar (no altitude): bullseye is a theater-wide planar reference used for
// brevity calls ("target at bullseye 045 for 20"). Reconstructing the
// absolute position requires the bullseye point itself (from_bullseye).
// ============================================================================
struct BullseyeOffset {
    double bearing_rad{};   // [0, 2*pi)
    double range_ft{};

    auto operator<=>(const BullseyeOffset&) const = default;

    [[nodiscard]] constexpr double bearing_deg() const noexcept { return bearing_rad * RAD_TO_DEG; }
    [[nodiscard]] constexpr double range_nm()    const noexcept { return range_ft / FEET_PER_NM; }
};

[[nodiscard]] inline BullseyeOffset to_bullseye(const WorldPosition& bullseye,
                                                const WorldPosition& target) noexcept {
    const double dx = target.x - bullseye.x;   // east
    const double dy = target.y - bullseye.y;   // north
    double brg = std::atan2(dx, dy);
    if (brg < 0.0) brg += TWO_PI;
    return BullseyeOffset{brg, std::sqrt(dx * dx + dy * dy)};
}

/// Inverse: reconstruct a planar WorldPosition from a bullseye offset and the
/// bullseye point. Altitude is taken from the bullseye (bullseye offsets are
/// planar by convention); callers needing a specific target altitude should
/// set it after construction.
[[nodiscard]] inline WorldPosition from_bullseye(const WorldPosition& bullseye,
                                                 const BullseyeOffset& off) noexcept {
    const double n = off.range_ft * std::cos(off.bearing_rad);
    const double e = off.range_ft * std::sin(off.bearing_rad);
    return WorldPosition{bullseye.x + e, bullseye.y + n, bullseye.z};
}

} // namespace f4::geo
