// paint_map_check.cpp — paint the 2D map exactly like the world-viewer
// canvas does (L5 posts -> FArtILES thumbnails, 16px cells) and write a
// PNG next to the MEA color-band map for visual comparison.
//
// Usage: paint_map_check <theater_dir> <out.png>
#include <f4/terrain/far_tile_db.hpp>
#include <f4/terrain/post_level.hpp>
#include <f4/terrain/terrain_data.hpp>
#include <f4/terrain/theater_geometry.hpp>

#include "raylib.h"

#include <cstdio>
#include <filesystem>

int main(int argc, char* argv[]) {
    if (argc < 3) {
        std::printf("usage: paint_map_check <theater_dir> <out.png>\n");
        return 1;
    }
    const std::filesystem::path theater_dir = argv[1];
    const auto terrain_dir = theater_dir / "terrain";
    const auto texture_dir = theater_dir / "texture";

    const auto geom = f4::terrain::TheaterGeometry::korea();
    f4::terrain::TerrainData td;
    td.load(terrain_dir);
    f4::terrain::FarTileDB far_db;
    if (!far_db.load(texture_dir)) {
        std::printf("far db load failed\n");
        return 1;
    }
    f4::terrain::PostLevel lvl;
    // The far level WorldView uses (L4 for Korea) — its tex_ids match
    // the tile DB geography (L5's don't; see worklog TERRAIN-TEX-3).
    if (!lvl.load(terrain_dir, 4, geom)) {
        std::printf("L4 load failed\n");
        return 1;
    }
    const uint32_t pw = lvl.posts_wide();
    const int spp = pw / static_cast<uint32_t>(td.header.width);  // posts/cell side

    const int gw = static_cast<int>(td.header.width);
    const int gh = static_cast<int>(td.header.height);
    constexpr int kPx = 16;
    Image img = GenImageColor(gw * kPx, gh * kPx * 2 + 8, BLACK);
    auto* data = static_cast<uint8_t*>(img.data);
    const int stride = static_cast<std::size_t>(gw) * kPx * 4;

    // Top half: far-tile art exactly as canvas.cpp paints it (image
    // row 0 = south = post row 0; each cell averages its spp x spp
    // far posts' tiles).
    std::vector<uint8_t> tile;
    std::vector<uint32_t> acc(static_cast<std::size_t>(kPx) * kPx * 4);
    for (int r = 0; r < gh; ++r) {
        for (int c = 0; c < gw; ++c) {
            uint8_t cell[kPx * kPx * 4];
            uint32_t contributions = 0;
            std::fill(acc.begin(), acc.end(), 0u);
            for (int py = 0; py < spp; ++py) {
                for (int px = 0; px < spp; ++px) {
                    const auto p = lvl.post(
                        static_cast<uint32_t>(c * spp + px),
                        static_cast<uint32_t>(r * spp + py));
                    if (p.has_no_tile() || !far_db.tile_rgba(p.tex_id, tile)) {
                        continue;
                    }
                    ++contributions;
                    constexpr int S =
                        static_cast<int>(f4::terrain::FarTileDB::TILE_SIZE) / kPx;
                    for (int y = 0; y < kPx; ++y) {
                        for (int x = 0; x < kPx; ++x) {
                            int sr = 0, sg = 0, sb = 0;
                            for (int dy = 0; dy < S; ++dy)
                            for (int dx = 0; dx < S; ++dx) {
                                const std::size_t src_px =
                                    (static_cast<std::size_t>(y * S + dy) * 32 +
                                     (x * S + dx)) * 4;
                                sr += tile[src_px + 0];
                                sg += tile[src_px + 1];
                                sb += tile[src_px + 2];
                            }
                            const std::size_t n = static_cast<std::size_t>(S) * S;
                            const std::size_t dst_px =
                                (static_cast<std::size_t>(y) * kPx + x) * 4;
                            acc[dst_px + 0] += static_cast<uint32_t>(sr / n);
                            acc[dst_px + 1] += static_cast<uint32_t>(sg / n);
                            acc[dst_px + 2] += static_cast<uint32_t>(sb / n);
                            acc[dst_px + 3] += 255;
                        }
                    }
                }
            }
            if (contributions > 0) {
                for (std::size_t i = 0; i < sizeof(cell); i += 4) {
                    cell[i + 0] = static_cast<uint8_t>(acc[i + 0] / contributions);
                    cell[i + 1] = static_cast<uint8_t>(acc[i + 1] / contributions);
                    cell[i + 2] = static_cast<uint8_t>(acc[i + 2] / contributions);
                    cell[i + 3] = 255;
                }
            } else {
                const auto col = f4::terrain::TerrainData::color_for_tile_type(
                    td.tile_type_at(static_cast<uint32_t>(c),
                                    static_cast<uint32_t>(r)));
                for (std::size_t i = 0; i < sizeof(cell); i += 4) {
                    cell[i + 0] = col.r; cell[i + 1] = col.g;
                    cell[i + 2] = col.b; cell[i + 3] = 255;
                }
            }
            auto* dst = data +
                ((static_cast<std::size_t>(r) * kPx * stride) +
                 static_cast<std::size_t>(c) * kPx * 4);
            for (int y = 0; y < kPx; ++y) {
                std::memcpy(dst + static_cast<std::size_t>(y) * stride,
                            cell + static_cast<std::size_t>(kPx - 1 - y) * kPx * 4,
                            static_cast<std::size_t>(kPx) * 4);
            }
        }
    }

    // Bottom half: the MEA color-band map (known-correct geography),
    // same orientation (row 0 = south at image row gh*kPx+8).
    for (int r = 0; r < gh; ++r) {
        for (int c = 0; c < gw; ++c) {
            const auto col = f4::terrain::TerrainData::color_for_tile_type(
                td.tile_type_at(static_cast<uint32_t>(c),
                                static_cast<uint32_t>(r)));
            for (int y = 0; y < kPx; ++y) {
                auto* dst = data +
                    (static_cast<std::size_t>(gh * kPx + 8 + r * kPx + y) *
                         stride +
                     static_cast<std::size_t>(c) * kPx * 4);
                for (int x = 0; x < kPx; ++x) {
                    dst[x * 4 + 0] = col.r; dst[x * 4 + 1] = col.g;
                    dst[x * 4 + 2] = col.b; dst[x * 4 + 3] = 255;
                }
            }
        }
    }

    ExportImage(img, argv[2]);
    std::printf("wrote %s (%dx%d) — top: L5 far art, bottom: MEA colors\n",
                argv[2], gw * kPx, gh * kPx * 2 + 8);
    return 0;
}
