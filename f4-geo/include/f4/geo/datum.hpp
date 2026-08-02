// f4-geo/datum.hpp
//
// TheaterDatum — binds the simulation's local frame to the Earth.
//
// A datum defines:
//   - origin:   the geodetic point (LatLonAlt) corresponding to sim (0,0,0)
//   - heading:  the clockwise angle (radians) from true north to the sim +Y
//               axis, viewed from above. heading = 0 means sim +Y points
//               north (sim frame == ENU). heading = pi/2 means sim +Y points
//               east.
//
// With a datum, WorldPosition <-> LatLonAlt is a flat-earth (local tangent
// plane) conversion: a 2D rotation plus scale in the tangent plane, with
// altitude mapping directly. This is exact to <1 m over theater-scale
// distances (hundreds of NM) and is what Falcon-class sims use internally.
//
// This is the keystone for interoperability: a future f4-dis adapter
// serializes ECEF on the wire (via LatLonAlt) and converts to WorldPosition
// on ingest using the theater's datum — no rewrite, no special cases.
// Real-world data import (terrain, airbases, waypoints given as lat/lon)
// flows through the same datum into the sim frame.

#pragma once

#include <compare>

#include "position.hpp"

namespace f4::geo {

struct TheaterDatum {
    LatLonAlt origin{};
    double heading_rad = 0.0;

    constexpr TheaterDatum() = default;
    constexpr TheaterDatum(LatLonAlt o, double heading = 0.0) noexcept
        : origin(o), heading_rad(heading) {}

    auto operator<=>(const TheaterDatum&) const = default;

    // Convenience: a datum whose origin is the intersection of the equator
    // and the prime meridian, sim frame aligned to ENU. Useful for tests and
    // for theaters that have not yet been assigned a real-world placement.
    [[nodiscard]] static constexpr TheaterDatum identity() noexcept {
        return TheaterDatum{LatLonAlt{0.0, 0.0, 0.0}, 0.0};
    }
};

} // namespace f4::geo
