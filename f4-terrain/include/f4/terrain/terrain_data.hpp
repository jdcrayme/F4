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
//   THEATER.L0..L5  (~74 MB total) — per-LOD post data (not yet decoded;
//     preserved for future passes; see FreeFalcon's TdiskPost struct in
//     src/graphics/include/tdskpost.h)
//
// The decoder exposes the typed TerrainData struct. Two serialization
// formats are supported:
//   - load(terrain_dir)   : parse raw binary THEATER.* files
//   - load_terrain_json()  : parse the intermediate JSON produced by
//                            f4-terrain-convert (preferred for runtime —
//                            sim code never sees binary formats)

#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace f4::terrain {

/// THEATER.MAP file magic (bytes on disk in little-endian: AE FF 4C 44).
/// Documented in the header comment above; validated by load(). Exposed
/// publicly so consumers (hex inspector, tests, future converters) all
/// agree on a single source of truth instead of repeating the literal.
constexpr uint32_t THEATER_MAP_MAGIC = 0x444CFFAEu;

/// Coarse terrain classification used for color-coding and (future) movement
/// costs. Mirrors the bands used by FreeFalcon's renderer for fallback
/// shading when textures aren't loaded. Sufficient for strategic-map
/// visualization; a richer COVERAGE_* enum will replace this when we parse
/// THEATER.L* tile data.
enum class TileType : uint8_t {
    Water      = 0,   // elevation <= 0 (ocean/lake)
    Lowland    = 1,   // 0..500 ft
    Hills      = 2,   // 500..1500 ft
    Mountains  = 3,   // 1500..3000 ft
    HighMtn    = 4,   // 3000..5000 ft
    Peaks      = 5,   // > 5000 ft (snow)
};

[[nodiscard]] inline const char* tile_type_name(TileType t) noexcept {
    switch (t) {
        case TileType::Water:     return "water";
        case TileType::Lowland:   return "lowland";
        case TileType::Hills:     return "hills";
        case TileType::Mountains: return "mountains";
        case TileType::HighMtn:   return "high_mtn";
        case TileType::Peaks:     return "peaks";
    }
    return "unknown";
}

[[nodiscard]] inline TileType tile_type_from_elevation(int16_t elev_ft) noexcept {
    if (elev_ft <= 0)    return TileType::Water;
    if (elev_ft < 500)   return TileType::Lowland;
    if (elev_ft < 1500)  return TileType::Hills;
    if (elev_ft < 3000)  return TileType::Mountains;
    if (elev_ft < 5000)  return TileType::HighMtn;
    return TileType::Peaks;
}

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
    std::vector<uint8_t> tile_types;          // width*height, TileType values

    /// Load from a directory containing THEATER.MAP, .MEA, .O2.
    /// Throws on missing files or parse error. Computes tile_types from
    /// elevation via tile_type_from_elevation().
    void load(const std::filesystem::path& terrain_dir);

    /// Load from a terrain JSON file produced by f4-terrain-convert.
    /// Throws on I/O or parse error. Only width/height/tile_types (and
    /// optionally elevation) are read — palette/overlay are not in the JSON.
    void load_terrain_json(const std::filesystem::path& json_path);

    /// Load from an in-memory JSON string (for testing and direct embedding).
    void load_terrain_json_from_string(const std::string& json);

    /// Serialize to the terrain JSON format. Includes width, height, an
    /// optional theater name, the tile_types array (as numbers), and (if
    /// present) the elevation array. The JSON is human-readable for easy
    /// diffing in git.
    [[nodiscard]] std::string to_terrain_json(const std::string& theater_name = "") const;

    /// Write the terrain JSON to a file. Convenience wrapper around
    /// to_terrain_json().
    void save_terrain_json(const std::filesystem::path& json_path,
                           const std::string& theater_name = "") const;

    /// Elevation at grid (x, y), where y increases northward (sim convention).
    /// The file stores rows south-first; this accessor handles the flip.
    [[nodiscard]] int16_t elevation_at(uint32_t x, uint32_t y) const;

    /// Overlay byte at grid (x, y), sim convention (y northward).
    [[nodiscard]] uint8_t overlay_at(uint32_t x, uint32_t y) const;

    /// Tile type at grid (x, y), sim convention (y northward).
    [[nodiscard]] TileType tile_type_at(uint32_t x, uint32_t y) const;

    /// Classify an elevation into a terrain type for color-coding.
    /// Returns a palette index or a derived color.
    [[nodiscard]] Color4 terrain_color(uint32_t x, uint32_t y) const;

    /// Default RGB color for a tile type (used when no palette is loaded).
    [[nodiscard]] static Color4 color_for_tile_type(TileType t);
};

} // namespace f4::terrain
