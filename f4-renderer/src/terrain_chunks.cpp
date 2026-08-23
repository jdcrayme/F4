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
#include <f4/renderer/coord_transform.hpp>  // enu_to_raylib
#include <f4/renderer/draw_3d.hpp>          // extend_far_plane

#include <raylib.h>
#include <rlgl.h>              // rlDisableBackfaceCulling, rlEnableBackfaceCulling

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <vector>

namespace f4::renderer {

// ── Helpers shared with terrain_mesh.cpp ─────────────────────────────────────
//
// These three helpers (theater_ft_per_cell, world_to_cell_clamped,
// bilinear_elevation) are DUPLICATED from terrain_mesh.cpp. The
// alternative — exposing them in a shared internal header — would
// couple the two files more tightly than the duplication does. If a
// third consumer appears, extract them to a terrain_internal.hpp.
//
// Keep these in sync with terrain_mesh.cpp — same conventions, same
// hardcoded 1024*1024 ft theater size, same clamp behavior.

static double theater_ft_per_cell(const f4::terrain::TerrainData& td) {
    const double theater_size_ft = 1024.0 * 1024.0;
    const double w = static_cast<double>(td.header.width > 0 ? td.header.width : 128);
    return theater_size_ft / w;
}

static uint32_t world_to_cell_clamped(double world_ft, double ft_per_cell,
                                       uint32_t grid_size) {
    if (grid_size == 0) return 0;
    const double f = world_ft / ft_per_cell;
    const double clamped = std::clamp(f, 0.0, static_cast<double>(grid_size - 1));
    return static_cast<uint32_t>(clamped);
}

static double bilinear_elevation(const f4::terrain::TerrainData& td,
                                  double east_ft, double north_ft) {
    if (td.elevation.empty()) return 0.0;

    const double ft_per_cell = theater_ft_per_cell(td);
    if (ft_per_cell <= 0.0) return 0.0;

    const double fx = std::clamp(east_ft / ft_per_cell, 0.0,
                                  static_cast<double>(td.header.width - 1));
    const double fy = std::clamp(north_ft / ft_per_cell, 0.0,
                                  static_cast<double>(td.header.height - 1));

    const uint32_t x0 = static_cast<uint32_t>(fx);
    const uint32_t y0 = static_cast<uint32_t>(fy);
    const uint32_t x1 = std::min(x0 + 1, td.header.width - 1);
    const uint32_t y1 = std::min(y0 + 1, td.header.height - 1);

    const double tx = fx - static_cast<double>(x0);
    const double ty = fy - static_cast<double>(y0);

    const uint32_t sim_y0 = td.header.height - 1 - y0;
    const uint32_t sim_y1 = td.header.height - 1 - y1;

    const double e00 = td.elevation_at(x0, sim_y0);
    const double e10 = td.elevation_at(x1, sim_y0);
    const double e01 = td.elevation_at(x0, sim_y1);
    const double e11 = td.elevation_at(x1, sim_y1);

    const double e0 = e00 + (e10 - e00) * tx;
    const double e1 = e01 + (e11 - e01) * tx;
    return e0 + (e1 - e0) * ty;
}

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
