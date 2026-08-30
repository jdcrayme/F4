// dump_terrain_textures.cpp
// Validate the f4-terrain theater decoders (PostLevel, FarTileDB,
// NearTileDB) against a real Falcon install and report format facts:
// per-LOD post stats, texID ranges, tile-DB cross-checks, PCX dims.
//
// Usage: dump_terrain_textures <theater_dir>
//   e.g. dump_terrain_textures "D:/SteamLibrary/.../terrdata/korea"
// Expects terrain/ and texture/ subdirectories.

#include <f4/terrain/far_tile_db.hpp>
#include <f4/terrain/near_tile_db.hpp>
#include <f4/terrain/post_level.hpp>
#include <f4/terrain/terrain_data.hpp>
#include <f4/terrain/theater_geometry.hpp>

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <set>
#include <string>
#include <vector>

int main(int argc, char* argv[]) {
    if (argc < 2) {
        std::printf("usage: dump_terrain_textures <theater_dir>\n");
        return 1;
    }
    const std::filesystem::path theater_dir = argv[1];
    const std::filesystem::path terrain_dir = theater_dir / "terrain";
    const std::filesystem::path texture_dir = theater_dir / "texture";

    const auto geom = f4::terrain::TheaterGeometry::korea();

    // --- MEA summary (for elevation cross-checks) -----------------------
    f4::terrain::TerrainData td;
    try {
        td.load(terrain_dir);
        std::printf("THEATER.MAP/MEA: %ux%u cells, palette %zu entries\n",
                    td.header.width, td.header.height, td.palette.size());
    } catch (const std::exception& e) {
        std::printf("TerrainData load failed: %s\n", e.what());
    }

    // --- Far tiles -------------------------------------------------------
    f4::terrain::FarTileDB far_db;
    bool far_ok = false;
    try {
        far_ok = far_db.load(texture_dir);
    } catch (const std::exception& e) {
        std::printf("FarTileDB load threw: %s\n", e.what());
    }
    std::printf("FarTileDB: %s", far_ok ? "loaded" : "absent");
    if (far_ok) {
        std::printf(", %u tiles, palette[0]=#%02x%02x%02x palette[64]=#%02x%02x%02x\n",
                    far_db.tile_count(),
                    far_db.palette_rgba()[0], far_db.palette_rgba()[1], far_db.palette_rgba()[2],
                    far_db.palette_rgba()[256], far_db.palette_rgba()[257], far_db.palette_rgba()[258]);
        std::vector<uint8_t> px;
        if (far_db.tile_rgba(0, px)) {
            std::printf("  tile 0: RGBA %zu bytes, first pixel #%02x%02x%02x\n",
                        px.size(), px[0], px[1], px[2]);
        }
    } else {
        std::printf("\n");
    }

    // --- Near tiles --------------------------------------------------------
    f4::terrain::NearTileDB near_db;
    bool near_ok = false;
    try {
        near_ok = near_db.load(texture_dir);
    } catch (const std::exception& e) {
        std::printf("NearTileDB load threw: %s\n", e.what());
    }
    std::printf("NearTileDB: %s", near_ok ? "loaded" : "absent");
    if (near_ok) {
        std::printf(", %u sets, %u catalog tiles\n",
                    near_db.set_count(), near_db.tile_count());
        // Decode a couple of tiles from different sets, various res.
        const uint16_t probes[] = {0x0000, 0x0001, 0x0010, 0x0120, 0x1000, 0x2000};
        for (uint16_t id : probes) {
            const auto* t = near_db.find_tile(id);
            if (!t) { std::printf("  texID 0x%04x: no catalog entry\n", id); continue; }
            f4::terrain::NearTileImage img;
            if (near_db.tile_rgba(id, img)) {
                std::printf("  texID 0x%04x (%s): %ux%u, px0 #%02x%02x%02x\n",
                            id, t->name.c_str(), img.width, img.height,
                            img.rgba[0], img.rgba[1], img.rgba[2]);
            } else {
                std::printf("  texID 0x%04x (%s): NO ART\n", id, t->name.c_str());
            }
        }
    } else {
        std::printf("\n");
    }

    // --- Post levels -------------------------------------------------------
    for (int level = 0; level <= geom.last_level; ++level) {
        f4::terrain::PostLevel pl;
        bool ok = false;
        try {
            ok = pl.load(terrain_dir, level, geom);
        } catch (const std::exception& e) {
            std::printf("L%d: load threw: %s\n", level, e.what());
            continue;
        }
        if (!ok) { std::printf("L%d: files absent\n", level); continue; }

        // Sample a sparse grid of posts: elevation range, texID stats,
        // and cross-check against the tile DBs.
        const uint32_t n = pl.posts_wide();
        int16_t zmin = 30000, zmax = -30000;
        std::set<uint16_t> ids;
        uint32_t no_tile = 0, samples = 0;
        const uint32_t step = std::max<uint32_t>(1, n / 64);
        for (uint32_t r = 0; r < n; r += step)
        for (uint32_t c = 0; c < n; c += step) {
            const auto p = pl.post(c, r);
            zmin = std::min(zmin, p.elevation_ft);
            zmax = std::max(zmax, p.elevation_ft);
            if (p.has_no_tile()) ++no_tile;
            else ids.insert(p.tex_id);
            ++samples;
        }
        std::printf("L%d: %u posts wide (%u blocks), %u samples, z=[%d..%d], "
                    "%zu distinct texIDs, %u no-tile\n",
                    level, n, pl.blocks_wide(), samples, zmin, zmax,
                    ids.size(), no_tile);
        if (!ids.empty()) {
            std::printf("  texID range 0x%04x..0x%04x (max %u)\n",
                        *ids.begin(), *ids.rbegin(),
                        static_cast<uint32_t>(*ids.rbegin()));
            if (far_ok && *ids.rbegin() < far_db.tile_count())
                std::printf("  -> fits FarTileDB range (%u)\n", far_db.tile_count());
            if (near_ok) {
                // Near-packed? set=(id>>4)&0xFF must be < set_count for most ids.
                uint32_t in_set = 0;
                for (uint16_t id : ids) {
                    const uint32_t set = (id >> 4) & 0xFF;
                    if (set < near_db.set_count()) ++in_set;
                }
                std::printf("  -> %u/%zu ids have set < %u (near-packed plausibility)\n",
                            in_set, ids.size(), near_db.set_count());
            }
        }
        // Elevation sampler sanity vs MEA at the same point (theater center).
        if (!td.elevation.empty()) {
            const double e = pl.elevation_at_ft(524288.0, 524288.0);
            std::printf("  elevation_at_ft(theater center) = %.1f\n", e);
        }
    }
    return 0;
}
