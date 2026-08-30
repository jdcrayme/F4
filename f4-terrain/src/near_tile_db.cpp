// f4-terrain/src/near_tile_db.cpp
//
// NearTileDB implementation. See near_tile_db.hpp for the formats.

#include <f4/terrain/near_tile_db.hpp>

#include "file_util.hpp"
#include "pcx_reader.hpp"

#include <f4/io/cursor.hpp>
#include <f4/io/read_file.hpp>
#include <f4/io/zip_reader.hpp>

#include <stdexcept>
#include <utility>

namespace f4::terrain {

namespace {
using f4::io::Cursor;
} // namespace

NearTileDB::~NearTileDB() = default;
NearTileDB::NearTileDB() = default;
NearTileDB::NearTileDB(NearTileDB&&) noexcept = default;
NearTileDB& NearTileDB::operator=(NearTileDB&&) noexcept = default;

bool NearTileDB::load(const std::filesystem::path& texture_dir) {
    sets_.clear();
    cache_.clear();
    total_tiles_ = 0;
    texture_dir_ = texture_dir;

    const std::filesystem::path bin_path =
        detail::find_file_ci(texture_dir, "TEXTURE.BIN");
    if (bin_path.empty()) return false;

    // Parse TEXTURE.BIN. Layout in near_tile_db.hpp; strides verified
    // against the stock Korea file (110 sets, 1051 tiles).
    const auto bin = f4::io::read_file(bin_path, "near_tile_db");
    Cursor c{bin.data(), bin.data() + bin.size()};
    const int32_t num_sets = c.i32();
    const int32_t total = c.i32();
    if (num_sets < 0 || num_sets > 4096 || total < 0 || total > 65536)
        throw std::runtime_error("near_tile_db: implausible TEXTURE.BIN header");

    sets_.resize(static_cast<std::size_t>(num_sets));
    for (int32_t s = 0; s < num_sets; ++s) {
        NearTileSet& set = sets_[static_cast<std::size_t>(s)];
        const int32_t n_tiles = c.i32();
        set.terrain_type = c.u8();
        if (n_tiles < 0 || n_tiles > 16)
            throw std::runtime_error("near_tile_db: bad tile count in set " +
                                     std::to_string(s));
        set.tiles.resize(static_cast<std::size_t>(n_tiles));
        for (int32_t t = 0; t < n_tiles; ++t) {
            NearTile& tile = set.tiles[static_cast<std::size_t>(t)];
            tile.name = c.fixed_string(20);
            tile.n_areas = c.i32();
            tile.n_paths = c.i32();
            if (tile.n_areas < 0 || tile.n_areas > 64 ||
                tile.n_paths < 0 || tile.n_paths > 64)
                throw std::runtime_error("near_tile_db: bad area/path count for " +
                                         tile.name);
            c.skip(static_cast<std::size_t>(tile.n_areas) * 16 +
                   static_cast<std::size_t>(tile.n_paths) * 24);
        }
    }
    c.check_and_throw("near_tile_db: TEXTURE.BIN truncated");
    total_tiles_ = static_cast<uint32_t>(total);

    // texture.zip is optional — loose art files also work (FreeFalcon
    // installs extract the zip). An unreadable zip degrades to loose
    // files only.
    zip_.reset();
    const std::filesystem::path zip_path =
        detail::find_file_ci(texture_dir, "texture.zip");
    if (!zip_path.empty()) {
        auto zip = std::make_unique<f4::io::ZipReader>();
        try {
            zip->load(zip_path);
            if (zip->size() > 0) zip_ = std::move(zip);
        } catch (const std::exception&) {
        }
    }
    return true;
}

const NearTile* NearTileDB::find_tile(uint16_t tex_id) const {
    uint32_t set_i, tile_i, res;
    unpack_tex_id(tex_id, set_i, tile_i, res);
    if (set_i >= sets_.size()) return nullptr;
    const auto& set = sets_[set_i];
    if (tile_i >= set.tiles.size()) return nullptr;
    return &set.tiles[tile_i];
}

bool NearTileDB::tile_rgba(uint16_t tex_id, NearTileImage& out) const {
    if (const auto it = cache_.find(tex_id); it != cache_.end()) {
        out = it->second;
        return out.width > 0;
    }

    const NearTile* tile = find_tile(tex_id);
    bool ok = false;
    NearTileImage img;
    if (tile && tile->name.size() > 1) {
        // Resolution variants: rewrite the first character — res 1 -> 'M',
        // res 0 -> 'L', other -> stored name (high-res art). If the
        // preferred variant has no art, walk the family in quality order.
        uint32_t set_i, tile_i, res;
        unpack_tex_id(tex_id, set_i, tile_i, res);
        const std::string stored = tile->name;
        const std::string m = "M" + stored.substr(1);
        const std::string l = "L" + stored.substr(1);
        std::vector<std::string> candidates;
        if (res == 1)      candidates = {m, stored, l};
        else if (res == 0) candidates = {l, stored, m};
        else               candidates = {stored, m, l};

        std::vector<uint8_t> bytes;
        std::string err;
        for (const std::string& cand : candidates) {
            bytes.clear();
            if (zip_ && zip_->has(cand)) {
                try { bytes = zip_->read(cand); } catch (const std::exception&) {}
            }
            if (bytes.empty()) {
                const std::filesystem::path loose =
                    detail::find_file_ci(texture_dir_, cand);
                if (!loose.empty()) {
                    try { bytes = f4::io::read_file(loose, "near_tile_db"); }
                    catch (const std::exception&) {}
                }
            }
            PcxImage pcx;
            if (!bytes.empty() && decode_pcx(bytes.data(), bytes.size(), pcx, err)) {
                img.width = pcx.width;
                img.height = pcx.height;
                img.rgba = std::move(pcx.rgba);
                ok = true;
                break;
            }
        }
    }

    cache_[tex_id] = img;   // Negative results cache too (width 0 = miss).
    out = img;
    return ok;
}

} // namespace f4::terrain
