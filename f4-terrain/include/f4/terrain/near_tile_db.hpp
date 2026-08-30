// f4-terrain/include/f4/terrain/near_tile_db.hpp
//
// NearTileDB — the theater's near-tile texture database: TEXTURE.BIN
// (set/tile catalog) plus the tile art (texture.zip or loose files in
// the theater's texture directory).
//
// TEXTURE.BIN layout (FreeFalcon terrtex.cpp, TextureDB::Setup — layout
// verified byte-for-byte against the stock Korea file):
//
//   int32 numSets
//   int32 totalTiles
//   per set:  int32 numTiles; uint8 terrainType (COVERAGE_* class);
//     per tile: char filename[20];       // NUL-padded, e.g. "HCOST00F.pcx"
//               int32 nAreas;
//               int32 nPaths;
//               TexArea areas[nAreas];   // 16 bytes each (skipped in v1)
//               TexPath paths[nPaths];   // 24 bytes each (skipped in v1)
//
// texID packing in near-LOD posts (terrtex.h):
//   set  = (texID >> 4) & 0xFF      // index into the set list
//   tile =  texID        & 0xF      // index into the set's tile list
//   res  = (texID >> 12) & 0xF      // art resolution variant
//
// Art resolution variants: each tile ships as a family whose first
// filename character selects the resolution — 'H' (high), 'M' (medium),
// 'L' (low). FreeFalcon resolves a variant by rewriting the stored
// name's first character (res 1 -> 'M', res 0 -> 'L', anything else ->
// the stored name, which is the high-res art). The stock zip contains
// 1,109 files per prefix (plus a handful of 'S'/'T' specials we don't
// use). Each PCX carries its own 256-color palette; we decode per file
// rather than applying FreeFalcon's "first tile's palette wins per set"
// rule, so colors are correct even if a set's art is inconsistent.
//
// Decoded tiles are cached by (texID) on first fetch. Korea: 1,051
// catalog tiles; art is 64x64-ish PCX, decoded lazily.
//
// C++20.

#pragma once

#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace f4::io { class ZipReader; }   // internal (f4-io is a private dep)

namespace f4::terrain {

struct NearTile {
    std::string name;      ///< As stored in TEXTURE.BIN ("HCOST00F.pcx").
    int32_t n_areas = 0;   ///< TexArea record count (records skipped in v1).
    int32_t n_paths = 0;   ///< TexPath record count (records skipped in v1).
};

struct NearTileSet {
    uint8_t terrain_type = 0;   ///< COVERAGE_* class byte.
    std::vector<NearTile> tiles;
};

struct NearTileImage {
    uint32_t width = 0;
    uint32_t height = 0;
    std::vector<uint8_t> rgba;   ///< width*height*4
};

class NearTileDB {
public:
    ~NearTileDB();   // out-of-line: zip_ holds an incomplete type

    NearTileDB();    // ditto — inline = default would instantiate
                     // ~unique_ptr where ZipReader is incomplete
    NearTileDB(const NearTileDB&) = delete;
    NearTileDB& operator=(const NearTileDB&) = delete;
    NearTileDB(NearTileDB&&) noexcept;
    NearTileDB& operator=(NearTileDB&&) noexcept;

    /// Load TEXTURE.BIN from a theater texture directory and index the
    /// tile art sources (texture.zip when present, plus loose files).
    /// Returns false (no throw) when TEXTURE.BIN is absent; throws on
    /// malformed data.
    bool load(const std::filesystem::path& texture_dir);

    [[nodiscard]] bool loaded() const noexcept { return !sets_.empty(); }

    [[nodiscard]] uint32_t set_count() const noexcept {
        return static_cast<uint32_t>(sets_.size());
    }
    [[nodiscard]] uint32_t tile_count() const noexcept { return total_tiles_; }

    /// Unpack a near texID into (set, tile, res).
    static void unpack_tex_id(uint16_t tex_id, uint32_t& set, uint32_t& tile,
                              uint32_t& res) noexcept {
        set  = (tex_id >> 4) & 0xFF;
        tile = tex_id & 0xF;
        res  = (tex_id >> 12) & 0xF;
    }

    /// Resolve a near texID to its catalog entry, or nullptr when the
    /// set/tile indices are out of range.
    [[nodiscard]] const NearTile* find_tile(uint16_t tex_id) const;

    /// Fetch decoded art for a near texID, selecting the resolution
    /// variant from the texID's res nibble (FreeFalcon's first-character
    /// rewrite rule, with fallback to the stored name when the preferred
    /// variant is missing). Cached (memoized — const; misses cache too).
    /// Returns false when no art resolves.
    [[nodiscard]] bool tile_rgba(uint16_t tex_id, NearTileImage& out) const;

    /// Diagnostic: number of tiles fetched and decoded so far.
    [[nodiscard]] uint32_t cache_size() const noexcept {
        return static_cast<uint32_t>(cache_.size());
    }

private:
    std::vector<NearTileSet> sets_;
    uint32_t total_tiles_ = 0;

    std::unique_ptr<f4::io::ZipReader> zip_;  ///< Null when no/failed zip.
    std::filesystem::path texture_dir_;       ///< For loose-file fallback.
    mutable std::map<uint16_t, NearTileImage> cache_; ///< Memoized by texID.
};

} // namespace f4::terrain
