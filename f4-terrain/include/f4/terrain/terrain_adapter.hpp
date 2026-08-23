// f4-terrain/include/f4/terrain/terrain_adapter.hpp
//
// TerrainDataAdapter — bridges f4::terrain::TerrainData (the 128×128
// elevation grid) to f4::simulation::TerrainSource (the abstract elevation
// query interface).
//
// The adapter converts sim-local ENU feet → terrain grid cell → bilinear-
// interpolated elevation. This is the concrete TerrainSource the scenario
// player registers with Simulation::set_terrain_source() so the flight
// model's ground clamp follows real Korea elevation instead of a flat plane.
//
// Coordinate mapping:
//   - The theater grid is 1024×1024 grid units, each 1024 ft.
//   - So the theater spans 1,048,576 ft (1024*1024) on each side.
//   - The terrain grid is 128×128 cells covering the same area.
//   - Each terrain cell = 1,048,576 / 128 = 8192 ft.
//   - ENU (0,0) = grid (0,0) = terrain cell (0,0) = SW corner.
//   - cell_x = east_ft / 8192, cell_y = north_ft / 8192.
//
// Bilinear interpolation: the four surrounding cell centers are weighted
// by fractional distance. Cells outside the grid are clamped to the edge
// (no wraparound — the theater is an island).
//
// Dependencies: f4-terrain (TerrainData), f4-simulation (TerrainSource).
// C++20.

#pragma once

#include <f4/terrain/terrain_data.hpp>
#include <f4/terrain/terrain_source.hpp>

namespace f4::terrain {

/// TerrainSource implementation backed by a TerrainData elevation grid.
/// Bilinear-interpolates the 128×128 elevation grid at the aircraft's
/// ENU position. The adapter does NOT own the TerrainData — the host
/// must keep it alive for the adapter's lifetime.
class TerrainDataAdapter final : public TerrainSource {
public:
    /// Construct an adapter over the given TerrainData. The data must
    /// outlive the adapter. If the TerrainData has no elevation array
    /// (only tile_types), elevation_at_ft returns 0 everywhere.
    explicit TerrainDataAdapter(const TerrainData& data) noexcept
        : data_(data) {}

    [[nodiscard]] double elevation_at_ft(double east_ft, double north_ft) const override;

private:
    const TerrainData& data_;

    /// Feet per terrain cell = theater_size_ft / terrain_width.
    /// theater_size_ft = 1024 grid * 1024 ft/grid = 1,048,576 ft.
    /// terrain_width = 128 cells (typical; read from header at runtime).
    [[nodiscard]] double ft_per_cell() const noexcept {
        const double theater_size_ft = 1024.0 * 1024.0;
        const double w = static_cast<double>(data_.header.width > 0 ? data_.header.width : 128);
        return theater_size_ft / w;
    }
};

} // namespace f4::terrain
