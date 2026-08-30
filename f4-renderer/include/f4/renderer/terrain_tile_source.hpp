// f4-renderer/include/f4/renderer/terrain_tile_source.hpp
//
// TerrainTileSource — the CPU-side bundle of decoded theater post/tile
// data the textured terrain path needs. Assembled by the app (Phase 3:
// WorldView) from f4-terrain decoders; consumed by the terrain chunk
// builder and the 2D map painter.
//
// All pointers are non-owning; the owner must keep everything alive for
// as long as terrain built from this source is drawn.
//
// C++20.

#pragma once

#include <f4/terrain/far_tile_db.hpp>
#include <f4/terrain/near_tile_db.hpp>
#include <f4/terrain/post_level.hpp>
#include <f4/terrain/theater_geometry.hpp>

namespace f4::renderer {

struct TerrainTileSource {
    /// Theater grid geometry (ENU <-> post conversions).
    const f4::terrain::TheaterGeometry* geometry = nullptr;

    /// Near-textured post level (stock Korea: L0..L2 carry near texIDs;
    /// L2 default, L1 for closer views).
    const f4::terrain::PostLevel* near_level = nullptr;

    /// Far-textured post level (stock Korea: L3..L5 carry far-tile
    /// indices; L4 default).
    const f4::terrain::PostLevel* far_level = nullptr;

    /// Near tile catalog + art (TEXTURE.BIN + texture.zip).
    const f4::terrain::NearTileDB* near_tiles = nullptr;

    /// Far tile database (FArtILES.PAL/.RAW).
    const f4::terrain::FarTileDB* far_tiles = nullptr;

    /// True when every component is present and loaded — the textured
    /// terrain path activates only then (callers fall back to the
    /// vertex-color path otherwise).
    [[nodiscard]] bool usable() const noexcept {
        return geometry != nullptr
            && near_level != nullptr && near_level->loaded()
            && far_level != nullptr && far_level->loaded()
            && near_tiles != nullptr && near_tiles->loaded()
            && far_tiles != nullptr && far_tiles->loaded();
    }
};

} // namespace f4::renderer
