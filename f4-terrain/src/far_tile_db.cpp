// f4-terrain/src/far_tile_db.cpp
//
// FarTileDB implementation. See far_tile_db.hpp for the format.

#include <f4/terrain/far_tile_db.hpp>

#include "file_util.hpp"

#include <f4/io/read_file.hpp>

#include <stdexcept>

namespace f4::terrain {

namespace {
// Candidates cover the stock install's quirky casing ("FArtILES") and the
// conventional spelling.
constexpr const char* PAL_NAMES[] = {"FArtILES.PAL", "FARTILES.PAL", "fartiles.pal"};
constexpr const char* RAW_NAMES[] = {"FArtILES.RAW", "FARTILES.RAW", "fartiles.raw"};
} // namespace

bool FarTileDB::load(const std::filesystem::path& texture_dir) {
    raw_.clear();

    std::filesystem::path pal_path, raw_path;
    for (const char* n : PAL_NAMES) {
        pal_path = detail::find_file_ci(texture_dir, n);
        if (!pal_path.empty()) break;
    }
    for (const char* n : RAW_NAMES) {
        raw_path = detail::find_file_ci(texture_dir, n);
        if (!raw_path.empty()) break;
    }
    if (pal_path.empty() || raw_path.empty()) return false;

    // Palette: 256 DWORDs. The DWORDs may carry trailing count fields
    // (per-LOD tile counts appended by composetiles) — informational
    // only; the RAW size is authoritative.
    auto pal = f4::io::read_file(pal_path, "far_tile_db");
    if (pal.size() < 1024)
        throw std::runtime_error("far_tile_db: palette too small (need 1024 bytes)");
    for (int i = 0; i < 256; ++i) {
        const uint32_t b = static_cast<uint32_t>(pal[i * 4]) |
                           (static_cast<uint32_t>(pal[i * 4 + 1]) << 8) |
                           (static_cast<uint32_t>(pal[i * 4 + 2]) << 16);
        palette_[i * 4 + 0] = static_cast<uint8_t>(b & 0xFF);         // r
        palette_[i * 4 + 1] = static_cast<uint8_t>((b >> 8) & 0xFF);  // g
        palette_[i * 4 + 2] = static_cast<uint8_t>((b >> 16) & 0xFF); // b
        palette_[i * 4 + 3] = 0xFF;
    }

    raw_ = f4::io::read_file(raw_path, "far_tile_db");
    if (raw_.size() % TILE_PIXELS != 0)
        throw std::runtime_error("far_tile_db: RAW size not a multiple of tile size");
    return true;
}

uint32_t FarTileDB::tile_count() const noexcept {
    return static_cast<uint32_t>(raw_.size() / TILE_PIXELS);
}

bool FarTileDB::tile_rgba(uint32_t index, std::vector<uint8_t>& out) const {
    if (index >= tile_count()) return false;
    out.resize(TILE_PIXELS * 4);
    const std::size_t base = static_cast<std::size_t>(index) * TILE_PIXELS;
    for (std::size_t i = 0; i < TILE_PIXELS; ++i) {
        const uint8_t pi = raw_[base + i];
        out[i * 4 + 0] = palette_[pi * 4 + 0];
        out[i * 4 + 1] = palette_[pi * 4 + 1];
        out[i * 4 + 2] = palette_[pi * 4 + 2];
        out[i * 4 + 3] = palette_[pi * 4 + 3];
    }
    return true;
}

} // namespace f4::terrain
