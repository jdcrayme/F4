// f4-renderer/src/terrain_chunks.cpp
//
// TerrainChunkSet implementation — builds a grid of small terrain chunk
// meshes for frustum-culled theater-scale rendering. See terrain_chunks.hpp.
//
// Each chunk is a regular grid of (chunk_resolution+1)² vertices, sampled
// from the terrain's elevation grid via bilinear interpolation. Adjacent
// chunks duplicate edge vertices (no shared vertices) so there are no
// T-junctions or cracks. The winding, y-flip, and color sampling
// conventions match build_terrain_mesh() in terrain_mesh.cpp exactly.
//
// The mesh arrays for each chunk are allocated with RL_MALLOC so Raylib's
// UnloadMesh can free them. UploadMesh() pushes the data to the GPU.

#include <f4/renderer/terrain_chunks.hpp>
#include <f4/renderer/terrain_shader.hpp>
#include <f4/renderer/terrain_tile_cache.hpp>
#include <f4/renderer/coord_transform.hpp>  // enu_to_raylib
#include <f4/renderer/draw_3d.hpp>          // extend_far_plane

#include "terrain_internal.hpp"

#include <raylib.h>
#include <rlgl.h>              // rlDisableBackfaceCulling, rlEnableBackfaceCulling

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <vector>

