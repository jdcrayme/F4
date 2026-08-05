// f4-models-viewer/src/scene.cpp
//
// Converts f4::models::ModelGeometry → Raylib ::Mesh array.
// For each f4::models::Mesh:
//   - Create a Raylib ::Mesh with vertexCount and triangleCount
//   - Fill vertex positions (applying LH Z-up → RH Y-up conversion)
//   - Fill normals (same conversion)
//   - Fill texcoords if present
//   - Fill colors if present (convert ABGR uint32_t to Raylib Color)
//   - Call UploadMesh(&mesh, true) to upload to GPU

#include "viewer_state.hpp"
#include "scene.hpp"

#include <f4/models/geometry.hpp>

#include <raylib.h>

#include <cstring>
#include <vector>

namespace f4::models_viewer {

// ── ABGR uint32 → Raylib Color ─────────────────────────────────────────────
// f4::models::Vertex::color is packed RGBA in memory but stored as a
// uint32_t. The byte order on little-endian is ABGR when read as uint32.
// We extract individual bytes to get R, G, B, A.
static Color unpack_color(uint32_t c) {
    return {
        static_cast<unsigned char>(c & 0xFF),         // R
        static_cast<unsigned char>((c >> 8) & 0xFF),  // G
        static_cast<unsigned char>((c >> 16) & 0xFF), // B
        static_cast<unsigned char>((c >> 24) & 0xFF)  // A
    };
}

// ── build_raylib_meshes ────────────────────────────────────────────────────
std::vector<::Mesh> build_raylib_meshes(const f4::models::ModelGeometry& geom) {
    std::vector<::Mesh> result;
    result.reserve(geom.meshes.size());

    for (const auto& src_mesh : geom.meshes) {
        if (src_mesh.vertices.empty() || src_mesh.triangles.empty()) continue;

        ::Mesh rm = {};
        rm.vertexCount = static_cast<int>(src_mesh.vertices.size());
        rm.triangleCount = static_cast<int>(src_mesh.triangles.size());

        // Allocate Raylib mesh arrays. Raylib expects these as float*
        // (3 floats per vertex for positions/normals, 2 for texcoords).
        rm.vertices = new float[rm.vertexCount * 3];
        rm.normals  = new float[rm.vertexCount * 3];
        rm.texcoords = new float[rm.vertexCount * 2];
        rm.colors   = new unsigned char[rm.vertexCount * 4];

        // Indices: Raylib uses an int* index buffer (3 per triangle).
        rm.indices = new unsigned short[rm.triangleCount * 3];

        bool has_normals = false;
        bool has_texcoords = false;
        bool has_colors = false;

        // Fill vertex attributes
        for (int i = 0; i < rm.vertexCount; ++i) {
            const auto& v = src_mesh.vertices[static_cast<std::size_t>(i)];

            // Position: LH Z-up → RH Y-up
            const Vector3 pos = to_raylib(v.position.x, v.position.y, v.position.z);
            rm.vertices[i * 3 + 0] = pos.x;
            rm.vertices[i * 3 + 1] = pos.y;
            rm.vertices[i * 3 + 2] = pos.z;

            // Normal: same conversion
            const Vector3 nrm = to_raylib(v.normal.x, v.normal.y, v.normal.z);
            rm.normals[i * 3 + 0] = nrm.x;
            rm.normals[i * 3 + 1] = nrm.y;
            rm.normals[i * 3 + 2] = nrm.z;
            if (v.normal.x != 0 || v.normal.y != 0 || v.normal.z != 0) {
                has_normals = true;
            }

            // Texcoords
            rm.texcoords[i * 2 + 0] = v.uv.u;
            rm.texcoords[i * 2 + 1] = v.uv.v;
            if (v.uv.u >= 0 || v.uv.v >= 0) {
                has_texcoords = true;
            }

            // Color: ABGR uint32 → RGBA bytes
            const Color c = unpack_color(v.color);
            rm.colors[i * 4 + 0] = c.r;
            rm.colors[i * 4 + 1] = c.g;
            rm.colors[i * 4 + 2] = c.b;
            rm.colors[i * 4 + 3] = c.a;
            if (v.color != 0) {
                has_colors = true;
            }
        }

        // Fill indices
        for (int i = 0; i < rm.triangleCount; ++i) {
            const auto& tri = src_mesh.triangles[static_cast<std::size_t>(i)];
            rm.indices[i * 3 + 0] = static_cast<unsigned short>(tri.v0);
            rm.indices[i * 3 + 1] = static_cast<unsigned short>(tri.v1);
            rm.indices[i * 3 + 2] = static_cast<unsigned short>(tri.v2);
        }

        // If no normals were present, zero out the normal array so
        // Raylib doesn't try to use garbage data.
        if (!has_normals) {
            std::memset(rm.normals, 0, rm.vertexCount * 3 * sizeof(float));
        }

        // Upload to GPU (dynamic = false since we don't update per-frame)
        UploadMesh(&rm, false);

        result.push_back(rm);
    }

    return result;
}

// ── unload_meshes ──────────────────────────────────────────────────────────
void unload_meshes(std::vector<::Mesh>& meshes) {
    for (auto& m : meshes) {
        UnloadMesh(m);
    }
    meshes.clear();
}

// ── Impl wrappers ─────────────────────────────────────────────────────────
// These are called from the main loop via Impl.

void ViewerApp::Impl::rebuild_meshes() {
    // Free old meshes
    f4::models_viewer::unload_meshes(raylib_meshes);
    total_tri_count = 0;

    if (!doc_loaded || selected_parent < 0) {
        meshes_dirty = false;
        return;
    }

    // Parse the selected LOD if not already parsed
    const auto* rec = db.model(selected_parent);
    if (!rec) {
        meshes_dirty = false;
        return;
    }

    // Ensure the selected LOD is parsed
    if (selected_lod >= 0 && selected_lod < static_cast<int>(rec->lods.size())) {
        std::string err = db.parse_lod(selected_parent, selected_lod);
        if (!err.empty()) {
            status_msg = "Parse error: " + err;
            meshes_dirty = false;
            return;
        }
    }

    // Extract geometry with current model state
    auto geom = db.extract_model_geometry(selected_parent, selected_lod, model_state);

    if (geom.meshes.empty()) {
        status_msg = "No geometry extracted for model " +
                     std::to_string(selected_parent) + " LOD " +
                     std::to_string(selected_lod);
        meshes_dirty = false;
        return;
    }

    // Convert to Raylib meshes
    raylib_meshes = build_raylib_meshes(geom);

    // Count triangles
    for (const auto& m : geom.meshes) {
        total_tri_count += m.triangle_count();
    }

    meshes_dirty = false;
    status_msg = "Loaded model " + std::to_string(selected_parent) +
                 " LOD " + std::to_string(selected_lod) +
                 " — " + std::to_string(total_tri_count) + " triangles";
}

void ViewerApp::Impl::unload_meshes() {
    f4::models_viewer::unload_meshes(raylib_meshes);
    total_tri_count = 0;
}

} // namespace f4::models_viewer
