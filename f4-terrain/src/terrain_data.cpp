// f4-terrain/src/terrain_data.cpp

#include <f4/terrain/terrain_data.hpp>

#include <cstdio>
#include <cstring>
#include <sstream>
#include <stdexcept>

namespace f4::terrain {

std::string Color4::hex() const {
    char buf[8];
    std::snprintf(buf, sizeof(buf), "#%02x%02x%02x", r, g, b);
    return buf;
}

namespace {

struct Cursor {
    const uint8_t* p;
    const uint8_t* end;
    void read(void* dst, std::size_t n) {
        if (p + n > end) throw std::runtime_error("terrain: buffer truncated");
        std::memcpy(dst, p, n);
        p += n;
    }
    uint32_t u32() { uint32_t v=0; read(&v,4); return v; }
    uint16_t u16() { uint16_t v=0; read(&v,2); return v; }
    int16_t  i16() { int16_t v=0;  read(&v,2); return v; }
    uint8_t  u8()  { uint8_t v=0;  read(&v,1); return v; }
};

std::vector<uint8_t> read_file(const std::filesystem::path& path) {
    FILE* fp = std::fopen(path.string().c_str(), "rb");
    if (!fp) throw std::runtime_error("terrain: cannot open " + path.string());
    std::fseek(fp, 0, SEEK_END);
    const long sz = std::ftell(fp);
    std::fseek(fp, 0, SEEK_SET);
    std::vector<uint8_t> buf(static_cast<std::size_t>(sz));
    const std::size_t got = std::fread(buf.data(), 1, buf.size(), fp);
    std::fclose(fp);
    if (got != buf.size()) throw std::runtime_error("terrain: short read on " + path.string());
    return buf;
}

} // namespace

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
    }
}

int16_t TerrainData::elevation_at(uint32_t x, uint32_t y) const {
    if (x >= header.width || y >= header.height) return 0;
    return elevation[y * header.width + x];
}

uint8_t TerrainData::overlay_at(uint32_t x, uint32_t y) const {
    if (x >= header.width || y >= header.height) return 0;
    return overlay[y * header.width + x];
}

Color4 TerrainData::terrain_color(uint32_t x, uint32_t y) const {
    const int16_t elev = elevation_at(x, y);
    // Water (elevation <= 0): deep blue, matching the Falcon 4 screenshot.
    if (elev <= 0) return Color4{0x00, 0x69, 0x94, 0xFF};
    // Land: classify by elevation into terrain-type bands.
    // These thresholds approximate the Falcon 4 palette and produce a
    // recognizable Korea landmass. The palette from THEATER.MAP has the
    // exact colors but the index->terrain-type mapping isn't documented;
    // this derived palette is the first-pass visualization.
    if (elev < 500)  return Color4{0xB5, 0xA1, 0x88, 0xFF};  // lowland tan
    if (elev < 1500) return Color4{0x9C, 0x8C, 0x6B, 0xFF};  // hills brown
    if (elev < 3000) return Color4{0x8C, 0x7B, 0x5A, 0xFF};  // mountains
    if (elev < 5000) return Color4{0x6B, 0x5D, 0x4A, 0xFF};  // high mountains
    return Color4{0xE8, 0xE8, 0xE8, 0xFF};                    // peaks (snow)
}

} // namespace f4::terrain
