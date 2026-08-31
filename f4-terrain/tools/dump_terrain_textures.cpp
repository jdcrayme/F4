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
#include <cmath>
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

    // --- Near-vs-far surface disagreement (sizes the far z bias) --------
    {
        f4::terrain::PostLevel l2, l4;
        if (l2.load(terrain_dir, 2, geom) && l4.load(terrain_dir, 4, geom)) {
            double max_d = 0.0, sum_d = 0.0;
            long n = 0, over400 = 0;
            const double ft = geom.theater_size_ft;
            for (double e = 0; e <= ft; e += ft / 256.0) {
                for (double nn = 0; nn <= ft; nn += ft / 256.0) {
                    const double d = l2.elevation_at_ft(e, nn) -
                                     l4.elevation_at_ft(e, nn);
                    const double ad = std::abs(d);
                    max_d = std::max(max_d, ad);
                    sum_d += ad;
                    if (ad > 400.0) ++over400;
                    ++n;
                }
            }
            std::printf("\n|L2-L4| elevation over the theater: max=%.0f ft, mean=%.0f ft, %ld/%ld samples > 400 ft\n",
                        max_d, sum_d / n, over400, n);
        }
    }

    // --- Map-art cross-check: far-tile art vs MEA water/land ----------
    // Prints ASCII maps ('W'=water/blue art, '.'=land, '?'=no tile) for
    // each far post level — the MEA one is known-correct geography; a
    // level whose art doesn't reproduce the same coastline is broken.
    if (far_ok && !td.elevation.empty()) {
        std::vector<uint8_t> px;
        auto water_art = [&](uint16_t tex_id, bool& have) {
            have = false;
            if (tex_id == 0xFFFF) return false;
            if (!far_db.tile_rgba(tex_id, px)) return false;
            long r = 0, g = 0, b = 0, n = 0;
            for (std::size_t i = 0; i + 3 < px.size(); i += 4) {
                r += px[i]; g += px[i + 1]; b += px[i + 2]; ++n;
            }
            if (n == 0) return false;
            have = true;
            r /= n; g /= n; b /= n;
            return b > r + 8;   // ocean art is blue-dominant
        };
        const uint32_t gw = td.header.width, gh = td.header.height;
        const uint32_t step = std::max<uint32_t>(1, gw / 48);
        std::printf("\nMEA water/land ('W'=water, '.'=land), north at top:\n");
        for (uint32_t y = gh; y-- > 0;) {
            for (uint32_t x = 0; x < gw; x += step) {
                const auto c = f4::terrain::TerrainData::color_for_tile_type(
                    td.tile_type_at(x, y));
                std::putchar((c.b > c.r + 15 && c.b > c.g + 5) ? 'W' : '.');
            }
            std::putchar('\n');
        }
        for (int level = 3; level <= static_cast<int>(geom.last_level); ++level) {
            f4::terrain::PostLevel pl;
            if (!pl.load(terrain_dir, level, geom)) continue;
            // Correlation of per-cell art water-fraction vs MEA water,
            // under 4 orientations — the true orientation wins clearly.
            // (Also printed as ASCII for eyeballing.)
            const double scale = static_cast<double>(pl.posts_wide()) /
                                 static_cast<double>(gw);
            std::vector<double> art_w(gh * gw, 0.0);
            std::vector<double> mea_w(gh * gw, 0.0);
            std::vector<uint8_t> px;
            for (uint32_t y = 0; y < gh; ++y) {
                for (uint32_t x = 0; x < gw; ++x) {
                    const auto mc = f4::terrain::TerrainData::color_for_tile_type(
                        td.tile_type_at(x, y));
                    mea_w[y * gw + x] =
                        (mc.b > mc.r + 15 && mc.b > mc.g + 5) ? 1.0 : 0.0;
                    const uint32_t pc = static_cast<uint32_t>((x + 0.5) * scale);
                    const uint32_t pr = static_cast<uint32_t>((y + 0.5) * scale);
                    const auto p = pl.post(std::min(pc, pl.posts_wide() - 1),
                                           std::min(pr, pl.posts_wide() - 1));
                    double w = 0.0;
                    if (p.tex_id != 0xFFFF && far_db.tile_rgba(p.tex_id, px)) {
                        long r = 0, g = 0, b = 0; long n = 0, wb = 0;
                        for (std::size_t i = 0; i + 3 < px.size(); i += 4) {
                            r = px[i]; g = px[i + 1]; b = px[i + 2];
                            if (b > r + 8) ++wb;
                            ++n;
                        }
                        w = n ? static_cast<double>(wb) / n : 0.0;
                    }
                    art_w[y * gw + x] = w;
                }
            }
            auto corr = [&](int fy, int fx) {
                // fy/fx: +1 = same direction, -1 = flipped
                double sa = 0, sb = 0, saa = 0, sbb = 0, sab = 0;
                long n = 0;
                for (uint32_t y = 0; y < gh; ++y)
                for (uint32_t x = 0; x < gw; ++x) {
                    const uint32_t ay = fy > 0 ? y : gh - 1 - y;
                    const uint32_t ax = fx > 0 ? x : gw - 1 - x;
                    const double a = art_w[ay * gw + ax];
                    const double b = mea_w[y * gw + x];
                    sa += a; sb += b; saa += a * a; sbb += b * b; sab += a * b;
                    ++n;
                }
                const double cov = sab / n - (sa / n) * (sb / n);
                const double va = saa / n - (sa / n) * (sa / n);
                const double vb = sbb / n - (sb / n) * (sb / n);
                return va > 0 && vb > 0 ? cov / std::sqrt(va * vb) : 0.0;
            };
            std::printf("\nL%d art-vs-MEA water correlation: identity=%.3f flipNS=%.3f flipEW=%.3f rot180=%.3f\n",
                        level, corr(1, 1), corr(-1, 1), corr(1, -1), corr(-1, -1));
            std::printf("L%d far-tile art ('W'=blue art, '.'=land art, '?'=no tile), north at top:\n",
                        level);
            for (uint32_t y = gh; y-- > 0;) {
                for (uint32_t x = 0; x < gw; x += step) {
                    const uint32_t pc = static_cast<uint32_t>((x + 0.5) * scale);
                    const uint32_t pr = static_cast<uint32_t>((y + 0.5) * scale);
                    bool have = false;
                    const auto p = pl.post(std::min(pc, pl.posts_wide() - 1),
                                           std::min(pr, pl.posts_wide() - 1));
                    if (p.tex_id != 0xFFFF) {
                        if (far_db.tile_rgba(p.tex_id, px)) have = true;
                    }
                    const bool w = have && art_w[y * gw + x] > 0.5;
                    std::putchar(!have ? '?' : (w ? 'W' : '.'));
                }
                std::putchar('\n');
            }
        }
    }
    return 0;
}
