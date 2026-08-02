// f4-terrain/include/f4/terrain/terrain_data.hpp
//
// FreeFalcon theater terrain data decoder. Parses the binary terrain files
// that define a theater's elevation, terrain-type palette, and overlays.
//
// Files (from FreeFalcon's TMap class, src/graphics/terrain/tmap.cpp):
//
//   THEATER.MAP  (1100 bytes) — header + color palette
//     [0..3]   uint32  magic           (0x444CFFAE — "LD" + magic bytes)
//     [4..7]   uint32  MEAwidth        (grid columns, e.g. 128)
//     [8..11]  uint32  MEAheight       (grid rows, e.g. 128)
//     [12..15] uint32  FTtoMEAcell     (feet-to-cell conversion factor)
//     [16..]   RGBA    palette[]       (terrain type colors, 271 entries)
//
//   THEATER.MEA  (32768 = 128*128*2 bytes) — elevation grid
//     Int16[width * height] — elevation in feet, vertically flipped
//     (row 0 in file = southernmost row; the renderer flips y to match
//      the sim grid where y increases northward)
//
//   THEATER.O2   (16384 = 128*128*1 bytes) — secondary overlay
//     uint8[width * height] — per-cell overlay (cloud/land mask; semantics
//     not fully documented in FreeFalcon source — we expose the raw values)
//
//   THEATER.L2   (~4 MB) — per-cell texture tile data (not yet decoded;
//     preserved for future passes)
//
// The decoder exposes the typed TerrainData struct, which the visualization
// renders as color-coded tiles. The elevation grid enables hill-shading and
// contour lines in future passes.

#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace f4::terrain {

struct Color4 {
    uint8_t r = 0, g = 0, b = 0, a = 255;
    [[nodiscard]] std::string hex() const;   // "#rrggbb"
};

struct TerrainHeader {
    uint32_t magic = 0;
    uint32_t width = 0;          // grid columns (e.g. 128)
    uint32_t height = 0;         // grid rows (e.g. 128)
    uint32_t ft_to_mea_cell = 0; // feet-to-cell conversion
};

struct TerrainData {
    TerrainHeader header;
    std::vector<Color4> palette;              // from THEATER.MAP
    std::vector<int16_t> elevation;           // width*height, feet (flipped)
    std::vector<uint8_t> overlay;             // width*height, from THEATER.O2

    /// Load from a directory containing THEATER.MAP, .MEA, .O2.
    /// Throws on missing files or parse error.
    void load(const std::filesystem::path& terrain_dir);

    /// Elevation at grid (x, y), where y increases northward (sim convention).
    /// The file stores rows south-first; this accessor handles the flip.
    [[nodiscard]] int16_t elevation_at(uint32_t x, uint32_t y) const;

    /// Overlay byte at grid (x, y), sim convention (y northward).
    [[nodiscard]] uint8_t overlay_at(uint32_t x, uint32_t y) const;

    /// Classify an elevation into a terrain type for color-coding.
    ///   0           = water (ocean)
    ///   1..500      = lowland (green)
    ///   500..2000   = hills (tan)
    ///   2000..4000  = mountains (brown)
    ///   >4000       = peaks (grey/white)
    /// Returns a palette index or a derived color.
    [[nodiscard]] Color4 terrain_color(uint32_t x, uint32_t y) const;
};

} // namespace f4::terrain
