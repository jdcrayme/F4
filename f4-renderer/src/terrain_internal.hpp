// f4-renderer/src/terrain_internal.hpp
//
// Internal helpers shared by the terrain mesh builders (terrain_mesh.cpp,
// terrain_chunks.cpp). Per the note that lived in both files: the
// duplication was tolerated for two consumers; WorldView (the third)
// earned the extraction.
//
// Keep in sync with f4-terrain's TheaterGeometry — these mirror the same
// repo ENU convention (theater = 1024×1024 grid units × 1024 ft) until
// the theater scale is reconciled (see TheaterGeometry's header note).

#pragma once

#include <f4/terrain/terrain_data.hpp>

#include <algorithm>

namespace f4::renderer::detail {

/// Feet per MEA cell in the repo ENU convention (1,048,576 ft theater
/// over the 128-cell grid; header width overrides for other grids).
inline double theater_ft_per_cell(const f4::terrain::TerrainData& td) {
    const double theater_size_ft = 1024.0 * 1024.0;
    const double w = static_cast<double>(td.header.width > 0 ? td.header.width : 128);
    return theater_size_ft / w;
}

/// World ENU feet → clamped terrain cell index.
///
/// CRITICAL: this is the ONLY correct way to convert feet → cell index.
/// Truncating without clamping is undefined behavior when world_ft < 0
/// (mesh vertices west/south of the theater) — the negative value wraps
/// to ~4 billion, std::min clamps to grid_size-1 (the FAR edge), and
/// coastal objectives showed land instead of water on the mesh edge.
inline uint32_t world_to_cell_clamped(double world_ft, double ft_per_cell,
                                      uint32_t grid_size) {
    if (grid_size == 0) return 0;
    const double f = world_ft / ft_per_cell;
    const double clamped = std::clamp(f, 0.0, static_cast<double>(grid_size - 1));
    return static_cast<uint32_t>(clamped);
}

/// Bilinear MEA-grid elevation at ENU feet (clamped at edges).
inline double bilinear_elevation(const f4::terrain::TerrainData& td,
                                 double east_ft, double north_ft) {
    if (td.elevation.empty()) return 0.0;

    const double ft_per_cell = theater_ft_per_cell(td);
    if (ft_per_cell <= 0.0) return 0.0;

    const double fx = std::clamp(east_ft / ft_per_cell, 0.0,
                                 static_cast<double>(td.header.width - 1));
    const double fy = std::clamp(north_ft / ft_per_cell, 0.0,
                                 static_cast<double>(td.header.height - 1));

    const uint32_t x0 = static_cast<uint32_t>(fx);
    const uint32_t y0 = static_cast<uint32_t>(fy);
    const uint32_t x1 = std::min(x0 + 1, td.header.width - 1);
    const uint32_t y1 = std::min(y0 + 1, td.header.height - 1);

    const double tx = fx - static_cast<double>(x0);
    const double ty = fy - static_cast<double>(y0);

    // TerrainData stores rows north-first (sim convention); ENU y grows
    // northward, so flip.
    const uint32_t sim_y0 = td.header.height - 1 - y0;
    const uint32_t sim_y1 = td.header.height - 1 - y1;

    const double e00 = td.elevation_at(x0, sim_y0);
    const double e10 = td.elevation_at(x1, sim_y0);
    const double e01 = td.elevation_at(x0, sim_y1);
    const double e11 = td.elevation_at(x1, sim_y1);

    const double e0 = e00 + (e10 - e00) * tx;
    const double e1 = e01 + (e11 - e01) * tx;
    return e0 + (e1 - e0) * ty;
}

} // namespace f4::renderer::detail
