// f4-terrain/include/f4/terrain/far_tile_db.hpp
//
// FarTileDB — the theater's far-tile texture database (FARTILES.PAL +
// FARTILES.RAW in the theater's texture directory; the stock Korea
// install spells them "FArtILES.PAL"/"FArtILES.RAW").
//
// Far tiles cover the coarse terrain LODs: each tile is a 32x32,
// 8-bit-per-pixel image indexed into ONE 256-color palette shared by the
// whole database (FreeFalcon fartex.cpp / FarTexDB). Tiles are combined
// previews of 2x2 near tiles, produced at theater-build time by
// composetiles. Far-LOD posts (L3..L5 on Korea) store the tile index
// directly in their texID field; 0xFFFF means "no tile".
//
// On-disk format:
//
//   FARTILES.PAL  uint32 palette[256]  (r = b&0xFF, g = (b>>8)&0xFF,
//                                       b = (b>>16)&0xFF)
//                 uint32 tile_count[d] (appended per far LOD by
//                                       composetiles; informational only —
//                                       the count is derived from the RAW
//                                       size, which is authoritative)
//   FARTILES.RAW  tiles * 32*32 bytes of palette indices, row-major from
//                 the top-left pixel of each tile
//
// The RAW data is kept 8-bit in memory (Korea: 53,400 tiles = 54.7 MB);
// callers convert to RGBA lazily for only the tiles they draw.
//
// C++20.

#pragma once

#include <cstdint>
#include <filesystem>
#include <vector>

namespace f4::terrain {

class FarTileDB {
public:
    static constexpr uint32_t TILE_SIZE = 32;   ///< Pixels per side.
    static constexpr std::size_t TILE_PIXELS =
        static_cast<std::size_t>(TILE_SIZE) * TILE_SIZE;

    /// Load PAL+RAW from a theater texture directory (e.g.
    /// .../terrdata/korea/texture). Returns false (no throw) when either
    /// file is missing; throws on malformed data.
    bool load(const std::filesystem::path& texture_dir);

    [[nodiscard]] bool loaded() const noexcept { return !raw_.empty(); }

    /// Number of far tiles in the database.
    [[nodiscard]] uint32_t tile_count() const noexcept;

    /// Decode tile `index` to RGBA (TILE_SIZE*TILE_SIZE*4 bytes, top row
    /// first). Returns false when index is out of range (the caller falls
    /// back to untextured color).
    [[nodiscard]] bool tile_rgba(uint32_t index, std::vector<uint8_t>& out) const;

    /// The shared 256-entry palette as RGBA bytes (1024 bytes).
    [[nodiscard]] const uint8_t* palette_rgba() const noexcept { return palette_; }

private:
    std::vector<uint8_t> raw_;   ///< 8-bit indices, tile_count * 1024 bytes.
    uint8_t palette_[1024] = {}; ///< 256 * RGBA.
};

} // namespace f4::terrain
