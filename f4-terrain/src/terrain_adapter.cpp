// f4-terrain/src/terrain_adapter.cpp
//
// TerrainDataAdapter implementation — bilinear interpolation over the
// TerrainData elevation grid. See terrain_adapter.hpp for design.

#include <f4/terrain/terrain_adapter.hpp>

#include <algorithm>
#include <cmath>

namespace f4::terrain {

double TerrainDataAdapter::elevation_at_ft(double east_ft, double north_ft) const {
    // If no elevation data (only tile_types), return 0 — the ground is
    // at sea level. This matches the pre-elevation behavior.
    if (data_.elevation.empty()) return 0.0;

    const double fpc = ft_per_cell();
    if (fpc <= 0.0) return 0.0;

    // Convert ENU feet → fractional terrain cell coordinates.
    // cell 0,0 = SW corner = ENU (0,0).
    const double fx = east_ft / fpc;
    const double fy = north_ft / fpc;

    // Clamp to the grid (no wraparound — the theater is an island).
    const double max_x = static_cast<double>(data_.header.width - 1);
    const double max_y = static_cast<double>(data_.header.height - 1);
    const double cx = std::clamp(fx, 0.0, max_x);
    const double cy = std::clamp(fy, 0.0, max_y);

    // Bilinear interpolation over the four surrounding cell centers.
    const uint32_t x0 = static_cast<uint32_t>(cx);
    const uint32_t y0 = static_cast<uint32_t>(cy);
    const uint32_t x1 = std::min(x0 + 1, data_.header.width - 1);
    const uint32_t y1 = std::min(y0 + 1, data_.header.height - 1);

    const double tx = cx - static_cast<double>(x0);
    const double ty = cy - static_cast<double>(y0);

    // elevation_at() uses sim convention: y=0 is north, but our fy
    // has y=0 at south (ENU). The TerrainData::elevation_at() already
    // handles the file-flip (file row 0 = south → sim y = height-1).
    // So we need to flip our y to match sim convention.
    const uint32_t sim_y0 = data_.header.height - 1 - y0;
    const uint32_t sim_y1 = data_.header.height - 1 - y1;

    const double e00 = data_.elevation_at(x0, sim_y0);
    const double e10 = data_.elevation_at(x1, sim_y0);
    const double e01 = data_.elevation_at(x0, sim_y1);
    const double e11 = data_.elevation_at(x1, sim_y1);

    // Bilinear: blend x first, then y.
    const double e0 = e00 + (e10 - e00) * tx;  // bottom edge
    const double e1 = e01 + (e11 - e01) * tx;  // top edge
    return e0 + (e1 - e0) * ty;
}

} // namespace f4::terrain
