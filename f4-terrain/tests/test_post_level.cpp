// test_post_level.cpp — PostLevel + TheaterGeometry tests.
//
// Synthetic O/L fixtures are written to a temp directory (tiny geometry),
// plus integration against the real THEATER.L2/O2 fixtures.

#include <gtest/gtest.h>
#include <f4/terrain/post_level.hpp>
#include <f4/terrain/theater_geometry.hpp>

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <vector>

using namespace f4::terrain;

namespace {

struct TempDir {
    std::filesystem::path path;
    TempDir() {
        path = std::filesystem::temp_directory_path() /
               ("f4_post_level_" + std::to_string(::rand()));
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

/// Build a 2-blocks-wide L1 (32 posts wide) with three unique blocks;
/// block (1,0) duplicates block (0,0)'s storage like mapdice's dedup.
struct SyntheticL1 {
    TheaterGeometry geom{4096.0, 32, 1};   // L1: 32 posts, 2 blocks, 128 ft/post
    TempDir dir;

    static constexpr uint16_t T0_TEX = 0x1234;

    SyntheticL1() {
        constexpr std::size_t BLOCK = 256 * 7;
        std::vector<uint8_t> l;

        auto make_block = [&](int16_t base_z, bool special_first) {
            std::vector<uint8_t> b(BLOCK);
            for (std::size_t i = 0; i < 256; ++i) {
                const std::size_t at = i * 7;
                uint16_t tex = static_cast<uint16_t>(0x0005 + (base_z >> 8));
                int16_t z = static_cast<int16_t>(base_z + static_cast<int16_t>(i));
                uint8_t col = 1, th = 2, ph = 3;
                if (special_first && i == 0) {
                    tex = T0_TEX; z = 42; col = 7; th = 8; ph = 9;
                }
                std::memcpy(&b[at], &tex, 2);
                std::memcpy(&b[at + 2], &z, 2);
                b[at + 4] = col; b[at + 5] = th; b[at + 6] = ph;
            }
            return b;
        };

        const auto b00 = make_block(100, true);
        const auto b01 = make_block(300, false);
        const auto b11 = make_block(500, false);
        // Block (0,1) (row 1, col 0) uses z=300; (1,1) z=500.
        // O order is row-major from SW: (0,0), (1,0), (0,1), (1,1).
        const uint32_t off00 = 0;
        const uint32_t off01 = static_cast<uint32_t>(b00.size());
        const uint32_t off11 = static_cast<uint32_t>(b00.size() + b01.size());
        l.insert(l.end(), b00.begin(), b00.end());
        l.insert(l.end(), b01.begin(), b01.end());
        l.insert(l.end(), b11.begin(), b11.end());

        std::vector<uint8_t> o;
        for (uint32_t v : {off00, off00, off01, off11}) {  // (1,0) dedups to (0,0)
            o.push_back(v & 0xFF); o.push_back((v >> 8) & 0xFF);
            o.push_back((v >> 16) & 0xFF); o.push_back((v >> 24) & 0xFF);
        }
        write_bytes(dir.path / "THEATER.O1", o);
        write_bytes(dir.path / "THEATER.L1", l);
    }
};

} // namespace

// ---------------------------------------------------------------------------
// TheaterGeometry
// ---------------------------------------------------------------------------
TEST(TheaterGeometry, KoreaPostCounts) {
    const auto g = TheaterGeometry::korea();
    EXPECT_EQ(g.posts_wide(0), 4096u);
    EXPECT_EQ(g.posts_wide(1), 2048u);
    EXPECT_EQ(g.posts_wide(2), 1024u);
    EXPECT_EQ(g.posts_wide(5), 128u);
    EXPECT_EQ(g.blocks_wide(0), 256u);
    EXPECT_EQ(g.blocks_wide(5), 8u);
    // Coarsest posts == MEA cells; ft per L5 post == ft per MEA cell in
    // the repo convention (1,048,576 / 128 = 8192).
    EXPECT_DOUBLE_EQ(g.ft_per_post(5), 8192.0);
    EXPECT_DOUBLE_EQ(g.ft_per_post(0), 1048576.0 / 4096.0);
}

TEST(TheaterGeometry, FtPostRoundTrip) {
    const auto g = TheaterGeometry::korea();
    EXPECT_DOUBLE_EQ(g.post_col(2, 2.0 * g.ft_per_post(2)), 2.0);
    EXPECT_DOUBLE_EQ(g.east_ft(5, 3.0), 3.0 * g.ft_per_post(5));
    EXPECT_DOUBLE_EQ(g.north_ft(5, 7.0), 7.0 * g.ft_per_post(5));
}

// ---------------------------------------------------------------------------
// PostLevel — synthetic
// ---------------------------------------------------------------------------
TEST(PostLevel, LoadsSyntheticLevel) {
    const SyntheticL1 s;
    PostLevel pl;
    ASSERT_TRUE(pl.load(s.dir.path, 1, s.geom));
    EXPECT_TRUE(pl.loaded());
    EXPECT_EQ(pl.posts_wide(), 32u);
    EXPECT_EQ(pl.blocks_wide(), 2u);
}

TEST(PostLevel, DecodesPostFields) {
    const SyntheticL1 s;
    PostLevel pl;
    ASSERT_TRUE(pl.load(s.dir.path, 1, s.geom));
    const auto p = pl.post(0, 0);
    EXPECT_EQ(p.tex_id, SyntheticL1::T0_TEX);
    EXPECT_EQ(p.elevation_ft, 42);
    EXPECT_EQ(p.color, 7);
    EXPECT_EQ(p.theta, 8);
    EXPECT_EQ(p.phi, 9);
}

TEST(PostLevel, DedupedBlockSharesStorage) {
    const SyntheticL1 s;
    PostLevel pl;
    ASSERT_TRUE(pl.load(s.dir.path, 1, s.geom));
    // Block (1,0) shares block (0,0)'s bytes: post (16,0) must equal (0,0).
    EXPECT_EQ(pl.post(16, 0).elevation_ft, pl.post(0, 0).elevation_ft);
    EXPECT_EQ(pl.post(16, 0).tex_id, pl.post(0, 0).tex_id);
    // Row-major-from-SW block order: block (0,1) (north half) uses z=300.
    EXPECT_EQ(pl.post(0, 16).elevation_ft, 300);
    EXPECT_EQ(pl.post(16, 16).elevation_ft, 500);
}

TEST(PostLevel, ElevationBilinearInFt) {
    const SyntheticL1 s;
    PostLevel pl;
    ASSERT_TRUE(pl.load(s.dir.path, 1, s.geom));
    const double s_ft = s.geom.ft_per_post(1);   // 128
    // Post (0,0) is the special z=42 post; its east neighbor is the
    // block pattern value 100 + 1 = 101.
    EXPECT_DOUBLE_EQ(pl.elevation_at_ft(0.0, 0.0), 42.0);
    EXPECT_DOUBLE_EQ(pl.elevation_at_ft(s_ft, 0.0), 101.0);
    EXPECT_DOUBLE_EQ(pl.elevation_at_ft(0.5 * s_ft, 0.0), 71.5);
}

TEST(PostLevel, TexIdSamplesSwPost) {
    const SyntheticL1 s;
    PostLevel pl;
    ASSERT_TRUE(pl.load(s.dir.path, 1, s.geom));
    EXPECT_EQ(pl.tex_id_at_ft(0.0, 0.0), SyntheticL1::T0_TEX);
    // Mid-quad still returns the SW post's texID.
    EXPECT_EQ(pl.tex_id_at_ft(64.0, 64.0), SyntheticL1::T0_TEX);
}

TEST(PostLevel, MissingFilesReturnFalse) {
    const TempDir d;
    PostLevel pl;
    EXPECT_FALSE(pl.load(d.path, 2, TheaterGeometry::korea()));
    EXPECT_FALSE(pl.loaded());
}

TEST(PostLevel, OutOfRangeOffsetThrows) {
    const SyntheticL1 s;
    // Corrupt the first O1 offset to point past the end of the L file;
    // load() must reject the whole level loudly.
    std::vector<uint8_t> o(16);
    {
        std::ifstream f(s.dir.path / "THEATER.O1", std::ios::binary);
        f.read(reinterpret_cast<char*>(o.data()), 16);
    }
    const uint32_t bad = 0x7FFFFFFFu;
    std::memcpy(o.data(), &bad, 4);
    write_bytes(s.dir.path / "THEATER.O1", o);
    PostLevel pl;
    EXPECT_THROW(pl.load(s.dir.path, 1, s.geom), std::runtime_error);
}

// ---------------------------------------------------------------------------
// PostLevel — real Korea L2 fixture
// ---------------------------------------------------------------------------
TEST(PostLevel, KoreaL2Fixture) {
    PostLevel pl;
    ASSERT_TRUE(pl.load(TERRAIN_FIXTURE_DIR, 2, TheaterGeometry::korea()));
    EXPECT_EQ(pl.posts_wide(), 1024u);
    EXPECT_EQ(pl.blocks_wide(), 64u);

    // Sparse sample (every 16th post, matching dump-terrain-textures):
    // elevation range matches Korea (sea level to ~6.5k ft) and
    // near-packed texIDs reference valid NearTileDB sets (< 110).
    int16_t zmin = 30000, zmax = -30000;
    for (uint32_t r = 0; r < 1024; r += 16)
    for (uint32_t c = 0; c < 1024; c += 16) {
        const auto p = pl.post(c, r);
        zmin = std::min(zmin, p.elevation_ft);
        zmax = std::max(zmax, p.elevation_ft);
        EXPECT_LT(((p.tex_id >> 4) & 0xFF), 110u) << "near texID set out of range";
        EXPECT_NE(p.tex_id, 0xFFFFu);
    }
    EXPECT_LE(zmin, 0);
    EXPECT_GT(zmax, 6000);
    EXPECT_LT(zmax, 7000);

    // Theater-center elevation — cross-validated against all other LODs
    // on the real install (dump-terrain-textures).
    EXPECT_NEAR(pl.elevation_at_ft(524288.0, 524288.0), 551.0, 100.0);
}
