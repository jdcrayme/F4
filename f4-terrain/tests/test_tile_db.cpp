// test_tile_db.cpp — FarTileDB + NearTileDB against synthetic theaters.

#include <gtest/gtest.h>
#include <f4/terrain/far_tile_db.hpp>
#include <f4/terrain/near_tile_db.hpp>

#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

using namespace f4::terrain;

namespace {

struct TempDir {
    std::filesystem::path path;
    TempDir() {
        path = std::filesystem::temp_directory_path() /
               ("f4_tile_db_" + std::to_string(::rand()));
        std::filesystem::create_directories(path);
    }
    ~TempDir() {
        std::error_code ec;
        std::filesystem::remove_all(path, ec);
    }
};

void write_bytes(const std::filesystem::path& p, const std::vector<uint8_t>& b) {
    std::ofstream f(p, std::ios::binary);
    f.write(reinterpret_cast<const char*>(b.data()),
            static_cast<std::streamsize>(b.size()));
}

void push32(std::vector<uint8_t>& v, uint32_t x) {
    for (int i = 0; i < 4; ++i) v.push_back(static_cast<uint8_t>(x >> (8 * i)));
}

void push16(std::vector<uint8_t>& v, uint16_t x) {
    v.push_back(static_cast<uint8_t>(x & 0xFF));
    v.push_back(static_cast<uint8_t>(x >> 8));
}

/// Minimal 4x4 8-bit PCX, all pixels = `color_index`, palette entry 0
/// forced to (#10,#20,#30) so decodes are distinguishable.
std::vector<uint8_t> make_pcx(uint8_t color_index) {
    std::vector<uint8_t> p(128, 0);
    p[0] = 0x0A; p[1] = 5; p[2] = 1; p[3] = 8;
    p[8] = 3; p[10] = 3;        // xmax=3, ymax=3 -> 4x4
    p[65] = 1;                  // one plane
    p[66] = 4; p[67] = 0;       // bytes per line = 4
    p[68] = 1; p[69] = 0;       // palette info: color
    for (int row = 0; row < 4; ++row) {
        p.push_back(0xC0 | 4);  // RLE run of 4
        p.push_back(color_index);
    }
    p.push_back(0x0C);          // palette marker
    for (int i = 0; i < 256; ++i)
        for (int c : {0x10, 0x20, 0x30}) p.push_back(static_cast<uint8_t>(c));
    return p;
}

struct SyntheticTheater {
    TempDir dir;
    SyntheticTheater() {
        const auto texture = dir.path / "texture";
        std::filesystem::create_directories(texture);

        // --- Far tiles: palette + 2 tiles -----------------------------
        std::vector<uint8_t> pal;
        for (int i = 0; i < 256; ++i) {
            // DWORD = (b << 16) | (g << 8) | r
            push32(pal, (static_cast<uint32_t>(0x30 + i) << 16) |
                            (static_cast<uint32_t>(0x20) << 8) | 0x10u);
        }
        push32(pal, 2);   // trailing count(s), informational
        write_bytes(texture / "fartiles.pal", pal);   // lowercase: ci-match path

        std::vector<uint8_t> raw;
        raw.insert(raw.end(), 1024, 0x05);   // tile 0: index 5
        raw.insert(raw.end(), 1024, 0xFF);   // tile 1: index 255
        write_bytes(texture / "fartiles.raw", raw);

        // --- Near tiles: TEXTURE.BIN with 2 sets ----------------------
        std::vector<uint8_t> bin;
        push32(bin, 2);    // numSets
        push32(bin, 3);    // totalTiles
        // set 0: 2 tiles, terrainType 1
        push32(bin, 2); bin.push_back(1);
        for (const char* name : {"HAAA0000.pcx", "HAAA0001.pcx"}) {
            char field[20] = {};
            std::memcpy(field, name, std::strlen(name));
            for (char c : field) bin.push_back(static_cast<uint8_t>(c));
            push32(bin, 0);   // nAreas
            push32(bin, 0);   // nPaths
        }
        // set 1: 1 tile, terrainType 2
        push32(bin, 1); bin.push_back(2);
        {
            char field[20] = {};
            std::memcpy(field, "HBBB0000.pcx", 12);
            for (char c : field) bin.push_back(static_cast<uint8_t>(c));
            push32(bin, 1);   // nAreas
            push32(bin, 0);   // nPaths
            for (int i = 0; i < 16; ++i) bin.push_back(0);  // TexArea, skipped
        }
        write_bytes(texture / "TEXTURE.BIN", bin);

        // --- Near art as loose PCX files (lowercase names) ------------
        write_bytes(texture / "haaa0000.pcx", make_pcx(7));    // H variant
        write_bytes(texture / "maaa0000.pcx", make_pcx(9));    // M variant
        write_bytes(texture / "lAAA0001.pcx", make_pcx(11));   // L variant (mixed case)
    }
};

} // namespace

// ---------------------------------------------------------------------------
// FarTileDB
// ---------------------------------------------------------------------------
TEST(FarTileDB, LoadsPaletteAndTiles) {
    const SyntheticTheater s;
    FarTileDB db;
    ASSERT_TRUE(db.load(s.dir.path / "texture"));
    EXPECT_TRUE(db.loaded());
    EXPECT_EQ(db.tile_count(), 2u);
    // Palette byte order: r = low byte (0x10), g = 0x20, b = 0x30 + i.
    EXPECT_EQ(db.palette_rgba()[0], 0x10);
    EXPECT_EQ(db.palette_rgba()[1], 0x20);
    EXPECT_EQ(db.palette_rgba()[2], 0x30);
}

