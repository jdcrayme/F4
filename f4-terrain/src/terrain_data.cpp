// f4-terrain/src/terrain_data.cpp
//
// Uses f4-json's dependency-free Reader and Writer for the terrain JSON
// intermediate format. The field parsers below are unchanged from the
// original implementation; only the local Reader/Writer classes
// have been replaced with #include <f4/json/f4_json.hpp>.

#include <f4/terrain/terrain_data.hpp>

#include <f4/io/cursor.hpp>
#include <f4/io/read_file.hpp>
#include <f4/json/f4_json.hpp>

#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <stdexcept>

namespace f4::terrain {

std::string Color4::hex() const {
    char buf[8];
    std::snprintf(buf, sizeof(buf), "#%02x%02x%02x", r, g, b);
    return buf;
}

namespace {

using f4::io::Cursor;
using f4::json::Reader;
using f4::json::Writer;

// Thin wrapper around f4::io::read_file that preserves the historical
// "terrain:" diagnostic prefix.
std::vector<uint8_t> read_file(const std::filesystem::path& path) {
    return f4::io::read_file(path, "terrain");
}

} // namespace

// ---------------------------------------------------------------------------
// Binary load (THEATER.* files)
// ---------------------------------------------------------------------------
void TerrainData::load(const std::filesystem::path& terrain_dir) {
    // --- THEATER.MAP: header + palette ---
    auto map_data = read_file(terrain_dir / "THEATER.MAP");
    if (map_data.size() < 16) throw std::runtime_error("THEATER.MAP: too small for header");
    Cursor mc{map_data.data(), map_data.data() + map_data.size()};
    header.magic         = mc.u32();
    header.width         = mc.u32();
    header.height        = mc.u32();
    header.ft_to_mea_cell= mc.u32();
    if (header.width == 0 || header.height == 0 || header.width > 4096 || header.height > 4096)
        throw std::runtime_error("THEATER.MAP: implausible grid dimensions");
    // Remaining bytes = RGBA palette.
    const std::size_t pal_bytes = map_data.size() - 16;
    const std::size_t pal_count = pal_bytes / 4;
    palette.reserve(pal_count);
    for (std::size_t i = 0; i < pal_count; ++i) {
        Color4 c;
        c.r = mc.u8(); c.g = mc.u8(); c.b = mc.u8(); c.a = mc.u8();
        palette.push_back(c);
    }
    if (mc.error) throw std::runtime_error("terrain: buffer truncated");

    // --- THEATER.MEA: elevation grid (Int16, vertically flipped) ---
    auto mea_data = read_file(terrain_dir / "THEATER.MEA");
    const std::size_t expected = static_cast<std::size_t>(header.width) * header.height * 2;
    if (mea_data.size() < expected)
        throw std::runtime_error("THEATER.MEA: size mismatch");
    Cursor ec{mea_data.data(), mea_data.data() + expected};
    elevation.resize(static_cast<std::size_t>(header.width) * header.height);
    // FreeFalcon reads rows from MEAheight-1 down to 0 (vertical flip).
    // File row 0 = southernmost; sim y=0 = northernmost. We store in sim
    // convention: elevation[y*width + x] where y=0 is north.
    for (uint32_t file_row = 0; file_row < header.height; ++file_row) {
        const uint32_t sim_y = header.height - 1 - file_row;
        for (uint32_t x = 0; x < header.width; ++x) {
            elevation[sim_y * header.width + x] = ec.i16();
        }
    }
    if (ec.error) throw std::runtime_error("terrain: buffer truncated");

    // --- THEATER.O2: overlay (uint8, same flip) ---
    auto o2_data = read_file(terrain_dir / "THEATER.O2");
    const std::size_t o2_expected = static_cast<std::size_t>(header.width) * header.height;
    if (o2_data.size() >= o2_expected) {
        Cursor oc{o2_data.data(), o2_data.data() + o2_expected};
        overlay.resize(o2_expected);
        for (uint32_t file_row = 0; file_row < header.height; ++file_row) {
            const uint32_t sim_y = header.height - 1 - file_row;
            for (uint32_t x = 0; x < header.width; ++x) {
                overlay[sim_y * header.width + x] = oc.u8();
            }
        }
        if (oc.error) throw std::runtime_error("terrain: buffer truncated");
    }

    // --- Derive tile_types from elevation ---
    tile_types.resize(elevation.size());
    for (std::size_t i = 0; i < elevation.size(); ++i) {
        tile_types[i] = static_cast<uint8_t>(tile_type_from_elevation(elevation[i]));
    }
}

// ---------------------------------------------------------------------------
// JSON load (intermediate format produced by f4-terrain-convert)
// ---------------------------------------------------------------------------
void TerrainData::load_terrain_json_from_string(const std::string& json) {
    Reader r(json);
    r.skip_ws();
    r.expect('{');
    if (r.consume('}')) return;

    // We collect fields by walking the top-level object. Both tile_types
    // and elevations are arrays of integers — we detect which is which by
    // key name.
    for (;;) {
        std::string key = r.read_string();
        r.expect(':');

        if (key == "width") {
            header.width = static_cast<uint32_t>(r.read_int());
        } else if (key == "height") {
            header.height = static_cast<uint32_t>(r.read_int());
        } else if (key == "theater" || key == "tile_size_feet" ||
                   key == "origin_lat_rad" || key == "origin_lon_rad") {
            // Strings (theater) or floats — read and skip.
            r.skip_value();
        } else if (key == "tile_types" || key == "elevations_ft") {
            // Array of integers.
            r.skip_ws();
            r.expect('[');
            std::vector<int> tmp;
            if (!r.peek(']')) for (;;) {
                tmp.push_back(static_cast<int>(r.read_int()));
                if (r.consume(']')) break;
                r.expect(',');
            }
            if (key == "tile_types") {
                tile_types.clear();
                tile_types.reserve(tmp.size());
                for (int v : tmp) tile_types.push_back(static_cast<uint8_t>(v));
            } else {
                elevation.clear();
                elevation.reserve(tmp.size());
                for (int v : tmp) elevation.push_back(static_cast<int16_t>(v));
            }
        } else {
            r.skip_value();
        }

        if (r.consume('}')) break;
        r.expect(',');
    }

    if (header.width == 0 || header.height == 0)
        throw std::runtime_error("terrain JSON: missing width/height");
    if (tile_types.empty())
        throw std::runtime_error("terrain JSON: missing tile_types array");
    const std::size_t expected_count =
        static_cast<std::size_t>(header.width) * header.height;
    if (tile_types.size() != expected_count)
        throw std::runtime_error("terrain JSON: tile_types length mismatch");
    // If elevation wasn't included, leave it empty (viewer doesn't need it
    // for tile rendering; only the binary loader populates it).
}

void TerrainData::load_terrain_json(const std::filesystem::path& json_path) {
    std::ifstream f(json_path);
    if (!f) throw std::runtime_error("terrain: cannot open " + json_path.string());
    std::ostringstream ss;
    ss << f.rdbuf();
    load_terrain_json_from_string(ss.str());
}

// ---------------------------------------------------------------------------
// JSON save (to_terrain_json / save_terrain_json)
// ---------------------------------------------------------------------------
std::string TerrainData::to_terrain_json(const std::string& theater_name) const {
    Writer w;
    w.raw("{\n");

    if (!theater_name.empty()) {
        w.raw("  \"theater\": "); w.string(theater_name); w.raw(",\n");
    }
    w.raw("  \"width\": ");  w.number(header.width);  w.raw(",\n");
    w.raw("  \"height\": "); w.number(header.height); w.raw(",\n");

    // Tile types: one integer per cell, row-major (y=0 north, x=0 west).
    // Emitted as a flat JSON array — compact but still diff-friendly at
    // 16,384 entries (~50 KB). For multi-MB L-file grids we'll switch to
    // a binary sidecar; the JSON shape stays the same.
    w.raw("  \"tile_types\": [");
    const std::size_t n = tile_types.size();
    for (std::size_t i = 0; i < n; ++i) {
        if (i) w.raw(",");
        w.number(tile_types[i]);
    }
    w.raw("]");

    // Elevation is optional in the JSON — only emit if populated (i.e.
    // loaded from binary). When loading back, the viewer can reconstruct
    // tile colors from tile_types alone.
    if (!elevation.empty()) {
        w.raw(",\n  \"elevations_ft\": [");
        for (std::size_t i = 0; i < elevation.size(); ++i) {
            if (i) w.raw(",");
            w.number(elevation[i]);
        }
        w.raw("]");
    }

    w.raw("\n}\n");
    return w.str();
}

void TerrainData::save_terrain_json(const std::filesystem::path& json_path,
                                    const std::string& theater_name) const {
    std::ofstream f(json_path);
    if (!f) throw std::runtime_error("terrain: cannot write " + json_path.string());
    f << to_terrain_json(theater_name);
}

// ---------------------------------------------------------------------------
// Accessors
// ---------------------------------------------------------------------------
int16_t TerrainData::elevation_at(uint32_t x, uint32_t y) const {
    if (x >= header.width || y >= header.height) return 0;
    return elevation[y * header.width + x];
}

uint8_t TerrainData::overlay_at(uint32_t x, uint32_t y) const {
    if (x >= header.width || y >= header.height) return 0;
    return overlay[y * header.width + x];
}

TileType TerrainData::tile_type_at(uint32_t x, uint32_t y) const {
    if (x >= header.width || y >= header.height) return TileType::Water;
    if (tile_types.empty()) return TileType::Water;
    return static_cast<TileType>(tile_types[y * header.width + x]);
}

Color4 TerrainData::terrain_color(uint32_t x, uint32_t y) const {
    return color_for_tile_type(tile_type_at(x, y));
}

Color4 TerrainData::color_for_tile_type(TileType t) {
    switch (t) {
        case TileType::Water:     return Color4{0x00, 0x69, 0x94, 0xFF}; // deep blue
        case TileType::Lowland:   return Color4{0xB5, 0xA1, 0x88, 0xFF}; // tan
        case TileType::Hills:     return Color4{0x9C, 0x8C, 0x6B, 0xFF}; // brown
        case TileType::Mountains: return Color4{0x8C, 0x7B, 0x5A, 0xFF}; // dark brown
        case TileType::HighMtn:   return Color4{0x6B, 0x5D, 0x4A, 0xFF}; // darker
        case TileType::Peaks:     return Color4{0xE8, 0xE8, 0xE8, 0xFF}; // snow
    }
    return Color4{0, 0, 0, 0xFF};
}

} // namespace f4::terrain
