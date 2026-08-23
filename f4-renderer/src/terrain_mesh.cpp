// f4-renderer/src/terrain_mesh.cpp
//
// TerrainMesh implementation — builds a Raylib heightmap mesh from
// f4::terrain::TerrainData. See terrain_mesh.hpp for design.
//
// The mesh is a regular grid of (resolution+1)² vertices. Each vertex:
//   - position: (east_ft, up_ft, north_ft) in Raylib's Y-up convention
//     (note: Raylib's Y is up, so ENU up → Y; ENU east → X; ENU north → Z)
//   - color: from tile_type palette or a single grass-green color
//
// The mesh is uploaded to the GPU via UploadMesh() and wrapped in a
// Model with a default material (vertex colors enabled).

#include <f4/renderer/terrain_mesh.hpp>
#include <f4/renderer/coord_transform.hpp>  // enu_to_raylib
#include <f4/renderer/draw_3d.hpp>        // extend_far_plane

#include <raylib.h>
#include <rlgl.h>              // rlDisableBackfaceCulling, rlEnableBackfaceCulling

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <vector>

namespace f4::renderer {

// Forward declaration — the bilinear interpolation is in TerrainDataAdapter
// (f4-terrain), but we need it here too. We re-implement it locally to
// avoid the f4-terrain → f4-renderer dependency (f4-renderer already
// depends on f4-terrain transitively via f4-world, but keeping the
// interpolation local makes this file self-contained).
//
// Theater size in feet: hardcoded to 1024 × 1024 ft = 1,048,576 ft per
// side, matching TerrainDataAdapter::ft_per_cell() in f4-terrain. This
// is correct for Korea (the only theater currently supported). The
// THEATER.MAP header's `ft_to_mea_cell` field would in principle give
// a per-theater value, but its semantics are unclear (the Korea fixture
// reads 941611082, which doesn't match any obvious feet/cell ratio),
// so we keep the hardcoded constant until the field is reverse-engineered.
// See f4-terrain/include/f4/terrain/terrain_data.hpp for the field layout.
static double theater_ft_per_cell(const f4::terrain::TerrainData& td) {
    const double theater_size_ft = 1024.0 * 1024.0;
    const double w = static_cast<double>(td.header.width > 0 ? td.header.width : 128);
    return theater_size_ft / w;
}

static double bilinear_elevation(const f4::terrain::TerrainData& td,
                                  double east_ft, double north_ft) {
    if (td.elevation.empty()) return 0.0;

    const double ft_per_cell = theater_ft_per_cell(td);
    if (ft_per_cell <= 0.0) return 0.0;

    const double fx = east_ft / ft_per_cell;
    const double fy = north_ft / ft_per_cell;

    const double max_x = static_cast<double>(td.header.width - 1);
    const double max_y = static_cast<double>(td.header.height - 1);
    const double cx = std::clamp(fx, 0.0, max_x);
    const double cy = std::clamp(fy, 0.0, max_y);

    const uint32_t x0 = static_cast<uint32_t>(cx);
    const uint32_t y0 = static_cast<uint32_t>(cy);
    const uint32_t x1 = std::min(x0 + 1, td.header.width - 1);
    const uint32_t y1 = std::min(y0 + 1, td.header.height - 1);

    const double tx = cx - static_cast<double>(x0);
    const double ty = cy - static_cast<double>(y0);

    // Flip y: file row 0 = south, sim y = height-1 (north). Our fy has
    // y=0 at south (ENU), so flip to match TerrainData's sim convention.
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

TerrainMesh build_terrain_mesh(const f4::terrain::TerrainData& terrain,
                                const TerrainMeshConfig& config) {
    TerrainMesh tm;
    tm.config = config;

    if (terrain.elevation.empty()) {
        // No elevation data — can't build a meaningful mesh.
        tm.valid = false;
        return tm;
    }

    // ── Index type cap ───────────────────────────────────────────────────
    // Raylib's Mesh.indices is `unsigned short*`, so vertex indices must
    // fit in 65535. With (res+1)² vertices, the max vertex index is
    // (res+1)² - 1. Cap res at 254 (→ 65025 verts, max idx 65024) and
    // warn if the caller asked for more — a higher value would silently
    // wrap indices and produce "triangles stretching across the entire
    // terrain" artifacts.
    static constexpr int kMaxResolution = 254;  // (254+1)² - 1 = 65024
    int req_res = config.resolution;
    if (req_res > kMaxResolution) {
        std::fprintf(stderr,
            "terrain_mesh: resolution=%d exceeds unsigned-short index cap "
            "(max %d); clamping. Reduce resolution to avoid this warning.\n",
            req_res, kMaxResolution);
        req_res = kMaxResolution;
    }
    tm.config.resolution = req_res;

    const int res = std::max(2, req_res);
    const int vert_count = (res + 1) * (res + 1);
    const int tri_count = res * res * 2;

    // Allocate mesh arrays. Normals are required for the default lit
    // shader — without them, DrawModel renders everything black.
    std::vector<float> vertices(vert_count * 3);
    std::vector<float> texcoords(vert_count * 2);
    std::vector<float> normals(vert_count * 3, 0.0f);  // computed after vertices
    std::vector<unsigned char> colors(vert_count * 4);
    std::vector<unsigned short> indices(tri_count * 3);

    const float extent = config.extent_ft;
    const float step = (2.0f * extent) / static_cast<float>(res);
    const float min_east  = config.center_east_ft  - extent;
    const float min_north = config.center_north_ft - extent;

    float min_up = 1e30f, max_up = -1e30f;

    // Build vertices.
    for (int j = 0; j <= res; ++j) {
        for (int i = 0; i <= res; ++i) {
            const int idx = j * (res + 1) + i;
            const float east_ft  = min_east  + i * step;
            const float north_ft = min_north + j * step;

            const double elev = bilinear_elevation(terrain, east_ft, north_ft);
            const float up_ft = static_cast<float>(elev) * config.vertical_scale
                              + config.z_offset_ft;

            // Raylib RH Y-up: X=east, Y=up, Z=-north (see coord_transform.hpp).
            // ENU north maps to NEGATIVE Z in Raylib space. The previous
            // version used +north_ft for Z, which mirrored the mesh on the
            // Z axis and placed it at the wrong position relative to the
            // camera (which correctly uses -north).
            vertices[idx * 3 + 0] = east_ft;
            vertices[idx * 3 + 1] = up_ft;
            vertices[idx * 3 + 2] = -north_ft;   // FIXED: was +north_ft

            min_up = std::min(min_up, up_ft);
            max_up = std::max(max_up, up_ft);

            // Texture coords (0..1 across the mesh) — not used for texturing
            // but required by Raylib's default shader.
            texcoords[idx * 2 + 0] = static_cast<float>(i) / res;
            texcoords[idx * 2 + 1] = static_cast<float>(j) / res;

            // Color by tile type or single color.
            Color c;
            if (config.color_by_tile_type) {
                // Sample the terrain tile type at this position.
                // theater_ft_per_cell() handles the (currently hardcoded)
                // theater-size → cell-size conversion.
                const double ft_per_cell = theater_ft_per_cell(terrain);
                const uint32_t cx = std::min(static_cast<uint32_t>(east_ft / ft_per_cell),
                                              terrain.header.width - 1);
                const uint32_t cy_raw = std::min(static_cast<uint32_t>(north_ft / ft_per_cell),
                                                  terrain.header.height - 1);
                const uint32_t cy = terrain.header.height - 1 - cy_raw;  // flip to sim
                const auto tt = terrain.tile_type_at(cx, cy);
                const auto c4 = f4::terrain::TerrainData::color_for_tile_type(tt);
                c = {c4.r, c4.g, c4.b, c4.a};
            } else {
                c = {80, 110, 60, 255};  // grass green
            }
            colors[idx * 4 + 0] = c.r;
            colors[idx * 4 + 1] = c.g;
            colors[idx * 4 + 2] = c.b;
            colors[idx * 4 + 3] = c.a;
        }
    }

    // Build triangle indices (two triangles per grid cell).
    // Winding: CCW when viewed from above (+Y looking down) so the
    // triangles are front-facing under Raylib's default CCW convention.
    // With Z = -north, increasing j (north) decreases Z. The correct
    // CCW order for a quad (v00=v(i,j), v10=v(i+1,j), v01=v(i,j+1),
    // v11=v(i+1,j+1)) is: v00 → v10 → v01 and v10 → v11 → v01.
    int ti = 0;
    for (int j = 0; j < res; ++j) {
        for (int i = 0; i < res; ++i) {
            const int v00 = j * (res + 1) + i;
            const int v10 = j * (res + 1) + (i + 1);
            const int v01 = (j + 1) * (res + 1) + i;
            const int v11 = (j + 1) * (res + 1) + (i + 1);

            // Triangle 1: v00, v10, v01 (CCW from above)
            indices[ti++] = static_cast<unsigned short>(v00);
            indices[ti++] = static_cast<unsigned short>(v10);
            indices[ti++] = static_cast<unsigned short>(v01);

            // Triangle 2: v10, v11, v01 (CCW from above)
            indices[ti++] = static_cast<unsigned short>(v10);
            indices[ti++] = static_cast<unsigned short>(v11);
            indices[ti++] = static_cast<unsigned short>(v01);
        }
    }

    // Compute per-vertex normals by averaging face normals of adjacent
    // triangles. Required for the default lit shader — without normals,
    // DrawModel renders everything black (the shader multiplies vertex
    // color by N·L, and with zero normals that's zero).
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
        // Cross product a × b (face normal)
        const float nx = ay * bz - az * by;
        const float ny = az * bx - ax * bz;
        const float nz = ax * by - ay * bx;
        // Accumulate into the 3 vertices
        normals[i0*3+0] += nx; normals[i0*3+1] += ny; normals[i0*3+2] += nz;
        normals[i1*3+0] += nx; normals[i1*3+1] += ny; normals[i1*3+2] += nz;
        normals[i2*3+0] += nx; normals[i2*3+1] += ny; normals[i2*3+2] += nz;
    }
    // Normalize
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
            normals[i*3+1] = 1.0f;  // default up
            normals[i*3+2] = 0.0f;
        }
    }

    // Populate the Raylib Mesh struct.
    tm.mesh.vertexCount = vert_count;
    tm.mesh.triangleCount = tri_count;
    tm.mesh.vertices = vertices.data();
    tm.mesh.texcoords = texcoords.data();
    tm.mesh.normals = normals.data();
    tm.mesh.colors = colors.data();
    tm.mesh.indices = indices.data();

    // UploadMesh copies the data to GPU memory. The arrays we passed
    // are local to this function — UploadMesh makes a copy, so it's
    // safe for them to go out of scope. BUT: Raylib's UploadMesh does
    // NOT copy the indices array if it's already set — it just uploads
    // it. We need to keep the arrays alive until after UploadMesh.
    // Actually, UploadMesh uploads ALL set arrays to the GPU and the
    // mesh struct's pointers are then pointing at the LOCAL arrays.
    // We need to NOT free them until the mesh is unloaded. The clean
    // way: allocate the arrays on the heap and let unload_terrain_mesh
    // free them. But Raylib's UnloadMesh only frees the GPU buffers,
    // not the CPU-side arrays.
    //
    // The simplest correct approach: let Raylib own the CPU arrays.
    // UploadMesh with dynamic=true allocates GPU buffers; the CPU
    // arrays remain as the pointers we set. We must keep them alive.
    // Since we're returning a TerrainMesh by value, the local vectors
    // would be destroyed. So we MUST make a deep copy.
    //
    // The standard Raylib pattern: call UploadMesh(&mesh, true), which
    // uploads to GPU AND keeps the CPU arrays. The caller owns both.
    // We'll allocate the arrays on the heap and store them... but
    // TerrainMesh doesn't have room for that.
    //
    // CLEANEST FIX: use rlUploadMesh-style approach where we just let
    // Raylib manage everything. Actually, the standard pattern in
    // Raylib examples is:
    //   Mesh mesh = {0};
    //   mesh.vertices = RL_MALLOC(...);
    //   ... fill ...
    //   UploadMesh(&mesh, false);  // false = don't free CPU data
    // And then UnloadMesh() frees both CPU and GPU.
    //
    // Let's redo it with RL_MALLOC so UnloadMesh cleans up properly.

    // Free the std::vectors and re-allocate with RL_MALLOC.
    // (We built into std::vectors for bounds safety; now copy to
    //  Raylib-owned memory so UnloadMesh can free it.)

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

    tm.mesh.vertices = verts;
    tm.mesh.texcoords = tcs;
    tm.mesh.normals = nrms;
    tm.mesh.colors = cols;
    tm.mesh.indices = idx;

    UploadMesh(&tm.mesh, false);  // false = don't free CPU data on upload

    // Wrap in a Model so DrawModel works.
    tm.model = LoadModelFromMesh(tm.mesh);
    // Enable vertex colors on the material.
    tm.model.materials[0].maps[MATERIAL_MAP_ALBEDO].color = WHITE;

    tm.valid = true;
    tm.min_east = min_east;
    tm.max_east = min_east + 2.0f * extent;
    tm.min_north = min_north;
    tm.max_north = min_north + 2.0f * extent;
    tm.min_up = min_up;
    tm.max_up = max_up;

    return tm;
}

