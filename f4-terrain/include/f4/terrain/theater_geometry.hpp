// f4-terrain/include/f4/terrain/theater_geometry.hpp
//
// TheaterGeometry — the single source of truth for converting between
// ENU feet (the world coordinate space used by the apps, the sim, and
// the campaign converter) and the terrain's internal grids:
//
//   * the MEA elevation grid (TerrainData, 128x128 for Korea)
//   * the per-LOD post grids (PostLevel, THEATER.L0..L5)
//
// Post-grid layout (from FreeFalcon ttypes.h / TLevel):
//   * The theater is covered by levels L0 (finest) .. L5 (coarsest).
//   * Each level is a grid of 16x16-post blocks; blocks_wide halves per
//     level (Korea: L0 = 256x256 blocks = 4096x4096 posts ... L5 = 8x8
//     blocks = 128x128 posts).
//   * Post spacing doubles per level: ft_per_post(L) = theater_size_ft /
//     posts_wide(L).
//
// Calibration anchor: the coarsest level's post count equals the MEA
// grid dimensions (Korea: 128). This ties the post system onto the
// existing ENU convention without disturbing world coordinates:
//
//   post (col=0, row=0)  <-> ENU (0, 0)   (SW corner of the theater)
//   col increases eastward, row increases northward.
//
// NOTE on scale: the repo convention is theater = 1024x1024 grid units
// x 1024 ft = 1,048,576 ft per side (see terrain_adapter.hpp). The real
// Korea theater is 3,358,720 ft wide (THEATER.MAP's FTtoMEAcell implies
// 26,240 ft per MEA cell). The two disagree by ~3.2x; the repo value is
// kept for internal consistency (campaign coordinates, terrain meshes,
// and airfield layouts all use it). Correcting it is a separate effort;
// until then this constant is the ONE place to change it.
//
// Zero dependencies. C++20.

#pragma once

#include <cstdint>

namespace f4::terrain {

/// Theater grid geometry + coordinate conversion. Cheap value type.
struct TheaterGeometry {
    /// Theater extent per side in ENU feet (repo convention; see header).
    double theater_size_ft = 1024.0 * 1024.0;

    /// Posts across the coarsest post level (equals the MEA grid width;
    /// 128 for Korea).
    uint32_t coarse_posts = 128;

    /// Finest-to-coarsest post levels are L0..last_level (5 for stock
    /// theaters: THEATER.L0..L5).
    int last_level = 5;

    /// Posts across level L (corner-counted): coarse_posts << (last_level - L).
    [[nodiscard]] uint32_t posts_wide(int level) const {
        if (level < 0 || level > last_level) return 0;
        return coarse_posts << (last_level - level);
    }

    /// 16x16-post blocks across level L.
    [[nodiscard]] uint32_t blocks_wide(int level) const {
        return posts_wide(level) >> 4;
    }

    /// Feet between adjacent posts at level L.
    [[nodiscard]] double ft_per_post(int level) const {
        const uint32_t n = posts_wide(level);
        return n ? theater_size_ft / static_cast<double>(n) : 0.0;
    }

    /// ENU east feet -> continuous post column (unclamped; caller clamps).
    [[nodiscard]] double post_col(int level, double east_ft) const {
        const double s = ft_per_post(level);
        return s > 0.0 ? east_ft / s : 0.0;
    }

    /// ENU north feet -> continuous post row (unclamped; row 0 = south).
    [[nodiscard]] double post_row(int level, double north_ft) const {
        const double s = ft_per_post(level);
        return s > 0.0 ? north_ft / s : 0.0;
    }

    /// Post column -> ENU east feet (west edge of the post cell).
    [[nodiscard]] double east_ft(int level, double col) const {
        return col * ft_per_post(level);
    }

    /// Post row -> ENU north feet (south edge of the post cell).
    [[nodiscard]] double north_ft(int level, double row) const {
        return row * ft_per_post(level);
    }

    /// Geometry for the stock Korea theater.
    [[nodiscard]] static TheaterGeometry korea() {
        return TheaterGeometry{1024.0 * 1024.0, 128, 5};
    }
};

} // namespace f4::terrain