namespace f4::renderer {

// ── Helpers shared with terrain_mesh.cpp ─────────────────────────────────────
// theater_ft_per_cell / world_to_cell_clamped / bilinear_elevation live in
// terrain_internal.hpp (extracted once WorldView became the third consumer).

using f4::renderer::detail::theater_ft_per_cell;
using f4::renderer::detail::world_to_cell_clamped;
using f4::renderer::detail::bilinear_elevation;

// ── Per-chunk build ────────────────────────────────────────────────────────
//
// Builds one chunk's vertices/normals/colors/indices and uploads to GPU.
// The chunk covers [min_east, min_east + chunk_size] × [min_north, min_north
// + chunk_size] ft, sampled at (res+1)² points.

static TerrainChunk build_one_chunk(
    const f4::terrain::TerrainData& terrain,
    const TerrainChunkSetConfig& cfg,
    float min_east, float min_north, float chunk_size_ft,
    int chunk_x, int chunk_y) {

    TerrainChunk ch;
    ch.chunk_x = chunk_x;
    ch.chunk_y = chunk_y;
    ch.min_east = min_east;
    ch.max_east = min_east + chunk_size_ft;
    ch.min_north = min_north;
    ch.max_north = min_north + chunk_size_ft;

    // Cap resolution at 254 (same as build_terrain_mesh — unsigned short
    // indices can't exceed 65535).
    static constexpr int kMaxRes = 254;
    int res = cfg.chunk_resolution;
    if (res > kMaxRes) {
        std::fprintf(stderr,
            "terrain_chunks: chunk_resolution=%d exceeds unsigned-short "
            "index cap (max %d); clamping.\n", res, kMaxRes);
        res = kMaxRes;
    }
    res = std::max(2, res);

    const int vert_count = (res + 1) * (res + 1);
    const int tri_count = res * res * 2;

    std::vector<float> vertices(vert_count * 3);
    std::vector<float> texcoords(vert_count * 2);
    std::vector<float> normals(vert_count * 3, 0.0f);
    std::vector<unsigned char> colors(vert_count * 4);
    std::vector<unsigned short> indices(tri_count * 3);

    const float step = chunk_size_ft / static_cast<float>(res);
    float min_up = 1e30f, max_up = -1e30f;

    const double ft_per_cell = theater_ft_per_cell(terrain);

    // Build vertices.
    for (int j = 0; j <= res; ++j) {
        for (int i = 0; i <= res; ++i) {
            const int idx = j * (res + 1) + i;
            const float east_ft  = min_east  + i * step;
            const float north_ft = min_north + j * step;

            const double elev = bilinear_elevation(terrain, east_ft, north_ft);
            const float up_ft = static_cast<float>(elev) * cfg.vertical_scale
                              + cfg.z_offset_ft;

            // Raylib RH Y-up: X=east, Y=up, Z=-north (see coord_transform.hpp).
            vertices[idx * 3 + 0] = east_ft;
            vertices[idx * 3 + 1] = up_ft;
            vertices[idx * 3 + 2] = -north_ft;

            min_up = std::min(min_up, up_ft);
            max_up = std::max(max_up, up_ft);

            texcoords[idx * 2 + 0] = static_cast<float>(i) / res;
            texcoords[idx * 2 + 1] = static_cast<float>(j) / res;

            // Color by tile type — uses world_to_cell_clamped so vertices
            // outside the theater boundary sample the edge cell (not UB).
            Color c;
            if (cfg.color_by_tile_type) {
                const uint32_t cx = world_to_cell_clamped(
                    east_ft, ft_per_cell, terrain.header.width);
                const uint32_t cy_raw = world_to_cell_clamped(
                    north_ft, ft_per_cell, terrain.header.height);
                const uint32_t cy = terrain.header.height - 1 - cy_raw;
                const auto tt = terrain.tile_type_at(cx, cy);
                const auto c4 = f4::terrain::TerrainData::color_for_tile_type(tt);
                c = {c4.r, c4.g, c4.b, c4.a};
            } else {
                c = {80, 110, 60, 255};
            }
            colors[idx * 4 + 0] = c.r;
            colors[idx * 4 + 1] = c.g;
            colors[idx * 4 + 2] = c.b;
            colors[idx * 4 + 3] = c.a;
        }
    }

    // Build triangle indices (same winding as build_terrain_mesh).
    int ti = 0;
    for (int j = 0; j < res; ++j) {
        for (int i = 0; i < res; ++i) {
            const int v00 = j * (res + 1) + i;
            const int v10 = j * (res + 1) + (i + 1);
            const int v01 = (j + 1) * (res + 1) + i;
            const int v11 = (j + 1) * (res + 1) + (i + 1);

            indices[ti++] = static_cast<unsigned short>(v00);
            indices[ti++] = static_cast<unsigned short>(v10);
            indices[ti++] = static_cast<unsigned short>(v01);

            indices[ti++] = static_cast<unsigned short>(v10);
            indices[ti++] = static_cast<unsigned short>(v11);
            indices[ti++] = static_cast<unsigned short>(v01);
        }
    }

    // Compute per-vertex normals (same as build_terrain_mesh).
    for (int t = 0; t < tri_count; ++t) {
        const unsigned short i0 = indices[t * 3 + 0];
        const unsigned short i1 = indices[t * 3 + 1];
        const unsigned short i2 = indices[t * 3 + 2];
        const float ax = vertices[i1*3+0] - vertices[i0*3+0];
        const float ay = vertices[i1*3+1] - vertices[i0*3+1];
        const float az = vertices[i1*3+2] - vertices[i0*3+2];
        const float bx = vertices[i2*3+0] - vertices[i0*3+0];
        const float by = vertices[i2*3+1] - vertices[i0*3+1];
        const float bz = vertices[i2*3+2] - vertices[i0*3+2];
        const float nx = ay * bz - az * by;
        const float ny = az * bx - ax * bz;
        const float nz = ax * by - ay * bx;
        normals[i0*3+0] += nx; normals[i0*3+1] += ny; normals[i0*3+2] += nz;
        normals[i1*3+0] += nx; normals[i1*3+1] += ny; normals[i1*3+2] += nz;
        normals[i2*3+0] += nx; normals[i2*3+1] += ny; normals[i2*3+2] += nz;
    }
    for (int i = 0; i < vert_count; ++i) {
        const float nx = normals[i*3+0];
        const float ny = normals[i*3+1];
        const float nz = normals[i*3+2];
        const float len = std::sqrt(nx*nx + ny*ny + nz*nz);
        if (len > 1e-10f) {
            normals[i*3+0] = nx / len;
            normals[i*3+1] = ny / len;
            normals[i*3+2] = nz / len;
        } else {
            normals[i*3+0] = 0.0f;
            normals[i*3+1] = 1.0f;
            normals[i*3+2] = 0.0f;
        }
    }

    // Allocate Raylib-owned arrays (RL_MALLOC so UnloadMesh frees them).
    float* verts = static_cast<float*>(RL_MALLOC(sizeof(float) * vert_count * 3));
    float* tcs   = static_cast<float*>(RL_MALLOC(sizeof(float) * vert_count * 2));
    float* nrms  = static_cast<float*>(RL_MALLOC(sizeof(float) * vert_count * 3));
    unsigned char* cols = static_cast<unsigned char*>(RL_MALLOC(sizeof(unsigned char) * vert_count * 4));
    unsigned short* idx = static_cast<unsigned short*>(RL_MALLOC(sizeof(unsigned short) * tri_count * 3));

    std::copy(vertices.begin(), vertices.end(), verts);
    std::copy(texcoords.begin(), texcoords.end(), tcs);
    std::copy(normals.begin(), normals.end(), nrms);
    std::copy(colors.begin(), colors.end(), cols);
    std::copy(indices.begin(), indices.end(), idx);

    ch.mesh.vertexCount = vert_count;
    ch.mesh.triangleCount = tri_count;
    ch.mesh.vertices = verts;
    ch.mesh.texcoords = tcs;
    ch.mesh.normals = nrms;
    ch.mesh.colors = cols;
    ch.mesh.indices = idx;

    UploadMesh(&ch.mesh, false);
    ch.model = LoadModelFromMesh(ch.mesh);
    ch.model.materials[0].maps[MATERIAL_MAP_ALBEDO].color = WHITE;

    ch.valid = true;
    ch.min_up = min_up;
    ch.max_up = max_up;
    return ch;
}

// ── Textured path (Phase 2) ─────────────────────────────────────────────────
//
// Emits post-aligned quads: one quad per PostLevel cell, four dedicated
// vertices each (no sharing — neighboring quads can reference different
// tile layers). texcoords carry the tile UV; texcoords2 carries
// (array layer, sampler family) consumed by TerrainShader. Chunks are
// 16x16 quads — the natural TdiskPost block granularity.
//
// Near-region UVs port FreeFalcon's DiskblockToMemblock math verbatim:
// a tile spans 4 posts at LOD 0, 2 at LOD 1, and 1 (a full tile) at
// LOD 2+, with the tile origin advancing per block-local post index.

namespace {

// FreeFalcon UV constants (tdskpost.cpp).
constexpr float kUvStart = 0.00001f;
constexpr float kUvStop  = 1.0f - kUvStart;
constexpr float kUvMinStep = (kUvStop - kUvStart) * 0.25f;
constexpr int   kLastNearTexturedLod = 2;    // stock theaters
constexpr int   kQuadsPerChunkSide = 16;     // == POSTS_ACROSS_BLOCK

struct TexturedRegion {
    const f4::terrain::PostLevel* level = nullptr;
    double ft_per_post = 0.0;
    int qmin_col = 0, qmax_col = 0;   // quad range [qmin, qmax) in posts
    int qmin_row = 0, qmax_row = 0;
    float z_bias = 0.0f;              // extra offset (far ring)
    bool near_region = false;
};

/// Build one 16x16-quad chunk from a region. `qc0`/`qr0` are the chunk's
/// quad origin in level post space.
TerrainChunk build_one_textured_chunk(
    const f4::terrain::TerrainData& terrain,
    const TerrainTileSource& tiles,
    TerrainTileCache& cache,
    const TerrainChunkSetConfig& cfg,
    const TexturedRegion& reg,
    int qc0, int qr0) {

    const int ncols = std::min(kQuadsPerChunkSide, reg.qmax_col - qc0);
    const int nrows = std::min(kQuadsPerChunkSide, reg.qmax_row - qr0);
    TerrainChunk ch;
    ch.textured = true;
    if (ncols <= 0 || nrows <= 0) return ch;

    const int level = reg.level->level();
    const float uv_d = (level < kLastNearTexturedLod)
                           ? static_cast<float>((1 << level)) * kUvMinStep
                           : kUvStop;
    const float z_off = cfg.z_offset_ft + reg.z_bias;

    const int quads = ncols * nrows;
    const int vert_count = quads * 4;
    const int tri_count = quads * 2;

    std::vector<float> vertices(static_cast<std::size_t>(vert_count) * 3);
    std::vector<float> texcoords(static_cast<std::size_t>(vert_count) * 2);
    std::vector<float> texcoords2(static_cast<std::size_t>(vert_count) * 2);
    std::vector<float> normals(static_cast<std::size_t>(vert_count) * 3, 0.0f);
    std::vector<unsigned char> colors(static_cast<std::size_t>(vert_count) * 4, 255);
    std::vector<unsigned short> indices(static_cast<std::size_t>(tri_count) * 3);

    float min_up = 1e30f, max_up = -1e30f;
    int untextured = 0;
    int v = 0;

    for (int qr = 0; qr < nrows; ++qr) {
        for (int qc = 0; qc < ncols; ++qc) {
            const int col = qc0 + qc;
            const int row = qr0 + qr;
            const auto sw = reg.level->post(static_cast<uint32_t>(std::max(col, 0)),
                                            static_cast<uint32_t>(std::max(row, 0)));
            const auto se = reg.level->post(static_cast<uint32_t>(std::max(col + 1, 0)),
                                            static_cast<uint32_t>(std::max(row, 0)));
            const auto ne = reg.level->post(static_cast<uint32_t>(std::max(col + 1, 0)),
                                            static_cast<uint32_t>(std::max(row + 1, 0)));
            const auto nw = reg.level->post(static_cast<uint32_t>(std::max(col, 0)),
                                            static_cast<uint32_t>(std::max(row + 1, 0)));

            // Resolve the tile (SW post supplies the texture — FreeFalcon
            // DrawTerrainSquare convention).
            int layer = -1;
            float kind = -1.0f;   // untextured until resolved
            if (!sw.has_no_tile()) {
                if (reg.near_region) {
                    int tile_size = 0;
                    layer = cache.near_layer(*tiles.near_tiles, sw.tex_id, &tile_size);
                    if (layer >= 0) kind = static_cast<float>(tile_size);
                } else {
                    layer = cache.far_layer(*tiles.far_tiles, sw.tex_id);
                    if (layer >= 0) kind = 0.0f;
                }
            }

            // UVs: near port of DiskblockToMemblock; far = full tile.
            float u0 = kUvStart, v0 = kUvStop, du = kUvStop - kUvStart;
            if (reg.near_region) {
                u0 = kUvStart + static_cast<float>(((col & 0xF) << level) & 0x3) * kUvMinStep;
                v0 = kUvStop  - static_cast<float>(((row & 0xF) << level) & 0x3) * kUvMinStep;
                du = uv_d;
            }

            // Corner heights (posts clamped at the theater edge).
            const float e = static_cast<float>(reg.ft_per_post);
            const float x0 = static_cast<float>(col) * e;
            const float x1 = x0 + e;
            const float n0 = static_cast<float>(row) * e;
            const float n1 = n0 + e;
            const float ysw = sw.elevation_ft * cfg.vertical_scale + z_off;
            const float yse = se.elevation_ft * cfg.vertical_scale + z_off;
            const float yne = ne.elevation_ft * cfg.vertical_scale + z_off;
            const float ynw = nw.elevation_ft * cfg.vertical_scale + z_off;

            // Per-quad normal: cross(SE-SW, NW-SW) — +Y up on flat ground.
            const float ax = (x1 - x0), ay = (yse - ysw), az = 0.0f;
            const float bx = 0.0f, by = (ynw - ysw), bz = -(n1 - n0);
            float nx = ay * bz - az * by;
            float ny = az * bx - ax * bz;
            float nz = ax * by - ay * bx;
            const float nl = std::sqrt(nx * nx + ny * ny + nz * nz);
            if (nl > 1e-10f) { nx /= nl; ny /= nl; nz /= nl; }
            else             { nx = 0.0f; ny = 1.0f; nz = 0.0f; }

            // Fallback color for untextured quads: the post's palette
            // index into THEATER.MAP (FreeFalcon's gouraud path).
            unsigned char r = 255, g = 255, b = 255;
            if (layer < 0) {
                ++untextured;
                const auto pal = sw.color;
                if (pal < terrain.palette.size()) {
                    const auto& pc = terrain.palette[pal];
                    r = pc.r; g = pc.g; b = pc.b;
                }
            }

            // Four vertices: SW, SE, NE, NW. Raylib Y-up: Z = -north.
            const float* xs[4] = {&x0, &x1, &x1, &x0};
            const float* ns[4] = {&n0, &n0, &n1, &n1};
            const float* ys[4] = {&ysw, &yse, &yne, &ynw};
            const float us[4] = {u0, u0 + du, u0 + du, u0};
            const float vs[4] = {v0, v0, v0 - du, v0 - du};
            for (int c = 0; c < 4; ++c) {
                const std::size_t at = static_cast<std::size_t>(v) * 3;
                vertices[at + 0] = *xs[c];
                vertices[at + 1] = *ys[c];
                vertices[at + 2] = -*ns[c];
                normals[at + 0] = nx;
                normals[at + 1] = ny;
                normals[at + 2] = nz;
                const std::size_t t2 = static_cast<std::size_t>(v) * 2;
                texcoords[t2 + 0] = us[c];
                texcoords[t2 + 1] = vs[c];
                texcoords2[t2 + 0] = static_cast<float>(layer >= 0 ? layer : 0);
                texcoords2[t2 + 1] = kind;
                const std::size_t c4 = static_cast<std::size_t>(v) * 4;
                colors[c4 + 0] = r; colors[c4 + 1] = g;
                colors[c4 + 2] = b; colors[c4 + 3] = 255;
                min_up = std::min(min_up, *ys[c]);
                max_up = std::max(max_up, *ys[c]);
                ++v;
            }
        }
    }

    // Indices (filled separately for clarity).
    for (int q = 0; q < quads; ++q) {
        const unsigned short b = static_cast<unsigned short>(q * 4);
        indices[static_cast<std::size_t>(q) * 6 + 0] = b + 0;
        indices[static_cast<std::size_t>(q) * 6 + 1] = b + 1;
        indices[static_cast<std::size_t>(q) * 6 + 2] = b + 2;
        indices[static_cast<std::size_t>(q) * 6 + 3] = b + 0;
        indices[static_cast<std::size_t>(q) * 6 + 4] = b + 2;
        indices[static_cast<std::size_t>(q) * 6 + 5] = b + 3;
    }

    // Bounding box.
    ch.min_east = static_cast<float>(qc0) * static_cast<float>(reg.ft_per_post);
    ch.max_east = static_cast<float>(qc0 + ncols) * static_cast<float>(reg.ft_per_post);
    ch.min_north = static_cast<float>(qr0) * static_cast<float>(reg.ft_per_post);
    ch.max_north = static_cast<float>(qr0 + nrows) * static_cast<float>(reg.ft_per_post);
    ch.min_up = min_up;
    ch.max_up = max_up;

    // Upload.
    float* verts = static_cast<float*>(RL_MALLOC(sizeof(float) * vert_count * 3));
    float* tcs   = static_cast<float*>(RL_MALLOC(sizeof(float) * vert_count * 2));
    float* tc2s  = static_cast<float*>(RL_MALLOC(sizeof(float) * vert_count * 2));
    float* nrms  = static_cast<float*>(RL_MALLOC(sizeof(float) * vert_count * 3));
    unsigned char* cols = static_cast<unsigned char*>(
        RL_MALLOC(sizeof(unsigned char) * vert_count * 4));
    unsigned short* idx = static_cast<unsigned short*>(
        RL_MALLOC(sizeof(unsigned short) * tri_count * 3));
    std::copy(vertices.begin(), vertices.end(), verts);
    std::copy(texcoords.begin(), texcoords.end(), tcs);
    std::copy(texcoords2.begin(), texcoords2.end(), tc2s);
    std::copy(normals.begin(), normals.end(), nrms);
    std::copy(colors.begin(), colors.end(), cols);
    std::copy(indices.begin(), indices.end(), idx);

    ch.mesh.vertexCount = vert_count;
    ch.mesh.triangleCount = tri_count;
    ch.mesh.vertices = verts;
    ch.mesh.texcoords = tcs;
    ch.mesh.texcoords2 = tc2s;
    ch.mesh.normals = nrms;
    ch.mesh.colors = cols;
    ch.mesh.indices = idx;

    UploadMesh(&ch.mesh, false);
    ch.model = LoadModelFromMesh(ch.mesh);
    ch.model.materials[0].maps[MATERIAL_MAP_ALBEDO].color = WHITE;
    ch.model.materials[0].shader = cfg.terrain_shader->shader();
    ch.valid = true;
    ch.untextured_quads = untextured;
    return ch;
}

} // namespace

// ── build_terrain_chunk_set ────────────────────────────────────────────────

TerrainChunkSet build_terrain_chunk_set(
    const f4::terrain::TerrainData& terrain,
    const TerrainChunkSetConfig& config) {

    TerrainChunkSet tcs;
    tcs.config = config;

    if (terrain.elevation.empty()) {
        tcs.valid = false;
        return tcs;
    }

    // ── Textured path ─────────────────────────────────────────────────
    if (config.tiles && config.tiles->usable() && config.tile_cache &&
        config.terrain_shader && config.terrain_shader->is_loaded()) {
        const TerrainTileSource& tiles = *config.tiles;
        const auto& geom = *tiles.geometry;

        // Near region: quad range of the near level inside
        // [center - near_extent, center + near_extent].
        const double nftpp = geom.ft_per_post(tiles.near_level->level());
        const int nc0 = static_cast<int>(std::floor(
            (config.center_east_ft - config.near_extent_ft) / nftpp));
        const int nc1 = static_cast<int>(std::ceil(
            (config.center_east_ft + config.near_extent_ft) / nftpp));
        const int nr0 = static_cast<int>(std::floor(
            (config.center_north_ft - config.near_extent_ft) / nftpp));
        const int nr1 = static_cast<int>(std::ceil(
            (config.center_north_ft + config.near_extent_ft) / nftpp));

        // Far region: quad range of the far level inside the full
        // extent, inflated by one far post around the near rect so the
        // ring starts slightly under the near region's edge.
        const double fftpp = geom.ft_per_post(tiles.far_level->level());
        const int fc0 = static_cast<int>(std::floor(
            (config.center_east_ft - config.extent_ft) / fftpp));
        const int fc1 = static_cast<int>(std::ceil(
            (config.center_east_ft + config.extent_ft) / fftpp));
        const int fr0 = static_cast<int>(std::floor(
            (config.center_north_ft - config.extent_ft) / fftpp));
        const int fr1 = static_cast<int>(std::ceil(
            (config.center_north_ft + config.extent_ft) / fftpp));

        // Chunk-aligned (16 quads) iteration helpers.
        auto for_chunks = [&](int q0, int q1, auto&& emit) {
            for (int q = q0 - ((q0 % kQuadsPerChunkSide) + kQuadsPerChunkSide)
                              % kQuadsPerChunkSide; q < q1; q += kQuadsPerChunkSide)
                emit(q);
        };

        auto add_region_chunks = [&](const TexturedRegion& reg) {
            int cy = 0;
            for_chunks(reg.qmin_row, reg.qmax_row, [&](int qr0) {
                int cx = 0;
                for_chunks(reg.qmin_col, reg.qmax_col, [&](int qc0) {
                    // Far ring: skip chunks that lie ENTIRELY inside the
                    // near rect — the near region covers them. Edge chunks
                    // overlap and are hidden by the far z bias instead of
                    // being skipped (skipping partials would leave holes).
                    if (!reg.near_region) {
                        const float e = static_cast<float>(reg.ft_per_post);
                        const bool fully_inside =
                            qc0 * e >= config.center_east_ft - config.near_extent_ft &&
                            (qc0 + kQuadsPerChunkSide) * e <= config.center_east_ft + config.near_extent_ft &&
                            qr0 * e >= config.center_north_ft - config.near_extent_ft &&
                            (qr0 + kQuadsPerChunkSide) * e <= config.center_north_ft + config.near_extent_ft;
                        if (fully_inside) {
                            ++cx;
                            return;
                        }
                    }
                    TerrainChunk ch = build_one_textured_chunk(
                        terrain, tiles, *config.tile_cache, config, reg, qc0, qr0);
                    if (!ch.valid) { ++cx; return; }
                    ch.chunk_x = cx;
                    ch.chunk_y = cy;
                    tcs.min_up = std::min(tcs.min_up, ch.min_up);
                    tcs.max_up = std::max(tcs.max_up, ch.max_up);
                    tcs.quads_untextured += ch.untextured_quads;
                    const int quads = ch.mesh.triangleCount / 2;
                    if (reg.near_region) tcs.near_quads += quads;
                    else                 tcs.far_quads  += quads;
                    tcs.chunks.push_back(std::move(ch));
                    ++cx;
                });
                ++cy;
            });
        };

        tcs.min_east = config.center_east_ft - config.extent_ft;
        tcs.max_east = config.center_east_ft + config.extent_ft;
        tcs.min_north = config.center_north_ft - config.extent_ft;
        tcs.max_north = config.center_north_ft + config.extent_ft;
        tcs.min_up = 1e30f;
        tcs.max_up = -1e30f;

        TexturedRegion near_reg;
        near_reg.level = tiles.near_level;
        near_reg.ft_per_post = nftpp;
        near_reg.qmin_col = nc0; near_reg.qmax_col = nc1;
        near_reg.qmin_row = nr0; near_reg.qmax_row = nr1;
        near_reg.z_bias = 0.0f;
        near_reg.near_region = true;
        add_region_chunks(near_reg);

        TexturedRegion far_reg;
        far_reg.level = tiles.far_level;
        far_reg.ft_per_post = fftpp;
        far_reg.qmin_col = fc0; far_reg.qmax_col = fc1;
        far_reg.qmin_row = fr0; far_reg.qmax_row = fr1;
        far_reg.z_bias = config.far_z_bias_ft;
        far_reg.near_region = false;
        add_region_chunks(far_reg);

        tcs.textured = true;
        tcs.valid = !tcs.chunks.empty();
        tcs.chunks_total = static_cast<int>(tcs.chunks.size());
        tcs.chunks_visible = 0;
        return tcs;
    }

    // ── Legacy vertex-color path ──────────────────────────────────────
    const int n = std::max(1, config.chunks_per_side);
    const float total_extent = config.extent_ft * 2.0f;  // full side
    const float chunk_size_ft = total_extent / static_cast<float>(n);
    const float min_east  = config.center_east_ft  - config.extent_ft;
    const float min_north = config.center_north_ft - config.extent_ft;

    tcs.chunks.reserve(static_cast<std::size_t>(n) * n);
    tcs.min_east = min_east;
    tcs.max_east = min_east + total_extent;
    tcs.min_north = min_north;
    tcs.max_north = min_north + total_extent;
    tcs.min_up = 1e30f;
    tcs.max_up = -1e30f;

    for (int cy = 0; cy < n; ++cy) {
        for (int cx = 0; cx < n; ++cx) {
            const float ce = min_east  + cx * chunk_size_ft;
            const float cn = min_north + cy * chunk_size_ft;
            TerrainChunk ch = build_one_chunk(terrain, config, ce, cn,
                                                chunk_size_ft, cx, cy);
            tcs.min_up = std::min(tcs.min_up, ch.min_up);
            tcs.max_up = std::max(tcs.max_up, ch.max_up);
            tcs.chunks.push_back(std::move(ch));
        }
    }

    tcs.valid = true;
    tcs.chunks_total = static_cast<int>(tcs.chunks.size());
    tcs.chunks_visible = 0;
    return tcs;
}

// ── Frustum culling helper ─────────────────────────────────────────────────
//
// Simple AABB-vs-frustum test using Raylib's camera. We use the
// camera's forward vector + a dot-product test against the chunk's
// center, with a margin for the chunk's half-diagonal so chunks
// straddling the behind-plane aren't culled.
//
// This is a conservative test — a chunk straddling the frustum edge
// still passes — but cheap and correct for culling. For the typical
// orbit-camera case (looking at a target), the "behind the camera"
// plane is what culls ~50% of chunks; the side planes rarely matter
// at theater scale since the terrain extent is small vs. the far plane.

static bool chunk_in_frustum(const TerrainChunk& ch, const Camera3D& camera) {
    // Forward vector (camera → target), normalized.
    const Vector3 forward = {
        camera.target.x - camera.position.x,
        camera.target.y - camera.position.y,
        camera.target.z - camera.position.z
    };
    const float flen = std::sqrt(forward.x*forward.x + forward.y*forward.y + forward.z*forward.z);
    if (flen < 1e-6f) return true;  // degenerate camera — draw everything
    const Vector3 fwd_n = { forward.x / flen, forward.y / flen, forward.z / flen };

    // Chunk center in Raylib RH Y-up coords (X=east, Y=up, Z=-north).
    const Vector3 center = {
        (ch.min_east + ch.max_east) * 0.5f,
        (ch.min_up + ch.max_up) * 0.5f,
        -(ch.min_north + ch.max_north) * 0.5f
    };
    const Vector3 to_center = {
        center.x - camera.position.x,
        center.y - camera.position.y,
        center.z - camera.position.z
    };
    const float tc_len = std::sqrt(to_center.x*to_center.x + to_center.y*to_center.y + to_center.z*to_center.z);
    if (tc_len < 1e-6f) return true;

    // Dot of (to_center normalized) and forward — if > -margin, the
    // chunk is at least partially in front of (or beside) the camera.
    const float half_diag = std::sqrt(
        (ch.max_east - ch.min_east) * (ch.max_east - ch.min_east) +
        (ch.max_north - ch.min_north) * (ch.max_north - ch.min_north) +
        (ch.max_up - ch.min_up) * (ch.max_up - ch.min_up)
    ) * 0.5f;
    const float angle_dot =
        (to_center.x * fwd_n.x + to_center.y * fwd_n.y + to_center.z * fwd_n.z) / tc_len;
    // cos(half-diagonal-angle) ≈ 1 - half_diag / tc_len (small-angle approx).
    // If angle_dot > -(half_diag / tc_len), the chunk's near edge is in front.
    const float threshold = -half_diag / std::max(tc_len, 1.0f);
    return angle_dot > threshold;
}

// ── draw_terrain_chunk_set ─────────────────────────────────────────────────

void draw_terrain_chunk_set(TerrainChunkSet& tcs, const Camera3D& camera) {
    if (!tcs.valid) return;

    // Self-healing far-plane extension (same as draw_terrain_mesh).
    if (tcs.config.far_plane_ft > 0.0f &&
        tcs.config.far_plane_ft > tcs.config.near_plane_ft) {
        extend_far_plane(tcs.config.camera_fovy_deg,
                         /*projection_is_ortho=*/false,
                         tcs.config.near_plane_ft,
                         tcs.config.far_plane_ft);
    }

    // Textured sets: point the terrain shader's samplers at the tile
    // arrays (texture-unit bindings are global GL state; rebind per
    // frame since other draw paths touch unit 0).
    if (tcs.textured && tcs.config.terrain_shader) {
        tcs.config.terrain_shader->bind_tile_samplers(*tcs.config.tile_cache);
    }

    rlDisableBackfaceCulling();

    int visible = 0;
    for (auto& ch : tcs.chunks) {
        if (!ch.valid) continue;
        if (!chunk_in_frustum(ch, camera)) continue;
        DrawModel(ch.model, Vector3{0, 0, 0}, 1.0f, WHITE);
        ++visible;
    }
    tcs.chunks_visible = visible;

    rlEnableBackfaceCulling();
}

// ── unload_terrain_chunk_set ───────────────────────────────────────────────

void unload_terrain_chunk_set(TerrainChunkSet& tcs) {
    if (!tcs.valid) return;
    for (auto& ch : tcs.chunks) {
        if (ch.valid) {
            UnloadModel(ch.model);
            ch.model = {};
            ch.mesh = {};
            ch.valid = false;
        }
    }
    tcs.chunks.clear();
    tcs.valid = false;
    tcs.chunks_total = 0;
    tcs.chunks_visible = 0;
}

} // namespace f4::renderer
