// f4-terrain/src/post_level.cpp
//
// PostLevel implementation. See post_level.hpp for the on-disk format.

#include <f4/terrain/post_level.hpp>

#include <f4/io/cursor.hpp>
#include <f4/io/read_file.hpp>

#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <string>

namespace f4::terrain {

namespace {

// TdiskPost is 7 bytes packed on disk (tdskpost.h, #pragma pack(1)).
constexpr std::size_t POST_BYTES = 7;
constexpr std::size_t POSTS_PER_BLOCK = 256;

using f4::io::Cursor;

std::vector<uint8_t> read_file(const std::filesystem::path& path, const char* label) {
    return f4::io::read_file(path, label);
}

} // namespace

bool PostLevel::load(const std::filesystem::path& terrain_dir, int level,
                     const TheaterGeometry& geometry) {
    level_ = -1;
    offsets_.clear();
    data_.clear();
    blocks_wide_ = 0;
    ft_per_post_ = 0.0;

    if (level < 0 || level > geometry.last_level) return false;

    const std::string suffix = std::to_string(level);
    const std::filesystem::path o_path = terrain_dir / ("THEATER.O" + suffix);
    const std::filesystem::path l_path = terrain_dir / ("THEATER.L" + suffix);

    // Missing files are not an error — the caller degrades to untextured.
    std::error_code ec;
    if (!std::filesystem::exists(o_path, ec) ||
        !std::filesystem::exists(l_path, ec)) {
        return false;
    }

    const uint32_t expected_blocks = geometry.blocks_wide(level);
    if (expected_blocks == 0) return false;

    auto o_data = read_file(o_path, "post_level");
    const std::size_t expected_o_bytes =
        static_cast<std::size_t>(expected_blocks) * expected_blocks * 4;
    if (o_data.size() < expected_o_bytes)
        throw std::runtime_error("THEATER.O" + suffix + ": size mismatch (expected " +
                                 std::to_string(expected_o_bytes) + " bytes of offsets)");

    data_ = read_file(l_path, "post_level");

    // Validate every block offset up front so post() can decode without
    // per-access bounds branches (a bad theater fails loudly at load).
    Cursor oc{o_data.data(), o_data.data() + expected_o_bytes};
    offsets_.reserve(static_cast<std::size_t>(expected_blocks) * expected_blocks);
    for (std::size_t i = 0; i < offsets_.capacity(); ++i) {
        const uint32_t off = oc.u32();
        if (off + POST_BYTES * POSTS_PER_BLOCK > data_.size()) {
            throw std::runtime_error("THEATER.O" + suffix + ": block offset " +
                                     std::to_string(off) + " out of range for THEATER.L" +
                                     suffix + " (" + std::to_string(data_.size()) + " bytes)");
        }
        offsets_.push_back(off);
    }
    oc.check_and_throw("post_level: THEATER.O" + suffix + " truncated");

    level_ = level;
    blocks_wide_ = expected_blocks;
    ft_per_post_ = geometry.ft_per_post(level);
    return true;
}

TerrainPost PostLevel::post(uint32_t col, uint32_t row) const {
    const uint32_t n = posts_wide();
    if (n == 0 || col >= n || row >= n) return TerrainPost{};

    const uint32_t block = (row >> 4) * blocks_wide_ + (col >> 4);
    const std::size_t in_block =
        static_cast<std::size_t>((row & 0xF) << 4 | (col & 0xF));
    const std::size_t at = static_cast<std::size_t>(offsets_[block]) + in_block * POST_BYTES;

    TerrainPost p;
    p.tex_id       = static_cast<uint16_t>(data_[at] | (data_[at + 1] << 8));
    p.elevation_ft = static_cast<int16_t>(data_[at + 2] | (data_[at + 3] << 8));
    p.color        = data_[at + 4];
    p.theta        = data_[at + 5];
    p.phi          = data_[at + 6];
    return p;
}

double PostLevel::elevation_at_ft(double east_ft, double north_ft) const {
    const uint32_t n = posts_wide();
    if (n == 0 || ft_per_post_ <= 0.0) return 0.0;

    // Continuous post coordinates, clamped so the 2x2 sample footprint
    // stays inside the grid (edge behavior matches terrain_mesh.cpp).
    const double max_coord = static_cast<double>(n - 1);
    const double fc = std::clamp(east_ft / ft_per_post_, 0.0, max_coord);
    const double fr = std::clamp(north_ft / ft_per_post_, 0.0, max_coord);

    const uint32_t c0 = static_cast<uint32_t>(fc);
    const uint32_t r0 = static_cast<uint32_t>(fr);
    const uint32_t c1 = std::min(c0 + 1, n - 1);
    const uint32_t r1 = std::min(r0 + 1, n - 1);
    const double tc = fc - static_cast<double>(c0);
    const double tr = fr - static_cast<double>(r0);

    const double e00 = post(c0, r0).elevation_ft;
    const double e10 = post(c1, r0).elevation_ft;
    const double e01 = post(c0, r1).elevation_ft;
    const double e11 = post(c1, r1).elevation_ft;

    const double e0 = e00 + (e10 - e00) * tc;
    const double e1 = e01 + (e11 - e01) * tc;
    return e0 + (e1 - e0) * tr;
}

uint16_t PostLevel::tex_id_at_ft(double east_ft, double north_ft) const {
    const uint32_t n = posts_wide();
    if (n == 0 || ft_per_post_ <= 0.0) return 0xFFFF;

    const double max_coord = static_cast<double>(n - 1);
    const double fc = std::clamp(east_ft / ft_per_post_, 0.0, max_coord);
    const double fr = std::clamp(north_ft / ft_per_post_, 0.0, max_coord);
    return post(static_cast<uint32_t>(fc), static_cast<uint32_t>(fr)).tex_id;
}

} // namespace f4::terrain
