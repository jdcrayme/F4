// f4-terrain/include/f4/terrain/post_level.hpp
//
// PostLevel — decodes one Falcon 4 terrain LOD level (THEATER.O<N> +
// THEATER.L<N>) into addressable per-post records.
//
// On-disk format (FreeFalcon src/graphics/terrain, tlevel.cpp/tdskpost.cpp):
//
//   THEATER.O<N>  — array of uint32 byte-offsets, one per block
//                   (blocks_wide * blocks_wide entries, row-major from the
//                   SW corner). Entry i gives the byte offset of block i's
//                   256 posts inside THEATER.L<N>. Identical blocks are
//                   deduplicated by the theater build tools (mapdice), so
//                   many entries share offsets and the L file is smaller
//                   than blocks*256*7.
//
//   THEATER.L<N>  — raw concatenated post blocks, no file header. Each
//                   post is TdiskPost, 7 bytes packed:
//
//                     uint16 texID;  // near LODs: set/tile/res packed;
//                                    // far LODs: index into the far-tile
//                                    // RAW (0xFFFF = no tile)
//                     int16  z;      // elevation, feet MSL (positive up)
//                     uint8  color;  // index into THEATER.MAP's palette
//                     uint8  theta;  // surface normal azimuth 0..255 -> 0..2pi
//                     uint8  phi;    // surface normal elevation 0..63 -> 0..pi/2
//
//                   Posts inside a block are row-major from the block's SW
//                   corner: index = row * 16 + col.
//
// Row/column convention: row 0 = SOUTH edge, col 0 = WEST edge, both
// increasing ENU-natural (north / east). This differs from TerrainData's
// sim convention (y = 0 = north) — PostLevel does NOT flip.
//
// Memory model: the L file is kept raw (f4-terrain stays a data library;
// a whole-file load is L0 = 56 MB, L2 = 4 MB) and posts are decoded on
// access through the O-file offset table. No paging in v1 — load only
// the levels a view needs.
//
// C++20.

#pragma once

#include <f4/terrain/theater_geometry.hpp>

#include <cmath>
#include <cstdint>
#include <filesystem>
#include <vector>

namespace f4::terrain {

/// One decoded terrain post (values exactly as on disk, plus helpers for
/// the packed normal).
struct TerrainPost {
    uint16_t tex_id = 0xFFFF;   ///< Near: set/tile/res. Far: raw index. 0xFFFF = none.
    int16_t  elevation_ft = 0;  ///< Feet MSL, positive up.
    uint8_t  color = 0;         ///< THEATER.MAP palette index.
    uint8_t  theta = 0;         ///< Normal azimuth 0..255 -> 0..2*pi.
    uint8_t  phi = 0;           ///< Normal elevation 0..63 -> 0..pi/2.

    /// True when the post carries no usable tile reference.
    [[nodiscard]] bool has_no_tile() const noexcept { return tex_id == 0xFFFF; }

    /// Decoded surface normal in ENU (unit length; theta measured from
    /// east toward north). FreeFalcon's lighting used this encoding.
    [[nodiscard]] void normal_enu(float& nx, float& ny, float& nz) const noexcept {
        const float az = static_cast<float>(theta) * (6.2831853f / 255.99f);
        const float el = static_cast<float>(phi)   * (1.5707963f / 63.99f);
        const float c = std::cos(el);
        nx = std::cos(az) * c;
        ny = std::sin(az) * c;
        nz = std::sin(el);
    }
};

/// One loaded LOD level. Immutable after load().
class PostLevel {
public:
    /// Load THEATER.O<level> + THEATER.L<level> from a theater's terrain
    /// directory (e.g. .../terrdata/korea/terrain).
    ///
    /// Returns false (no throw) when either file is absent — callers use
    /// this to degrade to untextured terrain. Throws std::runtime_error
    /// when a file exists but is malformed (bad dims, offsets out of
    /// range, truncated).
    bool load(const std::filesystem::path& terrain_dir, int level,
              const TheaterGeometry& geometry);

    [[nodiscard]] bool loaded() const noexcept { return level_ >= 0; }
    [[nodiscard]] int level() const noexcept { return level_; }
    [[nodiscard]] uint32_t posts_wide() const noexcept { return blocks_wide_ * 16u; }
    [[nodiscard]] uint32_t blocks_wide() const noexcept { return blocks_wide_; }

    /// Post at (col, row). col: 0 = west .. posts_wide-1; row: 0 = south.
    /// Out-of-range coordinates return a zero post with tex_id = 0xFFFF.
    [[nodiscard]] TerrainPost post(uint32_t col, uint32_t row) const;

    /// Bilinearly interpolated elevation at ENU feet (clamped at edges).
    [[nodiscard]] double elevation_at_ft(double east_ft, double north_ft) const;

    /// texID of the SW post of the quad containing ENU feet (clamped).
    /// This mirrors FreeFalcon's DrawTerrainSquare, which takes the
    /// texture from the quad's SW corner post.
    [[nodiscard]] uint16_t tex_id_at_ft(double east_ft, double north_ft) const;

private:
    int level_ = -1;
    uint32_t blocks_wide_ = 0;
    double ft_per_post_ = 0.0;
    std::vector<uint8_t> data_;          ///< Raw THEATER.L<N> bytes.
    std::vector<uint32_t> offsets_;      ///< THEATER.O<N> block offsets into data_.
};

} // namespace f4::terrain
