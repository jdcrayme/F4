// f4-geo/conversions.hpp
//
// The conversion lattice between the three absolute position types:
//
//                  TheaterDatum
//   WorldPosition <==========> LatLonAlt <==========> ECEFPosition
//      (sim, ft)     flat-earth      (geodetic)   exact WGS84   (ECEF, m)
//
// All conversions are explicit free functions (no implicit conversion
// operators). This makes every frame crossing greppable and prevents the
// classic bug of silently treating a relative/Earth coordinate as a sim
// coordinate.
//
// Accuracy:
//   - LatLonAlt <-> ECEFPosition : exact (WGS84), closed-form (Ferrari).
//   - WorldPosition <-> LatLonAlt : flat-earth tangent plane, <1 m error
//     over theater scale (~hundreds of NM). Sufficient for any Falcon-class
//     theater; upgrade to a proper projection only if global extents are
//     ever required.

#pragma once

#include <cmath>

#include "constants.hpp"
#include "position.hpp"
#include "datum.hpp"

namespace f4::geo {

// ============================================================================
// LatLonAlt  <->  ECEFPosition   (exact WGS84)
// ============================================================================

[[nodiscard]] inline ECEFPosition to_ecef(const LatLonAlt& lla) noexcept {
    const double sinlat = std::sin(lla.lat);
    const double coslat = std::cos(lla.lat);
    const double sinlon = std::sin(lla.lon);
    const double coslon = std::cos(lla.lon);
    const double h_m    = lla.alt * METERS_PER_FOOT;
    const double N      = WGS84_A / std::sqrt(1.0 - WGS84_E2 * sinlat * sinlat);
    return ECEFPosition{
        (N + h_m) * coslat * coslon,
        (N + h_m) * coslat * sinlon,
        (N * (1.0 - WGS84_E2) + h_m) * sinlat
    };
}

[[nodiscard]] inline LatLonAlt to_lla(const ECEFPosition& ecef) noexcept {
    const double p   = std::sqrt(ecef.x * ecef.x + ecef.y * ecef.y);
    const double lon = std::atan2(ecef.y, ecef.x);
    // Closed-form (Bowring/Ferrari) — no iteration, robust at all latitudes.
    const double b     = WGS84_B;
    const double theta = std::atan2(ecef.z * WGS84_A, p * b);
    const double st    = std::sin(theta);
    const double ct    = std::cos(theta);
    const double st3   = st * st * st;
    const double ct3   = ct * ct * ct;
    const double lat   = std::atan2(
        ecef.z + WGS84_EP2 * b * st3,
        p     - WGS84_E2  * WGS84_A * ct3);
    const double sinlat = std::sin(lat);
    const double N      = WGS84_A / std::sqrt(1.0 - WGS84_E2 * sinlat * sinlat);
    // Altitude. The p/cos(lat) form is ill-conditioned near the poles; use
    // the z/sin(lat) form there.
    double h_m;
    if (p > 1e-9) {
        h_m = p / std::cos(lat) - N;
    } else {
        h_m = ecef.z / sinlat - N * (1.0 - WGS84_E2);
    }
    return LatLonAlt{lat, lon, h_m * FEET_PER_METER};
}

// ============================================================================
// WorldPosition  <->  LatLonAlt   (flat-earth, via TheaterDatum)
// ============================================================================

namespace detail {

// Local radii of curvature at a latitude (meters).
//   R_M : meridional  (north-south travel changes lat by dN / R_M)
//   R_N : transverse  (east-west travel changes lon by dE / (R_N * cos(lat)))
struct LocalRadii { double R_M; double R_N; };

[[nodiscard]] inline LocalRadii local_radii(double lat) noexcept {
    const double sinlat = std::sin(lat);
    const double w      = std::sqrt(1.0 - WGS84_E2 * sinlat * sinlat);
    return LocalRadii{
        WGS84_A * (1.0 - WGS84_E2) / (w * w * w),  // R_M
        WGS84_A / w                                  // R_N
    };
}

} // namespace detail

/// WorldPosition -> LatLonAlt, given the theater datum.
[[nodiscard]] inline LatLonAlt to_lla(const WorldPosition& w,
                                      const TheaterDatum& d) noexcept {
    // Sim feet -> ENU meters.
    double e_m = w.x * METERS_PER_FOOT;
    double n_m = w.y * METERS_PER_FOOT;
    const double u_m = w.z * METERS_PER_FOOT;

    // Inverse heading rotation: sim offset -> ENU offset.
    // (to_world applies the forward rotation; here we invert it.)
    if (d.heading_rad != 0.0) {
        const double ch = std::cos(d.heading_rad);
        const double sh = std::sin(d.heading_rad);
        const double e2 =  e_m * ch + n_m * sh;
        const double n2 = -e_m * sh + n_m * ch;
        e_m = e2;
        n_m = n2;
    }

    const auto r = detail::local_radii(d.origin.lat);
    const double lat = d.origin.lat + n_m / r.R_M;
    const double cos_lat0 = std::cos(d.origin.lat);
    const double lon = (cos_lat0 > 1e-12)
        ? d.origin.lon + e_m / (r.R_N * cos_lat0)
        : d.origin.lon;  // near pole, longitude is singular
    const double alt = d.origin.alt + u_m * FEET_PER_METER;
    return LatLonAlt{lat, lon, alt};
}

/// LatLonAlt -> WorldPosition, given the theater datum.
[[nodiscard]] inline WorldPosition to_world(const LatLonAlt& lla,
                                            const TheaterDatum& d) noexcept {
    const auto r = detail::local_radii(d.origin.lat);
    const double cos_lat0 = std::cos(d.origin.lat);
    double e_m = (lla.lon - d.origin.lon) * r.R_N * cos_lat0;
    double n_m = (lla.lat - d.origin.lat) * r.R_M;
    const double u_m = (lla.alt - d.origin.alt) * METERS_PER_FOOT;

    // Forward heading rotation: ENU offset -> sim offset.
    if (d.heading_rad != 0.0) {
        const double ch = std::cos(d.heading_rad);
        const double sh = std::sin(d.heading_rad);
        const double e2 = e_m * ch - n_m * sh;
        const double n2 = e_m * sh + n_m * ch;
        e_m = e2;
        n_m = n2;
    }

    return WorldPosition{
        e_m * FEET_PER_METER,
        n_m * FEET_PER_METER,
        u_m * FEET_PER_METER   // u_m is in meters; convert back to feet
    };
}

// ============================================================================
// WorldPosition  <->  ECEFPosition   (composed via LatLonAlt)
// ============================================================================

[[nodiscard]] inline ECEFPosition to_ecef(const WorldPosition& w,
                                          const TheaterDatum& d) noexcept {
    return to_ecef(to_lla(w, d));
}

[[nodiscard]] inline WorldPosition to_world(const ECEFPosition& ecef,
                                            const TheaterDatum& d) noexcept {
    return to_world(to_lla(ecef), d);
}

} // namespace f4::geo