TEST(FarTileDB, DecodesTiles) {
    const SyntheticTheater s;
    FarTileDB db;
    ASSERT_TRUE(db.load(s.dir.path / "texture"));
    std::vector<uint8_t> px;
    ASSERT_TRUE(db.tile_rgba(0, px));
    EXPECT_EQ(px.size(), FarTileDB::TILE_PIXELS * 4);
    // Tile 0 is all index 5 -> palette[5] = (0x10, 0x20, 0x30 + 5).
    EXPECT_EQ(px[0], 0x10); EXPECT_EQ(px[1], 0x20); EXPECT_EQ(px[2], 0x35);
    ASSERT_TRUE(db.tile_rgba(1, px));
    // Tile 1 is all index 255 -> palette[255] b wraps: (0x30 + 255) & 0xFF.
    EXPECT_EQ(px[0], 0x10); EXPECT_EQ(px[1], 0x20); EXPECT_EQ(px[2], 0x2F);
    EXPECT_FALSE(db.tile_rgba(2, px));
}

TEST(FarTileDB, MissingFilesReturnFalse) {
    const TempDir d;
    FarTileDB db;
    EXPECT_FALSE(db.load(d.path));
    EXPECT_FALSE(db.loaded());
}

// ---------------------------------------------------------------------------
// NearTileDB
// ---------------------------------------------------------------------------
TEST(NearTileDB, ParsesCatalog) {
    const SyntheticTheater s;
    NearTileDB db;
    ASSERT_TRUE(db.load(s.dir.path / "texture"));
    EXPECT_EQ(db.set_count(), 2u);
    EXPECT_EQ(db.tile_count(), 3u);
    const auto* t0 = db.find_tile(0x0000);   // set 0, tile 0
    ASSERT_NE(t0, nullptr);
    EXPECT_EQ(t0->name, "HAAA0000.pcx");
    const auto* t1 = db.find_tile(0x0001);   // set 0, tile 1
    ASSERT_NE(t1, nullptr);
    EXPECT_EQ(t1->name, "HAAA0001.pcx");
    const auto* t2 = db.find_tile(0x0010);   // set 1, tile 0
    ASSERT_NE(t2, nullptr);
    EXPECT_EQ(t2->name, "HBBB0000.pcx");
    EXPECT_EQ(t2->n_areas, 1);               // TexArea skipped but counted
    EXPECT_EQ(db.find_tile(0x0020), nullptr); // set 2 doesn't exist
}

TEST(NearTileDB, ResolvesStoredNameVariant) {
    const SyntheticTheater s;
    NearTileDB db;
    ASSERT_TRUE(db.load(s.dir.path / "texture"));
    NearTileImage img;
    // res nibble != 0/1 -> stored (H) art: palette forced (0x10,0x20,0x30).
    EXPECT_TRUE(db.tile_rgba(0x2000, img));
    EXPECT_EQ(img.width, 4u);
    EXPECT_EQ(img.height, 4u);
    EXPECT_EQ(img.rgba.size(), 4u * 4u * 4u);
    EXPECT_EQ(img.rgba[0], 0x10);
}

TEST(NearTileDB, ResNibbleSelectsVariant) {
    const SyntheticTheater s;
    NearTileDB db;
    ASSERT_TRUE(db.load(s.dir.path / "texture"));
    NearTileImage img;
    // res 1 -> 'M' variant (maaa0000.pcx). Both PCX palettes are identical
    // in this fixture, so verify via cache distinctness: fetching H then M
    // yields two cache entries and both succeed.
    EXPECT_TRUE(db.tile_rgba(0x2000, img));   // H
    EXPECT_TRUE(db.tile_rgba(0x1000, img));   // M (same set/tile)
    EXPECT_EQ(db.cache_size(), 2u);
}

TEST(NearTileDB, MissingArtCachesNegative) {
    const SyntheticTheater s;
    NearTileDB db;
    ASSERT_TRUE(db.load(s.dir.path / "texture"));
    NearTileImage img;
    // set 0 tile 1 has only the L variant (lAAA0001.pcx); res 2 prefers
    // the stored H name, which doesn't exist -> falls back through the
    // family and finds L.
    EXPECT_TRUE(db.tile_rgba(0x2001, img));
    // set 1 tile 0 has no art at all.
    EXPECT_FALSE(db.tile_rgba(0x2010, img));
    EXPECT_EQ(db.cache_size(), 2u);
}

TEST(NearTileDB, MissingBinReturnsFalse) {
    const TempDir d;
    NearTileDB db;
    EXPECT_FALSE(db.load(d.path));
}

// ---------------------------------------------------------------------------
// TexID packing
// ---------------------------------------------------------------------------
TEST(NearTileDB, TexIdUnpack) {
    struct { uint16_t id; uint32_t set, tile, res; } cases[] = {
        {0x0000, 0, 0, 0}, {0x0001, 0, 1, 0}, {0x0010, 1, 0, 0},
        {0x1234, 0x23, 4, 1}, {0x2000, 0, 0, 2}, {0x2502, 0x50, 2, 2},
    };
    for (const auto& c : cases) {
        uint32_t s, t, r;
        NearTileDB::unpack_tex_id(c.id, s, t, r);
        EXPECT_EQ(s, c.set);
        EXPECT_EQ(t, c.tile);
        EXPECT_EQ(r, c.res);
    }
}
