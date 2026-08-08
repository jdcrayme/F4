// f4-recorder/src/path_geometry.cpp
//
// Path geometry computation — cross-track errors, glide slopes.

#include "f4/recorder/path_geometry.hpp"

#include <cmath>

namespace f4::recorder {

double cross_track_error(
    const geo::WorldPosition& point,
    const geo::WorldPosition& line_start,
    const geo::WorldPosition& line_end) noexcept
{
    // 2D cross-track error (horizontal plane).
    // Line direction: d = end - start
    // Vector to point: v = point - start
    // Cross product magnitude / |d| gives perpendicular distance.
    // Sign: positive = right of the line (looking from start to end).

    const double dx = line_end.x - line_start.x;
    const double dy = line_end.y - line_start.y;
    const double len_sq = dx * dx + dy * dy;

    if (len_sq < 1e-12) {
        // Degenerate line segment
        return point.distance_horiz_to(line_start);
    }

    const double vx = point.x - line_start.x;
    const double vy = point.y - line_start.y;

    // 2D cross product: dy * vx - dx * vy
    // Positive = point is to the right of the line (looking from start to end)
    // In ENU: right of a northbound line is east (+x).
    const double cross = dy * vx - dx * vy;

    return cross / std::sqrt(len_sq);
}

double along_track_fraction(
    const geo::WorldPosition& point,
    const geo::WorldPosition& line_start,
    const geo::WorldPosition& line_end) noexcept
{
    // Project (point - start) onto (end - start), normalized to [0, 1].

    const double dx = line_end.x - line_start.x;
    const double dy = line_end.y - line_start.y;
    const double dz = line_end.z - line_start.z;
    const double len_sq = dx * dx + dy * dy + dz * dz;

    if (len_sq < 1e-12) {
        return 0.0;
    }

    const double vx = point.x - line_start.x;
    const double vy = point.y - line_start.y;
    const double vz = point.z - line_start.z;

    const double dot = vx * dx + vy * dy + vz * dz;
    return dot / len_sq;
}

double glide_slope_altitude_ft(
    double distance_from_threshold_ft,
    double glide_slope_angle_rad,
    double threshold_altitude_ft) noexcept
{
    // altitude = threshold_alt + distance * tan(glideslope_angle)
    return threshold_altitude_ft +
           distance_from_threshold_ft * std::tan(glide_slope_angle_rad);
}

} // namespace f4::recorder