void draw_terrain_mesh(const TerrainMesh& tm) {
    if (!tm.valid) return;

    // ── Defensive far-plane extension ───────────────────────────────────
    // Raylib's BeginMode3D uses RL_CULL_DISTANCE_FAR (patched to 100000 ft
    // in the world-viewer, default 1000 ft elsewhere) as the projection
    // far plane. For theater-scale terrain, this is too small: a 50000-ft
    // half-extent mesh + a 20000-ft camera distance can put far-edge
    // vertices past the far plane, causing triangles to clip out at
    // certain camera angles (the "triangles disappearing at certain
    // angles" symptom) and producing visual stretches when a triangle
    // straddles the clip plane.
    //
    // When the caller sets TerrainMeshConfig::far_plane_ft > 0 (default
    // 250000 ft), we override the projection here so terrain rendering
    // is correct regardless of whether the caller remembered to call
    // extend_far_plane() themselves. This makes draw_terrain_mesh()
    // self-healing — the scenario-player already extends via
    // render_world(), and the world-viewer extends explicitly, but any
    // future caller gets the right behavior for free.
    //
    // The override preserves the camera's FOV (from the config) and the
    // active render target's aspect ratio (read inside extend_far_plane).
    if (tm.config.far_plane_ft > 0.0f &&
        tm.config.far_plane_ft > tm.config.near_plane_ft) {
        extend_far_plane(tm.config.camera_fovy_deg,
                         /*projection_is_ortho=*/false,
                         tm.config.near_plane_ft,
                         tm.config.far_plane_ft);
    }

    // Disable backface culling — the terrain mesh should be visible from
    // both above and below (e.g. when the camera dips below a ridge line).
    // Also protects against any winding-convention mismatch.
    rlDisableBackfaceCulling();
    // The mesh vertices are already in world ENU feet coordinates
    // (Raylib Y-up: X=east, Y=up, Z=-north). So we draw at the origin
    // with no transform — the vertex positions ARE the world positions.
    DrawModel(tm.model, Vector3{0, 0, 0}, 1.0f, WHITE);
    rlEnableBackfaceCulling();
}

void unload_terrain_mesh(TerrainMesh& tm) {
    if (!tm.valid) return;
    UnloadModel(tm.model);  // frees the mesh + material + GPU buffers
    tm.model = {};
    tm.mesh = {};
    tm.valid = false;
}

} // namespace f4::renderer
